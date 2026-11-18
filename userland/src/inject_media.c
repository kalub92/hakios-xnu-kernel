/*
 * libredwood-media.dylib: code of ours inside mediaserverd (see inject.c for how it gets there).
 *
 * mediaserverd is the audio server, and also the camera's: the capture stack of CMCapture lives
 * in the same process. Soon after it starts, that stack "prewarms" the Core Image filters of the
 * portrait mode, for which Core Image wants an OpenGL ES context; there is no GPU, the context
 * has no functions behind it, and the daemon jumps to address 0 -- every time launchd restarts
 * it. Nobody is going to take a portrait here: the prewarming is skipped.
 *
 * What the daemon is wanted for meanwhile is neither sound nor the camera: it is where
 * VideoToolbox decodes for everybody else (the service com.apple.coremedia.decompressionsession),
 * and a HEIC picture is HEVC inside. Without the daemon ImageIO has "no way to decode a HEIC",
 * and the stock wallpaper of iOS 16, whose layers are four such pictures, is its gradient alone.
 * There is no hardware decoder here; VideoToolbox takes its software one (VCPHEVC.videocodec).
 *
 * The flashlight (only where the kernel has one: torch=3, /dev/redwood-torch): Control Center's
 * button and AVFlashlight reach FigFlashlight in here, which asks the capture stack's device vendor
 * (+[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor]) for the camera device and its back
 * camera's stream, and sets the stream's "TorchLevel" (0 to 1). Without a camera there is no
 * vendor (it is nil), every message to it does nothing, and FigFlashlight says it has set the level
 * when nothing has been. So where there is no vendor, FigFlashlight -- and only FigFlashlight,
 * by where it calls from -- is given one of ours, with a device and a stream whose torch level goes
 * to the kernel (see flashlight_*() below). Everybody else still gets none, as before.
 *
 * The camera (only where the kernel has one: cam=5, /dev/redwood-camera): the vendor is made for
 * everybody, with the create function of our capture device (camera_device.c) in place of the
 * one H10ISP.mediacapture would give for the iPhone's ISP. NOT FINISHED: no frames reach
 * applications yet (see camera_device.c). Its stream takes the torch level too, so FigFlashlight
 * would need nothing of its own then (not tried yet).
 *
 * CMCapture is not loaded yet when this library is: the methods are replaced when its image
 * arrives.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <notify.h>
#include <execinfo.h>
#include <objc/message.h>
#include <objc/objc-exception.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */

static uintptr_t slide;
uintptr_t redwood_cache_slide(void);
uintptr_t
redwood_cache_slide(void)
{
	return slide;
}

/* +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:] */
static void
prewarm_portrait_filter(__unused id self, __unused SEL selector, __unused int version, __unused BOOL styles)
{
}

/* ---- the flashlight ------------------------------------------------------------------------------- */

static int torch = -1;                  /* /dev/redwood-torch */
static bool camera;                     /* /dev/redwood-camera is there */
extern int redwood_camera_device_create(const void *allocator, const void *options, void **out);
static Class vendor_class, device_class, stream_class;
static IMP original_shared_vendor;
static unsigned torch_said;

/*
 * FigFlashlight's functions that ask for the vendor, unslid (20F66): flashlight_IsAvailable,
 * _NotifyForCurrentState and _setupDevice lie together, _teardownDevice apart.
 */
static const struct { uintptr_t from, to; } flashlight_code[] = {
	{ 0x1a1fb0830UL, 0x1a1fb0c5cUL }, { 0x1a1eee5c0UL, 0x1a1eee6e8UL },
};

/* +[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor] */
static id
shared_vendor(id self, SEL selector)
{
	uintptr_t caller = (uintptr_t)__builtin_return_address(0) - slide;
	id vendor = ((id (*)(id, SEL))original_shared_vendor)(self, selector);
	static id ours, camera_vendor;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	if (vendor != NULL) {
		return vendor;
	}
	if (!camera && access("/dev/redwood-camera", F_OK) == 0) {
		camera = true;          /* (should the kernel have made it after mediaserverd started) */
		say("redwood: mediaserverd: the kernel has a camera (/dev/redwood-camera)\n");
	}
	if (camera) {
		pthread_mutex_lock(&lock);
		if (camera_vendor == NULL) {
			id made = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("BWFigCaptureDeviceVendor"), sel_registerName("alloc"));

			camera_vendor = ((id (*)(id, SEL, void *))objc_msgSend)(made, sel_registerName("initWithDeviceCreateFunction:"),
			    (void *)redwood_camera_device_create);
			say("redwood: mediaserverd: the camera's device vendor %s\n", camera_vendor ? "made (camera_device.c)" :
			    "CANNOT BE MADE");
		}
		pthread_mutex_unlock(&lock);
		return camera_vendor;
	}
	for (size_t i = 0; torch >= 0 && i < sizeof(flashlight_code) / sizeof(flashlight_code[0]); i++) {
		if (caller >= flashlight_code[i].from && caller < flashlight_code[i].to) {
			if (ours == NULL) {
				ours = class_createInstance(vendor_class, 0);
				say("redwood: mediaserverd: no camera, so no device vendor: FigFlashlight gets the flashlight's own\n");
			}
			return ours;
		}
	}
	return NULL;
}

static void
flashlight_set(uint32_t level)
{
	ssize_t written = write(torch, &level, sizeof(level));

	if (written != (ssize_t)sizeof(level) || torch_said < 40) {
		torch_said++;
		say("redwood: mediaserverd: the flashlight at %u of 1000%s\n", level, written == (ssize_t)sizeof(level) ? "" :
		    " (NOT TAKEN by /dev/redwood-torch)");
	}
}

/* the vendor's: one client, one device, always available to it */
static int
flashlight_register(__unused id self, __unused SEL selector, __unused int pid, __unused id description, __unused int priority,
    __unused BOOL steal, __unused BOOL share, __unused id handler)
{
	return 1;
}

static id
flashlight_copy_device(__unused id self, __unused SEL selector, __unused int client, __unused BOOL inform, int *error)
{
	if (error != NULL) {
		*error = 0;
	}
	return class_createInstance(device_class, 0);
}

static id
flashlight_copy_stream(__unused id self, __unused SEL selector, __unused int position, __unused int type, id device)
{
	return device != NULL && object_getClass(device) == device_class ? class_createInstance(stream_class, 0) : NULL;
}

