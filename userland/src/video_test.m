/*
 * video_test: is video recording possible, and did it work?
 *   video_test scan      every .MOV/.MP4 under /private/var/mobile/Media: size, duration, video and audio tracks
 *                        (what Camera's video mode leaves after rwtouch pressed its record button twice)
 *   video_test encode    no camera: AVAssetWriter writes 30 synthetic 640x480 frames as H.264 into /tmp/vt.mov,
 *                        and says whether the system can encode it (no frames: unused AAC branch kept for a later step)
 */
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/sysctl.h>

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
write_one(BOOL video)
{
	NSString *path = video ? @"/tmp/vt.mov" : @"/tmp/vt-audio.m4a";
	[NSFileManager.defaultManager removeItemAtPath:path error:nil];
	NSError *error = nil;
	AVAssetWriter *w = [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:path] fileType:video ? AVFileTypeQuickTimeMovie : AVFileTypeAppleM4A error:&error];
	if (!w) {
		say("encode %s: no writer: %s", video ? "H.264" : "AAC", error.description.UTF8String);
		return 1;
	}
	NSDictionary *vs = @{ AVVideoCodecKey: AVVideoCodecTypeH264, AVVideoWidthKey: @640, AVVideoHeightKey: @480 };
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
	say("encode %s: startWriting %d, %d frames appended, status %ld, error %s, file %llu bytes", video ? "H.264" : "AAC", ok,
	    sent, (long)w.status, w.error ? w.error.description.UTF8String : "none", [[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil] fileSize]);
	return w.status != AVAssetWriterStatusCompleted;
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		setvbuf(stdout, NULL, _IOLBF, 0);
		if (argc > 1 && strcmp(argv[1], "scan") == 0)
			return scan();
		return write_one(YES);
	}
}
