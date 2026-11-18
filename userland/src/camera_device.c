/*
 * The wide camera as iOS's capture stack wants a camera: a FigCaptureDevice with one FigCaptureStream
 * (the back camera), made for mediaserverd's BWFigCaptureDeviceVendor in place of the one Apple's
 * H10ISP.mediacapture makes of the iPhone's ISP (part of libredwood-media.dylib; inject_media.c
 * gives the vendor our device's create function when the kernel has a camera: cam=5).
 *
 * Both are CoreMedia "derived objects" (CMDerivedObjectCreate): a table of functions for the base
 * class (properties) and one for the device or stream class, as H10ISP's are laid out in iOS 16.5:
 *   device  RequestStreams, RelinquishStreams (and a processing session and Pearl calibration, not here)
 *   stream  Start, Stop, AddAttachmentsToSampleBuffer
 * The stream's frames go to the capture graph through the blocks it is given in its property
 * "VideoOutputHandlers" ({ output: { PixelBufferHandler: ^(CVPixelBufferRef, CMTime) } }), for the
 * outputs "VideoOutputConfigurations" describes (size, pixel format) and "VideoOutputsEnabled"
 * switches on. A frame: read from the kernel's /dev/redwood-camera (RAW10), made into a picture by
 * camera_isp.c (exposure, white balance, gamma, scaling), one pixel buffer for each output.
 *
 * The formats the stream offers are named as the iPhone X's back camera's are in CMCapture's
 * D22/AVCaptureSession.plist (FigCaptureStreamFormats "Photo" and "Video-60", by the sensor sizes
 * and frame rates the plist matches them by), so that its AVCaptureDevice formats and session presets
 * resolve: the pictures are scaled from the sensor's 4000x3000 to whatever size is asked for.
 *
 * Everything asked of the device and the stream that is not answered here is said in mediaserverd's
 * log ("redwood: camera: ..."), once per property.
 *
 * STATE (7 November, the Infinix X6826B, docs/camera-x6826b.md): Camera.app's viewfinder shows this camera's frames. What the capture graph needed beyond the
 * obvious: tccd running (the camera permission), a processing session (device slot 1: device_processing_session_create), the stream's ModuleInfo (the sensor ID
 * and module IDs that find the sensor's tuning in D22/CameraSetup.plist), VideoOutputsColorInfo, no picture before the graph has enabled the outputs / called
 * Start, and the pictures made in each output's own BufferPool (VideoOutputConfigurations). Not done: autofocus, stills (the shutter), recording, the other cameras.
 * Debugging aids, off by default: CAM_TRACE=1 (the Objective-C message trace of CMCapture, BWGraph hooks), CAM_MARK=2 (every picture grey).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <notify.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <mach/mach.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "camera_isp.h"

void redwood_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));

void
redwood_camera_say(const char *format, ...)
{
	char line[400];
	va_list ap;
	int n = snprintf(line, sizeof(line), "redwood: camera: ");

	va_start(ap, format);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = sizeof(line) - 2;
	}
	line[n++] = '\n';
	write(2, line, (size_t)n);
	{
		/* the kernel keeps the line (kern.redwood.say, "!" first) and says it again every minute: mediaserverd's stderr is not in any log */
		char kept[200];

		snprintf(kept, sizeof(kept), "!%.*s", n - 1 > 190 ? 190 : n - 1, line);
		sysctlbyname("kern.redwood.say", NULL, NULL, kept, strlen(kept));
	}
}
#define say redwood_camera_say
#if __has_include("camera_trace_local.h")
#include "camera_trace_local.h"          /* (not in git: a diagnostic build's CAM_TRACE=1 and CAM_TRACE_STUB=0x...) */
#endif
#ifndef CAM_MARK
#define CAM_MARK 0         /* 1: a bright square in the picture's corner, 2: the whole picture grey (a diagnosis: the preview turned grey in run 560 = our pictures are shown) */
#endif
#ifndef CAM_TRACE
#define CAM_TRACE 0        /* 1: the Objective-C message trace of CMCapture and the BWGraph hooks (they cost every message) */
#endif
/* this file is made small (the boot partition's image is full to the byte): size over speed here; the pixel work is camera_isp.c's */
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

/* every property the capture graph asks of, or sets on, our device and stream (first sight of each, with how many times after) */
static void
trace_key(const char *who, CFStringRef key)
{
	static struct { char tag[96]; unsigned said; } seen[96];
	char name[80] = "?", tag[96];

	CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
	snprintf(tag, sizeof(tag), "%s: %s", who, name);
	for (int i = 0; i < 96; i++) {
		if (seen[i].tag[0] == 0) {
			strlcpy(seen[i].tag, tag, sizeof(seen[i].tag));
		}
		if (strcmp(seen[i].tag, tag) == 0) {
			if (seen[i].said++ >= 8) {
				return;         /* (the graph's retry loop asks the same things 30 times a second) */
			}
			break;
		}
	}
	say("%s", tag);
}

uintptr_t redwood_cache_slide(void);

/* who calls us: the return addresses, unslid, for tools/dsc to name (the capture graph's calls on our device and stream) */
static void
say_bt(const char *what)
{
	static unsigned said;
	void *frames[40];
	char line[200];
	int k, used;
	uintptr_t slide = redwood_cache_slide();

	/* only in a CAM_TRACE build: ~40 KB a camera start, a third of what pstore keeps of a run's end (tools/r1-auto.sh) */
	if (!CAM_TRACE || said++ >= 120) {
		return;
	}
	k = backtrace(frames, 40);
	used = snprintf(line, sizeof(line), "BT %s:", what);
	for (int i = 1; i < k; i++) {
		if (used >= (int)sizeof(line) - 14) {
			say("%s", line);
			used = snprintf(line, sizeof(line), "BT+:");
		}
		used += snprintf(line + used, sizeof(line) - (size_t)used, " %lx", (unsigned long)((uintptr_t)frames[i] - slide));
	}
	say("%s", line);
}

/*
 * A trace of CMCapture's Objective-C messages, to see what its capture graph does just before it gives our streams back: every objc_msgSend stub of
 * CMCapture (the objc_stubs section, five instructions each: adrp/ldr of the selector, adrp/ldr of objc_msgSend's pointer in the image's GOT, br)
 * jumps through ONE slot of that GOT; the slot is pointed at redwood_msg_hook, which records (self's class, selector, thread) in a ring and goes on
 * to the real objc_msgSend with every argument register as it was. dump_msgs() says the last ones of the calling thread.
 */
struct msg_entry {
	void            *cls, *sel;
	uint64_t        thread;
};
#define MSG_N 16384
static struct msg_entry msg_ring[MSG_N];
static volatile uint64_t msg_count;
void *redwood_msg_orig;
void redwood_msg_log(void *self, void *sel);
void redwood_msg_hook(void);

/*
 * Every selector sent since the trace began, and how often (the ring above holds only the last 16384 messages, which the preview's traffic replaces in seconds):
 * what the shutter starts is the selectors first sent after a mark taken before it (seltab_mark, then dump_new_selectors).
 */
#define SELTAB_N 32768
static struct {
	void *volatile          sel;
	void                    *cls;           /* of the receiver: a selector is counted for each class that gets it */
	volatile uint32_t       n, n_mark;
} seltab[SELTAB_N];

static void
seltab_note(void *cls, void *sel)
{
	unsigned h = (unsigned)(((((uintptr_t)sel >> 2) ^ ((uintptr_t)cls >> 3)) * 0x9E3779B97F4A7C15ULL) >> 49);

	for (unsigned k = 0; k < 128; k++) {
		unsigned i = (h + k) & (SELTAB_N - 1);
		void *cur = seltab[i].sel;

		if (cur == NULL) {
			void *expect = NULL;

			if (__atomic_compare_exchange_n(&seltab[i].sel, &expect, sel, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
				seltab[i].cls = cls;
				cur = sel;
			} else {
				cur = expect;
			}
		}
		if (cur == sel && seltab[i].cls == cls) {
			__atomic_fetch_add(&seltab[i].n, 1, __ATOMIC_RELAXED);
			return;
		}
	}
}

void
redwood_msg_log(void *self, void *sel)
{
	uint64_t i = __atomic_fetch_add(&msg_count, 1, __ATOMIC_RELAXED) % MSG_N;
	void *cls = self != NULL ? (void *)object_getClass((id)self) : NULL;

	msg_ring[i].cls = cls;
	msg_ring[i].sel = sel;
	msg_ring[i].thread = pthread_mach_thread_np(pthread_self());
	if (sel != NULL) {
		seltab_note(cls, sel);
	}
}

static bool seltab_marked;

static void
seltab_mark(void)
{
	for (unsigned i = 0; i < SELTAB_N; i++) {
		seltab[i].n_mark = seltab[i].n;
	}
}

static void keep_for_host(const char *line);
static void keep_for_host_fmt(const char *format, ...) __printflike(1, 2);

/* a line for the host, kept whole (kern.redwood.host_keep: the console log keeps only its last 261 KB) */
static void
keep_for_host(const char *line)
{
	(void)sysctlbyname("kern.redwood.host_keep", NULL, NULL, line, strnlen(line, 400));
}

static void
keep_for_host_fmt(const char *format, ...)
{
	char line[300];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	keep_for_host(line);
}

/* the selectors that were first sent after seltab_mark(), with their first receiver's class and how often since */
static void
dump_new_selectors(const char *what)
{
	char line[400];
	int used = snprintf(line, sizeof(line), "SELNEW %s:", what);
	unsigned found = 0;

	for (unsigned i = 0; i < SELTAB_N; i++) {
		void *sel = seltab[i].sel;

		if (sel == NULL || seltab[i].n_mark != 0 || seltab[i].n == 0) {
			continue;
		}
		char one[160];
		int add = snprintf(one, sizeof(one), " %s.%s x%u", seltab[i].cls != NULL ? class_getName((Class)seltab[i].cls) : "nil", sel_getName((SEL)sel), seltab[i].n);

		if (used + add >= (int)sizeof(line) - 2) {
			keep_for_host(line);
			used = snprintf(line, sizeof(line), "SELNEW+:");
		}
		used += snprintf(line + used, sizeof(line) - (size_t)used, "%s", one);
		found++;
	}
	keep_for_host(line);
	snprintf(line, sizeof(line), "SELNEW %s: %u selectors new since the mark", what, found);
	keep_for_host(line);
	/* and the old ones that the nodes of the still path got more of since the mark (a frame that reached a node shows as a render call there) */
	used = snprintf(line, sizeof(line), "SELGROW %s:", what);
	for (unsigned i = 0; i < SELTAB_N; i++) {
		void *sel = seltab[i].sel;
		const char *cn;

		if (sel == NULL || seltab[i].n_mark == 0 || seltab[i].n == seltab[i].n_mark || seltab[i].cls == NULL) {
			continue;
		}
		cn = class_getName((Class)seltab[i].cls);
		if (strstr(sel_getName((SEL)sel), "renderSampleBuffer") == NULL && strstr(sel_getName((SEL)sel), "emitSampleBuffer") == NULL) {
			continue;
		}
		char one[160];
		int add = snprintf(one, sizeof(one), " %s.%s +%u", cn, sel_getName((SEL)sel), seltab[i].n - seltab[i].n_mark);

		if (used + add >= (int)sizeof(line) - 2) {
			keep_for_host(line);
			used = snprintf(line, sizeof(line), "SELGROW+:");
		}
		used += snprintf(line + used, sizeof(line) - (size_t)used, "%s", one);
	}
	keep_for_host(line);
}

__asm__(
	".text\n.align 4\n.globl _redwood_msg_hook\n_redwood_msg_hook:\n"
	"stp x29, x30, [sp, #-0xe0]!\n"
	"mov x29, sp\n"
	"stp x0, x1, [sp, #0x10]\n"
	"stp x2, x3, [sp, #0x20]\n"
	"stp x4, x5, [sp, #0x30]\n"
	"stp x6, x7, [sp, #0x40]\n"
	"str x8, [sp, #0x50]\n"
	"stp q0, q1, [sp, #0x60]\n"
	"stp q2, q3, [sp, #0x80]\n"
	"stp q4, q5, [sp, #0xa0]\n"
	"stp q6, q7, [sp, #0xc0]\n"
	"bl _redwood_msg_log\n"
	"ldp q6, q7, [sp, #0xc0]\n"
	"ldp q4, q5, [sp, #0xa0]\n"
	"ldp q2, q3, [sp, #0x80]\n"
	"ldp q0, q1, [sp, #0x60]\n"
	"ldr x8, [sp, #0x50]\n"
	"ldp x6, x7, [sp, #0x40]\n"
	"ldp x4, x5, [sp, #0x30]\n"
	"ldp x2, x3, [sp, #0x20]\n"
	"ldp x0, x1, [sp, #0x10]\n"
	"ldp x29, x30, [sp], #0xe0\n"
	"adrp x16, _redwood_msg_orig@PAGE\n"
	"ldr x16, [x16, _redwood_msg_orig@PAGEOFF]\n"
	"br x16\n"
);


/* a fault in mediaserverd (it has been dying of SIGBUS a few ms after the capture graph gives the streams back): the signal, the faulting address, pc, lr
 * and the frames, unslid, said to the kernel's kept log before the process goes */
static void
crash_handler(int sig, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;
	uintptr_t slide = redwood_cache_slide(), pc = (uintptr_t)uc->uc_mcontext->__ss.__pc, lr = (uintptr_t)uc->uc_mcontext->__ss.__lr;
	uintptr_t fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
	char line[200];
	int used = snprintf(line, sizeof(line), "!redwood: camera: CRASH signal %d addr %p pc %lx lr %lx frames:", sig, info->si_addr, (unsigned long)(pc - slide), (unsigned long)(lr - slide));

	for (int i = 0; i < 9 && fp != 0 && (fp & 7) == 0 && used < (int)sizeof(line) - 12; i++) {
		used += snprintf(line + used, sizeof(line) - (size_t)used, " %lx", (unsigned long)((((uintptr_t *)fp)[1] & 0xfffffffffULL) - slide));
		fp = ((uintptr_t *)fp)[0];
	}
	sysctlbyname("kern.redwood.say", NULL, NULL, line, strnlen(line, sizeof(line)));
	if (CAM_TRACE) {
		keep_for_host(line + 1);
		dump_new_selectors("crash");
	}
	signal(sig, SIG_DFL);
}

static void
install_crash_handler(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = crash_handler;
	sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);          /* (an uncaught exception, an assertion: mediaserverd went down 9 s after the shutter, run 579) */
	sigaction(SIGTRAP, &sa, NULL);
}