static void
flashlight_take_back(__unused id self, __unused SEL selector, __unused id device, __unused int client, __unused BOOL inform)
{
	flashlight_set(0);
}

static BOOL
flashlight_active_equals(__unused id self, __unused SEL selector, id device)
{
	return device == NULL || object_getClass(device) == device_class;
}

/* the stream's -supportedProperties: none that FigFlashlight looks for ("HighCurrentTorchEnabled") */
static id
flashlight_stream_properties(__unused id self, __unused SEL selector)
{
	static CFDictionaryRef none;

	if (none == NULL) {
		none = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	return (id)none;
}

/* the stream's -setProperty:value: -- "TorchLevel", 0 to 1, as thousandths to the kernel */
static int
flashlight_stream_set(__unused id self, __unused SEL selector, id key, id value)
{
	float level = 0;

	if (key != NULL && CFEqual((CFTypeRef)key, CFSTR("TorchLevel"))) {
		if (value != NULL && CFGetTypeID((CFTypeRef)value) == CFNumberGetTypeID()) {
			CFNumberGetValue((CFNumberRef)value, kCFNumberFloatType, &level);
		}
		level = level < 0 ? 0 : level > 1 ? 1 : level;
		flashlight_set((uint32_t)(level * 1000 + 0.5f));
	}
	return 0;
}

struct method_spec {
	const char *selector, *types;
	IMP imp;
};

static Class
make_class(const char *name, const struct method_spec *methods, size_t count)
{
	Class cls = objc_allocateClassPair(objc_getClass("NSObject"), name, 0);

	for (size_t i = 0; i < count; i++) {
		class_addMethod(cls, sel_registerName(methods[i].selector), methods[i].imp, methods[i].types);
	}
	objc_registerClassPair(cls);
	return cls;
}

static void
flashlight_classes(void)
{
	static const struct method_spec vendor[] = {
		{ "registerClientWithPID:clientDescription:clientPriority:canStealFromClientsWithSamePriority:"
		  "deviceSharingWithOtherClientsAllowed:deviceAvailabilityChangedHandler:", "i@:i@iBB@", (IMP)flashlight_register },
		{ "copyDeviceForClient:informClientWhenDeviceAvailableAgain:error:", "@@:iB^i", (IMP)flashlight_copy_device },
		{ "copyStreamForFlashlightWithPosition:deviceType:forDevice:", "@@:ii@", (IMP)flashlight_copy_stream },
		{ "takeBackDevice:forClient:informClientWhenDeviceAvailableAgain:", "v@:@iB", (IMP)flashlight_take_back },
		{ "activeDeviceEquals:", "B@:@", (IMP)flashlight_active_equals },
	}, stream[] = {
		{ "supportedProperties", "@@:", (IMP)flashlight_stream_properties },
		{ "setProperty:value:", "i@:@@", (IMP)flashlight_stream_set },
	};

	vendor_class = make_class("RedwoodFlashlightVendor", vendor, sizeof(vendor) / sizeof(vendor[0]));
	device_class = make_class("RedwoodFlashlightDevice", NULL, 0);
	stream_class = make_class("RedwoodFlashlightStream", stream, sizeof(stream) / sizeof(stream[0]));
}

/*
 * The class methods replaced, each as soon as its class exists. `expected` is the unslid address
 * of the code in iOS 16.5 (20F66): a method that is not what it was is left alone. `original`, where
 * there is one, keeps what was there (the vendor's: given as it is wherever there is one, and where
 * there is none, ours only with the kernel's camera or flashlight).
 */
static struct replacement {
	const char *class_name, *selector;
	uintptr_t expected;
	IMP replacement, *original;
	bool done;
} replacements[] = {
	{ "BWCoreImageFilterRenderer", "prewarmPortraitFilterVersion:semanticStyleFilters:", 0x1a2115c0cUL,
	  (IMP)prewarm_portrait_filter, NULL, false },
	{ "BWFigCaptureDeviceVendor", "sharedCaptureDeviceVendor", 0x1a1ef0d04UL,
	  (IMP)shared_vendor, &original_shared_vendor, false },
};

static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method method;
		IMP original;

		if (r->done || (cls = objc_getClass(r->class_name)) == NULL) {
			continue;
		}
		r->done = true;
		method = class_getClassMethod(cls, sel_registerName(r->selector));
		original = method ? method_getImplementation(method) : NULL;
		if (original == NULL || (uintptr_t)original != r->expected + slide) {
			say("redwood: +[%s %s]: found %#lx, expected %#lx; left alone\n", r->class_name, r->selector,
			    original ? (uintptr_t)original - slide : 0, r->expected);
			continue;
		}
		if (r->original != NULL) {
			*r->original = original;
		}
		method_setImplementation(method, r->replacement);
		say("redwood: mediaserverd: +[%s %s] replaced\n", r->class_name, r->selector);
	}
}

/*
 * 40 s (boot-arg media_threads=SECONDS; 0: never) after the daemon starts, every thread of it in one kept
 * line each -- its first frames that are not the kernel's, dispatch's or pthread's, by symbol. On the
 * Infinix mediaserverd answers nobody: SpringBoard waits for ever for its system controller and a
 * poster for a decompression session (runs 253, 259), and this says what the daemon is doing instead.
 */
static void __attribute__((unused))
frame_name(uintptr_t address, char *out, size_t size)
{
	Dl_info info;
	const char *image;

	if (dladdr((void *)address, &info) && info.dli_fname != NULL) {
		image = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
		snprintf(out, size, "%s+%#lx(%.14s)", info.dli_sname ? info.dli_sname : "?",
		    info.dli_saddr ? (unsigned long)(address - (uintptr_t)info.dli_saddr) : 0UL, image);
	} else {
		snprintf(out, size, "%#lx", (unsigned long)address);
	}
}

static int __attribute__((unused))
frame_boring(uintptr_t address)
{
	Dl_info info;

	return dladdr((void *)address, &info) && info.dli_fname != NULL && (strstr(info.dli_fname, "libsystem_kernel") ||
	    strstr(info.dli_fname, "libdispatch") || strstr(info.dli_fname, "libsystem_pthread"));
}

