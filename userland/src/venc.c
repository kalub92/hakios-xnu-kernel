/*
 * venc.c: VideoToolbox video encoders registered at run time (docs/video-recording-spec-3.md A.4, C.1). Compiled into libredwood-media.dylib
 * (registered inside mediaserverd, where compression sessions really run, behind the boot-arg venc=N) and into video_test (in-process tests).
 *   venc_register(1)  the null encoder: 'avc1', takes everything, emits dropped frames (proves selection)
 *   venc_register(2)  VideoProcessing's H264VideoEncoder_CreateInstance as 'avc1' (Apple's FaceTime software encoder), logged by a shim
 *   venc_register(3)  VCPHEVC's HEVCVideoEncoder_CreateInstance as 'hvc1', logged by a shim
 *   venc_register(4)  venc_h264.c's encoder (minih264e), when linked in
 * Every encoder is registered with VTRating 1000 and the ID com.redwood.videoencoder.<kind>. Lines go to kern.redwood.host_keep.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "venc.h"

void
venc_log(const char *fmt, ...)
{
	char text[300], line[330];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	snprintf(line, sizeof(line), "venc[%d]: %s", getpid(), text);
	(void)sysctlbyname("kern.redwood.host_keep", NULL, NULL, line, strnlen(line, sizeof(line)));
	(void)sysctlbyname("kern.redwood.say", NULL, NULL, line, strnlen(line, sizeof(line)));
}

unsigned venc_factory_calls;
OSStatus (*venc_set_pba)(void *, CFDictionaryRef);
OSStatus (*venc_emit)(void *, void *, OSStatus, VTEncodeInfoFlags, CMSampleBufferRef);

static void
cc4(unsigned code, char *out)
{
	for (int i = 0; i < 4; i++) {
		unsigned char c = (code >> (24 - 8 * i)) & 0xff;

		out[i] = c >= 32 && c < 127 ? (char)c : '?';
	}
	out[4] = 0;
}

/* ---- the null encoder ---- */
static void *g_null_session;
static unsigned g_null_frames;

static OSStatus
n_copy_property(BaseRef o, CFStringRef key, CFAllocatorRef a, void *out)
{
	char k[100] = "?";
	static unsigned said;

	(void)o;
	(void)a;
	CFStringGetCString(key, k, sizeof(k), kCFStringEncodingUTF8);
	if (said++ < 40)
		venc_log("null: copyProperty %s", k);
	if (strstr(k, "Supported") != NULL) {
		*(CFTypeRef *)out = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		return 0;
	}
	if (strstr(k, "UsingHardwareAccelerated") != NULL) {
		*(CFTypeRef *)out = kCFBooleanFalse;
		return 0;
	}
	return -12900;
}

static OSStatus
n_set_property(BaseRef o, CFStringRef key, CFTypeRef v)
{
	char k[100] = "?";
	static unsigned said;

	(void)o;
	(void)v;
	CFStringGetCString(key, k, sizeof(k), kCFStringEncodingUTF8);
	if (said++ < 40)
		venc_log("null: setProperty %s", k);
	return 0;
}

static OSStatus n_invalidate(BaseRef o) { (void)o; return 0; }

