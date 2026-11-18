/*
 * video_test: is video recording possible, and did it work?
 *   video_test scan      every .MOV/.MP4 under /private/var/mobile/Media: size, duration, video and audio tracks
 *                        (what Camera's video mode leaves after rwtouch pressed its record button twice)
 *   video_test probe     (docs/video-recording-spec.md 3.1 C) the encoder list; VideoProcessing's H.264 and VCPHEVC's software encoders
 *                        registered in this process as 'avc1' / 'hvc1', a VTCompressionSession at 640x480 and 1280x720 each, 30 NV12 frames,
 *                        ms a frame; then AVAssetWriter H.264 and HEVC
 *   video_test encode    no camera: AVAssetWriter writes 30 synthetic 640x480 frames as H.264 into /tmp/vt.mov,
 *                        and says whether the system can encode it (no frames: unused AAC branch kept for a later step)
 */
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/resource.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <time.h>
#include <unistd.h>
#import <VideoToolbox/VideoToolbox.h>

/* stdout, and the kernel's kept log (the console's early lines fall out of the pstore tail) */
static void
say(const char *fmt, ...)
{
	char text[300], line[320];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	printf("video_test: %s\n", text);
	snprintf(line, sizeof(line), "video_test: %s", text);
	sysctlbyname("kern.redwood.host_keep", NULL, NULL, line, strlen(line));
}

static int
scan(void)
{
	NSFileManager *fm = NSFileManager.defaultManager;
	int found = 0;

	for (NSString *rel in [fm enumeratorAtPath:@"/private/var/mobile/Media"]) {
		NSString *ext = rel.pathExtension.lowercaseString;
		if (![ext isEqual:@"mov"] && ![ext isEqual:@"mp4"])
			continue;
		NSString *path = [@"/private/var/mobile/Media" stringByAppendingPathComponent:rel];
		AVURLAsset *a = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path] options:nil];
		say("%s: %llu bytes, %.2f s, %lu video track(s), %lu audio track(s)", rel.UTF8String,
		    [[fm attributesOfItemAtPath:path error:nil] fileSize], CMTimeGetSeconds(a.duration),
		    (unsigned long)[a tracksWithMediaType:AVMediaTypeVideo].count, (unsigned long)[a tracksWithMediaType:AVMediaTypeAudio].count);
		found++;
	}
	say("scan: %d movie file(s)", found);
	return found == 0;
}

static int
write_one(BOOL video, NSString *codec)
{
	NSString *path = video ? @"/tmp/vt.mov" : @"/tmp/vt-audio.m4a";
	[NSFileManager.defaultManager removeItemAtPath:path error:nil];
	NSError *error = nil;
	AVAssetWriter *w = [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:path] fileType:video ? AVFileTypeQuickTimeMovie : AVFileTypeAppleM4A error:&error];
	if (!w) {
		say("encode %s: no writer: %s", video ? "H.264" : "AAC", error.description.UTF8String);
		return 1;
	}
	NSDictionary *vs = @{ AVVideoCodecKey: codec, AVVideoWidthKey: @640, AVVideoHeightKey: @480 };
	NSDictionary *as = @{ AVFormatIDKey: @(kAudioFormatMPEG4AAC), AVSampleRateKey: @44100, AVNumberOfChannelsKey: @1, AVEncoderBitRateKey: @64000 };
	AVAssetWriterInput *in = [AVAssetWriterInput assetWriterInputWithMediaType:video ? AVMediaTypeVideo : AVMediaTypeAudio outputSettings:video ? vs : as];
	in.expectsMediaDataInRealTime = YES;
	AVAssetWriterInputPixelBufferAdaptor *ad = video ? [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:in sourcePixelBufferAttributes:@{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA), (id)kCVPixelBufferWidthKey: @640, (id)kCVPixelBufferHeightKey: @480 }] : nil;
	if (![w canAddInput:in]) {
		say("encode %s: cannot add input", video ? "H.264" : "AAC");
		return 1;
	}
	[w addInput:in];
	BOOL ok = [w startWriting];
	[w startSessionAtSourceTime:kCMTimeZero];
	int sent = 0;
	for (int i = 0; ok && i < (video ? 30 : 0); i++) {
		CVPixelBufferRef pb = NULL;
		for (int n = 0; n < 200 && !in.readyForMoreMediaData; n++)
			usleep(10000);
		CVPixelBufferPoolCreatePixelBuffer(NULL, ad.pixelBufferPool, &pb);
		if (!pb) {
			say("encode H.264: no pixel buffer (pool %p)", ad.pixelBufferPool);
			break;
		}
		CVPixelBufferLockBaseAddress(pb, 0);
		memset(CVPixelBufferGetBaseAddress(pb), i * 8, CVPixelBufferGetDataSize(pb));
		CVPixelBufferUnlockBaseAddress(pb, 0);
		if ([ad appendPixelBuffer:pb withPresentationTime:CMTimeMake(i, 30)])
			sent++;
		CVPixelBufferRelease(pb);
	}
	[in markAsFinished];
	dispatch_semaphore_t s = dispatch_semaphore_create(0);
	[w finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(s); }];
	dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC));
	say("encode %s: startWriting %d, %d frames appended, status %ld, error %s, file %llu bytes", video ? codec.UTF8String : "AAC", ok,
	    sent, (long)w.status, w.error ? w.error.description.UTF8String : "none", [[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil] fileSize]);
	return w.status != AVAssetWriterStatusCompleted;
}