/* another thread's return addresses (pc first), walked by its frame pointers while it is stopped; how many */
static unsigned
thread_frames(thread_t thread, uintptr_t *frames, unsigned max)
{
	arm_thread_state64_t state;
	mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
	unsigned depth = 0;

	if (thread_suspend(thread) != KERN_SUCCESS) {
		return 0;
	}
	/* the addresses only while it is stopped (it may hold a lock that dladdr wants), the names after */
	if (thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&state, &n) == KERN_SUCCESS) {
		uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(state);

		frames[depth++] = (uintptr_t)arm_thread_state64_get_pc(state);
		while (depth < max && fp != 0 && (fp & 7) == 0) {
			uintptr_t next = ((uintptr_t *)fp)[0];

			frames[depth++] = ((uintptr_t *)fp)[1] & 0x0000000fffffffffULL;
			fp = next > fp ? next : 0;
		}
	}
	thread_resume(thread);
	return depth;
}

static void *
media_threads(void *argument)
{
	thread_act_array_t threads;
	mach_msg_type_number_t count;

	sleep((unsigned)(uintptr_t)argument);
	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
		return NULL;
	}
	for (unsigned i = 0; i < count && i < 40; i++) {
		uintptr_t frames[24];
		unsigned depth = threads[i] == mach_thread_self() ? 0 : thread_frames(threads[i], frames, 24);

		if (depth != 0) {
			/* no names: a daemon stuck holding dyld's lock would stop dladdr here too. The addresses are
			 * unslid, for the shared cache to name them on the Mac (tools/cache-name.py) */
			char line[200];
			size_t used = (size_t)snprintf(line, sizeof(line), "!media t%u", i);

			for (unsigned k = 0; k < depth && k < 11 && used < sizeof(line); k++) {
				used += (size_t)snprintf(line + used, sizeof(line) - used, " %lx", (unsigned long)(frames[k] - slide));
			}
			sysctlbyname("kern.redwood.say", NULL, NULL, line, strnlen(line, sizeof(line)));
		}
	}
	return NULL;
}

/*
 * Inside Camera.app (inserted by the diagnostic image only: the line "libredwood-media.dylib mediaserverd Camera" of etc/inject.conf): what the app does with a
 * shutter tap and what it is told. mediaserverd's capture graph compresses the photo (run 581) and nothing reaches DCIM (runs 582, 583); the part between is the
 * app's: -[AVCapturePhotoOutput capturePhotoWithSettings:delegate:] and the delegate's callbacks, with their errors, said to kern.redwood.host_keep.
 */
/*
 * host_line: kept for the host AND said in the console (with its time). The host's buffer is 64 KB for the whole run and other lines fill it (shutter3:
 * "65264 bytes (100 dropped)", 45 KB of them pvr_stats=1's, so the photo's last lines were the ones dropped); quiet_line: the console only, for what
 * is not needed to follow a tap.
 */
static void
line_out(bool keep, const char *format, va_list ap)
{
	char line[400];

	vsnprintf(line, sizeof(line), format, ap);
	if (keep) {
		(void)sysctlbyname("kern.redwood.host_keep", NULL, NULL, line, strnlen(line, sizeof(line)));
	}
	(void)sysctlbyname("kern.redwood.say", NULL, NULL, line, strnlen(line, sizeof(line)));
}

static void
host_line(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	line_out(true, format, ap);
	va_end(ap);
}

static void
quiet_line(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	line_out(false, format, ap);
	va_end(ap);
}

/* a long text as lines of its own (the host buffer takes 400 bytes a line) */
static void
host_long(const char *prefix, const char *text)
{
	size_t length = strlen(text), at = 0;

	for (unsigned part = 0; at < length && part < 8; part++, at += 300) {
		host_line("%s [%u] %.300s", prefix, part, text + at);
	}
}

static const char *
text_of_object(id object)
{
	id text = object ? ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("description")) : NULL;

	return text ? ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String")) : "-";
}

/* the delegate's callbacks that carry (output, object, error) or (output, object): each gets a line, then the original */
static const char *const cb3[] = {
	"captureOutput:didFinishProcessingPhoto:error:", "captureOutput:didFinishCaptureForResolvedSettings:error:",
	"captureOutput:didFinishRecordingLivePhotoMovieForEventualFileAtURL:resolvedSettings:", "captureOutput:didFinishProcessingLivePhotoToMovieFileAtURL:duration:photoDisplayTime:resolvedSettings:error:",
};
static const char *const cb2[] = {
	"captureOutput:willBeginCaptureForResolvedSettings:", "captureOutput:willCapturePhotoForResolvedSettings:", "captureOutput:didCapturePhotoForResolvedSettings:",
};

static void
wrap_delegate(Class cls)
{
	static Class done[16];
	static unsigned ndone;

	for (unsigned i = 0; i < ndone; i++) {
		if (done[i] == cls) {
			return;
		}
	}
	if (ndone < 16) {
		done[ndone++] = cls;
	}
	for (unsigned k = 0; k < sizeof(cb3) / sizeof(cb3[0]); k++) {
		SEL sel = sel_registerName(cb3[k]);
		Method m = class_getInstanceMethod(cls, sel);

		if (m == NULL) {
			continue;
		}
		const char *name = cb3[k];
		IMP orig = method_getImplementation(m);
		unsigned nargs = method_getNumberOfArguments(m);

		if (nargs == 5) {
			method_setImplementation(m, imp_implementationWithBlock((id)^(id me, id output, id object, id error) {
				host_long(error ? "camera app: error" : "camera app: no error", error ? text_of_object(error) : name);
				((void (*)(id, SEL, id, id, id))orig)(me, sel, output, object, error);
			}));
		} else if (nargs == 4) {
			method_setImplementation(m, imp_implementationWithBlock((id)^(id me, id output, id object) {
				quiet_line("camera app: %s %s", class_getName(cls), name);
				((void (*)(id, SEL, id, id))orig)(me, sel, output, object);
			}));
		}
	}
	for (unsigned k = 0; k < sizeof(cb2) / sizeof(cb2[0]); k++) {
		SEL sel = sel_registerName(cb2[k]);
		Method m = class_getInstanceMethod(cls, sel);

		if (m == NULL || method_getNumberOfArguments(m) != 4) {
			continue;
		}
		const char *name = cb2[k];
		IMP orig = method_getImplementation(m);

		method_setImplementation(m, imp_implementationWithBlock((id)^(id me, id output, id object) {
			quiet_line("camera app: %s %s", class_getName(cls), name);
			((void (*)(id, SEL, id, id))orig)(me, sel, output, object);
		}));
	}
	host_line("camera app: delegate class %s wrapped", class_getName(cls));
}

