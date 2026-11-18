/*
 * RedwoodPVR: the rabbit r1's PowerVR GE8320 for programs to draw with.
 *
 * bsd/dev/redwood_pvr.c brings the GPU and its firmware up (boot-arg pvr=6) and then starts this
 * service. A program that opens it speaks the interface of Linux's "powervr" DRM driver, the one
 * Mesa's pvr Vulkan driver knows: every request is a call of the user client with the request's
 * number as the selector and its structure passed in and back -- as RedwoodMali does it for the
 * Infinix's Mali and panfrost, and RedwoodGPU for the Adreno and freedreno. What Linux does with
 * mmap() on the device is the client memory of the type that is the buffer's handle.
 *
 * All of the interface itself is in bsd/dev/redwood_pvr_drm.c (bsd/sys/redwood_pvr.h between the
 * two). What is here is what IOKit has and the BSD side has not: the connection, and the buffers'
 * memory -- wired kernel memory, zero when made and write-combined, so what a program writes goes
 * to memory where the GPU reads it.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <mach/vm_param.h>
#include <sys/errno.h>

#include "MiniIOSurface.h"
#include "redwood_pvr.h"   /* a copy of bsd/sys/redwood_pvr.h: bsd/sys is not on the IOKit include path for new headers */

/* A buffer for redwood_pvr_drm.c. Whole kernel pages, of which the GPU maps the 4 KiB pieces. */
static void *
pvrMake(uint64_t bytes)
{
	return IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	           kIOMapWriteCombineCache, (vm_size_t) round_page_64(bytes), PAGE_SIZE);
}

/* (a buffer's memory is an IOBufferMemoryDescriptor made here, or an IOSubMemoryDescriptor over an IOSurface's: both IOMemoryDescriptors) */
static void
pvrKeep(void * memory)
{
	((IOMemoryDescriptor *) memory)->retain();
}

static void
pvrDrop(void * memory)
{
	((IOMemoryDescriptor *) memory)->release();
}

static uint64_t
pvrPhys(void * memory, uint64_t offset)
{
	IOByteCount length = 0;

	return ((IOMemoryDescriptor *) memory)->getPhysicalSegment(offset, &length, kIOMemoryMapperNone);
}

static void
pvrDisown(void * owner)
{
	MiniIOSurfaceDrop((MiniIOSurface *) owner);
}

static void *
pvrBytes(void * memory)
{
	return ((IOBufferMemoryDescriptor *) memory)->getBytesNoCopy();
}

static const struct redwood_pvr_bo_ops gPVRBufferOps = { pvrMake, pvrKeep, pvrDrop, pvrPhys, pvrBytes, pvrDisown };

/* REDWOOD_PVR_SURFACE's structure: tools/mesa/redwood-drm/xf86drm.h's struct redwood_drm_surface, as RedwoodMali's struct redwood_surface */
struct redwood_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* kRedwoodDescribe: no buffer is wanted; else, back: the buffer, */
	uint64_t size;                          /* how big it is, */
	uint32_t offset, rowBytes;              /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixelFormat, planes;    /* and what they are */
};
#define kRedwoodDescribe        0xffffffffu

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class RedwoodPVR : public IOService
{
	OSDeclareDefaultStructors(RedwoodPVR);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(RedwoodPVR, IOService);

bool
RedwoodPVR::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "RedwoodPVRUserClient");
	setName("RedwoodPVR");
	registerService();
	return true;
}

