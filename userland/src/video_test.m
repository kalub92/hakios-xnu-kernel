/*
 * video_test: is video recording possible, and did it work?
 *   video_test scan      every .MOV/.MP4 under /private/var/mobile/Media: size, duration, video and audio tracks
 *                        (what Camera's video mode leaves after rwtouch pressed its record button twice)
 *   video_test inproc [MODE [W H]]  venc.c's encoder MODE (1 null, 2 VideoProcessing H.264, 4 minih264e; default 1) registered in this process, a
 *                        session forced local (AllowClientProcessEncode), 30 frames: samples, format, ms a frame (spec 3 C.1, E1)
 *   video_test encode [h264|hevc [W H]]  no camera: AVAssetWriter writes 30 synthetic frames into /tmp/vt.mov through the REMOTE session (mediaserverd,
 *                        where a venc=N encoder is registered): status, frames appended, ms a frame. No arguments: h264 640x480, h264 1280x720,
 *                        hevc 640x480, hevc 1280x720
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
#include <math.h>
#include <stdlib.h>
#include <unistd.h>
#import <VideoToolbox/VideoToolbox.h>
#include "venc.h"

/* stdout, and the kernel's kept log (the console's early lines fall out of the pstore tail) */
static void
say(const char *fmt, ...)
{
	char text[500], line[520];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	printf("video_test: %s\n", text);
	snprintf(line, sizeof(line), "video_test: %s", text);
	(void)sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
	sysctlbyname("kern.redwood.host_keep", NULL, NULL, line, strlen(line));
}

/* a track's samples: the count (compressed, or decoded to 16-bit mono 16 kHz for audio, with the peak and the level of each second) */
static void
scan_tracks(AVURLAsset *a, const char *rel)
{
	for (AVAssetTrack *t in [a tracksWithMediaType:AVMediaTypeVideo]) {
		NSError *e = nil;
		AVAssetReader *r = [AVAssetReader assetReaderWithAsset:a error:&e];
		AVAssetReaderTrackOutput *o = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:t outputSettings:nil];
		unsigned codec = 0;
		int n = 0, sync = 0;
		double first = -1, last = 0;
		char cc[5] = "";

		if (t.formatDescriptions.count)
			codec = CMFormatDescriptionGetMediaSubType((__bridge CMFormatDescriptionRef)t.formatDescriptions[0]);
		for (int i = 0; i < 4; i++) {
			unsigned char c = (codec >> (24 - 8 * i)) & 0xff;

			cc[i] = c >= 32 && c < 127 ? (char)c : '?';
		}
		if (r && [r canAddOutput:o] && ([r addOutput:o], [r startReading])) {
			CMSampleBufferRef sb;

			while ((sb = [o copyNextSampleBuffer])) {
				double pts = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sb));
				CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);

				if (first < 0)
					first = pts;
				last = pts;
				sync += !(att && CFArrayGetCount(att) && CFDictionaryContainsKey(CFArrayGetValueAtIndex(att, 0), kCMSampleAttachmentKey_NotSync));
				n++;
				CFRelease(sb);
			}
		}
		CGSize sz = t.naturalSize;
		say("%s: video '%s' %.0fx%.0f: %d samples (%d sync), pts %.2f..%.2f s, %.1f fps%s%s", rel, cc, sz.width, sz.height, n, sync, first, last, t.nominalFrameRate,
		    r.error ? ", error " : "", r.error ? r.error.description.UTF8String : "");
	}
	for (AVAssetTrack *t in [a tracksWithMediaType:AVMediaTypeAudio]) {
		NSError *e = nil;
		AVAssetReader *r = [AVAssetReader assetReaderWithAsset:a error:&e];
		NSDictionary *fmt = @{ AVFormatIDKey: @(kAudioFormatLinearPCM), AVLinearPCMBitDepthKey: @16, AVLinearPCMIsFloatKey: @NO, AVLinearPCMIsBigEndianKey: @NO,
		    AVLinearPCMIsNonInterleaved: @NO, AVSampleRateKey: @16000, AVNumberOfChannelsKey: @1 };
		AVAssetReaderTrackOutput *o = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:t outputSettings:fmt];
		long total = 0, peak = 0;
		double sumsq[40] = { 0 };
		long cnt[40] = { 0 };

		if (r && [r canAddOutput:o] && ([r addOutput:o], [r startReading])) {
			CMSampleBufferRef sb;

			while ((sb = [o copyNextSampleBuffer])) {
				CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
				size_t len = bb ? CMBlockBufferGetDataLength(bb) : 0;
				int16_t *buf = len ? malloc(len) : NULL;

				if (buf && CMBlockBufferCopyDataBytes(bb, 0, len, buf) == 0) {
					for (size_t i = 0; i < len / 2; i++, total++) {
						long v = buf[i] < 0 ? -buf[i] : buf[i], sec = total / 16000;

						if (v > peak)
							peak = v;
						if (sec < 40) {
							sumsq[sec] += (double)buf[i] * buf[i];
							cnt[sec]++;
						}
					}
				}
				free(buf);
				CFRelease(sb);
			}
		}
		char levels[400] = "";

		for (int i = 0; i < 40 && cnt[i]; i++)
			snprintf(levels + strlen(levels), sizeof(levels) - strlen(levels), " %.0f", sqrt(sumsq[i] / cnt[i]));
		say("%s: audio %ld samples (%.2f s at 16 kHz), peak %ld of 32768, rms per second:%s%s%s", rel, total, total / 16000.0, peak, levels, r.error ? ", error " : "",
		    r.error ? r.error.description.UTF8String : "");
	}
}