/* ---- probe ---- */
typedef OSStatus (*EncFactory)(CMVideoCodecType, CFAllocatorRef, void **);
typedef OSStatus (*RegisterWithInfo)(CMVideoCodecType, CFDictionaryRef, EncFactory);

static CFStringRef
vt_key(const char *symbol, CFStringRef fallback)
{
	CFStringRef *p = dlsym(RTLD_DEFAULT, symbol);

	return p && *p ? *p : fallback;
}

static void
fourcc_text(unsigned code, char *out)
{
	for (int i = 0; i < 4; i++) {
		unsigned char c = (code >> (24 - 8 * i)) & 0xff;

		out[i] = c >= 32 && c < 127 ? (char)c : '?';
	}
	out[4] = 0;
}

static void
encoder_list(const char *when)
{
	CFArrayRef list = NULL;
	CFMutableDictionaryRef opt = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFStringRef hidden = vt_key("kVTVideoEncoderListOption_RevealHiddenEncoders", NULL), hw = vt_key("kVTVideoEncoderListOption_RevealHardwareAcceleratedEncoders", NULL);

	if (hidden)
		CFDictionarySetValue(opt, hidden, kCFBooleanTrue);
	if (hw)
		CFDictionarySetValue(opt, hw, kCFBooleanTrue);
	OSStatus st = VTCopyVideoEncoderList(opt, &list);
	say("probe: %s: VTCopyVideoEncoderList status %d, %ld encoder(s) (hidden key %d, hw key %d)", when, (int)st, list ? (long)CFArrayGetCount(list) : -1L, hidden != NULL, hw != NULL);
	for (CFIndex i = 0; list && i < CFArrayGetCount(list) && i < 40; i++) {
		char text[300] = "";
		CFStringRef d = CFCopyDescription(CFArrayGetValueAtIndex(list, i));

		CFStringGetCString(d, text, sizeof(text), kCFStringEncodingUTF8);
		CFRelease(d);
		for (char *c = text; *c; c++)
			if (*c == '\n' || *c == '\t')
				*c = ' ';
		say("probe: %s: encoder %ld: %.250s", when, (long)i, text);
	}
	if (list)
		CFRelease(list);
	CFRelease(opt);
}

struct run {
	int samples;
	size_t first_size;
	unsigned subtype;
	int w, h;
	OSStatus cb_status;
};

static void
output_cb(void *refcon, void *src, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sb)
{
	struct run *r = refcon;

	(void)src;
	(void)flags;
	if (status != 0) {
		r->cb_status = status;
		return;
	}
	if (sb == NULL)
		return;
	if (r->samples++ == 0) {
		CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sb);

		r->first_size = CMSampleBufferGetTotalSampleSize(sb);
		if (fd) {
			CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(fd);

			r->subtype = CMFormatDescriptionGetMediaSubType(fd);
			r->w = d.width;
			r->h = d.height;
		}
	}
}

