/*
 * The libdrm of xf86drm.h for Mesa's PowerVR Vulkan driver (src/imagination, its "powervr" winsys):
 * Linux's DRM requests to the kernel's PowerVR driver as calls of an IOKit user client, the service
 * RedwoodPVR (iokit/Kernel/RedwoodPVR.cpp, bsd/dev/redwood_pvr_drm.c, bsd/sys/redwood_pvr.h). Beside
 * redwood_drm.c (freedreno's and panfrost's) and never linked with it: tools/mk-mesa-pvr.sh builds
 * this file into libvulkan_powervr_mesa.
 *
 *   - A request's number -- the low byte of Linux's ioctl code -- is the selector of
 *     IOConnectCallStructMethod, its structure (160 bytes at most) goes in and comes back as it is.
 *     pvr_drm.h's are 0x40..0x4d; of drm.h's the kernel has GEM_CLOSE 0x09 and the syncobjs' CREATE
 *     0xbf, DESTROY 0xc0, WAIT 0xc3, RESET 0xc4, SIGNAL 0xc5 and TRANSFER 0xcc. Pointers inside a
 *     structure are addresses in this task, which the kernel follows.
 *   - A failed request comes back as 0xe0008000 with Linux's errno in the low byte.
 *   - The timeout of SYNCOBJ_WAIT is how long to wait from now, not Linux's CLOCK_MONOTONIC time:
 *     converted here.
 *   - There are no timeline syncobjs (drmGetCap says so, and Mesa then keeps the timelines itself:
 *     tools/mesa/pvr/pvr-darwin.patch). What is kept of a timeline is that a later point waits for
 *     the earlier ones: SYNCOBJ_TRANSFER to a point adds the source's fence to what the destination
 *     waits for (E2BIG: too many, wait for one instead), and a wait takes no notice of points.
 *   - A buffer's memory in this process is the client memory whose type is the buffer's handle
 *     (IOConnectMapMemory64), for buffers made with DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS;
 *     GET_BO_MMAP_OFFSET (0x42) is not asked.
 *   - A fence as a sync file is a descriptor of /dev/null's that stands for the fence's number
 *     (the kernel's requests 0x7a, export, and 0x7b, import), in this process only, as panfrost's.
 *   - Memory somebody else made comes in as an IOSurface, by its ID where Linux has a dma-buf's
 *     descriptor: request 0x7e, struct redwood_drm_surface, as RedwoodGPU and RedwoodMali have it
 *     (asked with handle 0xffffffff it only describes the surface; otherwise it makes a buffer of
 *     it and says the handle, the same handle for the same surface while it is open). The buffer
 *     is mapped like any other and its size is whole pages of this system's (16 KiB). Nothing goes
 *     out as a descriptor.
 *   - A job that has not finished ten seconds after it was started (boot-arg pvr_timeout) makes
 *     its context faulty: its fences are signalled and SUBMIT_JOBS says EIO from then on.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>

#include "xf86drm.h"

#define REDWOOD_PVR_SERVICE     "RedwoodPVR"
#define REDWOOD_GPU_SURFACE     0x7e            /* the request that is not Linux's (redwood_drm.c) */
#define REDWOOD_GPU_DESCRIBE    0xffffffffu
#define REDWOOD_PVR_ERRNO       0xe0008000u     /* bsd/sys/redwood_pvr.h: a failed request, with the errno in the low byte */
#define REDWOOD_PVR_FENCE_EXPORT 0x7a
#define REDWOOD_PVR_FENCE_IMPORT 0x7b

struct redwood_pvr_fence {
	uint32_t handle, pad;
	uint64_t fence;                         /* 0: none; ~0: signalled already; else a job, done or not */
};
#define DEVICE_NODE             "/dev/null"     /* what the driver opens and gets a descriptor of */

static io_connect_t connection;
static pthread_once_t connected = PTHREAD_ONCE_INIT;

/*
 * What goes wrong between here and the kernel, for the kernel's log: a program's stderr is in no log,
 * and the kernel keeps a line given to kern.redwood.say with "!" first and says it again at the end.
 * The first 24 only: Mesa asks some things to see whether they are there.
 */
static void
say(const char *format, ...)
{
	static int said;
	char line[200] = "!rwpvr: ";
	va_list ap;

	if (__atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) >= 24) {
		return;
	}
	va_start(ap, format);
	vsnprintf(line + 8, sizeof(line) - 8, format, ap);
	va_end(ap);
	fprintf(stderr, "%s\n", line + 1);
	sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
}