static void
install_msg_hook(void)
{
	static bool done;
#ifndef CAM_TRACE_STUB
	/* No address of the user's own cache is kept in the tree: for CAM_TRACE=1 build with -DCAM_TRACE_STUB=0x... (the unslid address of an
	 * objc_stubs entry of CMCapture in the iOS cache you built from, found with tools/dsc/objcstub.py) */
	say("msg hook: CAM_TRACE_STUB is not set in this build: no message trace");
	return;
#else
	uintptr_t stub = (uintptr_t)(CAM_TRACE_STUB) + redwood_cache_slide();
	const uint32_t *w = (const uint32_t *)stub;
	uintptr_t page, slot;
	int64_t imm;
	kern_return_t kr;

	if (done) {
		return;
	}
	done = true;
	if ((w[2] & 0x9f000000u) != 0x90000000u || (w[3] & 0xffc00000u) != 0xf9400000u) {
		say("msg hook: the stub is not what was expected (%08x %08x)", w[2], w[3]);
		return;
	}
	imm = (int64_t)(((w[2] >> 5) & 0x7ffff) << 2 | ((w[2] >> 29) & 3));
	imm = (imm << 43) >> 43;                                        /* sign extend the 21 bits */
	page = ((stub + 8) & ~(uintptr_t)0xfff) + (uintptr_t)(imm << 12);
	slot = page + (((w[3] >> 10) & 0xfff) << 3);
	kr = vm_protect(mach_task_self(), slot & ~(uintptr_t)0x3fff, 0x4000, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("msg hook: the slot %lx cannot be written (%d)", (unsigned long)slot, kr);
		return;
	}
	redwood_msg_orig = *(void **)slot;
	*(void **)slot = (void *)redwood_msg_hook;
	say("msg hook: slot %lx, objc_msgSend %p -> %p", (unsigned long)slot, redwood_msg_orig, (void *)redwood_msg_hook);
#endif /* CAM_TRACE_STUB */
}


/* over the whole ring (all threads): which selectors with "tart", "rror", "repare", "Fail" in them were sent, and how often */
static const char *sel_filter;

static void
dump_selector_counts(const char *what)
{
	struct { const char *sel; const char *cls; unsigned n; } seen[48];
	unsigned have = 0;
	uint64_t n = msg_count;
	char line[200];
	int used;

	if (redwood_msg_orig == NULL) {
		return;
	}
	memset(seen, 0, sizeof(seen));
	for (uint64_t k = 0; k < MSG_N && k < n; k++) {
		const struct msg_entry *e = &msg_ring[(n - 1 - k) % MSG_N];
		const char *sn;

		if (e->sel == NULL) {
			continue;
		}
		sn = sel_getName((SEL)e->sel);
		if (sel_filter != NULL ? strstr(sn, sel_filter) == NULL :
		    (strstr(sn, "tart") == NULL && strstr(sn, "rror") == NULL && strstr(sn, "repare") == NULL && strstr(sn, "Fail") == NULL && strstr(sn, "didFinish") == NULL)) {
			continue;
		}
		unsigned i;

		for (i = 0; i < have; i++) {
			if (seen[i].sel == sn) {
				seen[i].n++;
				break;
			}
		}
		if (i == have && have < 48) {
			seen[have].sel = sn;
			seen[have].cls = e->cls != NULL ? class_getName((Class)e->cls) : "nil";
			seen[have].n = 1;
			have++;
		}
	}
	used = snprintf(line, sizeof(line), "SELS %s:", what);
	for (unsigned i = 0; i < have; i++) {
		int add = snprintf(NULL, 0, " %s.%s x%u", seen[i].cls, seen[i].sel, seen[i].n);

		if (used + add >= (int)sizeof(line) - 2) {
			say("%s", line);
			used = snprintf(line, sizeof(line), "SELS+:");
		}
		used += snprintf(line + used, sizeof(line) - (size_t)used, " %s.%s x%u", seen[i].cls, seen[i].sel, seen[i].n);
	}
	say("%s", line);
}


/*
 * Return values of the methods the capture graph's build can fail in, with the NSError it says: -[BWGraph commitConfigurationWithID:error:] (format negotiation
 * between the nodes) and -[BWGraph addNode:error:], swizzled to call the original and say a NO.
 */
static BOOL (*orig_commit)(id, SEL, uintptr_t, id *);
static BOOL (*orig_addnode)(id, SEL, id, id *);

static void
say_error(const char *what, id *err)
{
	static unsigned said;
	CFStringRef d;
	char text[400] = "?";

	if (said++ >= 12) {
		return;
	}
	if (err != NULL && *err != NULL && (d = CFErrorCopyDescription((CFErrorRef)*err)) != NULL) {
		CFStringGetCString(d, text, sizeof(text), kCFStringEncodingUTF8);
		CFRelease(d);
	}
	for (char *c = text; *c; c++) {
		*c = *c == '\n' ? ' ' : *c;
	}
	say("%s FAILED: %.150s", what, text);
	if (strlen(text) > 150) {
		say("%s ...: %.170s", what, text + 150);
	}
}

static BOOL
hook_commit(id self, SEL cmd, uintptr_t configuration, id *err)
{
	BOOL r = orig_commit(self, cmd, configuration, err);

	if (!r) {
		say_error("BWGraph commitConfigurationWithID:error:", err);
	}
	return r;
}

static BOOL
hook_addnode(id self, SEL cmd, id node, id *err)
{
	BOOL r = orig_addnode(self, cmd, node, err);

	if (!r) {
		say_error("BWGraph addNode:error:", err);
	}
	return r;
}

static void
install_graph_hooks(void)
{
	static bool done;
	Class graph = objc_getClass("BWGraph");
	Method m;

	if (done || graph == NULL) {
		return;
	}
	done = true;
	if ((m = class_getInstanceMethod(graph, sel_registerName("commitConfigurationWithID:error:"))) != NULL) {
		orig_commit = (BOOL (*)(id, SEL, uintptr_t, id *))method_getImplementation(m);
		method_setImplementation(m, (IMP)hook_commit);
	}
	if ((m = class_getInstanceMethod(graph, sel_registerName("addNode:error:"))) != NULL) {
		orig_addnode = (BOOL (*)(id, SEL, id, id *))method_getImplementation(m);
		method_setImplementation(m, (IMP)hook_addnode);
	}
	say("graph hooks: commit %p, addNode %p", (void *)orig_commit, (void *)orig_addnode);
}

/* what this thread sent last (CMCapture's own messages; oldest first), a few names to a line */
static void
dump_msgs(const char *what, unsigned want)
{
	uint64_t me = pthread_mach_thread_np(pthread_self()), n = msg_count, got = 0;
	static const char *names[600];
	unsigned have = 0;

	if (redwood_msg_orig == NULL || want > 600) {
		return;
	}
	for (uint64_t k = 0; k < MSG_N && k < n && got < want; k++) {   /* (the ring holds the last 16384 messages of all threads) */
		const struct msg_entry *e = &msg_ring[(n - 1 - k) % MSG_N];
		static char text[600][90];

		const char *cn;

		if (e->thread != me || e->sel == NULL) {
			continue;
		}
		cn = e->cls != NULL ? class_getName((Class)e->cls) : "nil";
		snprintf(text[got], sizeof(text[got]), "%s.%s", cn, sel_getName((SEL)e->sel));
		names[have++] = text[got];
		got++;
	}
	char line[200];
	int used = snprintf(line, sizeof(line), "MSGS %s:", what);

	for (unsigned i = have; i > 0; i--) {
		int add = (int)strlen(names[i - 1]) + 1;

		if (used + add >= (int)sizeof(line) - 2) {
			say("%s", line);
			used = snprintf(line, sizeof(line), "MSGS+:");
		}
		used += snprintf(line + used, sizeof(line) - (size_t)used, " %s", names[i - 1]);
	}
	say("%s", line);
}

/* ---- CoreMedia's derived objects, as iOS 16.5 lays them out ---------------------------------------- */

typedef struct OpaqueCMBaseObject *CMBaseObjectRef;
typedef const void *CMBaseClassID;

struct base_class {
	uint64_t        version;
	uint64_t        storage_size;
	Boolean         (*equal)(CMBaseObjectRef, CMBaseObjectRef);
	OSStatus        (*invalidate)(CMBaseObjectRef);
	void            (*finalize)(CMBaseObjectRef);
	CFStringRef     (*copy_debug_description)(CMBaseObjectRef);
	OSStatus        (*copy_property)(CMBaseObjectRef, CFStringRef, CFAllocatorRef, void *);
	OSStatus        (*set_property)(CMBaseObjectRef, CFStringRef, CFTypeRef);
	OSStatus        (*notification_barrier)(CMBaseObjectRef);
	const void *    protocols;
	const void *    reserved;
};
struct device_class {
	uint64_t        version;                /* 3 */
	void *          processing_session_create;
	OSStatus        (*request_streams)(CMBaseObjectRef, CFArrayRef, CFDictionaryRef);
	OSStatus        (*relinquish_streams)(CMBaseObjectRef, CFArrayRef, CFDictionaryRef);
	void *          pearl_calibration;
};
struct stream_class {
	uint64_t        version;                /* 2 */
	OSStatus        (*start)(CMBaseObjectRef);
	OSStatus        (*stop)(CMBaseObjectRef);
	OSStatus        (*add_attachments)(CMBaseObjectRef, CMSampleBufferRef, CFDictionaryRef);
};
struct vtable {
	const void *    reserved;
	const struct base_class *base;
	const void *    derived;
};

#define ERR_PARAM               (-12780)
#define ERR_NOT_AVAILABLE       (-12783)
#define ERR_UNSUPPORTED         (-12782)

static OSStatus (*derived_create)(CFAllocatorRef, const struct vtable *, CMBaseClassID, CMBaseObjectRef *);
static void *(*derived_storage)(CMBaseObjectRef);
static CMBaseClassID (*device_class_id)(void);
static CMBaseClassID (*stream_class_id)(void);
static CMBaseClassID (*session_class_id)(void);     /* FigCaptureISPProcessingSessionGetClassID */
static CFStringRef port_back;           /* kFigCapturePortType_BackFacingCamera */
static CFStringRef port_front;          /* kFigCapturePortType_FrontFacingCamera (r1cam_flip=1) */

/* a number from the kernel's boot-args ("name=N"), or otherwise (its sandbox may not let it read them) */
static long
bootarg_number(const char *name, long otherwise)
{
	char args[1024] = " ", key[40];
	size_t size = sizeof(args) - 2;
	const char *at;

	snprintf(key, sizeof(key), " %s=", name);
	if (sysctlbyname("kern.bootargs", args + 1, &size, NULL, 0) != 0 || (at = strstr(args, key)) == NULL) {
		return otherwise;
	}
	return strtol(at + strlen(key), NULL, 0);
}

static bool
find_functions(void)
{
	CFStringRef *port;

	derived_create = dlsym(RTLD_DEFAULT, "CMDerivedObjectCreate");
	derived_storage = dlsym(RTLD_DEFAULT, "CMBaseObjectGetDerivedStorage");
	device_class_id = dlsym(RTLD_DEFAULT, "FigCaptureDeviceGetClassID");
	stream_class_id = dlsym(RTLD_DEFAULT, "FigCaptureStreamGetClassID");
	session_class_id = dlsym(RTLD_DEFAULT, "FigCaptureISPProcessingSessionGetClassID");
	port = dlsym(RTLD_DEFAULT, "kFigCapturePortType_BackFacingCamera");
	port_back = port != NULL ? *port : NULL;
	port = dlsym(RTLD_DEFAULT, "kFigCapturePortType_FrontFacingCamera");
	port_front = port != NULL ? *port : NULL;
	if (derived_create == NULL || derived_storage == NULL || device_class_id == NULL || stream_class_id == NULL || port_back == NULL) {
		say("CoreMedia's or CMCapture's functions are not there: no camera");
		return false;
	}
	return true;
}

/* ---- what is asked and not answered: said once per name ------------------------------------------- */

static CFMutableSetRef said_names;
static pthread_mutex_t said_lock = PTHREAD_MUTEX_INITIALIZER;