static double
cpu_ms(void)
{
	struct rusage u;

	getrusage(RUSAGE_SELF, &u);
	return u.ru_utime.tv_sec * 1000.0 + u.ru_utime.tv_usec / 1000.0 + u.ru_stime.tv_sec * 1000.0 + u.ru_stime.tv_usec / 1000.0;
}

static double
wall_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int nframes = 30;
static void
try_session(const char *tag, CMVideoCodecType codec, const char *id, int w, int h, BOOL hevc)
{
	struct run r = { 0 };
	VTCompressionSessionRef session = NULL;
	const void *spec_k[1] = { vt_key("kVTVideoEncoderSpecification_EncoderID", CFSTR("EncoderID")) };
	CFStringRef idstr = CFStringCreateWithCString(NULL, id, kCFStringEncodingUTF8);
	const void *spec_v[1] = { idstr };
	CFDictionaryRef spec = id[0] == 0 ? NULL : CFDictionaryCreate(NULL, spec_k, spec_v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	int fmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	CFNumberRef nf = CFNumberCreate(NULL, kCFNumberIntType, &fmt), nw = CFNumberCreate(NULL, kCFNumberIntType, &w), nh = CFNumberCreate(NULL, kCFNumberIntType, &h);
	const void *sk[3] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey }, *sv[3] = { nf, nw, nh };
	CFDictionaryRef src = CFDictionaryCreate(NULL, sk, sv, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	char cc[5];

	fourcc_text(codec, cc);
	OSStatus st = VTCompressionSessionCreate(NULL, w, h, codec, spec, src, NULL, output_cb, &r, &session);
	say("probe: %s '%s' %dx%d 420v: VTCompressionSessionCreate status %d", tag, cc, w, h, (int)st);
	if (st == 0 && session) {
		OSStatus p1 = VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
		int bps = 2000000;
		CFNumberRef nb = CFNumberCreate(NULL, kCFNumberIntType, &bps);
		OSStatus p2 = VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate, nb);
		OSStatus p3 = VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel, hevc ? CFSTR("HEVC_Main_AutoLevel") : kVTProfileLevel_H264_Baseline_AutoLevel);
		OSStatus p4 = VTCompressionSessionPrepareToEncodeFrames(session);

		say("probe: %s %dx%d: RealTime %d, AverageBitRate %d, ProfileLevel %d, Prepare %d", tag, w, h, (int)p1, (int)p2, (int)p3, (int)p4);
		CFRelease(nb);
		double w0 = wall_ms(), c0 = cpu_ms();
		int sent = 0, bad = 0;

		for (int i = 0; i < nframes; i++) {
			CVPixelBufferRef pb = NULL;
			CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			const void *ak[1] = { kCVPixelBufferIOSurfacePropertiesKey }, *av[1] = { empty };
			CFDictionaryRef attrs = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

			CVPixelBufferCreate(NULL, w, h, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &pb);
			CFRelease(attrs);
			CFRelease(empty);
			if (!pb) {
				bad++;
				continue;
			}
			CVPixelBufferLockBaseAddress(pb, 0);
			for (size_t pl = 0; pl < 2; pl++) {
				uint8_t *base = CVPixelBufferGetBaseAddressOfPlane(pb, pl);
				size_t rows = CVPixelBufferGetHeightOfPlane(pb, pl), cols = CVPixelBufferGetBytesPerRowOfPlane(pb, pl);

				for (size_t y = 0; y < rows; y++)
					for (size_t x = 0; x < cols; x++)
						base[y * cols + x] = (uint8_t)(x + y + i * 6 + pl * 40);
			}
			CVPixelBufferUnlockBaseAddress(pb, 0);
			/* (the gradient is made before the clock of the frame is read: the loop below times the encoder, not this) */
			OSStatus e = VTCompressionSessionEncodeFrame(session, pb, CMTimeMake(i, 30), CMTimeMake(1, 30), NULL, NULL, NULL);

			if (e != 0) {
				bad++;
				if (bad < 3)
					say("probe: %s %dx%d: EncodeFrame %d status %d", tag, w, h, i, (int)e);
			} else
				sent++;
			CVPixelBufferRelease(pb);
		}
		OSStatus cf = VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
		double wall = wall_ms() - w0, cpu = cpu_ms() - c0;
		char sub[5];

		fourcc_text(r.subtype, sub);
		say("probe: %s %dx%d: %d frames given (%d refused), CompleteFrames %d, %d samples out, first %zu B, format '%s' %dx%d, cb status %d", tag, w, h, sent, bad,
		    (int)cf, r.samples, r.first_size, sub, r.w, r.h, (int)r.cb_status);
		say("probe: %s %dx%d: TIME %.1f ms wall (%.1f ms a frame), %.1f ms cpu (gradient included)", tag, w, h, wall, wall / nframes, cpu);
		VTCompressionSessionInvalidate(session);
		CFRelease(session);
	}
	CFRelease(src);
	CFRelease(nf);
	CFRelease(nw);
	CFRelease(nh);
	if (spec)
		CFRelease(spec);
	CFRelease(idstr);
}

