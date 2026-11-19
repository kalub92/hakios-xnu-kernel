/*
 * venc_h264.c: a software H.264 encoder for VideoToolbox on minih264e (third_party/minih264e.h, public domain), registered by venc.c as mode 4
 * (docs/video-recording-spec-3.md C.2). Constrained baseline, 4-byte AVCC lengths, synchronous: every EncodeFrame emits its sample before it returns.
 * The H264E encoder is created at the first frame (the bit rate and frame rate properties arrive after StartSession).
 */
#define H264E_SVC_API 0
#define H264E_MAX_THREADS 4
#define MINIH264_IMPLEMENTATION
#include "../third_party/minih264e.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <time.h>
#include "venc.h"

struct venc {
	void *session;
	int w, h;
	int bitrate, fps, gop;
	int started;
	H264E_persist_t *enc;
	H264E_scratch_t *scratch;
	H264E_create_param_t cp;
	uint8_t *y, *u, *v;             /* y: only when the source's alignment is not the encoder's */
	CMVideoFormatDescriptionRef fmt;
	unsigned frames;
	double ms_total;
};

static void *(*p_base)(void *);
static void *(*p_storage)(void *);

static struct venc *
storage(void *self)
{
	return p_storage(p_base(self));
}

static double
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void
run_in_threads(void *token, void (*job)(void *), void *data[], int n)
{
	(void)token;
	dispatch_apply((size_t)n, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) {
		job(data[i]);
	});
}

static OSStatus
h_copy_property(BaseRef o, CFStringRef key, CFAllocatorRef a, void *out)
{
	char k[100] = "?";
	static unsigned said;

	(void)o;
	(void)a;
	CFStringGetCString(key, k, sizeof(k), kCFStringEncodingUTF8);
	if (said++ < 20)
		venc_log("h264: copyProperty %s", k);
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
h_set_property(BaseRef o, CFStringRef key, CFTypeRef v)
{
	struct venc *e = storage(o);
	char k[100] = "?";
	int n = 0;
	static unsigned said;

	CFStringGetCString(key, k, sizeof(k), kCFStringEncodingUTF8);
	if (said++ < 20)
		venc_log("h264: setProperty %s", k);
	if (v && CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue(v, kCFNumberIntType, &n) && n > 0) {
		if (CFEqual(key, kVTCompressionPropertyKey_AverageBitRate))
			e->bitrate = n;
		else if (CFEqual(key, kVTCompressionPropertyKey_ExpectedFrameRate))
			e->fps = n;
		else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameInterval))
			e->gop = n;
	}
	return 0;
}

static void
h_free(struct venc *e)
{
	free(e->enc);
	free(e->scratch);
	free(e->y);
	free(e->u);
	free(e->v);
	e->enc = NULL;
	e->scratch = NULL;
	e->y = e->u = e->v = NULL;
}

static OSStatus
h_invalidate(BaseRef o)
{
	h_free(storage(o));
	return 0;
}

static void
h_finalize(BaseRef o)
{
	struct venc *e = storage(o);

	h_free(e);
	if (e->fmt)
		CFRelease(e->fmt);
	e->fmt = NULL;
}

