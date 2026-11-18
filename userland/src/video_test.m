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

		for (int i = 0; i < 30; i++) {
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
		say("probe: %s %dx%d: TIME %.1f ms wall (%.1f ms a frame), %.1f ms cpu (gradient included)", tag, w, h, wall, wall / 30, cpu);
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

static void
candidate(const char *tag, const char *image, const char *symbol, CMVideoCodecType codec, const char *id, BOOL hevc)
{
	void *h = dlopen(image, RTLD_NOW);
	EncFactory factory = h ? dlsym(h, symbol) : NULL;
	RegisterWithInfo reg = dlsym(RTLD_DEFAULT, "VTRegisterVideoEncoderWithInfo");

	say("probe: %s: dlopen %s %p (%s), dlsym %s %p, VTRegisterVideoEncoderWithInfo %p", tag, image, h, h ? "ok" : dlerror(), symbol, factory, reg);
	if (!factory || !reg)
		return;
	{
		/* the factory on its own: does it make an instance here at all? */
		void *inst = NULL;
		OSStatus fs = factory(codec, kCFAllocatorDefault, &inst);

		say("probe: %s: the factory called directly: status %d, instance %p", tag, (int)fs, inst);
	}
	int rating = 100;
	CFStringRef idstr = CFStringCreateWithCString(NULL, id, kCFStringEncodingUTF8);
	CFStringRef nm = CFStringCreateWithCString(NULL, tag, kCFStringEncodingUTF8);
	CFNumberRef nr = CFNumberCreate(NULL, kCFNumberIntType, &rating);
	const void *k[5] = { CFSTR("CMClassImplementationID"), vt_key("kVTVideoEncoder_Rating", CFSTR("Rating")), vt_key("kVTVideoEncoder_IsHardwareAccelerated", CFSTR("IsHardwareAccelerated")),
		vt_key("kVTVideoEncoder_EncoderNameKey", CFSTR("EncoderName")), vt_key("kVTVideoEncoder_CodecNameKey", CFSTR("CodecName")) };
	const void *v[5] = { idstr, nr, kCFBooleanTrue, nm, nm };
	CFDictionaryRef info = CFDictionaryCreate(NULL, k, v, 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	OSStatus st = reg(codec, info, factory);

	say("probe: %s: VTRegisterVideoEncoderWithInfo status %d", tag, (int)st);
	CFRelease(info);
	CFRelease(nr);
	CFRelease(nm);
	CFRelease(idstr);
	if (st != 0)
		return;
	encoder_list(tag);
	try_session(tag, codec, "", 640, 480, hevc);       /* (no encoder specification: any encoder of the codec) */
	try_session(tag, codec, id, 640, 480, hevc);
	try_session(tag, codec, id, 1280, 720, hevc);
}

static int
probe(void)
{
	alarm(240);     /* a stuck encoder must not hold the run */
	encoder_list("start");
	candidate("a1 VideoProcessing H264VideoEncoder", "/System/Library/PrivateFrameworks/VideoProcessing.framework/VideoProcessing", "H264VideoEncoder_CreateInstance",
	    'avc1', "com.redwood.probe.1", NO);
	candidate("a2 VCPHEVC HEVCVideoEncoder", "/System/Library/VideoCodecs/VCPHEVC.videocodec", "HEVCVideoEncoder_CreateInstance",
	    'hvc1', "com.redwood.probe.2", YES);
	write_one(YES, AVVideoCodecTypeH264);
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