/*
 * Run 585: addMainImage:metadata:options: answers -12908 (kVTCouldNotFindVideoEncoderErr) for the HEIC photos Camera asks for (hvc1): this phone has no hardware HEVC
 * encoder and no VideoToolbox plug-in for one. Run 586: emptying the output's codec list for HEIC only stopped the app's capture. The app's own switches are the ones to
 * turn: CameraUI's "HEVC encoding supported" and "prefer HEVC" answers become NO, so that it asks for JPEG.
 */
static BOOL
answer_no(__unused id me, __unused SEL sel)
{
	return NO;
}

/*
 * By name: the four the old scan found (every run says the same four). The scan (objc_copyClassList, which realises every class of the process under
 * the runtime's lock, then class_getImageName on each) held the main queue from the 12 s timer to the last line: combo3 183 -> 193.0, space1 245 -> 257.
 */
static void
no_hevc_in_camera_ui(void)
{
	static const char *const switches[][2] = {
		{ "CAMTimelapseStabilizationMovieWriter", "_preferHEVC" }, { "CAMTimelapseState", "preferHEVC" },
		{ "CAMUserPreferences", "_preferHEVCWhenAvailable" }, { "CAMCaptureCapabilities", "isHEVCEncodingSupported" },
	};
	unsigned changed = 0;

	for (unsigned i = 0; i < sizeof(switches) / sizeof(switches[0]); i++) {
		Class cls = objc_getClass(switches[i][0]);
		Method m = cls ? class_getInstanceMethod(cls, sel_registerName(switches[i][1])) : NULL;
		const char *type = m ? method_getTypeEncoding(m) : NULL;

		if (type != NULL && type[0] == 'B') {
			method_setImplementation(m, (IMP)answer_no);
			quiet_line("camera app: %s %s -> NO", switches[i][0], switches[i][1]);
			changed++;
		}
	}
	host_line("camera app: %u HEVC switches of CameraUI turned off", changed);
}

static IMP orig_capture_photo;

/*
 * PhotoKit's check before it takes a file (-[PHAssetResourceBag _validateAssetResourceForAssetCreation:...], PHPhotosErrorDomain 3305 when it
 * fails): +[PLFileUtilities hasDiskSpaceToCopyFileAtURL:] wants the file's size under MobileGestalt's DiskUsage AmountDataAvailable, which is
 * the data volume's free space less a fixed 200 MiB (AmountDataReserved, a constant of libMobileGestalt). /private/var here is the kernel's
 * ramfs, a quarter of memory (248 MB, ~77 MB free), so that is always 0 and every photo was refused. Here: the file and a 32nd of the volume
 * (its low-disk threshold, ramfs_ioctl) must fit in what statfs says is free. Asked both in Camera and in assetsd, which validates the
 * request again (so both get this library: INJECT "... mediaserverd Camera assetsd").
 * ponytail: the 200 MiB reserve ignored for photos only: the RAM volume can still fill up; a data volume on the flash would make the reserve right.
 */
static BOOL
has_space_to_copy(__unused id me, __unused SEL sel, id url)
{
	id path = url ? ((id (*)(id, SEL))objc_msgSend)(url, sel_registerName("path")) : NULL;
	const char *file = path ? ((const char *(*)(id, SEL))objc_msgSend)(path, sel_registerName("fileSystemRepresentation")) : NULL;
	struct stat st;
	struct statfs sf;
	BOOL yes = file != NULL && stat(file, &st) == 0 && statfs("/private/var/mobile/Media", &sf) == 0 &&
	    (unsigned long long)st.st_size + sf.f_blocks * sf.f_bsize / 32 <= sf.f_bavail * sf.f_bsize;

	host_line("%s: hasDiskSpaceToCopyFileAtURL: %s -> %s", getprogname(), file ? file : "(no path)", yes ? "YES" : "NO");
	return yes;
}

static void
space_hook(void)
{
	Class utilities = objc_getClass("PLFileUtilities");
	Method m = utilities ? class_getClassMethod(utilities, sel_registerName("hasDiskSpaceToCopyFileAtURL:")) : NULL;

	if (m != NULL) {
		method_setImplementation(m, (IMP)has_space_to_copy);
	}
	host_line("%s: +[PLFileUtilities hasDiskSpaceToCopyFileAtURL:] %s", getprogname(), m ? "asks statfs" : "NOT FOUND");
}

/*
 * The photo itself, past the capture graph (camera_device.c still_save): each tap of the shutter is counted in the notify state of REDWOOD_STILL_ASK
 * (1, 2, 3..., kept by notifyd across launches of the app) and posted; mediaserverd's camera writes a JPEG of its frame for every count, to
 * REDWOOD_STILL_FILE with the count in its name, and says the count it has served in REDWOOD_STILL_DONE's state. Every count from the last one handled
 * to that one is then given to the photo library on a queue of ours, each on its own (still_to_library): -[PHPhotoLibrary
 * performChanges:completionHandler:] with a PHAssetCreationRequest that takes the file (resource type 1, a photo). The library is assetsd's: the job
 * com.apple.assetsd must be loaded.
 */
#define REDWOOD_STILL_ASK       "redwood.camera.still"
#define REDWOOD_STILL_DONE      "redwood.camera.still.done"
#define REDWOOD_STILL_FILE      "/private/var/tmp/redwood-still-%llu.jpg"     /* (camera_device.c: the tap's number) */
#define REDWOOD_STILL_ORIGINAL  "/private/var/redwood-still-original"   /* (a file of this name: the shutter also asks the capture graph, as before) */
#define REDWOOD_STILL_DCIM      "/private/var/mobile/Media/DCIM/100REDWD"
#define REDWOOD_STILL_TIMEOUT   20                                      /* seconds for the library to answer */

static int still_ask_token = -1, still_done_token = -1;
static unsigned long long still_handled;        /* the last count given to the library (or found to have no still) */
static dispatch_queue_t still_queue;

/* the shutter's tap counted: its number */
static unsigned long long
still_ask(void)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	uint64_t n = 0;
	uint32_t got, set;

	pthread_mutex_lock(&lock);
	got = notify_get_state(still_ask_token, &n);
	set = notify_set_state(still_ask_token, ++n);
	notify_post(REDWOOD_STILL_ASK);
	pthread_mutex_unlock(&lock);
	host_line("camera app: tap %llu received%s (notify status %u %u)", (unsigned long long)n, got || set ? ": THE COUNT IS NOT KEPT, mediaserverd will not see it" : "",
	    got, set);
	return n;
}