extern const void *_dyld_get_shared_cache_range(size_t *);
/* ---- vd2: a null encoder, a logging shim, and the registration matrix (docs/video-recording-spec-2.md C.1 B1-B6) ---- */
typedef struct OpaqueCMBaseObject *BaseRef;
struct vt_base_class {
	uint64_t version, storage_size;
	Boolean (*equal)(BaseRef, BaseRef);
	OSStatus (*invalidate)(BaseRef);
	void (*finalize)(BaseRef);
	CFStringRef (*debug)(BaseRef);
	OSStatus (*copy_property)(BaseRef, CFStringRef, CFAllocatorRef, void *);
	OSStatus (*set_property)(BaseRef, CFStringRef, CFTypeRef);
	OSStatus (*barrier)(BaseRef);
	const void *protocols, *reserved;
};
typedef uint64_t (*slot_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
struct vt_encoder_class {
	uint64_t version;
	slot_fn slot[8];
};
struct vt_vtable {
	const void *reserved;
	const struct vt_base_class *base;
	const void *derived;
};

static OSStatus (*p_set_pba)(void *, CFDictionaryRef);
static OSStatus (*p_emit)(void *, void *, OSStatus, VTEncodeInfoFlags, CMSampleBufferRef);
static const char *g_tag = "";
static unsigned g_factory_calls;

/* the null encoder: takes everything, emits dropped frames */
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
		say("null: copyProperty %s", k);
	if (strstr(k, "Supported") != NULL) {
		*(CFTypeRef *)out = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		return 0;
	}
	return -12782;
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
		say("null: setProperty %s", k);
	return 0;
}

static OSStatus n_invalidate(BaseRef o) { (void)o; return 0; }