static void
say_once(const char *what, CFStringRef key, CFTypeRef value)
{
	char name[160] = "?", text[300] = "";
	CFStringRef tag = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s %@"), what, key);

	pthread_mutex_lock(&said_lock);
	if (said_names == NULL) {
		said_names = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	bool first = !CFSetContainsValue(said_names, tag);

	CFSetAddValue(said_names, tag);
	pthread_mutex_unlock(&said_lock);
	CFRelease(tag);
	if (!first) {
		return;
	}
	CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
	if (value != NULL) {
		CFStringRef d = CFCopyDescription(value);

		CFStringGetCString(d, text, sizeof(text), kCFStringEncodingUTF8);
		CFRelease(d);
		for (char *c = text; *c; c++) {
			*c = *c == '\n' ? ' ' : *c;
		}
	}
	say("%s %s%s%s", what, name, value ? " = " : "", text);
}

/* ---- numbers and dictionaries ---------------------------------------------------------------------- */

static CFNumberRef
number_i(int64_t v)
{
	return CFNumberCreate(NULL, kCFNumberSInt64Type, &v);
}

static CFNumberRef
number_f(double v)
{
	return CFNumberCreate(NULL, kCFNumberDoubleType, &v);
}

static void
put(CFMutableDictionaryRef d, const char *key, CFTypeRef value)
{
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);

	CFDictionarySetValue(d, k, value);
	CFRelease(k);
	CFRelease(value);
}

static void
put_k(CFMutableDictionaryRef d, CFStringRef key, CFTypeRef value)
{
	CFDictionarySetValue(d, key, value);
	CFRelease(value);
}

static int64_t
get_i(CFDictionaryRef d, const char *key, int64_t otherwise)
{
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFTypeRef v = d ? CFDictionaryGetValue(d, k) : NULL;
	int64_t out = otherwise;

	CFRelease(k);
	if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID()) {
		CFNumberGetValue(v, kCFNumberSInt64Type, &out);
	} else if (v != NULL && CFGetTypeID(v) == CFBooleanGetTypeID()) {
		out = CFBooleanGetValue(v);
	}
	return out;
}

static CFDictionaryRef
new_dict(void)
{
	return CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

static bool
key_is(CFStringRef key, const char *name)
{
	char buffer[128];

	return CFStringGetCString(key, buffer, sizeof(buffer), kCFStringEncodingUTF8) && strcmp(buffer, name) == 0;
}

/* ---- what the device and the stream say they have ---------------------------------------------------- */

/*
 * The capture stack asks only for the properties an object lists in its "SupportedPropertiesDictionary"
 * ({ name: { PropertyType, ReadWriteStatus } }). Listed here: what is answered, and what is taken
 * (and, for now, mostly not acted on) without failing the graph that sets it.
 */
struct prop {
	const char *name, *type, *access;
};

static const struct prop stream_props[] = {
	{ "PortType", "String", "ReadOnly" }, { "ModuleInfo", "Collection", "ReadOnly" }, { "UniqueID", "String", "ReadOnly" }, { "NonLocalizedName", "String", "ReadOnly" },
	{ "SupportedFormatsArray", "Collection", "ReadOnly" }, { "FocalLength", "Number", "ReadOnly" },
	{ "LensFNumber", "Number", "ReadOnly" }, { "PixelSize", "Number", "ReadOnly" },
	{ "MaxZoomFactor", "Number", "ReadOnly" }, { "SupportedColorSpaces", "Collection", "ReadOnly" },
	{ "HasAutoFocusPositionSensor", "Boolean", "ReadOnly" }, { "Clock", "Clock", "ReadOnly" },
	{ "FormatIndex", "Number", "ReadWrite" }, { "MinimumFrameRate", "Number", "ReadWrite" }, { "MaximumFrameRate", "Number", "ReadWrite" },
	{ "MaximumAllowedFrameRate", "Number", "ReadWrite" }, { "VideoOutputConfigurations", "Collection", "ReadWrite" },
	{ "VideoOutputsEnabled", "Collection", "ReadWrite" }, { "VideoOutputHandlers", "Collection", "WriteOnly" },
	{ "DeferAdditionOfAttachments", "Boolean", "ReadWrite" }, { "TorchLevel", "Number", "ReadWrite" },
	{ "HighCurrentTorchEnabled", "Boolean", "ReadWrite" }, { "ColorSpace", "Number", "ReadWrite" }, { "ZoomFactor", "Number", "ReadWrite" },
	{ "ExposureBias", "Number", "ReadWrite" }, { "DeviceOrientation", "Number", "ReadWrite" }, { "FlashMode", "Number", "ReadWrite" },
	{ "StillImageCaptureNow", "Collection", "WriteOnly" }, { "LockAENow", "Boolean", "WriteOnly" }, { "UnlockAENow", "Boolean", "WriteOnly" },
	{ "LockAWBNow", "Boolean", "WriteOnly" }, { "UnlockAWBNow", "Boolean", "WriteOnly" }, { "MaxIntegrationTime", "Number", "ReadWrite" },
	{ "AEMaxGain", "Number", "ReadWrite" }, { "AEMinGain", "Number", "ReadWrite" }, { "FaceDetectionEnabled", "Boolean", "ReadWrite" },
	{ "VideoRecordingInProgress", "Boolean", "ReadWrite" }, { "MemoryPoolID", "Number", "ReadWrite" },
	{ "ChromaNoiseReductionEnabled", "Boolean", "ReadWrite" }, { "TemporalNoiseReductionEnabled", "Boolean", "ReadWrite" },
	{ "MultipleOutputSupportEnabled", "Boolean", "ReadWrite" }, { "VideoOutputsColorInfo", "Collection", "ReadWrite" },
	{ "RegionOfInterestForCameraControls", "Collection", "ReadWrite" }, { "AEMeteringMode", "Number", "ReadWrite" },
	{ "MaxFrameCoalescingDuration", "Number", "ReadWrite" }, { "LimitAERange", "Boolean", "ReadWrite" },
}, device_props[] = {
	{ "StreamArray", "Collection", "ReadOnly" }, { "Clock", "Clock", "ReadOnly" }, { "ClientIsActive", "Boolean", "ReadWrite" },
	{ "SupportedSynchronizedStreamsGroups", "Collection", "ReadOnly" }, { "MemoryPoolID", "Number", "ReadWrite" },
};

static CFDictionaryRef
supported(const struct prop *p, size_t n)
{
	CFMutableDictionaryRef all = (CFMutableDictionaryRef)new_dict();

	for (size_t i = 0; i < n; i++) {
		CFMutableDictionaryRef one = (CFMutableDictionaryRef)new_dict();

		put(one, "PropertyType", CFStringCreateWithCString(NULL, p[i].type, kCFStringEncodingUTF8));
		put(one, "ReadWriteStatus", CFStringCreateWithCString(NULL, p[i].access, kCFStringEncodingUTF8));
		put(all, p[i].name, one);
	}
	return all;
}

/* ---- the formats ----------------------------------------------------------------------------------- */

#define PF_420F         0x34323066      /* '420f' */
#define PF_420V         0x34323076      /* '420v' */
#define PF_BGRA         0x42475241      /* 'BGRA' */

static CFDictionaryRef
output_info(int width, int height)
{
	CFMutableDictionaryRef o = (CFMutableDictionaryRef)new_dict();
	CFNumberRef pf[3] = { number_i(PF_420F), number_i(PF_420V), number_i(PF_BGRA) };
	CFArrayRef pfs = CFArrayCreate(NULL, (const void **)pf, 3, &kCFTypeArrayCallBacks);

	for (int i = 0; i < 3; i++) {
		CFRelease(pf[i]);
	}
	put(o, "SupportedPixelFormats", pfs);
	put(o, "NativeWidth", number_i(width));
	put(o, "NativeHeight", number_i(height));
	put(o, "MaxDownscaleFactor", number_f(32));
	put(o, "MaxScaleFactor", number_f(1));
	put(o, "MinScaleFactor", number_f(1.0 / 32));
	put(o, "OutputWidthAlignmentRequirement", number_i(16));
	return o;
}

static CFDictionaryRef
format(int sensor_width, int sensor_height, int max_fps, bool binned)
{
	CFMutableDictionaryRef f = (CFMutableDictionaryRef)new_dict(), outputs = (CFMutableDictionaryRef)new_dict();
	CMVideoFormatDescriptionRef fd = NULL;

	CMVideoFormatDescriptionCreate(NULL, PF_420F, sensor_width, sensor_height, NULL, &fd);
	if (fd != NULL) {
		put(f, "FormatDescription", fd);
	}
	put(f, "VideoMinFrameRate", number_f(2));
	put(f, "VideoMaxFrameRate", number_f(max_fps));
	put(f, "VideoIsBinned", CFRetain(binned ? kCFBooleanTrue : kCFBooleanFalse));
	put(f, "VideoBinningFactorHorizontal", number_i(binned ? 2 : 1));
	put(f, "VideoBinningFactorVertical", number_i(binned ? 2 : 1));
	put(f, "VideoFieldOfView", number_f(65.0));
	put(f, "VideoMinIntegrationTime", number_i(100));
	put(f, "VideoMaxIntegrationTime", number_i(1000000 / 15));
	put(f, "VideoRawBitDepth", number_i(10));
	put(f, "VideoMaxWidth", number_i(sensor_width));
	put(f, "VideoMaxHeight", number_i(sensor_height));
	put(f, "Experimental", CFRetain(kCFBooleanFalse));
	put(f, "SensorWidth", number_i(sensor_width));
	put(f, "SensorHeight", number_i(sensor_height));
	put(f, "ISOBase", number_f(32));
	put(f, "DefaultAEMaxGain", number_f(64));

	/*
	 * The sensor crop geometry the capture-graph builder reads off this format to resolve non-zero
	 * output dimensions (confirmed by a hook: -[FigCaptureSourceVideoFormat sensorCropDimensions] /
	 * postGDCCropDimensions] return these). Everything spans the full sensor: no GDC crop.
	 */
	put(f, "SensorCropWidth", number_i(sensor_width));
	put(f, "SensorCropHeight", number_i(sensor_height));
	put(f, "PreGDCSensorCropWidth", number_i(sensor_width));
	put(f, "PreGDCSensorCropHeight", number_i(sensor_height));
	put(f, "PostGDCCropWidth", number_i(sensor_width));
	put(f, "PostGDCCropHeight", number_i(sensor_height));
	put(f, "GeometricDistortionCorrectedNativeWidth", number_i(sensor_width));
	put(f, "GeometricDistortionCorrectedNativeHeight", number_i(sensor_height));

	put(outputs, "PrimaryScaler", output_info(sensor_width, sensor_height));
	put(outputs, "SecondaryScaler", output_info(sensor_width, sensor_height));
	put(outputs, "StillImage", output_info(sensor_width, sensor_height));
	put(f, "SupportedOutputs", outputs);
	return f;
}

/* ---- the stream ------------------------------------------------------------------------------------- */

struct output {
	const char *    name;
	CFDictionaryRef config;                 /* from VideoOutputConfigurations */
	bool            enabled;
	void *          handler;                /* a block: ^(CVPixelBufferRef, CMTime) */
	CVPixelBufferPoolRef pool;
	int             pool_width, pool_height;
	uint32_t        pool_format;
	uint64_t        delivered;
	unsigned        pictures;               /* (the worker's ten-second line) given since, and the time that took */
	uint64_t        render_ns;
	uint64_t        isp_ns, handler_ns;     /* (of render_ns: the ISP's pieces; the graph's handler) */
};

#define OUTPUTS 3
static const char *const output_names[OUTPUTS] = { "PrimaryScaler", "SecondaryScaler", "StillImage" };

struct stream {
	CFArrayRef      formats;
	CFDictionaryRef supported;
	int             format_index;
	double          min_fps, max_fps;
	struct output   out[OUTPUTS];
	pthread_mutex_t lock;
	pthread_t       worker;
	volatile bool   running;
	bool            worker_started;
	volatile bool   delivering;             /* the capture graph has called Start: pictures may go to its handlers (before that its nodes are not all there: a crash) */
	int             torch;
	uint64_t        frames;
	bool            front;                  /* the front camera's stream (r1cam_flip=1): the motor turns to it before its frames */
};

static struct stream *
stream_of(CMBaseObjectRef o)
{
	return derived_storage(o);
}

static int
output_index(CFStringRef name)
{
	for (int i = 0; i < OUTPUTS; i++) {
		if (key_is(name, output_names[i])) {
			return i;
		}
	}
	return -1;
}

static OSStatus
stream_copy_property(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef allocator, void *out)
{
	struct stream *s = stream_of(o);
	CFTypeRef value = NULL;

	(void)allocator;
	trace_key("stream get", key);
	if (key_is(key, "PortType")) {
		value = CFRetain(s->front ? port_front : port_back);
	} else if (s->front && key_is(key, "ModuleInfo")) {
		/* D22/CameraSetup.plist's PortTypeFront: sensor 0x0354, FocalLengthCharacterization { LensID [1, 2, 5], scheme 1 } (no actuator or integrator) */
		CFMutableDictionaryRef info = (CFMutableDictionaryRef)new_dict();

		put(info, "sensorID", number_i(0x354));
		put(info, "ModuleLensID", number_i(1));
		put(info, "ModuleIDSchemeVersion", number_i(1));
		value = info;
	} else if (key_is(key, "ModuleInfo")) {
		/* what FigCaptureCameraParameters reads (+sensorIDStringFromModuleInfo: formats ["sensorID"] as "0x%04x") to find the tuning data of the sensor in
		 * D22/CameraSetup.plist: the iPhone X's back camera, 0x0343. Without it the graph has no sensor ID dictionary and BWVISNode refuses to be made. */
		CFMutableDictionaryRef info = (CFMutableDictionaryRef)new_dict();

		put(info, "sensorID", number_i(0x343));
		/* ... and the module's lens, actuator and integrator, which -[BWFigVideoCaptureStream init] matches against that sensor's
		 * FocalLengthCharacterization entries (LensID [1], ActuatorID [1], IntegratorID [1, 2, 3, 5, 6], scheme version 1); no match: its init fails */
		put(info, "ModuleLensID", number_i(1));
		put(info, "ModuleActuatorID", number_i(1));
		put(info, "ModuleIntegratorID", number_i(1));
		put(info, "ModuleFocusID", number_i(1));
		put(info, "ModuleIDSchemeVersion", number_i(1));
		value = info;
	} else if (key_is(key, "UniqueID") || key_is(key, "ModelID")) {
		value = CFRetain(s->front ? CFSTR("com.apple.avfoundation.avcapturedevice.built-in_video:1") :
		    CFSTR("com.apple.avfoundation.avcapturedevice.built-in_video:0"));
	} else if (key_is(key, "NonLocalizedName") || key_is(key, "LocalizedName")) {
		value = CFRetain(s->front ? CFSTR("Front Camera") : CFSTR("Back Camera"));
	} else if (key_is(key, "Manufacturer")) {
		value = CFRetain(CFSTR("Apple Inc."));
	} else if (key_is(key, "SupportedFormatsArray")) {
		value = CFRetain(s->formats);
	} else if (key_is(key, "FormatIndex")) {
		value = number_i(s->format_index);
	} else if (key_is(key, "MinimumFrameRate")) {
		value = number_f(s->min_fps);
	} else if (key_is(key, "MaximumFrameRate")) {
		value = number_f(s->max_fps);
	} else if (key_is(key, "Clock")) {
		value = CFRetain(CMClockGetHostTimeClock());
	} else if (key_is(key, "TorchLevel") || key_is(key, "ExposureBias")) {
		value = number_f(0);
	} else if (key_is(key, "VideoOutputsColorInfo")) {
		/* READ back by the capture graph right after it configures the stream (not IfSupported:), and a failed read tears the graph down: run 554,
		 * the relinquish came 4 ms after this get. A dictionary by output (none to say yet: empty). */
		value = new_dict();
	} else if (key_is(key, "VideoOutputConfigurations") || key_is(key, "VideoOutputsEnabled")) {
		/*
		 * ReadWrite, but the capture-graph builder READS these back while it builds the graph
		 * (BWMultiStreamCameraSourceNode's _updateOutputsStorage does a getProperty: for
		 * VideoOutputConfigurations, not IfSupported:), and a failed read tears the graph down. Return
		 * what each output holds now.
		 */
		bool configs = key_is(key, "VideoOutputConfigurations");
		CFMutableDictionaryRef d = (CFMutableDictionaryRef)new_dict();

		pthread_mutex_lock(&s->lock);
		for (int i = 0; i < OUTPUTS; i++) {
			CFStringRef name = CFStringCreateWithCString(NULL, output_names[i], kCFStringEncodingUTF8);

			if (configs) {
				if (s->out[i].config != NULL) {
					CFDictionarySetValue(d, name, s->out[i].config);
				}
			} else {
				CFDictionarySetValue(d, name, s->out[i].enabled ? kCFBooleanTrue : kCFBooleanFalse);
			}
			CFRelease(name);
		}
		pthread_mutex_unlock(&s->lock);
		value = d;
	} else if (key_is(key, "SupportedPropertiesDictionary")) {
		value = CFRetain(s->supported);
	} else if (key_is(key, "FocalLength")) {
		value = number_f(4.0);          /* (the iPhone X's wide camera's, in millimetres) */
	} else if (key_is(key, "LensFNumber")) {
		value = number_f(1.8);
	} else if (key_is(key, "PixelSize")) {
		/*
		 * The capture-graph builder REQUIRES this of the stream: -[BWFigVideoCaptureStream
		 * initWithCaptureStream:...] does a getProperty: (not IfSupported:) for it, and without it the
		 * init fails, so FigVideoCaptureSourcesActivateAndCreateDevices errors and the graph is never
		 * built. The sensor's pixel pitch in micrometres (the iPhone X wide camera's); any value in
		 * (0, 100) is accepted.
		 */
		value = number_f(1.22);
	} else if (key_is(key, "MaxZoomFactor")) {
		value = number_f(16);
	} else if (key_is(key, "ZoomFactor")) {
		value = number_f(1);
	} else if (key_is(key, "SupportedColorSpaces")) {
		CFNumberRef srgb = number_i(0);

		value = CFArrayCreate(NULL, (const void **)&srgb, 1, &kCFTypeArrayCallBacks);
		CFRelease(srgb);
	} else if (key_is(key, "ColorSpace") || key_is(key, "DeviceOrientation") || key_is(key, "FlashMode")) {
		value = number_i(0);
	} else if (key_is(key, "HasAutoFocusPositionSensor") || key_is(key, "HighCurrentTorchEnabled") ||
	    key_is(key, "DeferAdditionOfAttachments")) {
		value = CFRetain(kCFBooleanFalse);
	} else {
		say_once("stream: no answer for", key, NULL);
		return ERR_NOT_AVAILABLE;
	}
	*(CFTypeRef *)out = value;
	return 0;
}

static void torch_set(struct stream *s, CFTypeRef value);

static OSStatus
stream_set_property(CMBaseObjectRef o, CFStringRef key, CFTypeRef value)
{
	struct stream *s = stream_of(o);

	trace_key("stream set", key);
	if (key_is(key, "FormatIndex")) {
		int64_t i = 0;

		if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
			CFNumberGetValue(value, kCFNumberSInt64Type, &i);
		}
		if (i < 0 || i >= CFArrayGetCount(s->formats)) {
			return ERR_PARAM;
		}
		s->format_index = (int)i;
		{
			static unsigned said;

			if (said++ < 3) {
				say("stream: format %lld", (long long)i);
			}
		}
		return 0;
	}
	if (key_is(key, "MinimumFrameRate") || key_is(key, "MaximumFrameRate")) {
		double fps = 0;

		if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
			CFNumberGetValue(value, kCFNumberDoubleType, &fps);
		}
		*(key_is(key, "MinimumFrameRate") ? &s->min_fps : &s->max_fps) = fps;
		say_once("stream: set", key, value);
		return 0;
	}
	if (key_is(key, "VideoOutputConfigurations") || key_is(key, "VideoOutputsEnabled") || key_is(key, "VideoOutputHandlers")) {
		bool configs = key_is(key, "VideoOutputConfigurations"), enabled = key_is(key, "VideoOutputsEnabled");

		say_once("stream: set", key, value);
		if (value == NULL || CFGetTypeID(value) != CFDictionaryGetTypeID()) {
			return 0;
		}
		pthread_mutex_lock(&s->lock);
		CFIndex n = CFDictionaryGetCount(value);
		const void **keys = calloc((size_t)n + 1, sizeof(void *)), **values = calloc((size_t)n + 1, sizeof(void *));

		CFDictionaryGetKeysAndValues(value, keys, values);
		for (CFIndex i = 0; i < n; i++) {
			int k = CFGetTypeID(keys[i]) == CFStringGetTypeID() ? output_index(keys[i]) : -1;
			struct output *out;

			if (k < 0) {
				say_once("stream: an output not here:", keys[i], NULL);
				continue;
			}
			out = &s->out[k];
			if (configs) {
				if (out->config != NULL) {
					CFRelease(out->config);
				}
				out->config = CFGetTypeID(values[i]) == CFDictionaryGetTypeID() ? CFRetain(values[i]) : NULL;
			} else if (enabled) {
				int64_t on = 1;

				if (CFGetTypeID(values[i]) == CFBooleanGetTypeID()) {
					on = CFBooleanGetValue(values[i]);
				} else if (CFGetTypeID(values[i]) == CFNumberGetTypeID()) {
					CFNumberGetValue(values[i], kCFNumberSInt64Type, &on);
				}
				out->enabled = on != 0;
			} else {
				CFTypeRef h = CFGetTypeID(values[i]) == CFDictionaryGetTypeID() ?
				    CFDictionaryGetValue(values[i], CFSTR("PixelBufferHandler")) : NULL;

				if (out->handler != NULL) {
					CFRelease(out->handler);
				}
				out->handler = h != NULL ? (void *)CFRetain(h) : NULL;
			}
		}
		free(keys);
		free(values);
		pthread_mutex_unlock(&s->lock);
		return 0;
	}
	if (key_is(key, "TorchLevel")) {
		torch_set(s, value);
		return 0;
	}
	say_once("stream: set (not used)", key, value);
	return 0;
}

