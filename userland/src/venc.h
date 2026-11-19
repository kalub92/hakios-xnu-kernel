/* venc.c: run-time registered VideoToolbox encoders (docs/video-recording-spec-3.md C) */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdint.h>
void venc_log(const char *fmt, ...);
int venc_register(int mode);            /* 1 null, 2 VideoProcessing H.264, 3 VCPHEVC, 4 venc_h264.c */
OSStatus venc_h264_factory(CMVideoCodecType codec, CFAllocatorRef alloc, void **out);
extern unsigned venc_factory_calls;

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

typedef OSStatus (*EncFactory)(CMVideoCodecType, CFAllocatorRef, void **);
OSStatus venc_make_object(CFAllocatorRef alloc, const struct vt_vtable *vt, void **out);
extern OSStatus (*venc_set_pba)(void *, CFDictionaryRef);
extern OSStatus (*venc_emit)(void *, void *, OSStatus, VTEncodeInfoFlags, CMSampleBufferRef);