static uint64_t
n_start(uint64_t self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
	int pf[2] = { kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange };
	CFNumberRef n[2] = { CFNumberCreate(NULL, kCFNumberIntType, &pf[0]), CFNumberCreate(NULL, kCFNumberIntType, &pf[1]) };
	CFArrayRef arr = CFArrayCreate(NULL, (const void **)n, 2, &kCFTypeArrayCallBacks);
	CFDictionaryRef d = CFDictionaryCreate(NULL, (const void **)&kCVPixelBufferPixelFormatTypeKey, (const void **)&arr, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	OSStatus st = -1;

	(void)self;
	say("null: StartSession args %#llx %#llx %#llx %#llx %#llx %#llx %#llx", a1, a2, a3, a4, a5, a6, a7);
	g_null_session = (void *)(uintptr_t)a1;
	if (p_set_pba)
		st = p_set_pba(g_null_session, d);
	say("null: SetPixelBufferAttributes -> %d", (int)st);
	return 0;
}

static uint64_t
n_prepare(uint64_t self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
	(void)self; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
	say("null: PrepareToEncodeFrames");
	return 0;
}

static uint64_t
n_encode(uint64_t self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
	(void)self;
	if (g_null_frames++ < 3)
		say("null: EncodeFrame args %#llx %#llx %#llx %#llx %#llx %#llx %#llx", a1, a2, a3, a4, a5, a6, a7);
	if (p_emit && g_null_session) {
		/* (session, token): the token is guessed as the first argument after the session */
		OSStatus e = p_emit(g_null_session, (void *)(uintptr_t)a2, 0, kVTEncodeInfo_FrameDropped, NULL);

		if (g_null_frames <= 3)
			say("null: EmitEncodedFrame(dropped) -> %d", (int)e);
	}
	return 0;
}

static uint64_t
n_complete(uint64_t self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
	(void)self; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
	say("null: CompleteFrames (%u frames)", g_null_frames);
	return 0;
}

static const struct vt_base_class null_base = { 1, 8, NULL, n_invalidate, NULL, NULL, n_copy_property, n_set_property, NULL, NULL, NULL };
static const struct vt_encoder_class null_class = { 3, { n_start, n_prepare, n_encode, n_complete } };
static const struct vt_vtable null_vtable = { NULL, &null_base, &null_class };

static OSStatus
null_factory(CMVideoCodecType codec, CFAllocatorRef alloc, void **out)
{
	OSStatus (*create)(CFAllocatorRef, const struct vt_vtable *, const void *, void **) = dlsym(RTLD_DEFAULT, "CMDerivedObjectCreate");
	const void *(*class_id)(void) = dlsym(RTLD_DEFAULT, "VTVideoEncoderGetClassID");

	(void)codec;
	if (!create || !class_id)
		return -1;
	return create(alloc, &null_vtable, class_id(), out);
}

/* the shim: a copy of the instance's vtable whose slots log (first 3 per slot, then every 30th) and forward */
static struct vt_base_class shim_base;
static struct vt_encoder_class shim_class;
static struct vt_vtable shim_vtable;
static const struct vt_base_class *real_base;
static const struct vt_encoder_class *real_class;
static OSStatus (*real_copy)(BaseRef, CFStringRef, CFAllocatorRef, void *);
static OSStatus (*real_set)(BaseRef, CFStringRef, CFTypeRef);
static unsigned slot_calls[8];

#define SLOT(n) static uint64_t shim_slot##n(uint64_t self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) \
{ \
	unsigned c = ++slot_calls[n]; \
	uint64_t r; \
	if (c <= 3 || c % 30 == 0) \
		say("shim %s: slot %d #%u in: %#llx %#llx %#llx %#llx %#llx %#llx %#llx", g_tag, n, c, a1, a2, a3, a4, a5, a6, a7); \
	r = real_class->slot[n](self, a1, a2, a3, a4, a5, a6, a7); \
	if (c <= 3 || c % 30 == 0) \
		say("shim %s: slot %d #%u out: %#llx", g_tag, n, c, r); \
	return r; \
}
SLOT(0) SLOT(1) SLOT(2) SLOT(3)

static OSStatus
shim_copy_property(BaseRef o, CFStringRef key, CFAllocatorRef a, void *out)
{
	char k[100] = "?";
	OSStatus st = real_copy(o, key, a, out);
	unsigned c = ++slot_calls[4];

	CFStringGetCString(key, k, sizeof(k), kCFStringEncodingUTF8);
	if (c <= 12)
		say("shim %s: copyProperty %s -> %d", g_tag, k, (int)st);
	return st;
}

static OSStatus
shim_set_property(BaseRef o, CFStringRef key, CFTypeRef v)
{
	char k[100] = "?";
	OSStatus st = real_set(o, key, v);
	unsigned c = ++slot_calls[5];

	CFStringGetCString(key, k, sizeof(k), kCFStringEncodingUTF8);
	if (c <= 12)
		say("shim %s: setProperty %s -> %d", g_tag, k, (int)st);
	return st;
}

static void
shim_install(void *instance)
{
	const struct vt_vtable *(*get_vtable)(void *) = dlsym(RTLD_DEFAULT, "CMBaseObjectGetVTable");
	const struct vt_vtable *vt = get_vtable ? get_vtable(instance) : NULL;
	uint64_t *words = instance;

	if (!vt || !vt->base || !vt->derived) {
		say("shim %s: no vtable (%p)", g_tag, vt);
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
	slot_fn tramp[4] = { shim_slot0, shim_slot1, shim_slot2, shim_slot3 };
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
			say("shim %s: vtable %p replaced at word %d; class version %llu, slots %p %p %p %p", g_tag, vt, i, (unsigned long long)real_class->version,
			    real_class->slot[0], real_class->slot[1], real_class->slot[2], real_class->slot[3]);
			return;
		}
	}
	say("shim %s: vtable %p not found in the first 64 bytes of the instance", g_tag, vt);
}