/* (enc, session, dims, x3): dims = width in the low 32 bits, height in the high */
static uint64_t
n_start(uint64_t self, uint64_t session, uint64_t dims, uint64_t x3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
	int pf[2] = { kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange };
	CFNumberRef n[2] = { CFNumberCreate(NULL, kCFNumberIntType, &pf[0]), CFNumberCreate(NULL, kCFNumberIntType, &pf[1]) };
	CFArrayRef arr = CFArrayCreate(NULL, (const void **)n, 2, &kCFTypeArrayCallBacks);
	CFDictionaryRef d = CFDictionaryCreate(NULL, (const void **)&kCVPixelBufferPixelFormatTypeKey, (const void **)&arr, 1, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	OSStatus st = -1;

	(void)self; (void)x3; (void)a4; (void)a5; (void)a6; (void)a7;
	venc_log("null: StartSession %ux%u", (unsigned)(uint32_t)dims, (unsigned)(dims >> 32));
	g_null_session = (void *)(uintptr_t)session;
	if (venc_set_pba)
		st = venc_set_pba(g_null_session, d);
	venc_log("null: SetPixelBufferAttributes -> %d", (int)st);
	return 0;
}

/* (enc, token, ib, *pts, *dur, props, *flags) */
static uint64_t
n_encode(uint64_t self, uint64_t token, uint64_t ib, uint64_t pts, uint64_t dur, uint64_t props, uint64_t flags, uint64_t a7)
{
	(void)self; (void)ib; (void)pts; (void)dur; (void)props; (void)flags; (void)a7;
	if (g_null_frames++ < 3)
		venc_log("null: EncodeFrame #%u", g_null_frames);
	if (venc_emit && g_null_session) {
		OSStatus e = venc_emit(g_null_session, (void *)(uintptr_t)token, 0, kVTEncodeInfo_FrameDropped, NULL);

		if (g_null_frames <= 3)
			venc_log("null: EmitEncodedFrame(dropped) -> %d", (int)e);
	}
	return 0;
}

static uint64_t
n_complete(uint64_t self, uint64_t until, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
	(void)self; (void)until; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
	venc_log("null: CompleteFrames (%u frames)", g_null_frames);
	return 0;
}

static const struct vt_base_class null_base = { 1, 8, NULL, n_invalidate, NULL, NULL, n_copy_property, n_set_property, NULL, NULL, NULL };
/* the slot order read in spec 3 A.5: +0x08 StartSession, +0x10 EncodeFrame, +0x18 CompleteFrames */
static const struct vt_encoder_class null_class = { 3, { n_start, n_encode, n_complete, NULL, NULL, NULL, NULL, NULL } };
static const struct vt_vtable null_vtable = { NULL, &null_base, &null_class };

OSStatus
venc_make_object(CFAllocatorRef alloc, const struct vt_vtable *vt, void **out)
{
	OSStatus (*create)(CFAllocatorRef, const struct vt_vtable *, const void *, void **) = dlsym(RTLD_DEFAULT, "CMDerivedObjectCreate");
	const void *(*class_id)(void) = dlsym(RTLD_DEFAULT, "VTVideoEncoderGetClassID");

	if (!create || !class_id)
		return -1;
	return create(alloc, vt, class_id(), out);
}

static OSStatus
null_factory(CMVideoCodecType codec, CFAllocatorRef alloc, void **out)
{
	char cc[5];

	cc4(codec, cc);
	venc_log("factory called codec=%s (null)", cc);
	venc_factory_calls++;
	return venc_make_object(alloc, &null_vtable, out);
}

/* ---- Apple's encoders behind a logging shim: a copy of the instance's vtable whose slots log (first 3 per slot, then every 30th) and forward ---- */
static struct vt_base_class shim_base;
static struct vt_encoder_class shim_class;
static struct vt_vtable shim_vtable;
static const struct vt_base_class *real_base;
static const struct vt_encoder_class *real_class;
static OSStatus (*real_copy)(BaseRef, CFStringRef, CFAllocatorRef, void *);
static OSStatus (*real_set)(BaseRef, CFStringRef, CFTypeRef);
static unsigned slot_calls[8];
static const char *g_tag = "";

#define SLOT(n) static uint64_t shim_slot##n(uint64_t self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) \
{ \
	unsigned c = ++slot_calls[n]; \
	uint64_t r; \
	if (c <= 3 || c % 30 == 0) \
		venc_log("shim %s: slot %d #%u in: %#llx %#llx %#llx %#llx", g_tag, n, c, a1, a2, a3, a4); \
	r = real_class->slot[n](self, a1, a2, a3, a4, a5, a6, a7); \
	if (c <= 3 || c % 30 == 0) \
		venc_log("shim %s: slot %d #%u out: %#llx", g_tag, n, c, r); \
	return r; \
}
SLOT(0) SLOT(1) SLOT(2) SLOT(3)

static OSStatus
shim_copy_property(BaseRef o, CFStringRef key, CFAllocatorRef a, void *out)
{
	char k[100] = "?";
	OSStatus st = real_copy(o, key, a, out);

	CFStringGetCString(key, k, sizeof(k), kCFStringEncodingUTF8);
	if (++slot_calls[4] <= 12)
		venc_log("shim %s: copyProperty %s -> %d", g_tag, k, (int)st);
	return st;
}

static OSStatus
shim_set_property(BaseRef o, CFStringRef key, CFTypeRef v)
{
	char k[100] = "?";
	OSStatus st = real_set(o, key, v);

	CFStringGetCString(key, k, sizeof(k), kCFStringEncodingUTF8);
	if (++slot_calls[5] <= 12)
		venc_log("shim %s: setProperty %s -> %d", g_tag, k, (int)st);
	return st;
}

static void
shim_install(void *instance)
{
	const struct vt_vtable *(*get_vtable)(void *) = dlsym(RTLD_DEFAULT, "CMBaseObjectGetVTable");
	const struct vt_vtable *vt = get_vtable ? get_vtable(instance) : NULL;
	uint64_t *words = instance;
	slot_fn tramp[4] = { shim_slot0, shim_slot1, shim_slot2, shim_slot3 };

	if (!vt || !vt->base || !vt->derived) {
		venc_log("shim %s: no vtable (%p)", g_tag, vt);
		return;
	}
	real_base = vt->base;
	real_class = vt->derived;
	memcpy(&shim_base, real_base, sizeof(shim_base));
	memcpy(&shim_class, real_class, sizeof(shim_class));
	real_copy = real_base->copy_property;
	real_set = real_base->set_property;
	if (real_copy)
		shim_base.copy_property = shim_copy_property;
	if (real_set)
		shim_base.set_property = shim_set_property;
	for (int i = 0; i < 4; i++)
		if (real_class->slot[i])
			shim_class.slot[i] = tramp[i];
	shim_vtable.reserved = vt->reserved;
	shim_vtable.base = &shim_base;
	shim_vtable.derived = &shim_class;
	memset(slot_calls, 0, sizeof(slot_calls));
	for (int i = 0; i < 8; i++) {
		if (words[i] == (uint64_t)(uintptr_t)vt) {
			words[i] = (uint64_t)(uintptr_t)&shim_vtable;
			venc_log("shim %s: vtable replaced at word %d; class version %llu, slots %p %p %p %p", g_tag, i, (unsigned long long)real_class->version,
			    real_class->slot[0], real_class->slot[1], real_class->slot[2], real_class->slot[3]);
			return;
		}
	}
	venc_log("shim %s: vtable not found in the first 64 bytes of the instance", g_tag);
}

static EncFactory g_real_a1, g_real_a2;
static OSStatus
apple_factory(EncFactory real, const char *tag, CMVideoCodecType codec, CFAllocatorRef alloc, void **out)
{
	char cc[5];
	OSStatus st;

	cc4(codec, cc);
	venc_factory_calls++;
	venc_log("factory called codec=%s (%s) #%u", cc, tag, venc_factory_calls);
	g_tag = tag;
	st = real(codec, alloc, out);
	venc_log("factory %s: status %d instance %p", tag, (int)st, out ? *out : NULL);
	if (st == 0 && out && *out)
		shim_install(*out);
	return st;
}
static OSStatus a1_factory(CMVideoCodecType c, CFAllocatorRef a, void **o) { return apple_factory(g_real_a1, "a1", c, a, o); }
static OSStatus a2_factory(CMVideoCodecType c, CFAllocatorRef a, void **o) { return apple_factory(g_real_a2, "a2", c, a, o); }

static EncFactory
load_factory(const char *image, const char *symbol)
{
	void *h = dlopen(image, RTLD_NOW);
	EncFactory f = h ? dlsym(h, symbol) : NULL;

	venc_log("dlopen %s %p (%s), %s %p", image, h, h ? "ok" : dlerror(), symbol, f);
	return f;
}

/* the minih264e encoder (venc_h264.c), when linked in */
__attribute__((weak)) OSStatus
venc_h264_factory(CMVideoCodecType codec, CFAllocatorRef alloc, void **out)
{
	(void)codec; (void)alloc; (void)out;
	return -1;
}

/* ---- registration ---- */
int
venc_register(int mode)
{
	OSStatus (*reg)(CMVideoCodecType, CFDictionaryRef, EncFactory) = dlsym(RTLD_DEFAULT, "VTRegisterVideoEncoderWithInfo");
	CFStringRef *rating_key = dlsym(RTLD_DEFAULT, "kVTVideoEncoder_Rating");
	CFMutableDictionaryRef info = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	int rating = 1000;
	CFNumberRef nr = CFNumberCreate(NULL, kCFNumberIntType, &rating);
	CMVideoCodecType codec = 'avc1';
	EncFactory factory = NULL;
	const char *id = "null";
	OSStatus st;

	venc_set_pba = dlsym(RTLD_DEFAULT, "VTEncoderSessionSetPixelBufferAttributes");
	venc_emit = dlsym(RTLD_DEFAULT, "VTEncoderSessionEmitEncodedFrame");
	switch (mode) {
	case 1:
		factory = null_factory;
		break;
	case 2:
		id = "a1";
		g_real_a1 = load_factory("/System/Library/PrivateFrameworks/VideoProcessing.framework/VideoProcessing", "H264VideoEncoder_CreateInstance");
		factory = g_real_a1 ? a1_factory : NULL;
		break;
	case 3:
		id = "a2";
		codec = 'hvc1';
		g_real_a2 = load_factory("/System/Library/VideoCodecs/VCPHEVC.videocodec", "HEVCVideoEncoder_CreateInstance");
		factory = g_real_a2 ? a2_factory : NULL;
		break;
	case 4:
		id = "h264";
		factory = venc_h264_factory;
		break;
	}
	if (!reg || !factory) {
		venc_log("register mode %d: nothing to register (reg %p factory %p)", mode, reg, factory);
		return -1;
	}
	char name[80];
	CFStringRef idstr;

	snprintf(name, sizeof(name), "com.redwood.videoencoder.%s", id);
	idstr = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	CFDictionarySetValue(info, CFSTR("CMClassImplementationID"), idstr);
	CFDictionarySetValue(info, rating_key && *rating_key ? *rating_key : CFSTR("VTRating"), nr);
	st = reg(codec, info, factory);
	{
		CFArrayRef list = NULL;
		OSStatus ls = VTCopyVideoEncoderList(NULL, &list);
		char cc[5];

		cc4(codec, cc);
		venc_log("registered %s as '%s' rating 1000: status %d; encoder list status %d count %ld", name, cc, (int)st, (int)ls, list ? (long)CFArrayGetCount(list) : -1L);
		if (list)
			CFRelease(list);
	}
	CFRelease(info);
	CFRelease(nr);
	CFRelease(idstr);
	return st;
}