/*
 * A copy where the camera's pictures are, kept when the library has not taken the photo (refused, or no answer in REDWOOD_STILL_TIMEOUT s): assetsd
 * imports DCIM when it builds its library again. Nothing goes there before the library has failed: a DCIM with files in it and no library yet is what
 * assetsd takes for a library to rebuild. The name is one not taken: one past the highest IMG_nnnn.JPG there (they last as long as /private/var does),
 * made with O_EXCL. True if the copy is whole.
 */
static bool
still_to_dcim(const char *from, char *path, size_t size)
{
	unsigned next = 1, n;
	DIR *dir;
	struct dirent *e;
	char buffer[65536];
	ssize_t got = 0;
	int in = open(from, O_RDONLY), out = -1;
	bool ok;

	(void)mkdir("/private/var/mobile/Media/DCIM", 0755);
	(void)mkdir(REDWOOD_STILL_DCIM, 0755);
	if ((dir = opendir(REDWOOD_STILL_DCIM)) != NULL) {
		while ((e = readdir(dir)) != NULL) {
			if (sscanf(e->d_name, "IMG_%4u.JPG", &n) == 1 && n >= next) {
				next = n + 1;
			}
		}
		closedir(dir);
	}
	for (unsigned tries = 0; in >= 0 && out < 0 && tries < 100 && next <= 9999; tries++, next++) {
		snprintf(path, size, REDWOOD_STILL_DCIM "/IMG_%04u.JPG", next);
		if ((out = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644)) < 0 && errno != EEXIST) {
			break;
		}
	}
	ok = in >= 0 && out >= 0;
	while (ok && (got = read(in, buffer, sizeof(buffer))) > 0) {
		ok = write(out, buffer, (size_t)got) == got;
	}
	ok = ok && got == 0;
	if (out >= 0) {
		ok = fsync(out) == 0 && ok;
		ok = close(out) == 0 && ok;
		if (!ok) {
			unlink(path);
		}
	}
	if (in >= 0) {
		close(in);
	}
	if (out < 0) {
		snprintf(path, size, "(none: %s)", strerror(errno));
	}
	return ok;
}

/* appends to line: which of the library's database files exist and how big (shared by the early open and the diagnosis) */
static size_t
photo_files(char *line, size_t size, size_t used)
{
	static const char *const files[] = { "", "Photos.sqlite", "Photos.sqlite-wal", "Photos.sqlite-shm" };

	for (unsigned i = 0; i < 4 && used < size; i++) {
		struct stat st;
		char path[96];

		snprintf(path, sizeof(path), "/private/var/mobile/Media/PhotoData/%s", files[i]);
		if (stat(path, &st) != 0) {
			used += (size_t)snprintf(line + used, size - used, " %s MISSING;", i ? files[i] : "PhotoData");
		} else if (i != 0) {
			used += (size_t)snprintf(line + used, size - used, " %s %lld bytes;", files[i], (long long)st.st_size);
		}
	}
	return used;
}

/* why the library has not answered, as far as the app can see: its service registered with launchd, its database on the disk, what PhotoKit says */
extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_look_up(mach_port_t, const char *, mach_port_t *);

static void
still_diagnose(unsigned long long tap)
{
	char line[300];
	size_t used;
	mach_port_t port = MACH_PORT_NULL;
	kern_return_t kr = bootstrap_look_up(bootstrap_port, "com.apple.photos.service", &port);

	used = (size_t)snprintf(line, sizeof(line), "camera app: tap %llu: TIMEOUT: com.apple.photos.service (assetsd) %s;", tap, kr == KERN_SUCCESS ?
	    "is registered with launchd" : "IS NOT REGISTERED (com.apple.assetsd not loaded: MORE_JOBS)");
	if (port != MACH_PORT_NULL) {
		mach_port_deallocate(mach_task_self(), port);
	}
	used = photo_files(line, sizeof(line), used);
	host_line("%s", line);
	/* (asked on a queue of its own: it may wait for assetsd itself) */
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		Class library = objc_getClass("PHPhotoLibrary");
		id shared = library ? ((id (*)(id, SEL))objc_msgSend)((id)library, sel_registerName("sharedPhotoLibrary")) : NULL;
		SEL reason = sel_registerName("unavailabilityReason");

		if (shared != NULL && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(shared, sel_registerName("respondsToSelector:"), reason)) {
			id error = ((id (*)(id, SEL))objc_msgSend)(shared, reason);

			host_long(error ? "camera app: TIMEOUT: the library is UNAVAILABLE:" : "camera app: TIMEOUT: the library says it is available", error ?
			    text_of_object(error) : "(unavailabilityReason nil)");
		}
	});
}

/*
 * One tap's photo on its way to the library; every completion, retry and the timeout end on still_queue, the last of them frees it. combo3/space4:
 * a library still being made answers nothing for minutes, so the timeout only says so: the still waits in /private/var/tmp for the answer. Asked
 * again only after the library has ANSWERED no (a request still pending may yet succeed: asking again then would make two), at most
 * REDWOOD_STILL_TRIES times; DCIM only after the last no.
 * ponytail: a completion that never comes leaves the still in /private/var/tmp (logged by the TIMEOUT line); PhotoKit answers an XPC failure with an error.
 */
#define REDWOOD_STILL_TRIES     3
#define REDWOOD_STILL_RETRY     10                                      /* seconds before asking again */

struct still_add {
	unsigned long long tap;
	uint64_t        began;
	int             state;          /* 0 waiting, 1 timed out (still waiting), 2 answered for good */
	int             refs;
	unsigned        tries;
	id              url;            /* (CFRetain'd) */
	char            file[64], dcim[96];
};

static void
still_add_release(struct still_add *a)
{
	if (--a->refs == 0) {
		CFRelease((CFTypeRef)a->url);
		free(a);
	}
}