static void
connect_once(void)
{
	io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching(REDWOOD_PVR_SERVICE));
	kern_return_t result;

	if (service == 0) {
		say("no service " REDWOOD_PVR_SERVICE " in the kernel (boot-arg pvr=6?)");
		return;
	}
	result = IOServiceOpen(service, mach_task_self(), 0, &connection);
	if (result != 0) {
		say("IOServiceOpen of " REDWOOD_PVR_SERVICE ": %#x", result);
		connection = 0;
	} else {
		say("the service " REDWOOD_PVR_SERVICE " is open");
	}
	IOObjectRelease(service);
}

/*
 * What is asked of the kernel, counted: every ten seconds one line for the kernel to keep -- the five
 * requests that took the most time, with how often and how long, and how many of each kind of thing
 * the kernel now holds for this program (made less given back), which is where a leak shows:
 * buffers (0x41 CREATE_BO and 0x7e, less 0x09 GEM_CLOSE), their mappings for the GPU (0x45 VM_MAP less
 * 0x46) and for this program, contexts (0x47 less 0x48), free lists (0x49, 0x4a), render target data
 * (0x4b CREATE_HWRT_DATASET, 0x4c) and syncobjs (0xbf, 0xc0).
 */
static uint64_t call_count[256], call_ns[256], calls_said_ns, map_ns;
static int64_t live_bos, live_vm_maps, live_maps, live_contexts, live_free_lists, live_hwrts, live_syncobjs;

static void
calls_note(unsigned number, uint64_t began, uint64_t ended, int good)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	char line[200] = "!rwpvr: 10 s:";
	int length = 13;

	number &= 0xff;
	__atomic_fetch_add(&call_count[number], 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&call_ns[number], ended - began, __ATOMIC_RELAXED);
	if (good) {
		int64_t *live = number == 0x41 || number == 0x09 ? &live_bos : number == 0x45 || number == 0x46 ? &live_vm_maps :
		    number == 0x47 || number == 0x48 ? &live_contexts : number == 0x49 || number == 0x4a ? &live_free_lists :
		    number == 0x4b || number == 0x4c ? &live_hwrts : number == 0xbf || number == 0xc0 ? &live_syncobjs : NULL;

		if (live != NULL) {
			__atomic_fetch_add(live, number == 0x09 || number == 0x46 || number == 0x48 || number == 0x4a ||
			    number == 0x4c || number == 0xc0 ? -1 : 1, __ATOMIC_RELAXED);
		}
	}
	if (calls_said_ns == 0) {
		calls_said_ns = ended;
	}
	if (ended - calls_said_ns < 10000000000ull || pthread_mutex_trylock(&lock) != 0) {
		return;
	}
	if (ended - calls_said_ns >= 10000000000ull) {
		for (int top = 0; top < 5; top++) {
			int most = -1;

			for (int i = 0; i < 256; i++) {
				if (call_count[i] != 0 && (most < 0 || call_ns[i] > call_ns[most])) {
					most = i;
				}
			}
			if (most < 0) {
				break;
			}
			length += snprintf(line + length, sizeof(line) - (size_t)length, " %#x x%llu %llums", most,
			    (unsigned long long)call_count[most], (unsigned long long)(call_ns[most] / 1000000));
			call_count[most] = 0;
			call_ns[most] = 0;
		}
		snprintf(line + length, sizeof(line) - (size_t)length, "; maps %llums; held: bo %lld vm %lld map %lld ctx %lld fl %lld "
		    "hwrt %lld sync %lld", (unsigned long long)(map_ns / 1000000), (long long)live_bos, (long long)live_vm_maps,
		    (long long)live_maps, (long long)live_contexts, (long long)live_free_lists, (long long)live_hwrts,
		    (long long)live_syncobjs);
		memset(call_count, 0, sizeof(call_count));
		memset(call_ns, 0, sizeof(call_ns));
		map_ns = 0;
		calls_said_ns = ended;
		sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
	}
	pthread_mutex_unlock(&lock);
}