/* (enc, session, dims, x3) */
static uint64_t
h_start(uint64_t self, uint64_t session, uint64_t dims, uint64_t x3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
	struct venc *e = storage((void *)(uintptr_t)self);
	int pf[2] = { kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange };
	CFNumberRef n[2] = { CFNumberCreate(NULL, kCFNumberIntType, &pf[0]), CFNumberCreate(NULL, kCFNumberIntType, &pf[1]) };
	CFArrayRef arr = CFArrayCreate(NULL, (const void **)n, 2, &kCFTypeArrayCallBacks);
	CFDictionaryRef d = CFDictionaryCreate(NULL, (const void **)&kCVPixelBufferPixelFormatTypeKey, (const void **)&arr, 1, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	OSStatus st = -1;

	(void)x3; (void)a4; (void)a5; (void)a6; (void)a7;
	e->session = (void *)(uintptr_t)session;
	e->w = (int)(uint32_t)dims;
	e->h = (int)(dims >> 32);
	e->gop = 30;
	venc_log("h264: StartSession %dx%d", e->w, e->h);
	if (venc_set_pba)
		st = venc_set_pba(e->session, d);
	CFRelease(d);
	CFRelease(arr);
	CFRelease(n[0]);
	CFRelease(n[1]);
	if (st != 0 || (e->w | e->h) & 1)
		return (uint64_t)(int64_t)-12902;
	return 0;
}

static int
h_init(struct venc *e, int dur_fps)
{
	int ps = 0, ss = 0, fps = e->fps > 0 ? e->fps : dur_fps > 0 ? dur_fps : 30;
	int bitrate = e->bitrate > 0 ? e->bitrate : (int)((long long)e->w * e->h * fps / 14);   /* ~2 bit per 28 pixels: 1080p10 1.5 Mbit/s */

	memset(&e->cp, 0, sizeof(e->cp));
	e->cp.width = e->w;
	e->cp.height = e->h;
	e->cp.gop = e->gop;
	e->cp.vbv_size_bytes = bitrate / 8;
	e->cp.const_input_flag = 1;             /* never write into the camera's buffer */
	e->cp.fine_rate_control_flag = 0;
	e->cp.max_threads = 4;
	e->cp.token = NULL;
	e->cp.run_func_in_thread = run_in_threads;
	e->bitrate = bitrate;
	e->fps = fps;
	if (H264E_sizeof(&e->cp, &ps, &ss) != 0)
		return -1;
	e->enc = malloc((size_t)ps);
	e->scratch = malloc((size_t)ss);
	e->u = malloc((size_t)(e->w / 2) * (e->h / 2));
	e->v = malloc((size_t)(e->w / 2) * (e->h / 2));
	if (!e->enc || !e->scratch || !e->u || !e->v || H264E_init(e->enc, &e->cp) != 0)
		return -1;
	venc_log("h264: init %dx%d %d fps %d bit/s gop %d (persist %d, scratch %d bytes)", e->w, e->h, fps, bitrate, e->gop, ps, ss);
	return 0;
}

/* an Annex-B buffer as NAL units: calls back for each (without the start code) */
static void
split_nals(const uint8_t *p, int n, void (*cb)(const uint8_t *, int, void *), void *arg)
{
	int start = -1;

	for (int i = 0; i + 2 < n; i++) {
		if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
			if (start >= 0) {
				int end = i;

				while (end > start && p[end - 1] == 0)
					end--;
				cb(p + start, end - start, arg);
			}
			start = i + 3;
			i += 2;
		}
	}
	if (start >= 0 && start < n)
		cb(p + start, n - start, arg);
}

struct frame_nals {
	const uint8_t *sps, *pps;
	int sps_n, pps_n, idr;
	uint8_t *avcc;
	int avcc_n;
};

static void
collect(const uint8_t *nal, int n, void *arg)
{
	struct frame_nals *f = arg;
	int type = nal[0] & 31;

	if (n <= 0)
		return;
	if (type == 7) {
		f->sps = nal;
		f->sps_n = n;
	} else if (type == 8) {
		f->pps = nal;
		f->pps_n = n;
	} else if (type == 1 || type == 5) {
		f->idr |= type == 5;
		f->avcc[f->avcc_n] = (uint8_t)(n >> 24);
		f->avcc[f->avcc_n + 1] = (uint8_t)(n >> 16);
		f->avcc[f->avcc_n + 2] = (uint8_t)(n >> 8);
		f->avcc[f->avcc_n + 3] = (uint8_t)n;
		memcpy(f->avcc + f->avcc_n + 4, nal, (size_t)n);
		f->avcc_n += 4 + n;
	}
}

static void
emit_fail(struct venc *e, void *token, OSStatus st)
{
	venc_emit(e->session, token, st, 0, NULL);
}