static void
still_submit(struct still_add *a)
{
	id shared = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("PHPhotoLibrary"), sel_registerName("sharedPhotoLibrary"));
	unsigned long long tap = a->tap;

	a->tries++;
	a->refs++;
	((void (*)(id, SEL, void (^)(void), void (^)(BOOL, id)))objc_msgSend)(shared, sel_registerName("performChanges:completionHandler:"), ^{
		id r = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("PHAssetCreationRequest"), sel_registerName("creationRequestForAsset"));

		((void (*)(id, SEL, long, id, id))objc_msgSend)(r, sel_registerName("addResourceWithType:fileURL:options:"), 1, a->url, NULL);
		quiet_line("camera app: tap %llu: in the change block: creation request %p, resource given", tap, (void *)r);
	}, ^(BOOL ok, id error) {
		const char *text = ok ? "" : strdup(text_of_object(error));

		dispatch_async(still_queue, ^{
			unsigned long long ms = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - a->began) / 1000000;
			bool late = a->state == 1;

			if (ok) {
				a->state = 2;
				host_line("camera app: tap %llu: THE PHOTO IS IN THE LIBRARY, %llu ms after performChanges%s (try %u)", tap, ms,
				    late ? " (after the timeout)" : "", a->tries);
				unlink(a->file);
			} else {
				char prefix[120];

				snprintf(prefix, sizeof(prefix), "camera app: tap %llu: the library REFUSED it after %llu ms:", tap, ms);
				host_long(prefix, text);
				{
					struct statfs sf;
					bool got = statfs("/private/var/mobile/Media", &sf) == 0;

					host_line("camera app: tap %llu: /private/var/mobile/Media free %llu of %llu bytes (statfs f_bavail*f_bsize%s)", tap,
					    got ? (unsigned long long)sf.f_bavail * sf.f_bsize : 0ULL, got ? (unsigned long long)sf.f_blocks * sf.f_bsize : 0ULL,
					    got ? "" : ": STATFS FAILED");
				}
				if (a->tries < REDWOOD_STILL_TRIES) {
					host_line("camera app: tap %llu: asked again in %d s (try %u of %d)", tap, REDWOOD_STILL_RETRY, a->tries + 1, REDWOOD_STILL_TRIES);
					dispatch_after(dispatch_time(DISPATCH_TIME_NOW, REDWOOD_STILL_RETRY * NSEC_PER_SEC), still_queue, ^{
						still_submit(a);
						still_add_release(a);   /* (this completion's) */
					});
					free((void *)text);
					return;
				}
				a->state = 2;
				{
					bool kept = still_to_dcim(a->file, a->dcim, sizeof(a->dcim));

					host_line("camera app: tap %llu: %s %s (after %u tries)", tap, kept ? "kept in DCIM:" : "NOT KEPT IN DCIM", a->dcim, a->tries);
				}
				if (a->dcim[0] == '/') {
					unlink(a->file);        /* (only where the copy is: otherwise the file stays, the only one of the photo) */
				}
			}
			free((void *)(ok ? NULL : text));
			still_add_release(a);
		});
	});
}

static void
still_to_library(unsigned long long tap)
{
	Class library = objc_getClass("PHPhotoLibrary"), request = objc_getClass("PHAssetCreationRequest"), url_class = objc_getClass("NSURL");
	SEL status = sel_registerName("authorizationStatusForAccessLevel:");
	id shared, path, url;
	long add = -1, all = -1;
	struct stat st;
	struct still_add *a = calloc(1, sizeof(*a));
	const char *refusal = NULL;

	if (a == NULL) {
		host_line("camera app: tap %llu: NO MEMORY", tap);
		return;
	}
	a->tap = tap;
	a->began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	a->refs = 1;            /* (the timeout's; still_submit takes one for each completion) */
	snprintf(a->file, sizeof(a->file), REDWOOD_STILL_FILE, tap);
	if (stat(a->file, &st) != 0) {
		host_line("camera app: tap %llu: NO STILL from mediaserverd (%s: %s): nothing to add", tap, a->file, strerror(errno));
		free(a);
		return;
	}
	host_line("camera app: tap %llu: the still is there, %lld bytes: %s", tap, (long long)st.st_size, a->file);
	/*
	 * Only asked whether, never asked for: an application without a usage text for the photo library (Camera.app has none: it is allowed by
	 * com.apple.private.tcc.allow kTCCServicePhotos) is ended by the privacy library the moment the library has to ASK the user, which is what
	 * -performChanges: does when the answer is not "authorised" yet. 3 is PHAuthorizationStatusAuthorized (4 limited, 0 not determined, 2 denied).
	 */
	if (library == NULL || request == NULL || url_class == NULL) {
		refusal = "NO PHOTO LIBRARY CLASSES";
	} else {
		if (class_getClassMethod(library, status) != NULL) {
			add = ((long (*)(id, SEL, long))objc_msgSend)((id)library, status, 1);  /* PHAccessLevelAddOnly */
			all = ((long (*)(id, SEL, long))objc_msgSend)((id)library, status, 2);  /* PHAccessLevelReadWrite */
		}
		if (add != 3 && all != 3) {
			refusal = "NOT AUTHORISED (asking would end the application)";
		}
	}
	shared = refusal ? NULL : ((id (*)(id, SEL))objc_msgSend)((id)library, sel_registerName("sharedPhotoLibrary"));
	path = ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), a->file);
	url = path ? ((id (*)(id, SEL, id))objc_msgSend)((id)url_class, sel_registerName("fileURLWithPath:"), path) : NULL;
	if (refusal == NULL && (shared == NULL || url == NULL)) {
		refusal = "NO SHARED LIBRARY OR URL";
	}
	if (refusal != NULL) {
		bool kept = still_to_dcim(a->file, a->dcim, sizeof(a->dcim));

		host_line("camera app: tap %llu: the library is not asked: %s (authorisation to add %ld, to read and write %ld); %s %s", tap, refusal, add, all,
		    kept ? "kept in DCIM:" : "NOT KEPT IN DCIM", a->dcim);
		if (kept) {
			unlink(a->file);
		}
		free(a);
		return;
	}
	/* logs/shutter2.log: a block made in C keeps no hold on the objects it names, and the change block runs later on another queue: the URL is held
	 * until the library has answered */
	a->url = (id)CFRetain((CFTypeRef)url);
	host_line("camera app: tap %llu: performChanges submitted (authorisation to add %ld, to read and write %ld)", tap, add, all);
	still_submit(a);
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, REDWOOD_STILL_TIMEOUT * NSEC_PER_SEC), still_queue, ^{
		if (a->state == 0) {
			a->state = 1;
			still_diagnose(tap);
			host_line("camera app: tap %llu: TIMEOUT: no answer from the library in %d s; still waiting for it (no DCIM copy: the still stays at %s)", tap,
			    REDWOOD_STILL_TIMEOUT, a->file);
		}
		still_add_release(a);
	});
}

/*
 * What takes Camera.app down (logs/shutter1.log: it died 40-50 ms after "adding it to the photo library", both times, with nothing said): told here before
 * it goes. An Objective-C exception at the moment it is thrown (name, reason, who threw: return addresses less the shared cache's slide, for tools/dsc),
 * and a fatal signal (an abort is how the privacy library ends an application that asks for something it has no usage text for). This file is C, so
 * nothing here can catch an exception: the save below is kept from the one call known to end the application instead.
 */