/* 0, or an errno */
static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result;
	uint64_t began;

	pthread_once(&connected, connect_once);
	if (connection == 0) {
		return ENODEV;
	}
	began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	result = IOConnectCallStructMethod(connection, number & 0xff, data, size, data, &returned);
	calls_note(number, began, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), result == 0);
	if (result == 0) {
		return 0;
	}
	if ((number & 0xff) != 0xc3 || (result & 0xff) != ETIME) {      /* (a wait that ends unanswered is no fault) */
		say("request %#x (%zu bytes): %#x%s", number & 0xff, size, result,
		    ((uint32_t)result & 0xffffff00u) == REDWOOD_PVR_ERRNO ? ", an errno in its low byte" : ", IOKit's own");
	}
	return ((uint32_t)result & 0xffffff00u) == REDWOOD_PVR_ERRNO ? (int)(result & 0xff) : EIO;
}

/* as libdrm's functions answer: 0, or -1 with errno */
static int
answer(int error)
{
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	return answer(call((unsigned)request, arg, IOCPARM_LEN(request)));
}

/* (libdrm's way to a driver's own requests, by their index: Mesa's loader has a use of it for other drivers) */
int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}

/* The list of devices: the one GPU, where the kernel has it. */
static drmDevicePtr
device_new(void)
{
	static char *compatible[] = { "img,img-rogue", NULL };
	static drmPlatformDeviceInfo info = { compatible };
	static drmPlatformBusInfo bus = { "redwood-pvr" };
	static char *nodes[DRM_NODE_MAX] = { "", "", DEVICE_NODE };     /* no display: nothing to stat() */
	drmDevicePtr device = calloc(1, sizeof(*device));

	if (device != NULL) {
		device->nodes = nodes;
		device->available_nodes = 1 << DRM_NODE_RENDER;
		device->bustype = DRM_BUS_PLATFORM;
		device->businfo.platform = &bus;
		device->deviceinfo.platform = &info;
	}
	return device;
}

int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	(void)flags;
	pthread_once(&connected, connect_once);
	if (connection == 0) {
		return 0;
	}
	if (devices != NULL && max_devices > 0 && (devices[0] = device_new()) == NULL) {
		return -ENOMEM;
	}
	return 1;
}

void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	for (int i = 0; i < count; i++) {
		free(devices[i]);
		devices[i] = NULL;
	}
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)fd;
	(void)flags;
	*device = device_new();
	return *device == NULL ? -ENOMEM : 0;
}

void
drmFreeDevice(drmDevicePtr *device)
{
	free(*device);
	*device = NULL;
}

int
drmGetDeviceFromDevId(dev_t dev_id, uint32_t flags, drmDevicePtr *device)
{
	(void)dev_id;
	return drmGetDevice2(-1, flags, device);
}

/* (Mesa's OpenGL -- its loader and EGL -- asks these of the descriptor it opened) */
char *
drmGetDeviceNameFromFd2(int fd)
{
	(void)fd;
	return strdup(DEVICE_NODE);
}

char *
drmGetPrimaryDeviceNameFromFd(int fd)
{
	(void)fd;
	return strdup(DEVICE_NODE);
}

char *
drmGetRenderDeviceNameFromFd(int fd)
{
	(void)fd;
	return strdup(DEVICE_NODE);
}

int
drmGetNodeTypeFromFd(int fd)
{
	(void)fd;
	return DRM_NODE_RENDER;
}

int
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
	return a != NULL && b != NULL;          /* there is one */
}

int
drmIsKMS(int fd)
{
	(void)fd;
	return 0;
}

int
drmDropMaster(int fd)
{
	(void)fd;
	return 0;
}

/* The driver calls itself what Linux's is called (pvr_winsys.c picks its winsys by the name). */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version != NULL) {
		version->version_major = 1;             /* Linux's: 1.0 */
		version->name = strdup("powervr");
		version->name_len = (int)strlen(version->name);
		version->date = strdup("");
		version->desc = strdup("PowerVR on Darwin");
		version->desc_len = (int)strlen(version->desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	switch (capability) {
	case DRM_CAP_SYNCOBJ:
		*value = 1;
		break;
	default:        /* no timelines (DRM_CAP_SYNCOBJ_TIMELINE), no PRIME */
		*value = 0;
	}
	return 0;
}

/* Buffers. */
void *
redwood_pvr_map(int fd, uint32_t handle, uint64_t size, void *address)
{
	mach_vm_address_t at = (mach_vm_address_t)(uintptr_t)address;
	mach_vm_size_t mapped = 0;

	kern_return_t result = kIOReturnNotOpen;
	uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

	(void)fd;
	pthread_once(&connected, connect_once);
	if (connection == 0 ||
	    (result = IOConnectMapMemory64(connection, handle, mach_task_self(), &at, &mapped,
	        address == NULL ? kIOMapAnywhere : kIOMapOverwrite)) != 0 ||
	    mapped < size) {
		say("mapping buffer %u (%llu bytes): %#x, %llu bytes mapped", handle, (unsigned long long)size, result,
		    (unsigned long long)mapped);
		errno = ENOMEM;
		return (void *)-1;              /* mmap()'s MAP_FAILED */
	}
	__atomic_fetch_add(&map_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began, __ATOMIC_RELAXED);
	__atomic_fetch_add(&live_maps, 1, __ATOMIC_RELAXED);
	return (void *)(uintptr_t)at;
}

void
redwood_pvr_unmap(int fd, uint32_t handle, void *address)
{
	uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

	(void)fd;
	IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
	__atomic_fetch_add(&map_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began, __ATOMIC_RELAXED);
	__atomic_fetch_add(&live_maps, -1, __ATOMIC_RELAXED);
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close request = { .handle = handle };

	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &request);
}