/* the flashlight: TorchLevel (0 to 1) as thousandths to the kernel's /dev/redwood-torch (torch=3) */
static void
torch_set(struct stream *s, CFTypeRef value)
{
	float level = 0;
	uint32_t thousandths;

	if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
		CFNumberGetValue(value, kCFNumberFloatType, &level);
	}
	level = level < 0 ? 0 : level > 1 ? 1 : level;
	thousandths = (uint32_t)(level * 1000 + 0.5f);
	if (s->torch < 0) {
		s->torch = open("/dev/redwood-torch", O_WRONLY);
	}
	if (s->torch < 0 || write(s->torch, &thousandths, sizeof(thousandths)) != (ssize_t)sizeof(thousandths)) {
		say("the flashlight at %u of 1000 NOT TAKEN (no /dev/redwood-torch)", thousandths);
	} else {
		say("the flashlight at %u of 1000", thousandths);
	}
}

/* ---- the frames ------------------------------------------------------------------------------------- */

struct render_job {
	const struct isp *isp;
	const struct redwood_camera_frame *f;
	const uint8_t *raw;
	CVPixelBufferRef pb;
	uint32_t format, width, height, pieces;
};

static void
render_piece(void *context, size_t i)
{
	struct render_job *j = context;
	uint32_t rows = (j->height + j->pieces - 1) / j->pieces, first = (uint32_t)i * rows, end = first + rows;

	first &= ~1u;
	end = i + 1 == j->pieces ? j->height : end & ~1u;
	if (j->format == PF_BGRA) {
		isp_render_bgra(j->isp, j->f, j->raw, CVPixelBufferGetBaseAddress(j->pb), CVPixelBufferGetBytesPerRow(j->pb), j->width,
		    j->height, first, end);
	} else {
		isp_render_nv12(j->isp, j->f, j->raw, CVPixelBufferGetBaseAddressOfPlane(j->pb, 0), CVPixelBufferGetBytesPerRowOfPlane(j->pb, 0),
		    CVPixelBufferGetBaseAddressOfPlane(j->pb, 1), CVPixelBufferGetBytesPerRowOfPlane(j->pb, 1), j->width, j->height, first, end,
		    j->format == PF_420V);
	}
}

static CVPixelBufferRef
output_buffer(struct output *out, int width, int height, uint32_t pf)
{
	CVPixelBufferRef pb = NULL;

	if (out->pool == NULL || out->pool_width != width || out->pool_height != height || out->pool_format != pf) {
		CFMutableDictionaryRef a = (CFMutableDictionaryRef)new_dict();

		if (out->pool != NULL) {
			CVPixelBufferPoolRelease(out->pool);
			out->pool = NULL;
		}
		put_k(a, kCVPixelBufferWidthKey, number_i(width));
		put_k(a, kCVPixelBufferHeightKey, number_i(height));
		put_k(a, kCVPixelBufferPixelFormatTypeKey, number_i(pf));
		put_k(a, kCVPixelBufferIOSurfacePropertiesKey, new_dict());
		if (CVPixelBufferPoolCreate(NULL, NULL, a, &out->pool) != kCVReturnSuccess) {
			out->pool = NULL;
		}
		CFRelease(a);
		out->pool_width = width;
		out->pool_height = height;
		out->pool_format = pf;
		say("%s: %dx%d %c%c%c%c buffers%s", out->name, width, height, pf >> 24, pf >> 16 & 255, pf >> 8 & 255, pf & 255,
		    out->pool ? "" : " CANNOT BE MADE");
	}
	if (out->pool == NULL || CVPixelBufferPoolCreatePixelBuffer(NULL, out->pool, &pb) != kCVReturnSuccess) {
		return NULL;
	}
	return pb;
}

typedef void (^pixel_buffer_handler)(CVPixelBufferRef, CMTime);