static void
camera_frames(const char *what)
{
	void *frames[24];
	char line[380];
	int n = backtrace(frames, 24), used = snprintf(line, sizeof(line), "camera app: %s: frames", what);

	for (int i = 1; i < n && used < (int)sizeof(line) - 14; i++) {
		used += snprintf(line + used, sizeof(line) - (size_t)used, " %lx", (unsigned long)((uintptr_t)frames[i] - slide));
	}
	host_line("%s", line);
}

static volatile int early_open;  /* 1 while library_early opens the library, 2 once an exception was thrown meanwhile */

static id
camera_exception_thrown(id exception)
{
	static unsigned said;

	if (early_open == 1) {
		early_open = 2;
	}

	if (said++ < 6) {
		host_long("camera app: EXCEPTION THROWN", text_of_object(exception));
		host_line("camera app: exception name %s", text_of_object(((id (*)(id, SEL))objc_msgSend)(exception, sel_registerName("name"))));
		camera_frames("exception");
	}
	return exception;
}

static void
camera_exception_uncaught(id exception)
{
	host_long("camera app: UNCAUGHT EXCEPTION, the application ends", text_of_object(exception));
}

static void
camera_signal(int sig, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;

	host_line("camera app: FATAL SIGNAL %d, address %p, pc %lx lr %lx (less the slide)", sig, info->si_addr,
	    (unsigned long)((uintptr_t)uc->uc_mcontext->__ss.__pc - slide), (unsigned long)((uintptr_t)uc->uc_mcontext->__ss.__lr - slide));
	camera_frames("signal");
	signal(sig, SIG_DFL);
}

static void
camera_app_guard(void)
{
	static const int signals[] = { SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGTRAP };
	struct sigaction sa;
	size_t length = 0;

	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = camera_signal;
	sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
	for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		sigaction(signals[i], &sa, NULL);
	}
	objc_setExceptionPreprocessor(camera_exception_thrown);
	objc_setUncaughtExceptionHandler(camera_exception_uncaught);
}

/*
 * combo3: the second tap never reached capturePhotoWithSettings, with nothing said about the main thread. A ping to the main queue every 250 ms
 * from a queue of ours; one unanswered for 2 s is said with the main thread's frames (unslid, tools/cache-name.py), again every 5 s while it
 * lasts, and once more when it answers.
 */
static _Atomic uint64_t main_ping;      /* when the unanswered ping was sent (0: none) */
static _Atomic uint64_t main_said;      /* the ping said to be unanswered (0: none) */

static void
main_watch(void)
{
	static dispatch_source_t timer;
	static thread_t main_thread;
	static uint64_t next_ms;

	main_thread = mach_thread_self();       /* (called on the main queue) */
	timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
	dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
	dispatch_source_set_event_handler(timer, ^{
		uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), sent = main_ping, ms = (now - sent) / 1000000;

		if (sent == 0) {
			main_ping = now;
			next_ms = 2000;
			dispatch_async(dispatch_get_main_queue(), ^{
				uint64_t s = main_ping;

				main_ping = 0;
				if (s != 0 && main_said == s) {
					main_said = 0;
					host_line("camera app: main thread answered again after %llu ms", (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - s) / 1000000);
				}
			});
		} else if (ms >= next_ms) {
			uintptr_t frames[24];
			unsigned depth = thread_frames(main_thread, frames, 24);
			char line[380];
			int used = snprintf(line, sizeof(line), "camera app: main thread blocked %llu ms: frames", ms);

			for (unsigned k = 0; k < depth && used < (int)sizeof(line) - 14; k++) {
				used += snprintf(line + used, sizeof(line) - (size_t)used, " %lx", (unsigned long)(frames[k] - slide));
			}
			main_said = sent;
			next_ms = ms + 5000;
			host_line("%s", line);
		}
	});
	dispatch_resume(timer);
}

/*
 * The library opened (made, the first time after a boot: /private/var is a RAM volume) as soon as the app is up, on a queue of its own, so
 * that it is there by the first tap; the line says how long assetsd took. Only where the app may read it (asking would end it, see still_to_library).
 */
static void
library_early(void)
{
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
		Class library = objc_getClass("PHPhotoLibrary"), asset = objc_getClass("PHAsset");
		SEL status = sel_registerName("authorizationStatusForAccessLevel:");
		long all = library && class_getClassMethod(library, status) ? ((long (*)(id, SEL, long))objc_msgSend)((id)library, status, 2) : -1;
		id result;

		if (all != 3 || asset == NULL) {
			host_line("camera app: the photo library is not opened early (authorisation to read and write %ld)", all);
			return;
		}
		char line[200];
		size_t used = (size_t)snprintf(line, sizeof(line), "camera app: the early open begins;");

		(void)photo_files(line, sizeof(line), used);
		host_line("%s", line);
		early_open = 1;
		(void)((id (*)(id, SEL))objc_msgSend)((id)library, sel_registerName("sharedPhotoLibrary"));
		if (early_open == 2) {
			host_line("camera app: the early open stopped: an exception was thrown (assetsd may still be making the database); the tap's own retry opens it");
			return;
		}
		result = ((id (*)(id, SEL, id))objc_msgSend)((id)asset, sel_registerName("fetchAssetsWithOptions:"), NULL);
		if (early_open == 2) {
			host_line("camera app: the early open stopped: an exception was thrown; the tap's own retry opens it");
			return;
		}
		early_open = 0;
		host_line("camera app: the photo library opened early in %llu ms (%lu photos)", (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began) / 1000000,
		    result ? ((unsigned long (*)(id, SEL))objc_msgSend)(result, sel_registerName("count")) : 0UL);
	});
}