int
redwood_drm_surface(int fd, struct redwood_drm_surface *surface)
{
	(void)fd;
	surface->handle = REDWOOD_GPU_DESCRIBE;
	return answer(call(REDWOOD_GPU_SURFACE, surface, sizeof(*surface)));
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct redwood_drm_surface request = { .surface = (uint32_t)surface };
	int error = call(REDWOOD_GPU_SURFACE, &request, sizeof(request));

	(void)fd;
	*handle = request.handle;
	/* (a buffer the kernel holds, counted with the others: the same surface again is the same buffer, and pvr
	 * closes the handle once for all of them) */
	return answer(error);
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd;
	(void)handle;
	(void)flags;
	(void)prime_fd;
	return answer(ENOSYS);
}

/* Fences: the kernel's syncobjs, by Linux's requests. */
static int
syncobj_call(unsigned long request, void *arg)
{
	return answer(call((unsigned)request, arg, IOCPARM_LEN(request)));
}

/* Linux's absolute CLOCK_MONOTONIC time as the wait's length from now */
static int64_t
from_now(int64_t absolute_ns)
{
	struct timespec now;
	int64_t now_ns;

	clock_gettime(CLOCK_MONOTONIC, &now);
	now_ns = (int64_t)now.tv_sec * 1000000000ll + now.tv_nsec;
	return absolute_ns <= now_ns ? 0 : absolute_ns - now_ns;
}

/*
 * Syncobjs given back, kept for the next that is asked for: Mesa makes and destroys some seventeen
 * a frame (a fence for every submission, one for every job), two requests each; one that is kept
 * costs one, to make it as a new one is -- without a fence, or signalled.
 * ponytail: SYNCOBJS of them at most, and what a kept one's old fence holds in the kernel stays held
 * until it is used again; a request that makes one without a fence and gives it back unused could
 * do without the kernel altogether, if this is still seen in its numbers.
 */
#define SYNCOBJS 48
static uint32_t syncobj_kept[SYNCOBJS];
static unsigned syncobj_kept_count;
static pthread_mutex_t syncobj_lock = PTHREAD_MUTEX_INITIALIZER;

int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count);

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	struct drm_syncobj_create request = { .flags = flags };
	uint32_t kept = 0;
	int result;

	pthread_mutex_lock(&syncobj_lock);
	if (syncobj_kept_count != 0) {
		kept = syncobj_kept[--syncobj_kept_count];
	}
	pthread_mutex_unlock(&syncobj_lock);
	if (kept != 0) {
		if (((flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0 ? drmSyncobjSignal(fd, &kept, 1) : drmSyncobjReset(fd, &kept, 1)) == 0) {
			*handle = kept;
			return 0;
		}
		request.handle = kept;          /* (not to be had as new: gone, and a new one made) */
		syncobj_call(DRM_IOCTL_SYNCOBJ_DESTROY, &(struct drm_syncobj_destroy){ .handle = kept });
		request.handle = 0;
	}
	result = syncobj_call(DRM_IOCTL_SYNCOBJ_CREATE, &request);
	*handle = request.handle;
	return result;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	struct drm_syncobj_destroy request = { .handle = handle };
	int kept = 0;

	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	if (handle != 0 && syncobj_kept_count < SYNCOBJS) {
		syncobj_kept[syncobj_kept_count++] = handle;
		kept = 1;
	}
	pthread_mutex_unlock(&syncobj_lock);
	return kept ? 0 : syncobj_call(DRM_IOCTL_SYNCOBJ_DESTROY, &request);
}