static void
deliver(struct stream *s, struct output *out, const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw)
{
	int width = (int)get_i(out->config, "Width", 0), height = (int)get_i(out->config, "Height", 0);
	uint32_t pf = (uint32_t)get_i(out->config, "PixelFormat", PF_420F);
	CVPixelBufferRef pb;

	(void)s;
	{
		static unsigned said;

		if (said++ < 2) {
			CFStringRef d = out->config != NULL ? CFCopyDescription(out->config) : NULL;
			char text[500] = "(none)";

			if (d != NULL) {
				CFStringGetCString(d, text, sizeof(text), kCFStringEncodingUTF8);
				CFRelease(d);
				for (char *c = text; *c; c++) {
					*c = *c == '\n' ? ' ' : *c;
				}
			}
			{
				CFStringRef t = out->config != NULL ? CFCopyTypeIDDescription(CFGetTypeID(out->config)) : NULL;
				char tn[60] = "?";

				if (t != NULL) {
					CFStringGetCString(t, tn, sizeof(tn), kCFStringEncodingUTF8);
					CFRelease(t);
				}
				say("deliver %s: type %s, %ld entries", out->name, tn, out->config != NULL && CFGetTypeID(out->config) == CFDictionaryGetTypeID() ? (long)CFDictionaryGetCount(out->config) : -1L);
				if (out->config != NULL && CFGetTypeID(out->config) == CFDictionaryGetTypeID()) {
					CFIndex n = CFDictionaryGetCount(out->config);
					const void **k = calloc((size_t)n + 1, sizeof(void *)), **v = calloc((size_t)n + 1, sizeof(void *));
					char list[190] = "";
					size_t u = 0;

					CFDictionaryGetKeysAndValues(out->config, k, v);
					for (CFIndex i = 0; i < n && u < sizeof(list) - 40; i++) {
						char kn[40] = "?";

						if (CFGetTypeID(k[i]) == CFStringGetTypeID()) {
							CFStringGetCString(k[i], kn, sizeof(kn), kCFStringEncodingUTF8);
						}
						u += (size_t)snprintf(list + u, sizeof(list) - u, " %s", kn);
					}
					say("deliver %s: keys%s", out->name, list);
					free(k);
					free(v);
				}
			}
			say("deliver %s: %dx%d pf %08x config %.150s", out->name, width, height, pf, text);
			if (strlen(text) > 150) {
				say("deliver ...: %.170s", text + 150);
			}
		}
	}
	/*
	 * The capture graph hands each output a buffer pool of its own (VideoOutputConfigurations' "BufferPool", with "RetainedBufferCount"): the pictures are
	 * made in buffers from that pool, whose attributes say their size and format; the older shape of the configuration (Width, Height, PixelFormat) is the
	 * fallback.
	 */
	{
		CFStringRef pk = CFSTR("BufferPool");
		CFTypeRef gp = out->config != NULL ? CFDictionaryGetValue(out->config, pk) : NULL;

		if (gp != NULL && CFGetTypeID(gp) == CVPixelBufferPoolGetTypeID()) {
			CFDictionaryRef attrs = CVPixelBufferPoolGetPixelBufferAttributes((CVPixelBufferPoolRef)gp);

			width = (int)get_i(attrs, "Width", 0);
			height = (int)get_i(attrs, "Height", 0);
			pf = (uint32_t)get_i(attrs, "PixelFormatType", PF_420F);
			if (width > 0 && height > 0 && CVPixelBufferPoolCreatePixelBuffer(NULL, (CVPixelBufferPoolRef)gp, &pb) == kCVReturnSuccess) {
				if (out->delivered == 0) {
					say("%s: the graph's own buffer pool: %dx%d %c%c%c%c", out->name, width, height, pf >> 24, pf >> 16 & 255, pf >> 8 & 255, pf & 255);
				}
				goto have_buffer;
			}
			pb = NULL;
			return;
		}
	}
	if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
		return;
	}
	pf = pf == PF_420V || pf == PF_BGRA ? pf : PF_420F;
	if ((pb = output_buffer(out, width, height, pf)) == NULL) {
		return;
	}
have_buffer:
	CVPixelBufferLockBaseAddress(pb, 0);

	struct render_job job = { isp, f, raw, pb, pf, (uint32_t)width, (uint32_t)height, 8 };

	/*
	 * (QOS_CLASS_DEFAULT, not USER_INTERACTIVE: the software ISP takes whatever the cores have, and above the default it left the window
	 * server's GL threads -- Mesa's, at the default -- waiting tens of ms to run: camgl2, pvr "done to waiter woken" 64 ms in the middle)
	 */
	uint64_t isp_began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

	dispatch_apply_f(job.pieces, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), &job, render_piece);
	out->isp_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - isp_began;
	if (pf != PF_BGRA && CAM_MARK) {
		/* a bright square in the corner of every picture handed out: a dark scene is then told from frames that are not shown (CAM_MARK=1, a diagnosis) */
		uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
		size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);

		if (CAM_MARK == 2) {
			/* the whole picture mid-grey: if the preview turns grey the frames are shown, and a black preview is a dark scene or nothing shown at all */
			uint8_t *c = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
			size_t cstride = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);

			for (int r = 0; r < height; r++) {
				memset(y + (size_t)r * stride, 170, (size_t)width);
			}
			for (int r = 0; r < height / 2; r++) {
				memset(c + (size_t)r * cstride, 128, (size_t)width);
			}
		} else {
			for (int r = 0; r < height / 6 && r < 300; r++) {
				memset(y + (size_t)r * stride, 235, (size_t)(width / 6 < 300 ? width / 6 : 300));
			}
		}
	}
	CVPixelBufferUnlockBaseAddress(pb, 0);
	CVBufferSetAttachment(pb, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_601_4, kCVAttachmentMode_ShouldPropagate);
	CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
	CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
	    kCVAttachmentMode_ShouldPropagate);
	if (out == &s->out[2]) {
		/* the StillImage output's: stream_add_attachments() tells them by this, not by size (the preview's PrimaryScaler can be 4032x3024 too) */
		CVBufferSetAttachment(pb, CFSTR("RedwoodStillImage"), kCFBooleanTrue, kCVAttachmentMode_ShouldNotPropagate);
	}

	CMTime pts = CMClockMakeHostTimeFromSystemUnits(f->time);

	uint64_t handler_began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

	((pixel_buffer_handler)out->handler)(pb, pts);
	out->handler_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - handler_began;
	if (out == &s->out[0] && out->delivered >= 100 && out->delivered < 3000) {
		say("cam: pic %u given %llu us", (unsigned)out->delivered, (unsigned long long)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000));
	}
	CVPixelBufferRelease(pb);
	if (out->delivered++ == 0) {
		say("%s: the first picture given (%dx%d)", out->name, width, height);
	}
}

/*
 * A photo without the capture graph's still pipeline (which stops in BWGNRNode and has no HEVC encoder here: docs/camera-x6826b.md, runs 579-600): when
 * Camera.app's shutter is tapped, our library inside the app (inject_media.c, only where etc/inject.conf names Camera) counts the tap in the notify state
 * of REDWOOD_STILL_ASK (1, 2, 3...) and posts it. The worker owes a still for every count it has not served (one a frame, so taps that come together are
 * not lost to the notification's coalescing); for each it copies the frame and goes on with the preview, and the copy is made a picture of the sensor's
 * size and written as a JPEG (ImageIO's own encoder) on a queue of its own, to REDWOOD_STILL_FILE with the tap's number in it. The count it has written is
 * then the notify state of REDWOOD_STILL_DONE, posted (written or not: the app says which), and the app gives the file to the photo library.
 */
#define REDWOOD_STILL_ASK       "redwood.camera.still"
#define REDWOOD_STILL_DONE      "redwood.camera.still.done"
#define REDWOOD_STILL_FILE      "/private/var/tmp/redwood-still-%llu.jpg"     /* the tap's number */
#define REDWOOD_STILL_ORIGINAL  "/private/var/redwood-still-original"   /* (inject_media.c: the shutter also asks the capture graph's own still) */

/* (CoreGraphics and ImageIO by name, when a photo is asked for: the library is not linked with them, and nothing else here wants them) */
static void *
still_symbol(const char *framework, const char *name)
{
	char path[160];

	snprintf(path, sizeof(path), "/System/Library/Frameworks/%s.framework/%s", framework, framework);
	(void)dlopen(path, RTLD_NOW);
	return dlsym(RTLD_DEFAULT, name);
}

/* a still waiting for its JPEG: the tap's number, when it was asked, the ISP's settings of that moment, and the frame (header and RAW) */
struct still_job {
	uint64_t        tap, asked;
	bool            front;
	struct isp      isp;
	uint8_t         frame[];
};

/* the count served (written or not), as REDWOOD_STILL_DONE's state: only ever from the still queue, so that it only goes up */
static void
still_done(uint64_t tap)
{
	static int done_token = -1;

	if (done_token < 0) {
		(void)notify_register_check(REDWOOD_STILL_DONE, &done_token);
	}
	(void)notify_set_state(done_token, tap);
	notify_post(REDWOOD_STILL_DONE);
}

/*
 * The stills' queue. USER_INITIATED, above the preview's ISP (DEFAULT): at UTILITY, with the preview's dispatch_apply and the compositor on all
 * four cores, the first still of each mediaserverd took ~20 s (combo2: taps 1 and 6, 21023 and 19200 ms) against 1.1-1.5 s with nothing else
 * running; the preview loses a second of pictures to a tap instead.
 */
static dispatch_queue_t
still_dq(void)
{
	static dispatch_once_t once;
	static dispatch_queue_t queue;

	dispatch_once(&once, ^{
		queue = dispatch_queue_create("redwood.camera.still", dispatch_queue_attr_make_with_qos_class(
		    dispatch_queue_attr_make_with_autorelease_frequency(DISPATCH_QUEUE_SERIAL, DISPATCH_AUTORELEASE_FREQUENCY_WORK_ITEM), QOS_CLASS_USER_INITIATED, 0));
	});
	return queue;
}

/* taps after `from` up to `to` that get no still: said (kept for the host) and served as such, after the stills queued before them */
static void
still_skip(uint64_t from, uint64_t to, const char *why)
{
	dispatch_async(still_dq(), ^{
		char line[200];

		if (to == from + 1) {
			snprintf(line, sizeof(line), "still: tap %llu: NO STILL: %s", to, why);
		} else {
			snprintf(line, sizeof(line), "still: taps %llu to %llu: NO STILL: %s", from + 1, to, why);
		}
		say("%s", line);
		(void)sysctlbyname("kern.redwood.host_keep", NULL, NULL, line, strlen(line));
		still_done(to);
	});
}

static void
still_save(struct still_job *job)
{
	uint64_t t[5] = { clock_gettime_nsec_np(CLOCK_UPTIME_RAW) };   /* began, symbols, ISP, CGImage, JPEG written */
	void *(*space_create)(CFStringRef) = still_symbol("CoreGraphics", "CGColorSpaceCreateWithName");
	void *(*context_create)(void *, size_t, size_t, size_t, size_t, void *, uint32_t) = still_symbol("CoreGraphics", "CGBitmapContextCreate");
	void *(*context_image)(void *) = still_symbol("CoreGraphics", "CGBitmapContextCreateImage");
	CFStringRef *srgb = still_symbol("CoreGraphics", "kCGColorSpaceSRGB");
	void *(*file_create)(CFURLRef, CFStringRef, size_t, CFDictionaryRef) = still_symbol("ImageIO", "CGImageDestinationCreateWithURL");
	void (*file_add)(void *, void *, CFDictionaryRef) = still_symbol("ImageIO", "CGImageDestinationAddImage");
	bool (*file_finalize)(void *) = still_symbol("ImageIO", "CGImageDestinationFinalize");
	CFStringRef *orient_key = still_symbol("ImageIO", "kCGImagePropertyOrientation");
	CFStringRef *width_key = still_symbol("ImageIO", "kCGImagePropertyPixelWidth");
	CFStringRef *height_key = still_symbol("ImageIO", "kCGImagePropertyPixelHeight");
	void *(*source_create)(CFURLRef, CFDictionaryRef) = still_symbol("ImageIO", "CGImageSourceCreateWithURL");
	CFDictionaryRef (*source_props)(void *, size_t, CFDictionaryRef) = still_symbol("ImageIO", "CGImageSourceCopyPropertiesAtIndex");
	/* EXIF orientation of the JPEG (6 = rotate 90 clockwise to show): the sensor's landscape frame is saved as is, no pixels turned */
	int orient = (int)(job->front ? bootarg_number("r1cam_front_still_orient", 6) : bootarg_number("r1cam_still_orient", 6));
	const struct redwood_camera_frame *f = (const void *)job->frame;
	size_t width = f->width, height = f->height, stride = width * 4;
	uint8_t *bgra = NULL;
	void *space = NULL, *context = NULL, *image = NULL, *file = NULL;
	char path[64], part[72], line[300];
	CFURLRef url;
	bool ok = false, have = space_create && context_create && context_image && srgb && file_create && file_add && file_finalize;

	t[1] = t[2] = t[3] = t[4] = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	snprintf(path, sizeof(path), REDWOOD_STILL_FILE, job->tap);
	snprintf(part, sizeof(part), "%s.part", path);      /* written under another name and renamed: the app never sees half a file */
	url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)part, (CFIndex)strlen(part), false);
	if (have) {
		bgra = malloc(stride * height);
		space = space_create(*srgb);
		/* 8 bits a part, blue first, the fourth byte not used: kCGImageAlphaNoneSkipFirst (6) | kCGBitmapByteOrder32Little (2 << 12) */
		context = bgra && space ? context_create(bgra, width, height, 8, stride, space, 6u | (2u << 12)) : NULL;
		if (context != NULL) {
			isp_render_bgra(&job->isp, f, job->frame + f->header_bytes, bgra, stride, (uint32_t)width, (uint32_t)height, 0, (uint32_t)height);
			t[2] = t[3] = t[4] = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
			image = context_image(context);
			t[3] = t[4] = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
		}
		if (image != NULL && (file = file_create(url, CFSTR("public.jpeg"), 1, NULL)) != NULL) {
			CFDictionaryRef props = NULL;
			CFNumberRef number = CFNumberCreate(NULL, kCFNumberIntType, &orient);

			if (orient_key != NULL) {
				props = CFDictionaryCreate(NULL, (const void **)orient_key, (const void **)&number, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			}
			file_add(file, image, props);
			if (props != NULL) {
				CFRelease(props);
			}
			CFRelease(number);
			ok = file_finalize(file) && rename(part, path) == 0;
			t[4] = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
		}
	}
	/* (the queue's wait: asked to began; the time "in" split as symbols (dlopen), ISP, CGImage, JPEG (ImageIO's create/add/finalize and the rename)) */
	snprintf(line, sizeof(line), "still: tap %llu: frame %llu, %zux%zu: %s in %llu ms (%llu ms after the tap reached the camera; waited %llu, symbols %llu, "
	    "ISP %llu, CGImage %llu, JPEG %llu ms), orientation %d%s", job->tap, f->sequence,
	    width, height, ok ? "WRITTEN" : !have ? "NO CoreGraphics / ImageIO" : !bgra ? "NO MEMORY" : !image ? "NO IMAGE" :
	    !file ? "NO JPEG DESTINATION (ImageIO)" : "THE JPEG WAS NOT WRITTEN", (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t[0]) / 1000000,
	    (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - job->asked) / 1000000, (t[0] - job->asked) / 1000000, (t[1] - t[0]) / 1000000,
	    (t[2] - t[1]) / 1000000, (t[3] - t[2]) / 1000000, (t[4] - t[3]) / 1000000, orient, job->tap == 0 ? ": the warm-up, no tap's" : "");
	say("%s", line);
	(void)sysctlbyname("kern.redwood.host_keep", NULL, NULL, line, strlen(line));
	for (int i = 0; i < 4; i++) {
		void *o[4] = { file, image, context, space };

		if (o[i]) {
			CFRelease(o[i]);
		}
	}
	if (ok && source_create && source_props && width_key && height_key && orient_key) {
		/* read the file back: what ImageIO says it is */
		CFURLRef final = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
		void *src = source_create(final, NULL);
		CFDictionaryRef p = src != NULL ? source_props(src, 0, NULL) : NULL;
		int w = 0, h = 0, o = 0;
		CFNumberRef n;

		if (p != NULL) {
			if ((n = CFDictionaryGetValue(p, *width_key)) != NULL) CFNumberGetValue(n, kCFNumberIntType, &w);
			if ((n = CFDictionaryGetValue(p, *height_key)) != NULL) CFNumberGetValue(n, kCFNumberIntType, &h);
			if ((n = CFDictionaryGetValue(p, *orient_key)) != NULL) CFNumberGetValue(n, kCFNumberIntType, &o);
			CFRelease(p);
		}
		if (src != NULL) {
			CFRelease(src);
		}
		CFRelease(final);
		snprintf(line, sizeof(line), "still: tap %llu: file says %dx%d orientation %d", job->tap, w, h, o);
		say("%s", line);
		(void)sysctlbyname("kern.redwood.host_keep", NULL, NULL, line, strlen(line));
	}
	CFRelease(url);
	free(bgra);
	if (!ok) {
		unlink(part);
	}
	if (job->tap == 0) {
		unlink(path);           /* (the warm-up's: nobody's photo, and no count to say) */
		return;
	}
	/* the count served, written or not (the app looks for the file and says which) */
	still_done(job->tap);
}