static int
scan(void)
{
	NSFileManager *fm = NSFileManager.defaultManager;
	int found = 0;

	alarm(120);
	for (NSString *rel in [fm enumeratorAtPath:@"/private/var/mobile/Media"]) {
		NSString *ext = rel.pathExtension.lowercaseString;
		if (![ext isEqual:@"mov"] && ![ext isEqual:@"mp4"])
			continue;
		NSString *path = [@"/private/var/mobile/Media" stringByAppendingPathComponent:rel];
		AVURLAsset *a = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path] options:nil];
		say("%s: %llu bytes, %.2f s, %lu video track(s), %lu audio track(s)", rel.UTF8String,
		    [[fm attributesOfItemAtPath:path error:nil] fileSize], CMTimeGetSeconds(a.duration),
		    (unsigned long)[a tracksWithMediaType:AVMediaTypeVideo].count, (unsigned long)[a tracksWithMediaType:AVMediaTypeAudio].count);
		scan_tracks(a, rel.UTF8String);
		found++;
	}
	say("scan: %d movie file(s)", found);
	return found == 0;
}

static double
wall_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int
write_one(NSString *codec, int w, int h)
{
	NSString *path = @"/tmp/vt.mov";
	[NSFileManager.defaultManager removeItemAtPath:path error:nil];
	NSError *error = nil;
	AVAssetWriter *wr = [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:path] fileType:AVFileTypeQuickTimeMovie error:&error];
	if (!wr) {
		say("encode %s: no writer: %s", codec.UTF8String, error.description.UTF8String);
		return 1;
	}
	NSDictionary *vs = @{ AVVideoCodecKey: codec, AVVideoWidthKey: @(w), AVVideoHeightKey: @(h) };
	AVAssetWriterInput *in = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:vs];
	in.expectsMediaDataInRealTime = YES;
	AVAssetWriterInputPixelBufferAdaptor *ad = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:in
	    sourcePixelBufferAttributes:@{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA), (id)kCVPixelBufferWidthKey: @(w), (id)kCVPixelBufferHeightKey: @(h) }];
	if (![wr canAddInput:in]) {
		say("encode %s %dx%d: cannot add input", codec.UTF8String, w, h);
		return 1;
	}
	[wr addInput:in];
	BOOL ok = [wr startWriting];
	[wr startSessionAtSourceTime:kCMTimeZero];
	int sent = 0;
	double w0 = wall_ms();
	for (int i = 0; ok && i < 30; i++) {
		CVPixelBufferRef pb = NULL;
		for (int n = 0; n < 200 && !in.readyForMoreMediaData; n++)
			usleep(10000);
		CVPixelBufferPoolCreatePixelBuffer(NULL, ad.pixelBufferPool, &pb);
		if (!pb) {
			say("encode %s: no pixel buffer (pool %p)", codec.UTF8String, ad.pixelBufferPool);
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
	[wr finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(s); }];
	dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC));
	double ms = wall_ms() - w0;
	if (wr.error) {         /* the whole error, in lines of 230 characters (the log takes 300) */
		const char *d = wr.error.description.UTF8String;

		for (size_t at = 0; at < strlen(d) && at < 1400; at += 230)
			say("encode error[%zu]: %.230s", at / 230, d + at);
	}
	say("encode %s %dx%d: startWriting %d, %d frames appended, status %ld, error %s, file %llu bytes, %.0f ms (%.1f ms a frame)", codec.UTF8String, w, h, ok,
	    sent, (long)wr.status, wr.error ? wr.error.description.UTF8String : "none", [[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil] fileSize],
	    ms, ms / 30);
	return wr.status != AVAssetWriterStatusCompleted;
}