/* wrapper factories: log, call the real one, put the shim in. 3-argument form and the 5-argument form (R4) */
static EncFactory g_real_factory[3];
static void
wrapped(int kind, CMVideoCodecType codec, CFAllocatorRef alloc, void **out, OSStatus *st)
{
	char cc[5];
	static const char *const names[3] = { "null", "a1", "a2" };

	fourcc_text(codec, cc);
	g_factory_calls++;
	say("factory called: %s codec=%s (#%u)", names[kind], cc, g_factory_calls);
	g_tag = names[kind];
	*st = g_real_factory[kind](codec, alloc, out);
	say("factory %s: status %d instance %p", names[kind], (int)*st, out ? *out : NULL);
	if (*st == 0 && out && *out)
		shim_install(*out);
}
#define WF(kind) \
static OSStatus wf3_##kind(CMVideoCodecType c, CFAllocatorRef a, void **o) { OSStatus st; wrapped(kind, c, a, o, &st); return st; } \
static OSStatus wf5_##kind(CMVideoCodecType c, CFStringRef impl, CFDictionaryRef spec, CFAllocatorRef a, void **o) { OSStatus st; (void)impl; (void)spec; wrapped(kind, c, a, o, &st); return st; }
#define K_NULL 0
#define K_A1 1
#define K_A2 2
WF(0) WF(1) WF(2)
static EncFactory const wf3[3] = { (EncFactory)wf3_0, (EncFactory)wf3_1, (EncFactory)wf3_2 };
static void *const wf5[3] = { wf5_0, wf5_1, wf5_2 };

static void
register_variant(const char *tag, int kind, int variant, CMVideoCodecType codec, const char *id, BOOL hevc)
{
	RegisterWithInfo reg = dlsym(RTLD_DEFAULT, "VTRegisterVideoEncoderWithInfo");
	OSStatus (*reg_plain)(CMVideoCodecType, EncFactory) = dlsym(RTLD_DEFAULT, "VTRegisterVideoEncoder");
	int rating = 100;
	CFStringRef idstr = CFStringCreateWithCString(NULL, id, kCFStringEncodingUTF8);
	CFNumberRef nr = CFNumberCreate(NULL, kCFNumberIntType, &rating);
	CFMutableDictionaryRef info = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	OSStatus st;
	char label[80];

	snprintf(label, sizeof(label), "%s R%d", tag, variant);
	if (variant == 1) {
		st = reg_plain ? reg_plain(codec, wf3[kind]) : -1;
	} else {
		CFDictionarySetValue(info, CFSTR("CMClassImplementationID"), idstr);
		CFDictionarySetValue(info, vt_key("kVTVideoEncoder_Rating", CFSTR("Rating")), nr);
		if (variant == 3)
			CFDictionarySetValue(info, vt_key("kVTVideoEncoder_IsHardwareAccelerated", CFSTR("IsHardwareAccelerated")), kCFBooleanTrue);
		if (variant == 4)
			CFDictionarySetValue(info, CFSTR("VTFactoryFunctionHasSpecificationArgument"), kCFBooleanTrue);
		st = reg ? reg(codec, info, variant == 4 ? (EncFactory)wf5[kind] : wf3[kind]) : -1;
	}
	say("probe: %s: registered (id %s) status %d", label, variant == 1 ? "none" : id, (int)st);
	CFRelease(info);
	CFRelease(nr);
	CFRelease(idstr);
	if (st != 0)
		return;
	unsigned before = g_factory_calls;

	try_session(label, codec, "", 640, 480, hevc);
	say("probe: %s: no-spec create: factory called %u time(s)", label, g_factory_calls - before);
	if (variant != 1) {
		before = g_factory_calls;
		try_session(label, codec, id, 640, 480, hevc);
		say("probe: %s: EncoderID create: factory called %u time(s)", label, g_factory_calls - before);
	}
}