/*
 * The worker's part of a still: the frame copied (a few MB, a millisecond or two) and the rest left to a queue of its own (still_dq). Tap 0 is the
 * warm-up, a still of the stream's first frame in each mediaserverd, thrown away: whatever the first JPEG costs once (dlopen, ImageIO's first
 * encode) is paid before the shutter is tapped, and its line says how much.
 */
static void
still_queue(const struct isp *isp, const struct redwood_camera_frame *f, size_t bytes, uint64_t tap, bool front)
{
	struct still_job *job = malloc(sizeof(*job) + bytes);

	say("still: tap %llu asked: frame %llu copied%s", tap, f->sequence, job ? "" : ": NO MEMORY for the copy, this tap has no still");
	if (job == NULL) {
		if (tap != 0) {
			still_skip(tap - 1, tap, "no memory for the frame's copy");
		}
		return;
	}
	job->tap = tap;
	job->front = front;
	job->asked = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	job->isp = *isp;
	memcpy(job->frame, f, bytes);
	dispatch_async(still_dq(), ^{
		still_save(job);
		free(job);
	});
}

static uint64_t still_queued;   /* the last tap this mediaserverd queued a still for, or said it has none (across the stream's workers) */
static bool still_warmed;

static void *
worker(void *context)
{
	CMBaseObjectRef o = context;
	struct stream *s = stream_of(o);
	struct redwood_camera_frame head;
	struct isp isp;
	struct isp_stats st;
	uint8_t *buffer = NULL;
	size_t bytes = 0;
	int device = -1, failures = 0;

	pthread_setname_np("redwood-camera");
	for (int tries = 0; tries < 5 && s->running && (device = open(REDWOOD_CAMERA_DEVICE, O_RDWR)) < 0; tries++) {
		/* EIO is the kernel's "the camera has had a fault": its own lines (r1cam: / mtkcam: / camera:) say which. Said once, tried for 4 s more. */
		if (tries == 0) {
			say("%s does not open (%s)%s: trying for 4 s more", REDWOOD_CAMERA_DEVICE, strerror(errno),
			    errno == EIO ? ": the kernel's camera driver gave the camera up, see its own lines" : "");
		}
		sleep(1);
	}
	bool flip = bootarg_number("r1cam_flip", 0) == 1;

	say("worker: running, device fd %d", device);
	if (device < 0) {
		say("NO CAMERA: %s (%s): the preview stays black", REDWOOD_CAMERA_DEVICE, strerror(errno));
		return NULL;
	}
	int still_token = -1;
	uint64_t still_asked = 0, still_served = 0;       /* taps counted by the app (REDWOOD_STILL_ASK's state), and stills queued for them */
	bool graph_still = access(REDWOOD_STILL_ORIGINAL, F_OK) == 0;
	/*
	 * Boot-arg cam_fps=N: preview pictures at most N a second (0: every frame the worker gets to). A picture
	 * costs the ISP ~130 ms of processor time (fence3: mediaserverd 202% of a core for 15.5 a second) and
	 * the panel shows ~5-6 a second: the rest starve the window server on four A53s. The frames between are
	 * still read, measured for the exposure and taken for stills; only the ISP's picture is skipped.
	 * Its sandbox may not let it read the boot-args: then the default, 10.
	 */
	unsigned cam_fps = 10;
	uint64_t last_picture = 0;
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (at = strstr(args, "cam_fps=")) != NULL) {
			cam_fps = (unsigned)strtoul(at + 8, NULL, 10);
		}
		say("preview pictures: %s%u a second (boot-arg cam_fps)", cam_fps ? "at most " : "no limit, ", cam_fps);
	}

	say("the StillImage output: %s", graph_still ? "rendered (" REDWOOD_STILL_ORIGINAL " is there)" : "not rendered (the preview only)");
	(void)notify_register_check(REDWOOD_STILL_ASK, &still_token);
	{
		int first = 0;

		int done_token = -1;
		uint64_t done = 0, from;

		(void)notify_check(still_token, &first);        /* (the first check always says "changed") */
		(void)notify_get_state(still_token, &still_asked);
		/*
		 * Taps from before this stream are not this stream's, but each is told so: those neither served (REDWOOD_STILL_DONE) nor queued by this
		 * process. combo2: tap 5's still was owed by the mediaserverd that died (280.155) and the next one took its count for served, so the app
		 * heard of tap 5 only from tap 6's count, 49 s later, as "No such file".
		 */
		(void)notify_register_check(REDWOOD_STILL_DONE, &done_token);
		(void)notify_get_state(done_token, &done);
		(void)notify_cancel(done_token);
		from = done > still_queued ? done : still_queued;
		if (still_asked > from) {
			still_skip(from, still_asked, "asked before this stream began (mediaserverd ended, or the stream stopped, before its frame)");
		}
		still_queued = still_served = still_asked;
	}
	isp_init(&isp);
	bool skip = false;
	if (flip) {
		/* r1cam_flip=1: the kernel's motor turns to this stream's face before its next session (EBUSY while it moves) */
		struct redwood_camera_command c = { .command = REDWOOD_CAMERA_SENSOR, .a = s->front ? 2 : 1 };
		static int last_face = -1;      /* per process; r1cam_flip_ms (default 1000): a start on the other face waits this long, and is dropped if its stream stops first */

		if (last_face >= 0 && last_face != (s->front ? 1 : 0)) {
			long ms = bootarg_number("r1cam_flip_ms", 1000);

			for (long w = 0; w < ms && s->running; w += 50) {
				usleep(50000);
			}
		}
		skip = !s->running;
		if (!skip) {
			last_face = s->front ? 1 : 0;
		}
		for (int tries = 0; !skip && tries < 20 && write(device, &c, sizeof(c)) != (ssize_t)sizeof(c) && errno == EBUSY; tries++) {
			usleep(250000);
		}
		isp.rot180 = s->front && bootarg_number("r1cam_front_rot", 2) == 2;
		if (skip) {
			say("the %s camera not asked: its stream stopped within r1cam_flip_ms", s->front ? "front" : "back");
		} else {
			say("camera: face %d asked (%s) at %llu ms, rot180 %d", s->front ? 2 : 1, s->front ? "front" : "back",
			    (unsigned long long)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000), isp.rot180);
		}
	}
	if (isp_tuning_load(&isp)) {
		say("this camera's tuning file: its colour matrix%s", isp.lsc.on ? " and lens shading" : "");
	}
	while (s->running && !skip && failures < 5) {
		ssize_t n;

		if (buffer == NULL) {
			n = read(device, &head, sizeof(head));
			if (n != (ssize_t)sizeof(head)) {
				failures += n < 0 && errno == EAGAIN ? 0 : 1;
				if (n < 0 && errno == EIO) {
					say("the kernel's camera has had a fault: no frames");
					break;
				}
				continue;
			}
			bytes = sizeof(head) + (size_t)head.stride * head.height;
			buffer = malloc(bytes);
			say("frames of %ux%u from the kernel (header %u, stride %u)", head.width, head.height, head.header_bytes, head.stride);
			continue;
		}
		n = read(device, buffer, bytes);
		if (n != (ssize_t)bytes) {
			if (n < 0 && errno == EIO) {
				say("the kernel's camera has had a fault: no more frames");
				break;
			}
			failures += n < 0 && errno == EAGAIN ? 0 : 1;
			continue;
		}
		failures = 0;

		const struct redwood_camera_frame *f = (const void *)buffer;
		const uint8_t *raw = buffer + f->header_bytes;
		struct redwood_camera_command c;

		isp_measure(f, raw, &st);
		isp_awb(&isp, &st);
		if (isp_ae(&isp, f, &st, &c)) {
			(void)write(device, &c, sizeof(c));
		}
		s->frames++;
		{
			int changed = 0;

			if (still_token >= 0 && notify_check(still_token, &changed) == NOTIFY_STATUS_OK && changed) {
				(void)notify_get_state(still_token, &still_asked);
				if (still_asked < still_served) {
					still_served = still_asked;     /* (notifyd started again: its count with it) */
				}
			}
			if (!still_warmed) {
				still_warmed = true;
				still_queue(&isp, f, bytes, 0, s->front);                 /* (the warm-up) */
			}
			if (still_served < still_asked) {
				still_queue(&isp, f, bytes, ++still_served, s->front);    /* one a frame: every tap its own still */
				still_queued = still_served;
			}
		}
		{
			/* the mark is taken by the first still request (captureStillImageNowWithSettings:, probe_still_errors): what is new after it is what a photo needs */
			static bool did_a, did_b;
			static uint64_t armed_at;

			if (CAM_TRACE && seltab_marked && armed_at == 0) {
				armed_at = f->sequence;
			}
			if (CAM_TRACE && armed_at != 0 && !did_a && f->sequence >= armed_at + 60) {
				did_a = true;
				dump_new_selectors("4 s after the first still request");
			}
		}
		if (s->frames <= 3 || s->frames % 150 == 0) {
			say("frame %llu read: handlers %d/%d/%d, enabled %d/%d/%d, configs %d/%d/%d, delivering %d", f->sequence, s->out[0].handler != NULL,
			    OUTPUTS > 1 ? s->out[1].handler != NULL : 0, OUTPUTS > 2 ? s->out[2].handler != NULL : 0, s->out[0].enabled, OUTPUTS > 1 ? s->out[1].enabled : 0,
			    OUTPUTS > 2 ? s->out[2].enabled : 0, s->out[0].config != NULL, OUTPUTS > 1 ? s->out[1].config != NULL : 0, OUTPUTS > 2 ? s->out[2].config != NULL : 0,
			    s->delivering);
		}
		bool picture = true;

		if (cam_fps != 0) {
			uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

			/* (a frame's worth early still counts, so 30 a second divides evenly: 10 is every third frame) */
			picture = now - last_picture + 1000000000ull / 60 >= 1000000000ull / cam_fps;
			last_picture = picture ? now : last_picture;
		}
		pthread_mutex_lock(&s->lock);
		for (int i = 0; picture && i < OUTPUTS; i++) {
			struct output *out = &s->out[i];

			/*
			 * The StillImage output (index 2) is the sensor's size made 4032x3024: rendered every frame, it took the worker ~0.65 s a frame, so the
			 * preview got ~1 picture a second (camgl2: "the frames stopped (49)" after 48 s; shutter2: 27 in 23 s) and every core was busy at
			 * USER_INTERACTIVE. Nothing takes it now that the shutter does not ask the graph's still (inject_media.c, REDWOOD_STILL_ORIGINAL; our own
			 * photo is still_save()); rendered again only where that file asks for the graph's still.
			 */
			if (i == 2 && !graph_still) {
				continue;
			}
			/*
			 * Only between the graph's START and STOP (s->delivering): the handler block reaches its BWFigVideoCaptureStream's queue through an
			 * unretained __block pointer, so after a STOP the graph that set it may be gone. combo2 280.155: SIGSEGV in dispatch_async_f
			 * (18db2a900) from the handler's invoke (CMCapture 1a1ea6c24, block made at 1a1f185fc) on a queue already freed, a frame read with
			 * "enabled 1/0/1, delivering 0" (the last session's handler and VideoOutputsEnabled, while the next graph's reservations thrashed).
			 */
			if (s->delivering && out->handler != NULL && out->config != NULL && (out->enabled || i == 0)) {
				uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

				deliver(s, out, &isp, f, raw);
				out->pictures++;
				out->render_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began;
			}
		}
		pthread_mutex_unlock(&s->lock);
		{
			/* every ten seconds: how many pictures each output got, and what one cost (the preview's rate is this, not the kernel's frames) */
			static uint64_t since, since_frame, since_cpu_us;
			uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), cpu_us;
			struct rusage ru;

			/* (the process's processor time: mediaserverd's, all threads -- the ISP's pieces and the graph's own work on the pictures) */
			getrusage(RUSAGE_SELF, &ru);
			cpu_us = (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 + (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
			if (since == 0) {
				since = now;
				since_frame = f->sequence;
				since_cpu_us = cpu_us;
			} else if (now - since >= 10000000000ull) {
				unsigned n = s->out[0].pictures ? s->out[0].pictures : 1;

				say("10 s: pictures given: %s %u (%llu ms each), %s %u (%llu ms each); kernel frame %llu", s->out[0].name, s->out[0].pictures,
				    s->out[0].pictures ? s->out[0].render_ns / s->out[0].pictures / 1000000 : 0, s->out[2].name, s->out[2].pictures,
				    s->out[2].pictures ? s->out[2].render_ns / s->out[2].pictures / 1000000 : 0, f->sequence);
				/* where a preview picture's time goes, and whether the sensor or the worker sets the rate */
				say("10 s: %s: ISP %llu.%llu ms, the graph's handler %llu.%llu ms a picture; the sensor %llu.%llu frames a second; "
				    "mediaserverd's processor time %llu%% of one core", s->out[0].name, s->out[0].isp_ns / n / 1000000,
				    s->out[0].isp_ns / n / 100000 % 10, s->out[0].handler_ns / n / 1000000, s->out[0].handler_ns / n / 100000 % 10,
				    (f->sequence - since_frame) * 10000000000ull / (now - since) / 10, (f->sequence - since_frame) * 10000000000ull / (now - since) % 10,
				    (cpu_us - since_cpu_us) * 100 / ((now - since) / 1000));
				for (int i = 0; i < OUTPUTS; i++) {
					s->out[i].pictures = 0;
					s->out[i].render_ns = s->out[i].isp_ns = s->out[i].handler_ns = 0;
				}
				since = now;
				since_frame = f->sequence;
				since_cpu_us = cpu_us;
			}
		}
		if (s->frames == 1 || s->frames % 300 == 0) {
			say("frame %llu: exposure %.1f ms, gain %.1fx, digital %.2fx, green %.0f above black", f->sequence,
			    f->exposure_lines * (double)f->line_ns / 1e6, (double)f->gain / f->gain_unity, isp.digital, st.mean[1]);
		}
	}
	if (still_token >= 0 && notify_get_state(still_token, &still_asked) == NOTIFY_STATUS_OK && still_asked > still_served) {
		still_skip(still_served, still_asked, "the stream stopped before its frame");
		still_queued = still_served = still_asked;
	}
	free(buffer);
	close(device);
	say("the frames stopped (%llu)", s->frames);
	if (CAM_TRACE && seltab_marked) {
		dump_new_selectors("frames stopped");           /* (a stop after the shutter's tap: what the tap started) */
	}
	return NULL;
}