static int
encode_all(int argc, char **argv)
{
	alarm(240);
	if (argc > 5)           /* encode CODEC W H MODE: the encoder is registered in THIS process too (AVAssetWriter looks for it here as well) */
		venc_register(atoi(argv[5]));
	if (argc > 2) {
		NSString *c = strcmp(argv[2], "hevc") == 0 ? AVVideoCodecTypeHEVC : AVVideoCodecTypeH264;

		return write_one(c, argc > 4 ? atoi(argv[3]) : 640, argc > 4 ? atoi(argv[4]) : 480);
	}
	int rc = write_one(AVVideoCodecTypeH264, 640, 480);

	rc |= write_one(AVVideoCodecTypeH264, 1280, 720);
	rc |= write_one(AVVideoCodecTypeHEVC, 640, 480);
	rc |= write_one(AVVideoCodecTypeHEVC, 1280, 720);
	return rc;
}

/* ---- inproc (docs/video-recording-spec-3.md C.1 4): the encoder of venc.c registered HERE, and a session forced local with AllowClientProcessEncode ---- */
struct run {
	int samples, notsync;
	size_t first_size;
	unsigned subtype;
	int w, h;
	OSStatus cb_status;
	double last_pts;
	BOOL monotonic, first_sync, has_avcc;
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
	double pts = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sb));
	CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
	BOOL sync = !(att && CFArrayGetCount(att) && CFDictionaryContainsKey(CFArrayGetValueAtIndex(att, 0), kCMSampleAttachmentKey_NotSync));

	if (r->samples++ == 0) {
		CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sb);

		r->first_size = CMSampleBufferGetTotalSampleSize(sb);
		r->first_sync = sync;
		r->monotonic = YES;
		if (fd) {
			CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(fd);
			CFDictionaryRef ext = CMFormatDescriptionGetExtensions(fd);
			CFDictionaryRef atoms = ext ? CFDictionaryGetValue(ext, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms) : NULL;

			r->subtype = CMFormatDescriptionGetMediaSubType(fd);
			r->w = d.width;
			r->h = d.height;
			r->has_avcc = atoms && CFDictionaryContainsKey(atoms, CFSTR("avcC"));
		}
	} else if (pts <= r->last_pts)
		r->monotonic = NO;
	r->notsync += !sync;
	r->last_pts = pts;
}