static void
read_restrictions(void)
{
	size_t length = 0;
	uintptr_t slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - 0x180000000UL;
	const volatile uint8_t *p = (const volatile uint8_t *)(0x1dd401650UL + slide);

	say("probe: VT restriction bytes 0x1dd401650..55 (slide %#lx): %02x %02x %02x %02x %02x %02x", (unsigned long)slide, p[0], p[1], p[2], p[3], p[4], p[5]);
}

/* B6: VCPHEVC's stderr log level (0x1e246fef8 + slide) to 7 */
static void
hevc_verbose(void)
{
	size_t length = 0;
	uintptr_t slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - 0x180000000UL;
	volatile int *p = (volatile int *)(0x1e246fef8UL + slide);
	kern_return_t kr = vm_protect(mach_task_self(), (vm_address_t)((uintptr_t)p & ~0x3fffUL), 0x4000, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);

	say("probe: VCPHEVC log level was %d, vm_protect %d", *p, (int)kr);
	if (kr == 0) {
		*p = 7;
		say("probe: VCPHEVC log level now %d", *p);
	}
}

static EncFactory
load_factory(const char *image, const char *symbol)
{
	void *h = dlopen(image, RTLD_NOW);
	EncFactory f = h ? dlsym(h, symbol) : NULL;

	say("probe: dlopen %s %p (%s), %s %p", image, h, h ? "ok" : dlerror(), symbol, f);
	return f;
}

static int
probe(void)
{
	alarm(300);     /* a stuck encoder must not hold the run */
	nframes = 10;
	p_set_pba = dlsym(RTLD_DEFAULT, "VTEncoderSessionSetPixelBufferAttributes");
	p_emit = dlsym(RTLD_DEFAULT, "VTEncoderSessionEmitEncodedFrame");
	say("probe: VTEncoderSessionSetPixelBufferAttributes %p, EmitEncodedFrame %p", p_set_pba, p_emit);
	read_restrictions();
	encoder_list("start");
	g_real_factory[K_NULL] = null_factory;
	for (int v = 1; v <= 4; v++)
		register_variant("null avc1", K_NULL, v, 'avc1', v == 1 ? "x" : (const char *[]){ "", "", "com.redwood.null.2", "com.redwood.null.3", "com.redwood.null.4" }[v], NO);
	encoder_list("null");
	write_one(YES, AVVideoCodecTypeH264);
	if ((g_real_factory[K_A1] = load_factory("/System/Library/PrivateFrameworks/VideoProcessing.framework/VideoProcessing", "H264VideoEncoder_CreateInstance")))
		for (int v = 1; v <= 4; v++)
			register_variant("a1 avc1", K_A1, v, 'avc1', (const char *[]){ "", "", "com.redwood.a1.2", "com.redwood.a1.3", "com.redwood.a1.4" }[v], NO);
	write_one(YES, AVVideoCodecTypeH264);
	hevc_verbose();
	if ((g_real_factory[K_A2] = load_factory("/System/Library/VideoCodecs/VCPHEVC.videocodec", "HEVCVideoEncoder_CreateInstance")))
		for (int v = 1; v <= 4; v++)
			register_variant("a2 hvc1", K_A2, v, 'hvc1', (const char *[]){ "", "", "com.redwood.a2.2", "com.redwood.a2.3", "com.redwood.a2.4" }[v], YES);
	write_one(YES, AVVideoCodecTypeHEVC);
	say("probe: done");
	return 0;
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		setvbuf(stdout, NULL, _IOLBF, 0);
		if (argc > 1 && strcmp(argv[1], "scan") == 0)
			return scan();
		if (argc > 1 && strcmp(argv[1], "probe") == 0)
			return probe();
		return write_one(YES, AVVideoCodecTypeH264);
	}
}