/* (enc, token, ib, *pts, *dur, props, *flags) */
static uint64_t
h_encode(uint64_t self, uint64_t token_, uint64_t ib_, uint64_t pts_, uint64_t dur_, uint64_t props_, uint64_t flags, uint64_t a7)
{
	struct venc *e = storage((void *)(uintptr_t)self);
	void *token = (void *)(uintptr_t)token_;
	CVPixelBufferRef ib = (CVPixelBufferRef)(uintptr_t)ib_;
	const CMTime *pts = (const CMTime *)(uintptr_t)pts_, *dur = (const CMTime *)(uintptr_t)dur_;
	CFDictionaryRef props = (CFDictionaryRef)(uintptr_t)props_;
	double t0 = now_ms();

	(void)flags; (void)a7;
	if (!e->enc && h_init(e, dur && dur->value > 0 && dur->timescale > 0 ? (int)((dur->timescale + dur->value / 2) / dur->value) : 0) != 0) {
		venc_log("h264: init failed");
		emit_fail(e, token, -12902);
		return 0;
	}
	if (CVPixelBufferGetPlaneCount(ib) != 2 || (int)CVPixelBufferGetWidth(ib) < e->w || (int)CVPixelBufferGetHeight(ib) < e->h) {
		venc_log("h264: unexpected buffer: planes %zu %zux%zu", CVPixelBufferGetPlaneCount(ib), CVPixelBufferGetWidth(ib), CVPixelBufferGetHeight(ib));
		emit_fail(e, token, -12902);
		return 0;
	}
	CVPixelBufferLockBaseAddress(ib, kCVPixelBufferLock_ReadOnly);
	const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(ib, 0), *cbcr = CVPixelBufferGetBaseAddressOfPlane(ib, 1);
	size_t ys = CVPixelBufferGetBytesPerRowOfPlane(ib, 0), cs = CVPixelBufferGetBytesPerRowOfPlane(ib, 1);
	int cw = e->w / 2, ch = e->h / 2;

	for (int r = 0; r < ch; r++) {
		const uint8_t *s = cbcr + (size_t)r * cs;
		uint8_t *du = e->u + (size_t)r * cw, *dv = e->v + (size_t)r * cw;
		int x = 0;

		for (; x + 16 <= cw; x += 16) {
			uint8x16x2_t uv = vld2q_u8(s + 2 * x);

			vst1q_u8(du + x, uv.val[0]);
			vst1q_u8(dv + x, uv.val[1]);
		}
		for (; x < cw; x++) {
			du[x] = s[2 * x];
			dv[x] = s[2 * x + 1];
		}
	}
	if (((uintptr_t)y & 7) || (ys & 7)) {           /* the encoder wants 8-byte aligned planes */
		if (!e->y)
			e->y = malloc((size_t)e->w * e->h);
		for (int r = 0; r < e->h; r++)
			memcpy(e->y + (size_t)r * e->w, y + r * ys, (size_t)e->w);
		y = e->y;
		ys = (size_t)e->w;
	}
	H264E_io_yuv_t in = { { (uint8_t *)y, e->u, e->v }, { (int)ys, cw, cw } };
	H264E_run_param_t rp;
	uint8_t *coded = NULL;
	int coded_n = 0;
	int force = e->frames == 0 || (props && CFDictionaryGetValue(props, kVTEncodeFrameOptionKey_ForceKeyFrame) == kCFBooleanTrue);

	memset(&rp, 0, sizeof(rp));
	rp.frame_type = force ? H264E_FRAME_TYPE_KEY : H264E_FRAME_TYPE_DEFAULT;
	rp.encode_speed = 8;
	rp.desired_frame_bytes = e->bitrate / 8 / e->fps;
	rp.qp_min = 10;
	rp.qp_max = 44;
	int rc = H264E_encode(e->enc, e->scratch, &rp, &in, &coded, &coded_n);

	CVPixelBufferUnlockBaseAddress(ib, kCVPixelBufferLock_ReadOnly);
	if (rc != 0 || !coded || coded_n <= 0) {
		venc_log("h264: H264E_encode %d (%d bytes)", rc, coded_n);
		emit_fail(e, token, -12902);
		return 0;
	}
	struct frame_nals f = { 0 };

	f.avcc = malloc((size_t)coded_n + 64);
	split_nals(coded, coded_n, collect, &f);
	if (f.sps && f.pps && (!e->fmt || f.idr)) {
		const uint8_t *sets[2] = { f.sps, f.pps };
		size_t sizes[2] = { (size_t)f.sps_n, (size_t)f.pps_n };
		CMVideoFormatDescriptionRef fd = NULL;

		if (!e->fmt && CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, sets, sizes, 4, &fd) == 0)
			e->fmt = fd;
	}
	if (!e->fmt || f.avcc_n == 0) {
		venc_log("h264: no format (sps %d pps %d) or no slices (%d bytes)", f.sps_n, f.pps_n, f.avcc_n);
		free(f.avcc);
		emit_fail(e, token, -12902);
		return 0;
	}
	CMBlockBufferRef bb = NULL;
	CMSampleBufferRef sb = NULL;
	CMSampleTimingInfo timing = { dur ? *dur : kCMTimeInvalid, *pts, *pts };
	size_t size = (size_t)f.avcc_n;
	OSStatus st = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, size, NULL, NULL, 0, size, kCMBlockBufferAssureMemoryNowFlag, &bb);

	if (st == 0)
		st = CMBlockBufferReplaceDataBytes(f.avcc, bb, 0, size);
	if (st == 0)
		st = CMSampleBufferCreateReady(NULL, bb, e->fmt, 1, 1, &timing, 1, &size, &sb);
	free(f.avcc);
	if (bb)
		CFRelease(bb);
	if (st != 0 || !sb) {
		venc_log("h264: sample buffer: %d", (int)st);
		emit_fail(e, token, -12902);
		return 0;
	}
	if (!f.idr) {
		CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, true);

		if (att && CFArrayGetCount(att)) {
			CFMutableDictionaryRef d = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(att, 0);

			CFDictionarySetValue(d, kCMSampleAttachmentKey_NotSync, kCFBooleanTrue);
			CFDictionarySetValue(d, kCMSampleAttachmentKey_DependsOnOthers, kCFBooleanTrue);
		}
	}
	st = venc_emit(e->session, token, 0, 0, sb);
	CFRelease(sb);
	e->frames++;
	e->ms_total += now_ms() - t0;
	if (e->frames <= 3 || e->frames % 30 == 0)
		venc_log("h264: frame %u: %d bytes%s, %.1f ms (avg %.1f), emit %d", e->frames, coded_n, f.idr ? " IDR" : "", now_ms() - t0, e->ms_total / e->frames, (int)st);
	return 0;
}

static uint64_t
h_complete(uint64_t self, uint64_t until, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
	(void)until; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
	venc_log("h264: CompleteFrames (%u frames)", storage((void *)(uintptr_t)self)->frames);
	return 0;
}

static const struct vt_base_class h_base = { 1, sizeof(struct venc), NULL, h_invalidate, h_finalize, NULL, h_copy_property, h_set_property, NULL, NULL, NULL };
static const struct vt_encoder_class h_class = { 3, { h_start, h_encode, h_complete, NULL, NULL, NULL, NULL, NULL } };
static const struct vt_vtable h_vtable = { NULL, &h_base, &h_class };

OSStatus
venc_h264_factory(CMVideoCodecType codec, CFAllocatorRef alloc, void **out)
{
	(void)codec;
	venc_factory_calls++;
	venc_log("factory called (h264 minih264e) #%u", venc_factory_calls);
	if (!p_base)
		p_base = dlsym(RTLD_DEFAULT, "VTVideoEncoderGetCMBaseObject");
	if (!p_storage)
		p_storage = dlsym(RTLD_DEFAULT, "CMBaseObjectGetDerivedStorage");
	if (!p_base || !p_storage)
		return -1;
	return venc_make_object(alloc, &h_vtable, out);
}