static OSStatus
stream_start_worker(CMBaseObjectRef o)
{
	say_bt("stream_start");
	struct stream *s = stream_of(o);

	{
		static unsigned calls;

		if (calls++ < 5) {
			say("stream_start called (running %d)", s->running);
		}
	}
	if (s->running) {
		return 0;
	}
	s->running = true;
	s->frames = 0;
	say("stream: start (format %d)", s->format_index);
	if (pthread_create(&s->worker, NULL, worker, o) != 0) {
		s->running = false;
		return ERR_UNSUPPORTED;
	}
	s->worker_started = true;
	return 0;
}

static OSStatus
stream_start(CMBaseObjectRef o)
{
	static unsigned calls;

	if (calls++ < 4) {
		say("stream: START from the capture graph");
	}
	stream_of(o)->delivering = true;
	return stream_start_worker(o);
}

static OSStatus
stream_stop(CMBaseObjectRef o)
{
	say_bt("stream_stop");
	struct stream *s = stream_of(o);

	say("stream: stop");
	s->delivering = false;
	s->running = false;
	if (s->worker_started) {
		pthread_join(s->worker, NULL);
		s->worker_started = false;
	}
	return 0;
}

static OSStatus
stream_add_attachments(CMBaseObjectRef o, CMSampleBufferRef sbuf, CFDictionaryRef options)
{
	static bool said;
	CFDictionaryRef have;
	CFMutableDictionaryRef metadata;

	(void)o;
	if (!said) {
		said = true;
		say("stream: AddAttachmentsToSampleBuffer (options %s)", options ? "given" : "none");
	}
	/*
	 * What the camera's own stream puts on every frame: BWStillImageTimeMachineFrameCoordinatorNode takes the "MetadataDictionary" attachment's "PortType" to find the
	 * output of that camera, and a frame without it goes to no output at all (-[nil emitSampleBuffer:], run 595: the still photo's frame vanished there). The rest
	 * of the real stream's metadata (exposure, gain, ...) is not made up here.
	 */
	if (sbuf == NULL) {
		return 0;
	}
	{
		/* only the full-size pictures (the StillImage output's): the preview's frames crashed the graph with a half-made dictionary (run 598, in
		 * -[BWFigVideoCaptureDevice captureStream:willEmitVideoSampleBuffer:drivesCameraControls:]: CFDictionaryGetValue of nothing). Told by
		 * deliver()'s tag, not by width: g1, a 4032x3024 PrimaryScaler preview (with SecondaryScaler) given this dictionary crashed the same way --
		 * a frame driving camera controls with a sample buffer "MetadataDictionary" goes on (CMCapture 0x1a1ebe5ec) to 0x1a1ee0a84, which reads
		 * PortType from the pixel buffer's own "MetadataDictionary" (none: CFDictionaryGetValue(NULL)) */
		CVImageBufferRef image = CMSampleBufferGetImageBuffer(sbuf);

		if (image == NULL || CVBufferGetAttachment(image, CFSTR("RedwoodStillImage"), NULL) == NULL) {
			return 0;
		}
	}
	have = CMGetAttachment(sbuf, CFSTR("MetadataDictionary"), NULL);
	{
		static unsigned calls, had;

		calls++;
		had += have != NULL;
		if (calls <= 2 || calls % 100 == 0) {
			keep_for_host_fmt("attach: AddAttachmentsToSampleBuffer call %u, a MetadataDictionary there already in %u", calls, had);
		}
	}
	metadata = have ? CFDictionaryCreateMutableCopy(NULL, 0, have) : CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (metadata != NULL) {
		if (!CFDictionaryContainsKey(metadata, CFSTR("PortType"))) {
			CFDictionarySetValue(metadata, CFSTR("PortType"), port_back);
		}
		CMSetAttachment(sbuf, CFSTR("MetadataDictionary"), metadata, kCMAttachmentMode_ShouldPropagate);
		CFRelease(metadata);
	}
	return 0;
}

static OSStatus
stream_invalidate(CMBaseObjectRef o)
{
	say_bt("stream_invalidate");
	(void)stream_stop(o);
	return 0;
}

static void
stream_finalize(CMBaseObjectRef o)
{
	say_bt("stream_finalize");
	struct stream *s = stream_of(o);

	(void)stream_stop(o);
	for (int i = 0; i < OUTPUTS; i++) {
		if (s->out[i].config) {
			CFRelease(s->out[i].config);
		}
		if (s->out[i].handler) {
			CFRelease(s->out[i].handler);
		}
		if (s->out[i].pool) {
			CVPixelBufferPoolRelease(s->out[i].pool);
		}
	}
	if (s->formats) {
		CFRelease(s->formats);
	}
	if (s->supported) {
		CFRelease(s->supported);
	}
	if (s->torch >= 0) {
		close(s->torch);
	}
}

static CFStringRef
stream_description(CMBaseObjectRef o)
{
	(void)o;
	return CFRetain(CFSTR("<RedwoodCaptureStream: the wide camera>"));
}

static const struct base_class stream_base = {
	1, sizeof(struct stream), NULL, stream_invalidate, stream_finalize, stream_description, stream_copy_property, stream_set_property,
	NULL, NULL, NULL
};
static const struct stream_class stream_derived = { 2, stream_start, stream_stop, stream_add_attachments };
static const struct vtable stream_vtable = { NULL, &stream_base, &stream_derived };

static CMBaseObjectRef
stream_create(CFAllocatorRef allocator, bool front)
{
	CMBaseObjectRef o = NULL;
	struct stream *s;

	if (derived_create(allocator, &stream_vtable, stream_class_id(), &o) != 0 || o == NULL) {
		say("the stream cannot be made");
		return NULL;
	}
	s = stream_of(o);
	memset(s, 0, sizeof(*s));
	pthread_mutex_init(&s->lock, NULL);
	s->front = front;
	s->torch = -1;
	s->min_fps = 2;
	s->max_fps = 30;
	for (int i = 0; i < OUTPUTS; i++) {
		s->out[i].name = output_names[i];
	}

	/* "Photo" and "Video-60" of D22/AVCaptureSession.plist's back camera */
	CFDictionaryRef f[2] = { format(4032, 3024, 30, false), format(4224, 2376, 60, false) };

	if (front) {
		/* ... and "Photo" and "Video-30" of its position 2 entry */
		CFRelease(f[0]);
		CFRelease(f[1]);
		f[0] = format(3088, 2320, 30, false);
		f[1] = format(3840, 2160, 30, false);
	}

	s->formats = CFArrayCreate(NULL, (const void **)f, 2, &kCFTypeArrayCallBacks);
	s->supported = supported(stream_props, sizeof(stream_props) / sizeof(stream_props[0]));
	CFRelease(f[0]);
	CFRelease(f[1]);
	return o;
}

/* ---- the device ------------------------------------------------------------------------------------- */

struct device {
	CFArrayRef      streams;
	CFDictionaryRef supported;
};

static OSStatus
device_copy_property(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef allocator, void *out)
{
	struct device *d = derived_storage(o);
	CFTypeRef value = NULL;

	(void)allocator;
	trace_key("device get", key);
	if (key_is(key, "StreamArray")) {
		value = CFRetain(d->streams);
	} else if (key_is(key, "Clock")) {
		value = CFRetain(CMClockGetHostTimeClock());
	} else if (key_is(key, "SupportedPropertiesDictionary")) {
		value = CFRetain(d->supported);
	} else if (key_is(key, "SupportedSynchronizedStreamsGroups")) {
		value = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
	} else {
		say_once("device: no answer for", key, NULL);
		return ERR_NOT_AVAILABLE;
	}
	*(CFTypeRef *)out = value;
	return 0;
}

static OSStatus
device_set_property(CMBaseObjectRef o, CFStringRef key, CFTypeRef value)
{
	(void)o;
	trace_key("device set", key);
	say_once("device: set (not used)", key, value);
	return 0;
}

static unsigned requests, relinquishes;

static OSStatus
device_request_streams(CMBaseObjectRef o, CFArrayRef streams, CFDictionaryRef options)
{
	say_bt("device_request_streams");
	(void)o;
	if (requests < 5) {
		say("device_request_streams called (%u)", requests);
	}
	if (requests++ < 3) {
		say("device: streams asked for (%ld)%s", streams ? (long)CFArrayGetCount(streams) : 0L, options ? ", with options" : "");
		if (options != NULL) {
			say_once("device: the request's options", CFSTR("RequestStreams"), options);
		}
	}
	/*
	 * Requesting a stream is this device's cue to begin producing: mediaserverd's capture graph
	 * configures the stream (it installs the PrimaryScaler PixelBufferHandler) and then reserves the
	 * streams through here, but it never calls the FigCaptureStream start method, so start the stream's
	 * frame worker from the reservation. stream_start is idempotent (it no-ops when already running),
	 * and relinquish does not stop the worker, so the reservation thrash does not tear it down.
	 */
	if (streams != NULL) {
		CFIndex n = CFArrayGetCount(streams);

		for (CFIndex i = 0; i < n; i++) {
			CMBaseObjectRef st = (CMBaseObjectRef)CFArrayGetValueAtIndex(streams, i);

			if (st != NULL) {
				(void)stream_start_worker(st);
			}
		}
	}
	return 0;
}

static OSStatus
device_relinquish_streams(CMBaseObjectRef o, CFArrayRef streams, CFDictionaryRef options)
{
	say_bt("device_relinquish_streams");
	{
		static unsigned dumped;

		if (dumped++ == 150) {
			dump_msgs("before relinquish", 130);
		}
	}
	(void)o;
	(void)options;
	if (relinquishes++ < 3) {
		say("device: streams given back (%ld)", streams ? (long)CFArrayGetCount(streams) : 0L);
	} else if (relinquishes % 1000 == 0) {
		say("device: streams given back %u times", relinquishes);
	}
	return 0;
}

static OSStatus
device_invalidate(CMBaseObjectRef o)
{
	say_bt("device_invalidate");
	(void)o;
	return 0;
}

static void
device_finalize(CMBaseObjectRef o)
{
	say_bt("device_finalize");
	struct device *d = derived_storage(o);

	if (d->streams) {
		CFRelease(d->streams);
	}
	if (d->supported) {
		CFRelease(d->supported);
	}
}

static CFStringRef
device_description(CMBaseObjectRef o)
{
	(void)o;
	return CFRetain(CFSTR("<RedwoodCaptureDevice: the POCO X5 Pro's wide camera>"));
}

static const struct base_class device_base = {
	1, sizeof(struct device), NULL, device_invalidate, device_finalize, device_description, device_copy_property, device_set_property,
	NULL, NULL, NULL
};
/*
 * The processing session (FigCaptureISPProcessingSession): the capture graph asks the device for one (slot 1 of the device's class: device, allocator,
 * a configuration dictionary, where to put it) -- without tccd running it never got this far; with it, it asks again and again until it is given one.
 * A session is a CoreMedia derived object of class FigCaptureISPProcessingSessionGetClassID whose derived table is { version, prepare(session, handler),
 * process(session, buffer, refcon, outputDescriptors, numOutputDescriptors, parameters), flush(session) } (BWFigCaptureISPProcessingSession's methods
 * call these at +0x8, +0x10, +0x18) and whose base class answers its properties. Here: it says what it is asked and takes everything; the pictures come
 * through the stream, as before.
 */