static int
inproc(int mode, int w, int h)
{
	struct run r = { 0 };
	VTCompressionSessionRef session = NULL;
	typedef OSStatus (*CreateWithOptions)(CFAllocatorRef, int32_t, int32_t, CMVideoCodecType, CFDictionaryRef, CFDictionaryRef, CFAllocatorRef,
	    VTCompressionOutputCallback, void *, CFDictionaryRef, VTCompressionSessionRef *);
	CreateWithOptions create = dlsym(RTLD_DEFAULT, "VTCompressionSessionCreateWithOptions");
	CFStringRef *local_key = dlsym(RTLD_DEFAULT, "kVTCompressionSessionOption_AllowClientProcessEncode");
	int fmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, frames = 30;
	CFNumberRef nf = CFNumberCreate(NULL, kCFNumberIntType, &fmt), nw = CFNumberCreate(NULL, kCFNumberIntType, &w), nh = CFNumberCreate(NULL, kCFNumberIntType, &h);
	const void *sk[3] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey }, *sv[3] = { nf, nw, nh };
	CFDictionaryRef src = CFDictionaryCreate(NULL, sk, sv, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void *ok[1] = { local_key && *local_key ? *local_key : CFSTR("AllowClientProcessEncode") }, *ov[1] = { kCFBooleanTrue };
	CFDictionaryRef opts = CFDictionaryCreate(NULL, ok, ov, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	alarm(120);
	say("inproc: register %d: status %d", mode, venc_register(mode));
	OSStatus st = create ? create(NULL, w, h, 'avc1', NULL, src, NULL, output_cb, &r, opts, &session) : -1;
	say("inproc: %dx%d CreateWithOptions(AllowClientProcessEncode): status %d, factory called %u", w, h, (int)st, venc_factory_calls);
	if (st == 0 && session) {
		int bps = 1000000;
		CFNumberRef nb = CFNumberCreate(NULL, kCFNumberIntType, &bps);

		VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate, nb);
		CFRelease(nb);
		OSStatus p = VTCompressionSessionPrepareToEncodeFrames(session);
		double w0 = wall_ms();
		int sent = 0;

		for (int i = 0; i < frames; i++) {
			CVPixelBufferRef pb = NULL;
			CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			const void *ak[1] = { kCVPixelBufferIOSurfacePropertiesKey }, *av[1] = { empty };
			CFDictionaryRef attrs = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

			CVPixelBufferCreate(NULL, w, h, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &pb);
			CFRelease(attrs);
			CFRelease(empty);
			if (!pb)
				continue;
			CVPixelBufferLockBaseAddress(pb, 0);
			for (size_t pl = 0; pl < 2; pl++) {
				uint8_t *base = CVPixelBufferGetBaseAddressOfPlane(pb, pl);
				size_t rows = CVPixelBufferGetHeightOfPlane(pb, pl), cols = CVPixelBufferGetBytesPerRowOfPlane(pb, pl);

				for (size_t y = 0; y < rows; y++)
					for (size_t x = 0; x < cols; x++)
						base[y * cols + x] = (uint8_t)(x + y + i * 6 + pl * 40);
			}
			CVPixelBufferUnlockBaseAddress(pb, 0);
			if (VTCompressionSessionEncodeFrame(session, pb, CMTimeMake(i, 30), CMTimeMake(1, 30), NULL, NULL, NULL) == 0)
				sent++;
			CVPixelBufferRelease(pb);
		}
		OSStatus cf = VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
		double ms = wall_ms() - w0;
		char sub[5];

		for (int i = 0; i < 4; i++) {
			unsigned char c = (r.subtype >> (24 - 8 * i)) & 0xff;

			sub[i] = c >= 32 && c < 127 ? (char)c : '?';
		}
		sub[4] = 0;
		say("inproc: Prepare %d, %d frames given, CompleteFrames %d, %d samples out, first %zu B sync %d, format '%s' %dx%d avcC %d, pts increasing %d, not-sync %d, cb status %d",
		    (int)p, sent, (int)cf, r.samples, r.first_size, r.first_sync, sub, r.w, r.h, r.has_avcc, r.monotonic, r.notsync, (int)r.cb_status);
		say("inproc: TIME %.1f ms wall, %.1f ms a frame (gradient included)", ms, ms / frames);
		VTCompressionSessionInvalidate(session);
		CFRelease(session);
	}
	return st != 0 || r.samples == 0;
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		setvbuf(stdout, NULL, _IOLBF, 0);
		say("start %s", argc > 1 ? argv[1] : "");
		if (argc > 1 && strcmp(argv[1], "scan") == 0)
			return scan();
		if (argc > 1 && strcmp(argv[1], "inproc") == 0)
			return inproc(argc > 2 ? atoi(argv[2]) : 1, argc > 4 ? atoi(argv[3]) : 640, argc > 4 ? atoi(argv[4]) : 480);
		return encode_all(argc, argv);
	}
}