void
RedwoodPVRStart(void)
{
	IOService * provider = IOService::getPlatform();
	RedwoodPVR * gpu;

	redwood_pvr_provide(&gPVRBufferOps);
	gpu = OSTypeAlloc(RedwoodPVR);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("RedwoodPVR: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("RedwoodPVR: the service did not start\n");
	}
	gpu->release();
	IOLog("RedwoodPVR: the GPU is there for programs to draw with\n");
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class RedwoodPVRUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(RedwoodPVRUserClient);

	struct redwood_pvr_file *       fFile;
	uint32_t                        fRequests;

	IOReturn surfaceBuffer(struct redwood_surface * request);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(RedwoodPVRUserClient, IOUserClient);

bool
RedwoodPVRUserClient::start(IOService * provider)
{
	if (!redwood_pvr_ready() || !IOUserClient::start(provider)) {
		IOLog("RedwoodPVR: a client could not be started\n");
		return false;
	}
	fFile = redwood_pvr_open(false);
	IOLog("RedwoodPVR: a client opened the GPU\n");
	return fFile != NULL;
}

/* However the program went: everything it made is taken down, and the firmware told (redwood_pvr_close). */
IOReturn
RedwoodPVRUserClient::clientClose(void)
{
	struct redwood_pvr_file * file = fFile;

	fFile = NULL;
	if (file != NULL) {
		redwood_pvr_close(file);
	}
	terminate();
	return kIOReturnSuccess;
}

void
RedwoodPVRUserClient::free(void)
{
	if (fFile != NULL) {
		redwood_pvr_close(fFile);
		fFile = NULL;
	}
	IOUserClient::free();
}

/*
 * The pixels of an IOSurface as a buffer, from the page after the surface's shared state on
 * (MiniIOSurface.h), as RedwoodMali's surfaceBuffer: what the window server draws into and what is
 * shown. The surface is held for as long as the buffer is; a surface that is a buffer of this
 * client's already is that buffer again. Asked only to describe (kRedwoodDescribe), no buffer is
 * made. The buffer is whole kernel pages (16 KiB), so its size is a multiple of the GPU's 4 KiB.
 */
IOReturn
RedwoodPVRUserClient::surfaceBuffer(struct redwood_surface * request)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
	IOMemoryDescriptor * memory;
	uint64_t size;
	int error;

	if (s == NULL) {
		return (IOReturn) (REDWOOD_PVR_ERRNO | ENOENT);
	}
	if (s->client != NULL) {                /* a program's own memory with a surface around it: not whole pages */
		MiniIOSurfaceDrop(s);
		return (IOReturn) (REDWOOD_PVR_ERRNO | ENOTSUP);
	}
	size = round_page_64(s->info.allocSize);
	request->size = size;
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kRedwoodDescribe) {
		request->handle = 0;
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}
	memory = size != 0 ? IOSubMemoryDescriptor::withSubRange(s->memory, PAGE_SIZE, size, kIODirectionInOut) : NULL;
	if (memory == NULL) {
		MiniIOSurfaceDrop(s);
		return (IOReturn) (REDWOOD_PVR_ERRNO | ENOMEM);
	}
	error = redwood_pvr_import(fFile, memory, size, s, &request->handle);
	if (error != 0) {                       /* EEXIST: it is that buffer already, which has its own hold on both */
		memory->release();
		MiniIOSurfaceDrop(s);
	}
	return error == 0 || error == EEXIST ? kIOReturnSuccess : (IOReturn) (REDWOOD_PVR_ERRNO | (uint32_t) error);
}

/* mmap(): the buffer whose handle is the type, if it was made for a program to see (DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS). */
IOReturn
RedwoodPVRUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	void * kept = fFile != NULL ? redwood_pvr_memory(fFile, type) : NULL;

	if (kept == NULL) {
		return kIOReturnBadArgument;
	}
	*memory = (IOMemoryDescriptor *) kept;          /* kept once for us; IOKit lets it go with the mapping */
	*options = 0;
	return kIOReturnSuccess;
}

IOReturn
RedwoodPVRUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	uint8_t r[160];                         /* the largest request's structure is CREATE_HWRT_DATASET's, 136 bytes */
	size_t size = args->structureInputSize;
	int error;

	if (args->structureInput == NULL || size > sizeof(r) || fFile == NULL) {
		return kIOReturnBadArgument;
	}
	bzero(r, sizeof(r));
	memcpy(r, args->structureInput, size);
	if (fRequests++ < 48) {                 /* the first ones a client makes, for following it from the log */
		IOLog("RedwoodPVR: request %#x, %zu bytes\n", selector, size);
	}
	if (selector == REDWOOD_PVR_SURFACE) {
		IOReturn ret = size >= sizeof(struct redwood_surface) ? surfaceBuffer((struct redwood_surface *) r) : kIOReturnBadArgument;

		if (args->structureOutput != NULL && args->structureOutputSize >= size) {
			memcpy(args->structureOutput, r, size);
			args->structureOutputSize = (uint32_t) size;
		}
		return ret;
	}
	error = redwood_pvr_request(fFile, selector, r, (uint32_t) size);
	if (error != 0 && error != ETIME && fRequests <= 4096) {      /* (a wait that ran out of time is an answer, not a failure) */
		IOLog("RedwoodPVR: request %#x (%zu bytes) failed: errno %d\n", selector, size, error);
	}
	if (args->structureOutput != NULL && args->structureOutputSize >= size) {
		memcpy(args->structureOutput, r, size);
		args->structureOutputSize = (uint32_t) size;
	}
	return error == 0 ? kIOReturnSuccess : (IOReturn) (REDWOOD_PVR_ERRNO | (uint32_t) error);
}