struct session {
	CFDictionaryRef config;
	void            *handler;
};

static struct session *
session_of(CMBaseObjectRef o)
{
	return derived_storage(o);
}

static CFDictionaryRef
session_supported(void)
{
	/* (the keys are the plain strings of kFigCaptureISPProcessingSessionProperty_*, which CMCapture does not export) */
	static const struct { const char *name, *type, *access; } props[] = {
		{ "SupportedOutputs", "Collection", "ReadOnly" },
		{ "InputPixelBufferAttributes", "Collection", "ReadOnly" },
		{ "OutputPixelBufferAttributes", "Collection", "ReadOnly" },
		{ "DeferAdditionOfAttachments", "Boolean", "ReadWrite" },
		{ "DefaultProcessingParameters", "Collection", "ReadOnly" },
		{ "OutputHandler", "Collection", "WriteOnly" },
	};

	return supported((const struct prop *)props, sizeof(props) / sizeof(props[0]));
}

static OSStatus
session_copy_property(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef allocator, void *out)
{
	CFTypeRef value = NULL;

	(void)o;
	(void)allocator;
	trace_key("session get", key);
	if (key_is(key, "SupportedPropertiesDictionary")) {
		static unsigned calls;

		if (++calls == 100) {
			dump_selector_counts("session created");
		}
		value = session_supported();
	} else if (key_is(key, "SupportedOutputs")) {
		CFStringRef ids[2] = { CFSTR("PrimaryScaler"), CFSTR("SecondaryScaler") };

		value = CFArrayCreate(NULL, (const void **)ids, 2, &kCFTypeArrayCallBacks);
	} else if (key_is(key, "InputPixelBufferAttributes") || key_is(key, "OutputPixelBufferAttributes") || key_is(key, "DefaultProcessingParameters")) {
		value = new_dict();
	} else if (key_is(key, "DeferAdditionOfAttachments")) {
		value = CFRetain(kCFBooleanFalse);
	} else {
		return ERR_NOT_AVAILABLE;
	}
	*(CFTypeRef *)out = value;
	return 0;
}

static OSStatus
session_set_property(CMBaseObjectRef o, CFStringRef key, CFTypeRef value)
{
	(void)o;
	trace_key("session set", key);
	say_once("session: set", key, value);
	return 0;
}

static OSStatus
session_invalidate(CMBaseObjectRef o)
{
	static unsigned calls;

	(void)o;
	if (calls++ < 4) {
		say_bt("session_invalidate");
		say("session: invalidate");
	}
	if (calls == 100) {
		dump_selector_counts("session invalidated");
	}
	return 0;
}

static void
session_finalize(CMBaseObjectRef o)
{
	struct session *s = session_of(o);

	{
		static unsigned calls;

		if (calls++ < 4) {
			say("session: finalize");
		}
	}
	if (s->config) {
		CFRelease(s->config);
	}
}

static CFStringRef
session_description(CMBaseObjectRef o)
{
	(void)o;
	return CFRetain(CFSTR("<RedwoodISPProcessingSession>"));
}

static OSStatus
session_prepare(CMBaseObjectRef o, void *handler)
{
	struct session *s = session_of(o);

	say("session: prepare (handler %p)", handler);
	s->handler = handler;
	return 0;
}

static OSStatus
session_process(CMBaseObjectRef o, void *buffer, void *refcon, void *descriptors, int count, void *parameters)
{
	(void)o;
	say("session: process (buffer %p, refcon %p, descriptors %p x %d, parameters %p)", buffer, refcon, descriptors, count, parameters);
	return ERR_UNSUPPORTED;
}

static OSStatus
session_flush(CMBaseObjectRef o)
{
	(void)o;
	say("session: flush");
	return 0;
}

struct session_class {
	uint64_t        version;
	OSStatus        (*prepare)(CMBaseObjectRef, void *);
	OSStatus        (*process)(CMBaseObjectRef, void *, void *, void *, int, void *);
	OSStatus        (*flush)(CMBaseObjectRef);
};

static const struct base_class session_base = {
	1, sizeof(struct session), NULL, session_invalidate, session_finalize, session_description, session_copy_property, session_set_property,
	NULL, NULL, NULL
};
static const struct session_class session_derived = { 1, session_prepare, session_process, session_flush };
static const struct vtable session_vtable = { NULL, &session_base, &session_derived };

static OSStatus
device_processing_session_create(CMBaseObjectRef device, CFAllocatorRef allocator, CFDictionaryRef config, CMBaseObjectRef *out)
{
	CMBaseObjectRef o = NULL;
	static unsigned said;

	(void)device;
	if (said++ < 2) {
		CFStringRef d = config != NULL ? CFCopyDescription(config) : NULL;
		char text[600] = "(none)";

		if (d != NULL) {
			CFStringGetCString(d, text, sizeof(text), kCFStringEncodingUTF8);
			CFRelease(d);
			for (char *c = text; *c; c++) {
				*c = *c == '\n' ? ' ' : *c;
			}
		}
		say("processing_session_create: configuration %.170s", text);
		if (strlen(text) > 170) {
			say("processing_session_create: ... %.170s", text + 170);
		}
		if (strlen(text) > 340) {
			say("processing_session_create: ... %.170s", text + 340);
		}
		say_bt("processing_session_create");
	}
	if (session_class_id == NULL || out == NULL || derived_create(allocator, &session_vtable, session_class_id(), &o) != 0 || o == NULL) {
		say("processing_session_create: cannot be made");
		return ERR_UNSUPPORTED;
	}
	memset(session_of(o), 0, sizeof(struct session));
	if (config != NULL) {
		session_of(o)->config = CFRetain(config);
	}
	*out = o;
	return 0;
}

static OSStatus
device_pearl_calibration(void *a, void *b, void *c, void *d)
{
	say_bt("pearl_calibration");
	say("pearl_calibration(%p, %p, %p, %p)", a, b, c, d);
	return ERR_UNSUPPORTED;
}

static const struct device_class device_derived = { 3, (void *)device_processing_session_create, device_request_streams, device_relinquish_streams, device_pearl_calibration };
static const struct vtable device_vtable = { NULL, &device_base, &device_derived };

/* The vendor's device create function (as H10ISPCaptureDeviceCreate is called). */
OSStatus redwood_camera_device_create(CFAllocatorRef allocator, CFDictionaryRef options, CMBaseObjectRef *out);

/*
 * What the still pipeline's steps answer (run 584: Camera.app is told -16800, -11819, -16409 by the shutter): the photo compression session's calls and the capture
 * device's end-of-capture callback, said to the host with their results.
 */
typedef struct { int64_t value; int32_t timescale; uint32_t flags; int64_t epoch; } still_time;

static void
describe_arg(const char *what, id object)
{
	char line[400];
	id text = object ? ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("description")) : NULL;
	const char *chars = text ? ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String")) : "nil";

	snprintf(line, sizeof(line), "still: %s: %.330s", what, chars);
	keep_for_host(line);
}

static void
wrap_result3(Class cls, const char *selector)
{
	SEL sel = sel_registerName(selector);
	Method m = cls ? class_getInstanceMethod(cls, sel) : NULL;
	IMP orig;
	unsigned n;
	char tag[96];

	if (m == NULL) {
		snprintf(tag, sizeof(tag), "still: no %s", selector);
		keep_for_host(tag);
		return;
	}
	orig = method_getImplementation(m);
	n = method_getNumberOfArguments(m);
	snprintf(tag, sizeof(tag), "still: %s %s", class_getName(cls), selector);
	char *label = strdup(tag);

	if (n == 2) {
		method_setImplementation(m, imp_implementationWithBlock((id)^long(id me) {
			long r = ((long (*)(id, SEL))orig)(me, sel);
			char line[160];

			snprintf(line, sizeof(line), "%s -> %ld", label, r);
			keep_for_host(line);
			return r;
		}));
	} else if (n == 4) {
		method_setImplementation(m, imp_implementationWithBlock((id)^long(id me, id a, id b) {
			long r;
			char line[160];

			describe_arg("open options", a);
			r = ((long (*)(id, SEL, id, id))orig)(me, sel, a, b);
			snprintf(line, sizeof(line), "%s -> %ld", label, r);
			keep_for_host(line);
			return r;
		}));
	} else if (n == 5) {
		method_setImplementation(m, imp_implementationWithBlock((id)^long(id me, id a, id b, id c) {
			long r;
			char line[160];

			describe_arg("addMainImage image", a);
			describe_arg("addMainImage options", c);
			r = ((long (*)(id, SEL, id, id, id))orig)(me, sel, a, b, c);
			snprintf(line, sizeof(line), "%s -> %ld", label, r);
			keep_for_host(line);
			return r;
		}));
	}
}

/* the still settings' capture type and flags (BWStillImageTimeMachineFrameCoordinatorNode -renderSampleBuffer:forInput: holds the frame when captureFlags bit 13 is set) */
static IMP orig_stream_settings_init;

static id
logged_stream_settings_init(id me, SEL sel, id port, long type, long flags, long frames, id bracket, id valid)
{
	char line[200];

	snprintf(line, sizeof(line), "still: BWStillImageCaptureStreamSettings captureType %ld captureFlags %#lx timeMachineFrameCount %ld", type, flags, frames);
	keep_for_host(line);
	return ((id (*)(id, SEL, id, long, long, long, id, id))orig_stream_settings_init)(me, sel, port, type, flags, frames, bracket, valid);
}

static void
probe_still_errors(void)
{
	static bool done;
	Class photo = objc_getClass("BWCMPhotoCompressionSession"), device = objc_getClass("BWFigVideoCaptureDevice");
	Method m;

	if (done) {
		return;
	}
	done = true;
	m = device ? class_getInstanceMethod(device, sel_registerName("captureStillImageNowWithSettings:captureSettings:")) : NULL;
	if (m != NULL) {
		IMP first = method_getImplementation(m);

		method_setImplementation(m, imp_implementationWithBlock((id)^long(id me, id a, id b) {
			if (CAM_TRACE && !seltab_marked) {
				seltab_marked = true;
				seltab_mark();
			}
			return ((long (*)(id, SEL, id, id))first)(me, sel_registerName("captureStillImageNowWithSettings:captureSettings:"), a, b);
		}));
	}
	m = objc_getClass("BWStillImageCaptureStreamSettings") ? class_getInstanceMethod(objc_getClass("BWStillImageCaptureStreamSettings"),
	    sel_registerName("initWithPortType:captureType:captureFlags:timeMachineFrameCount:bracketSettings:validBracketedCaptureSequenceNumbers:")) : NULL;
	if (m != NULL && method_getNumberOfArguments(m) == 8) {
		orig_stream_settings_init = method_setImplementation(m, (IMP)logged_stream_settings_init);
	}
	wrap_result3(photo, "openContainerWithOptions:settingsID:");
	wrap_result3(photo, "addMainImage:metadata:options:");
	wrap_result3(photo, "closeContainer");
	m = device ? class_getInstanceMethod(device, sel_registerName("captureStream:didCompleteStillImageCaptureWithPTS:error:")) : NULL;
	if (m != NULL) {
		IMP orig = method_getImplementation(m);

		method_setImplementation(m, imp_implementationWithBlock((id)^(id me, id stream, still_time pts, id error) {
			char line[400];
			id text = error ? ((id (*)(id, SEL))objc_msgSend)(error, sel_registerName("description")) : NULL;
			const char *chars = text ? ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String")) : "none";

			snprintf(line, sizeof(line), "still: didCompleteStillImageCaptureWithPTS error: %.330s", chars);
			keep_for_host(line);
			((void (*)(id, SEL, id, still_time, id))orig)(me, sel_registerName("captureStream:didCompleteStillImageCaptureWithPTS:error:"), stream, pts, error);
		}));
	}
}

OSStatus
redwood_camera_device_create(CFAllocatorRef allocator, CFDictionaryRef options, CMBaseObjectRef *out)
{
	probe_still_errors();
	say_bt("device_create");
	if (CAM_TRACE) {
		install_msg_hook();
		install_graph_hooks();
	}
	install_crash_handler();
	static bool found, usable;
	CMBaseObjectRef device = NULL, stream, front = NULL;
	struct device *d;

	(void)options;
	if (!found) {
		found = true;
		usable = find_functions();
	}
	if (!usable || out == NULL) {
		return ERR_UNSUPPORTED;
	}
	if ((stream = stream_create(allocator, false)) == NULL) {
		return ERR_UNSUPPORTED;
	}
	if (bootarg_number("r1cam_flip", 0) == 1 && port_front != NULL) {
		front = stream_create(allocator, true);
	}
	if (derived_create(allocator, &device_vtable, device_class_id(), &device) != 0 || device == NULL) {
		CFRelease(stream);
		say("the device cannot be made");
		return ERR_UNSUPPORTED;
	}
	d = derived_storage(device);
	memset(d, 0, sizeof(*d));
	{
		CFTypeRef both[2] = { stream, front };

		d->streams = CFArrayCreate(NULL, (const void **)both, front != NULL ? 2 : 1, &kCFTypeArrayCallBacks);
	}
	d->supported = supported(device_props, sizeof(device_props) / sizeof(device_props[0]));
	CFRelease(stream);
	if (front != NULL) {
		CFRelease(front);
	}
	*out = device;
	say("the device made: %s", front != NULL ? "two streams, the back camera and the front camera (r1cam_flip=1)" : "one stream, the back camera");
	return 0;
}

#pragma clang attribute pop