/* (the kernel takes 64 handles at most) */
int
drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout, unsigned flags, uint32_t *first)
{
	struct drm_syncobj_wait request = {
		.handles = (uintptr_t)handles,
		.timeout_nsec = from_now(timeout),
		.count_handles = count,
		.flags = flags,
	};
	int result = syncobj_call(DRM_IOCTL_SYNCOBJ_WAIT, &request);

	(void)fd;
	if (result == 0 && first != NULL) {
		*first = request.first_signaled;
	}
	return result;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	struct drm_syncobj_array request = { .handles = (uintptr_t)handles, .count_handles = count };

	(void)fd;
	return syncobj_call(DRM_IOCTL_SYNCOBJ_RESET, &request);
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	struct drm_syncobj_array request = { .handles = (uintptr_t)handles, .count_handles = count };

	(void)fd;
	return syncobj_call(DRM_IOCTL_SYNCOBJ_SIGNAL, &request);
}

/* (to a point: the source's fence is added to the destination's; otherwise it becomes the destination's) */
int
drmSyncobjTransfer(int fd, uint32_t dst, uint64_t dst_point, uint32_t src, uint64_t src_point, uint32_t flags)
{
	struct drm_syncobj_transfer request = {
		.src_handle = src,
		.dst_handle = dst,
		.src_point = src_point,
		.dst_point = dst_point,
		.flags = flags,
	};

	(void)fd;
	return syncobj_call(DRM_IOCTL_SYNCOBJ_TRANSFER, &request);
}

/*
 * Sync files: a descriptor of /dev/null's, and by its number the fence it was exported with (the
 * kernel's number for it). The driver closes the descriptor with close(); the number in the table
 * is written over when the descriptor comes round again.
 * ponytail: a descriptor past the table has no fence and reads as signalled; a table that grows,
 * if a process ever holds more than SYNC_FILES descriptors.
 */
#define SYNC_FILES 4096
#define FENCE_SIGNALLED (~0ull)
static uint64_t sync_file_fence[SYNC_FILES];

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	struct redwood_pvr_fence request = { .handle = handle };
	int error = call(REDWOOD_PVR_FENCE_EXPORT, &request, sizeof(request));
	int file;

	(void)fd;
	if (error != 0 || request.fence == 0) {
		return answer(error != 0 ? error : EINVAL);     /* no fence yet: EINVAL, as Linux says */
	}
	file = open(DEVICE_NODE, O_RDONLY | O_CLOEXEC);
	if (file < 0) {
		return -1;
	}
	if (file < SYNC_FILES) {
		__atomic_store_n(&sync_file_fence[file], request.fence, __ATOMIC_RELEASE);
	}
	*sync_file_fd = file;
	return 0;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	struct redwood_pvr_fence request = { .handle = handle, .fence = FENCE_SIGNALLED };

	(void)fd;
	if (sync_file_fd >= 0 && sync_file_fd < SYNC_FILES) {
		request.fence = __atomic_load_n(&sync_file_fence[sync_file_fd], __ATOMIC_ACQUIRE);
	}
	return answer(call(REDWOOD_PVR_FENCE_IMPORT, &request, sizeof(request)));
}

/* (no timelines: the points are not looked at -- what a point waits for was put into the syncobj by TRANSFER) */
int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t timeout, unsigned flags,
    uint32_t *first)
{
	(void)points;
	return drmSyncobjWait(fd, handles, count, timeout, flags, first);
}

/* Timelines' other requests, and syncobjs as descriptors for other processes: none of that here. */
#define NONE(name, error, ...) int name(__VA_ARGS__) { return answer(error); }
#define UNUSED __attribute__((unused))
NONE(drmSyncobjTimelineSignal, ENOTTY, UNUSED int fd, UNUSED const uint32_t *handles, UNUSED uint64_t *points, UNUSED uint32_t count)
NONE(drmSyncobjQuery2, ENOTTY, UNUSED int fd, UNUSED uint32_t *handles, UNUSED uint64_t *points, UNUSED uint32_t count,
    UNUSED uint32_t flags)
NONE(drmSyncobjQuery, ENOTTY, UNUSED int fd, UNUSED uint32_t *handles, UNUSED uint64_t *points, UNUSED uint32_t count)
NONE(drmSyncobjHandleToFD, ENOSYS, UNUSED int fd, UNUSED uint32_t handle, UNUSED int *obj_fd)
NONE(drmSyncobjFDToHandle, ENOSYS, UNUSED int fd, UNUSED int obj_fd, UNUSED uint32_t *handle)
