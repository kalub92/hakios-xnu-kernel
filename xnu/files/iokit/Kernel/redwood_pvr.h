/*
 * The rabbit r1's PowerVR for programs: what iokit/Kernel/RedwoodPVR.cpp (the IOKit service and its
 * user client) and bsd/dev/redwood_pvr.c (the GPU, its firmware, and Linux's DRM interface to them)
 * know of each other. A request is one of Linux's DRM ioctls by its number -- include/uapi/drm/pvr_drm.h's
 * are 0x40 + n, drm.h's their own -- with its structure in kernel memory, read and written in place;
 * what the structure points to in the program is copied in and out by redwood_pvr.c itself.
 */
#ifndef _SYS_REDWOOD_PVR_H_
#define _SYS_REDWOOD_PVR_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A buffer's memory is the IOKit side's (an IOBufferMemoryDescriptor): redwood_pvr.c asks for it through these. */
struct redwood_pvr_bo_ops {
	void *(*make)(uint64_t bytes);                  /* wired, zeroed, whole pages; NULL if there is none */
	void (*keep)(void *memory);                     /* one more reference */
	void (*drop)(void *memory);                     /* one fewer; freed with the last */
	uint64_t (*phys)(void *memory, uint64_t offset);        /* the physical address of the 4 KiB page at `offset` */
	void *(*bytes)(void *memory);                   /* where the kernel sees it (the buffers `make` made only) */
	void (*disown)(void *owner);                    /* an imported buffer's owner (redwood_pvr_import) let go */
};

struct redwood_pvr_file;                                /* one open of the device: struct drm_file */

bool redwood_pvr_ready(void);                           /* the firmware runs and answers (boot-arg pvr=6 and all before it worked) */
void redwood_pvr_provide(const struct redwood_pvr_bo_ops *ops);
struct redwood_pvr_file *redwood_pvr_open(bool kernel); /* kernel: its pointers are the kernel's own (the self-test) */
void redwood_pvr_close(struct redwood_pvr_file *file);  /* everything it made is taken down, the firmware told */
/* 0 or an errno, Linux's for the same mistake. `size` is the structure's as the caller has it. */
int redwood_pvr_request(struct redwood_pvr_file *file, uint32_t number, void *data, uint32_t size);
/* The memory of buffer `handle`, kept once for the caller, if the buffer may be mapped by a program (else NULL). */
void *redwood_pvr_memory(struct redwood_pvr_file *file, uint32_t handle);

/* Memory made elsewhere as a buffer: 0 and a new handle (which now holds one reference to `memory` and `owner`),
 * EEXIST and the handle that owner's buffer already has (the caller's references are still its own), or an errno. */
int redwood_pvr_import(struct redwood_pvr_file *file, void *memory, uint64_t size, void *owner, uint32_t *handle);

void RedwoodPVRStart(void);                             /* RedwoodPVR.cpp: the service made and registered */

/* The requests that are not Linux's (the rest: pvr_drm.h's 0x40..0x4d, and drm.h's GEM_CLOSE 0x09, GET_CAP 0x0c,
 * SYNCOBJ_CREATE 0xbf, _DESTROY 0xc0, _WAIT 0xc3, _RESET 0xc4, _SIGNAL 0xc5, _TRANSFER 0xcc -- with a point on the
 * destination a TRANSFER adds the source's fence to the destination's). */
#define REDWOOD_PVR_FENCE_EXPORT        0x7a            /* struct redwood_pvr_fence: a syncobj's fence, as a number */
#define REDWOOD_PVR_FENCE_IMPORT        0x7b            /* ... and a number made a syncobj's fence */
#define REDWOOD_PVR_FENCE_MERGE         0x7c            /* struct redwood_pvr_fence: the syncobj's fence becomes "its own and this one" */
#define REDWOOD_PVR_FENCE_WAIT          0x7d            /* struct redwood_pvr_fence_wait */
#define REDWOOD_PVR_SURFACE             0x7e            /* struct redwood_drm_surface, as RedwoodGPU's and RedwoodMali's: an IOSurface's pixels as a buffer */

struct redwood_pvr_fence {
	uint32_t handle, pad;
	uint64_t fence;                                 /* 0: none; ~0: signalled already; else a job, done or not */
};
struct redwood_pvr_fence_wait {
	uint64_t fence;
	int64_t timeout_ns;                             /* from now; <= 0 only asks */
};

/* What a failed request comes back as from the user client: this, with the errno in the low byte. */
#define REDWOOD_PVR_ERRNO               0xe0008000u

#ifdef __cplusplus
}
#endif

#endif /* _SYS_REDWOOD_PVR_H_ */