static void
capture_photo(id me, SEL sel, id settings, id delegate)
{
	if (settings != NULL) {
		SEL speed = sel_registerName("setPhotoQualityPrioritization:"), fusion = sel_registerName("setAutoVirtualDeviceFusionEnabled:"), highres = sel_registerName("setHighResolutionPhotoEnabled:");

		if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(settings, sel_registerName("respondsToSelector:"), speed)) {
			((void (*)(id, SEL, long))objc_msgSend)(settings, speed, 1);              /* AVCapturePhotoQualityPrioritizationSpeed */
		}
		if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(settings, sel_registerName("respondsToSelector:"), fusion)) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(settings, fusion, NO);
		}
		if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(settings, sel_registerName("respondsToSelector:"), highres)) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(settings, highres, NO);
		}
	}
	quiet_line("camera app: capturePhotoWithSettings: %s delegate %s", text_of_object(settings), delegate ? class_getName(object_getClass(delegate)) : "nil");
	(void)still_ask();
	if (delegate) {
		wrap_delegate(object_getClass(delegate));
	}
	/*
	 * The capture graph's own still is not asked for any more (unless the file REDWOOD_STILL_ORIGINAL is there): it makes no photo here (addMainImage
	 * answers -12908) and leaves a sink node of the graph busy, and when the graph is next stopped -[BWGraph stop:] waits 10 s for it and then ends
	 * mediaserverd on purpose ("Not all sink nodes became idle after waiting 10s.": logs/shutter2.log, the SIGTRAP at CMCapture 1a1f1fe60), after which
	 * the application waits for ever for its capture. The application is told the capture failed the way AVFoundation tells it of a request it cannot
	 * make, by its own -_dispatchFailureCallbacksForPhotoSettings:toDelegate:withError:, so that the shutter is the application's again; the picture is
	 * ours (still_to_library).
	 */
	{
		SEL fail = sel_registerName("_dispatchFailureCallbacksForPhotoSettings:toDelegate:withError:");

		if (access(REDWOOD_STILL_ORIGINAL, F_OK) != 0 && settings != NULL && delegate != NULL && class_getInstanceMethod(object_getClass(me), fail) != NULL) {
			id domain = ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), "AVFoundationErrorDomain");
			id error = ((id (*)(id, SEL, id, long, id))objc_msgSend)((id)objc_getClass("NSError"), sel_registerName("errorWithDomain:code:userInfo:"), domain,
			    -11800L, NULL);         /* AVErrorUnknown */

			host_line("camera app: the graph's own still is not asked for; the application is told so");
			((void (*)(id, SEL, id, id, id))objc_msgSend)(me, fail, settings, delegate, error);
			host_line("camera app: the application was told");
			return;
		}
	}
	host_line("camera app: the graph's own still is asked for");
	((void (*)(id, SEL, id, id))orig_capture_photo)(me, sel, settings, delegate);
}

/*
 * combo1.log: the hook used to be put in 12 s after launch, and a tap in those 12 s (16375.3, 16484.7: "hyn: finger 0 down" before "watched")
 * went to the graph's own still (the "still: addMainImage image: nil ... DryRun ... -> 4294954388" lines), which never finishes: CameraUI waits
 * for it with the shutter off for good, and mediaserverd is ended by -[BWGraph stop:] 10 s after the stream stops. So it goes in on the main
 * queue's first turn (before any touch is handled), and again at 12 s should AVFoundation not have been there yet.
 */
static void
capture_hook(const char *when)
{
	if (orig_capture_photo != NULL) {
		return;
	}
	{
		Class output = objc_getClass("AVCapturePhotoOutput");
		Method m = output ? class_getInstanceMethod(output, sel_registerName("capturePhotoWithSettings:delegate:")) : NULL;

		if (m == NULL) {
			host_line("camera app: no -[AVCapturePhotoOutput capturePhotoWithSettings:delegate:] (%s)", when);
			return;
		}
		orig_capture_photo = method_setImplementation(m, (IMP)capture_photo);
		host_line("camera app: capturePhotoWithSettings:delegate: watched (%s)", when);
		{
			static int token;
			uint64_t done = 0;

			(void)dlopen("/System/Library/Frameworks/Photos.framework/Photos", RTLD_NOW);
			space_hook();
			main_watch();
			library_early();
			still_queue = dispatch_queue_create("redwood.camera.still", dispatch_queue_attr_make_with_autorelease_frequency(DISPATCH_QUEUE_SERIAL,
			    DISPATCH_AUTORELEASE_FREQUENCY_WORK_ITEM));
			(void)notify_register_check(REDWOOD_STILL_ASK, &still_ask_token);
			(void)notify_register_check(REDWOOD_STILL_DONE, &still_done_token);
			(void)notify_get_state(still_done_token, &done);
			still_handled = done;           /* (stills served before this launch of the app are not its own) */
			/* every count served since the last one handled, each on its own (notifications coalesce; counts do not) */
			(void)notify_register_dispatch(REDWOOD_STILL_DONE, &token, still_queue, ^(__unused int t) {
				uint64_t served = 0;

				(void)notify_get_state(still_done_token, &served);
				if (served < still_handled) {
					still_handled = served;         /* (notifyd started again) */
				}
				while (still_handled < served) {
					still_to_library(++still_handled);
				}
			});
		}
	}
}

static void
camera_app_probe(void)
{
	dispatch_async(dispatch_get_main_queue(), ^{
		capture_hook("at launch");
	});
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 12 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		capture_hook("12 s after launch");
		no_hevc_in_camera_ui();
	});
}

__attribute__((constructor))
static void
redwood_media_init(void)
{
	const char *name = getprogname();
	size_t length = 0;

	if (name != NULL && strcmp(name, "Camera") == 0) {
		CFPreferencesSetAppValue(CFSTR("CAMUserPreferenceEnableResponsiveShutter"), kCFBooleanFalse, CFSTR("com.apple.camera"));
		camera_app_guard();
		camera_app_probe();
		return;
	}
	if (name != NULL && strcmp(name, "assetsd") == 0) {
		space_hook();
		return;
	}
	if (name == NULL || strcmp(name, "mediaserverd") != 0) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	torch = open("/dev/redwood-torch", O_WRONLY);
	camera = access("/dev/redwood-camera", F_OK) == 0;
	if (torch >= 0) {
		flashlight_classes();
		say("redwood: mediaserverd: the kernel has a flashlight (/dev/redwood-torch)\n");
	}
	if (camera) {
		say("redwood: mediaserverd: the kernel has a camera (/dev/redwood-camera)\n");
	}
	_dyld_register_func_for_add_image(image_added);         /* also called for every image already there */
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at;
		pthread_t thread;

		int seconds = 40;       /* (its sandbox may not let it read the boot-args: then the default) */

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (at = strstr(args, "media_threads=")) != NULL) {
			seconds = atoi(at + 14);
		}
		if (seconds > 0) {
			pthread_create(&thread, NULL, media_threads, (void *)(uintptr_t)seconds);
			pthread_detach(thread);
		}
	}
}
