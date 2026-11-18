/*
 * ---- Linux's DRM interface to the PowerVR, for programs (pvr=6) ----
 *
 * NOT a file of its own to the build: bsd/dev/redwood_pvr.c includes it, so that everything above
 * stays private to one object (do not list it in bsd/conf/files).
 *
 * What Mesa's pvr driver asks of Linux's drivers/gpu/drm/imagination (v7.3), answered from here:
 * include/uapi/drm/pvr_drm.h's fourteen requests and the handful of drm.h's that go with them. The
 * IOKit side (iokit/Kernel/RedwoodPVR.cpp) owns the connection and the buffers' memory; everything
 * else is here, in C, where the host check can run it. Named throughout: pvr_drv.c (the ioctls),
 * pvr_gem.c, pvr_vm.c, pvr_context.c, pvr_free_list.c, pvr_hwrt.c, pvr_job.c, pvr_queue.c, pvr_cccb.c,
 * pvr_stream.c with pvr_stream_defs.c (through redwood_pvr_fwif_gen.h), pvr_sync.c.
 *
 * One lock around all of it. A job goes into its context's client command buffer as soon as there
 * is room -- in the order submitted, one queue for each of a context's data masters -- and what it
 * must wait for goes in front of it as a fence the FIRMWARE waits on (a job's number on its queue's
 * timeline word: pvr_queue_submit_job_to_cccb's FENCE_PR), which is how Linux treats one job of its
 * own waiting on another; so there is no scheduler thread here, only "is there room yet". A job is
 * done when the firmware has written its number, or a later one, into the timeline
 * (pvr_queue_signal_done_fences); with no interrupt that is seen by whoever looks next: a waiter
 * every millisecond, the service thread at least ten times a second.
 *
 * A geometry job with its fragment job right behind it in one call, on the same context and render
 * target data, is a pair as in Linux (pvr_job.c can_combine_jobs): both go into their buffers together
 * and one COMBINED_GEOM_FRAG_KICK starts them.
 *
 * A job the firmware does not finish in pvr_timeout seconds (boot-arg; 10) is a hang: what the
 * firmware says of itself is printed, the job's context is marked faulty (its later submissions get
 * EIO, pvr_queue_job_init), every job that was running is given up -- its fences signal -- and the
 * GPU is reset as pvr_power.c's pvr_power_reset does a hard reset: everything the firmware can see
 * zeroed and written again (pvr_fw_hard_reset), the image loaded again, the firmware started again.
 * The same when the firmware stops taking commands (the watchdog) or asks for a restart. A GPU that
 * does not come back is lost: every request EIO from then on (pvr_device_lost).
 *
 * Not Linux: a fence is a job, or several merged (a TRANSFER onto a timeline point adds the source's
 * fence to what the destination already waits for; the points themselves are not kept, and GET_CAP
 * says there are no timelines); a hang is found by the clock here, where Linux mostly leaves it to
 * the firmware's own recovery and a minute's scheduler timeout; unmapping takes whole mappings only,
 * which is all Linux's pvr_vm_unmap finds too. Boot-arg pvr_debug=1 says every job's life in the log.
 */

#define DRM_BOS         8192
#define DRM_VMS         8
#define DRM_CTXS        32
#define DRM_FLS         256
#define DRM_HWRTS       256
#define DRM_SYNCS       2048
#define DRM_ALL_CTXS    64
#define DRM_JOB_DEPS    64
#define DRM_MERGE_DEPTH 32
#define DRM_ALL_VMS     32
#define DRM_ALL_HWRTS   512
#define DRM_BO_MAX      (256ull << 20)
#define FENCE_SIGNALLED (~0ull)

struct drm_bo {
	void *memory;
	void *owner;                    /* an IOSurface it is the pixels of (REDWOOD_PVR_SURFACE), held with it */
	uint64_t size, flags;
	uint32_t refs;                  /* its handle, each mapping, each free list on it */
};
struct drm_map {
	struct drm_map *next;
	struct drm_bo *bo;
	uint64_t addr, size, offset;
};
struct drm_vm {
	struct pvr_vm vm;
	struct drm_map *maps;
	uint32_t refs;                  /* its handle, each context, each free list */
};
struct drm_fl {
	struct pvr_free_list fl;
	struct drm_vm *vm;
	struct drm_bo *bo;
	uint32_t refs;                  /* its handle, each render target on it */
};
struct drm_hwrt {
	struct pvr_hwrt hwrt;
	struct drm_fl *fl[2];
	uint64_t last_geom[2], last_frag[2];    /* the fences of the last jobs on each data: Linux's reservation object */
};
struct drm_ctx {
	struct pvr_context ctx;
	struct drm_vm *vm;
};
struct drm_job {
	struct drm_job *next;           /* of all the jobs not yet done */
	struct drm_job *queued;         /* of its queue's, waiting for room */
	struct drm_job *pair;           /* a geometry job's fragment job and the other way round: kicked together */
	uint64_t fence, born, kicked;   /* the times: mach_absolute_time */
	struct pvr_context *ctx;
	struct drm_hwrt *hwrt;
	uint32_t q, seqno, type, cccb_type, cmd_len, hwrt_index, deps;
	bool written;
	uint64_t dep[DRM_JOB_DEPS];
	uint8_t cmd[GEN_SIZEOF_rogue_fwif_ccb_cmd_header + stream_frag_DEST_SIZE];      /* the largest of the four */
};
struct redwood_pvr_file {
	bool kernel;
	struct drm_bo *bo[DRM_BOS];
	struct drm_vm *vm[DRM_VMS];
	struct drm_ctx *ctx[DRM_CTXS];
	struct drm_fl *fl[DRM_FLS];
	struct drm_hwrt *hwrt[DRM_HWRTS];
	uint64_t sync[DRM_SYNCS];       /* a syncobj's fence, + 1 so that 0 is "no such syncobj": see SYNC_* */
};
#define SYNC_FREE       0ull
#define SYNC_EMPTY      1ull            /* made, no fence yet */
#define SYNC_OF(fence)  ((fence) == FENCE_SIGNALLED ? FENCE_SIGNALLED : (fence) + 1)
#define FENCE_OF(sync)  ((sync) == FENCE_SIGNALLED ? FENCE_SIGNALLED : (sync) - 1)

struct drm_merge {                      /* a fence that is two */
	struct drm_merge *next;
	uint64_t fence, of[2];
	uint32_t depth;
};
static struct drm_merge *drm_merges;
#define DRM_UNMAPPED    64
static struct drm_unmapped {            /* the last unmappings, for saying of a faulting address that it WAS mapped */
	const struct pvr_vm *vm;
	uint64_t addr, size, when;
} drm_unmapped[DRM_UNMAPPED];
static struct {                         /* what there is now, for the statistics: a leak shows here */
	uint32_t files, bos, imported, maps, syncs, ctxs, fls, hwrts;
	uint32_t bucket[4];             /* the buffers made here, by size: to 64 KiB, to 1 MiB, to 16 MiB, larger */
	uint64_t bo_bytes, imported_bytes, bucket_bytes[4];
} drm_live;
#define DRM_REQUESTS    28
static struct {                         /* each kind of request since the last statistics line */
	uint32_t count, most_us;
	uint64_t total_us;
} drm_calls[DRM_REQUESTS];
static uint32_t drm_hold_most_us;       /* the longest the lock was held by one request, its sleeps apart */
static uint64_t drm_slept;              /* by the request running now, in mach_absolute_time's units */
static uint32_t drm_unmappings, drm_faults;      /* drm_faults: contexts the firmware reset on its own */
static void drm_queue_say(const struct pvr_context *ctx, unsigned q);
static bool drm_irq_from;               /* what is being looked at now was brought by the interrupt */
static struct drm_vm *drm_vms[DRM_ALL_VMS];             /* every client's, for a reset to write again */
static struct drm_hwrt *drm_hwrts[DRM_ALL_HWRTS];
static uint32_t drm_timeout_ms = 10000;
static bool drm_debug;
static uint64_t drm_fences = 1;
static struct pvr_context *drm_contexts[DRM_ALL_CTXS];          /* every client's, for the firmware's notifications and for starting jobs */

/* ---- memory ---- */

static void *
drm_new(size_t size)
{
	return kalloc_data(size, Z_WAITOK | Z_ZERO);
}

static void *bo_make(uint64_t bytes) { return bo_ops != NULL ? bo_ops->make(bytes) : NULL; }
static void bo_drop(void *memory) { bo_ops->drop(memory); }
static uint64_t bo_phys(void *memory, uint64_t offset) { return bo_ops->phys(memory, offset); }

/* A program's memory read or written; the kernel's own if the file is the self-test's. 0 or EFAULT. */
static int
drm_in(struct redwood_pvr_file *file, uint64_t from, void *to, size_t size)
{
	if (file->kernel) {
		memcpy(to, (const void *)(uintptr_t)from, size);
		return 0;
	}
	return copyin((user_addr_t)from, to, size) != 0 ? EFAULT : 0;
}

static int
drm_out(struct redwood_pvr_file *file, const void *from, uint64_t to, size_t size)
{
	if (file->kernel) {
		memcpy((void *)(uintptr_t)to, from, size);
		return 0;
	}
	return copyout(from, (user_addr_t)to, size) != 0 ? EFAULT : 0;
}

/* pvr_drv.c's pvr_set_uobj: an answer of `size` bytes into the program's `theirs`, the rest of theirs zeroed */
static int
drm_answer(struct redwood_pvr_file *file, uint64_t to, uint32_t theirs, const void *from, uint32_t size)
{
	static const uint8_t zeroes[64];
	int error = drm_out(file, from, to, theirs < size ? theirs : size);

	for (uint32_t done = size; error == 0 && done < theirs; done += sizeof(zeroes)) {
		error = drm_out(file, zeroes, to + done, theirs - done < sizeof(zeroes) ? theirs - done : sizeof(zeroes));
	}
	return error;
}

/*
 * Buffers' memory, made and given back. Three things a window server's frames showed (1600 buffers
 * made and closed in ten seconds): making wired, zeroed memory and giving it back are slow, and were
 * done with everybody's lock held; and every unmapping waited for its own MMU cache flush.
 *
 *   - Memory is made (zeroed) before the lock is taken (drm_premade), and given back after it is
 *     let go (drm_reap).
 *   - A buffer whose last use has gone is "dead" until an MMU cache flush newer than its unmapping
 *     has been answered: only then may its pages be used for anything else, since until then the
 *     GPU may still hold the translation. So an unmapping no longer sends a flush of its own
 *     (Linux's pvr_mmu_op_context_destroy does, "Unmaps should be flushed immediately", because it
 *     frees the pages at once); the next submission's flush, or the service thread's within 2 ms,
 *     covers all the unmappings before it.
 *
 * There was a pool of given-back memory here for the next buffer of the same size. On the device it
 * never held anything: it kept nothing while the system had under 6000 free pages, and this system
 * runs at 150 to 1300; and Mesa now keeps its own buffers, so few are made (56 in ten seconds).
 * Wired memory held back in the kernel is the last thing a 1 GB machine at 4 MB free wants: gone.
 */
#define DRM_DEAD        512
extern unsigned int vm_page_free_count;         /* osfmk/vm/vm_page.h: said in the statistics */

static struct drm_dead {
	void *memory, *owner;
	uint64_t bytes;
	uint32_t seq;                   /* the unmappings there had been when it died */
} drm_dead[DRM_DEAD];
static uint32_t drm_dead_count;
static uint64_t drm_dead_bytes, drm_dead_since;
static bool drm_memory_said;
static void *drm_premade;               /* CREATE_BO's memory, made before the lock was taken */

static unsigned
drm_bucket(uint64_t bytes)
{
	return bytes <= (64u << 10) ? 0 : bytes <= (1u << 20) ? 1 : bytes <= (16u << 20) ? 2 : 3;
}

/* With the lock: a buffer's last use gone. Its memory waits, dead, for a flush newer than now. */
static void
drm_bo_put(struct drm_bo *bo)
{
	if (--bo->refs != 0) {
		return;
	}
	drm_live.bos--;
	drm_live.bo_bytes -= bo->size;
	if (bo->owner != NULL) {
		drm_live.imported--;
		drm_live.imported_bytes -= bo->size;
	} else {
		drm_live.bucket[drm_bucket(bo->size)]--;
		drm_live.bucket_bytes[drm_bucket(bo->size)] -= bo->size;
	}
	if (drm_dead_count == DRM_DEAD) {       /* no room to wait in: the flush now, and this one given back at once (it does not happen at 512) */
		(void)pvr_mmu_flush();
		if (bo->owner != NULL) {
			bo_ops->disown(bo->owner);
		}
		bo_drop(bo->memory);
	} else {
		if (drm_dead_count == 0) {
			drm_dead_since = mach_absolute_time();
		}
		drm_dead[drm_dead_count++] = (struct drm_dead) { bo->memory, bo->owner, bo->size, drm_unmap_seq };
		drm_dead_bytes += bo->size;
	}
	kfree_data(bo, sizeof(*bo));
}

/* No lock held: the dead whose flush has been answered, given back. */
static void
drm_reap(void)
{
	struct drm_dead batch[16];
	uint32_t n;

	if (drm_lock == NULL) {
		return;
	}
	do {
		uint32_t kept = 0;

		n = 0;
		lck_mtx_lock(drm_lock);
		for (uint32_t i = 0; i < drm_dead_count; i++) {
			if (n < 16 && (int32_t)(drm_flushed_seq - drm_dead[i].seq) >= 0) {
				batch[n++] = drm_dead[i];
				drm_dead_bytes -= drm_dead[i].bytes;
			} else {
				drm_dead[kept++] = drm_dead[i];
			}
		}
		drm_dead_count = kept;
		lck_mtx_unlock(drm_lock);
		for (uint32_t i = 0; i < n; i++) {
			if (batch[i].owner != NULL) {
				bo_ops->disown(batch[i].owner);
				bo_drop(batch[i].memory);
			} else {
				bo_drop(batch[i].memory);
			}
		}
	} while (n == 16);
}

static void
drm_vm_put(struct drm_vm *vm)           /* pvr_vm_context_put: with the last reference everything in it is unmapped */
{
	if (--vm->refs != 0) {
		return;
	}
	while (vm->maps != NULL) {
		struct drm_map *map = vm->maps;

		vm->maps = map->next;
		drm_live.maps--;
		pvr_vm_unmap(&vm->vm, map->addr, map->size);
		drm_unmap_seq++;
		drm_bo_put(map->bo);
		kfree_data(map, sizeof(*map));
	}
	for (unsigned i = 0; i < DRM_ALL_VMS; i++) {
		if (drm_vms[i] == vm) {
			drm_vms[i] = NULL;
		}
	}
	pvr_vm_destroy(&vm->vm);
	(void)pvr_mmu_flush();
	kfree_data(vm, sizeof(*vm));
}

static void
drm_fl_put(struct drm_fl *fl)
{
	if (--fl->refs != 0) {
		return;
	}
	if (!pvr_free_list_destroy(&fl->fl)) {
		SAY("pvr: a free list the firmware will not let go of is left as it is (list %u)", fl->fl.id);
		return;                 /* its memory stays: the firmware may still write there */
	}
	(void)pvr_mmu_flush();
	drm_live.fls--;
	drm_bo_put(fl->bo);
	drm_vm_put(fl->vm);
	kfree_data(fl, sizeof(*fl));
}

/* ---- fences and jobs ---- */

static struct drm_job *
drm_job_of(uint64_t fence)              /* the job, if it is not done yet */
{
	for (struct drm_job *job = drm_jobs; job != NULL && fence != 0 && fence != FENCE_SIGNALLED; job = job->next) {
		if (job->fence == fence) {
			return job;
		}
	}
	return NULL;
}

/* Is there nothing left to wait for? A job not done is not; a merge is when both its halves are; anything else has been forgotten: done. */
static bool
drm_fence_done(uint64_t fence)
{
	if (drm_job_of(fence) != NULL) {
		return false;
	}
	for (const struct drm_merge *m = drm_merges; m != NULL; m = m->next) {
		if (m->fence == fence) {
			return drm_fence_done(m->of[0]) && drm_fence_done(m->of[1]);    /* DRM_MERGE_DEPTH deep at most */
		}
	}
	return true;
}

/* One fence for two: 0 if that cannot be had (no memory, or merges of merges too deep). */
static uint64_t
drm_fence_merge(uint64_t a, uint64_t b)
{
	struct drm_merge *m;
	uint32_t depth = 1;

	if (a == 0 || drm_fence_done(a)) {
		return b;
	}
	if (b == 0 || drm_fence_done(b) || a == b) {
		return a;
	}
	for (m = drm_merges; m != NULL; m = m->next) {
		if ((m->fence == a || m->fence == b) && m->depth >= depth) {
			depth = m->depth + 1;
		}
	}
	if (depth > DRM_MERGE_DEPTH || (m = drm_new(sizeof(*m))) == NULL) {
		return 0;
	}
	m->fence = drm_fences++;
	m->of[0] = a;
	m->of[1] = b;
	m->depth = depth;
	m->next = drm_merges;
	drm_merges = m;
	return m->fence;
}

static uint32_t
drm_us_since(uint64_t then)
{
	uint64_t ns = 0;

	absolutetime_to_nanoseconds(mach_absolute_time() - then, &ns);
	return ns / NSEC_PER_USEC > 0xffffffffu ? 0xffffffffu : (uint32_t)(ns / NSEC_PER_USEC);
}

static uint32_t
drm_ms_since(uint64_t then)
{
	return drm_us_since(then) / 1000;
}

/*
 * How long things take (boot-arg pvr_stats=1), in microseconds: for each kind of job from its
 * submission to its kick and from its kick to its being found done -- found, so the time the GPU
 * took and the time this took to notice, together -- and from a job's being found done to a waiter
 * going on. The last 256 of each, of which every ten seconds the least, the middle and the most.
 */
#define STATS_KEPT 256
enum { STAT_KICK, STAT_DONE = 4, STAT_WOKEN = 8, STATS };       /* + the job's type for the first two */
static struct drm_stat {
	uint32_t us[STATS_KEPT], count;
} drm_stat[STATS];
static bool drm_stats;
static uint64_t drm_last_done;
static uint32_t drm_irqs, drm_irqs_idle, drm_polls_found, drm_irqs_found;

static void
drm_stat_add(unsigned which, uint32_t us)
{
	if (drm_stats) {
		drm_stat[which].us[drm_stat[which].count++ % STATS_KEPT] = us;
	}
}

static void
drm_stats_woken(void)
{
	if (drm_last_done != 0) {
		drm_stat_add(STAT_WOKEN, drm_us_since(drm_last_done));
		drm_last_done = 0;
	}
}

/* Where the memory is, by kind. */
static void
drm_memory_say(const char *when)
{
	uint64_t lists = 0;

	for (unsigned i = 0; i < FREE_LISTS; i++) {
		for (unsigned n = 0; pvr_free_lists[i] != NULL && n < pvr_free_lists[i]->nodes; n++) {
			lists += (uint64_t)pvr_free_lists[i]->node[n].pages << 2;
		}
	}
	SAY("pvr: stats: memory %s: buffers to 64 KiB %u (%llu KiB), to 1 MiB %u (%llu KiB), to 16 MiB %u (%llu KiB), larger %u (%llu KiB); surfaces %u "
	    "(%llu KiB, not ours)", when, drm_live.bucket[0], drm_live.bucket_bytes[0] >> 10, drm_live.bucket[1], drm_live.bucket_bytes[1] >> 10,
	    drm_live.bucket[2], drm_live.bucket_bytes[2] >> 10, drm_live.bucket[3], drm_live.bucket_bytes[3] >> 10, drm_live.imported,
	    drm_live.imported_bytes >> 10);
	SAY("pvr: stats: memory %s: free lists %llu KiB, page tables %u KiB, arena %u of %u KiB wired; awaiting a flush %u (%llu KiB); "
	    "system free pages %u", when, lists, pt_pages << 2, arena_used >> 10, ARENA_SIZE >> 10,
	    drm_dead_count, drm_dead_bytes >> 10, vm_page_free_count);
}

static void
drm_stats_say(void)
{
	static const char *const names[STATS] = { "geometry: submit to kick", "fragment: submit to kick", "compute: submit to kick",
		                                  "transfer: submit to kick", "geometry: kick to done", "fragment: kick to done",
		                                  "compute: kick to done", "transfer: kick to done", "done to waiter woken" };
	static uint32_t sorted[STATS_KEPT];

	for (unsigned which = 0; drm_stats && which < STATS; which++) {
		uint32_t n = drm_stat[which].count < STATS_KEPT ? drm_stat[which].count : STATS_KEPT;

		if (n == 0) {
			continue;
		}
		for (uint32_t i = 0; i < n; i++) {      /* an insertion sort of at most 256, once in ten seconds */
			uint32_t v = drm_stat[which].us[i], j = i;

			for (; j > 0 && sorted[j - 1] > v; j--) {
				sorted[j] = sorted[j - 1];
			}
			sorted[j] = v;
		}
		SAY("pvr: stats: %s: %u us least, %u middle, %u most (of the last %u; %u in all)", names[which], sorted[0], sorted[n / 2],
		    sorted[n - 1], n, drm_stat[which].count);
		drm_stat[which].count = 0;
	}
	if (drm_stats) {
		drm_memory_say("now");
	}
	if (drm_stats) {
		static const char *const drm_call_names[DRM_REQUESTS] = { "DEV_QUERY", "CREATE_BO", "MMAP_OFFSET", "CREATE_VM", "DESTROY_VM", "VM_MAP",
			"VM_UNMAP", "CREATE_CONTEXT", "DESTROY_CONTEXT", "CREATE_FREE_LIST", "DESTROY_FREE_LIST", "CREATE_HWRT", "DESTROY_HWRT", "SUBMIT_JOBS",
			"GEM_CLOSE", "GET_CAP", "SYNCOBJ_CREATE", "SYNCOBJ_DESTROY", "SYNCOBJ_WAIT", "SYNCOBJ_RESET", "SYNCOBJ_SIGNAL", "SYNCOBJ_TRANSFER",
			"FENCE_EXPORT", "FENCE_IMPORT", "FENCE_MERGE", "FENCE_WAIT", "?", "other" };
		uint32_t jobs = 0, written = 0, merges = 0, pages = 0, lists = 0, run = 0, largest = 0, calls = 0;
		char line[180];
		size_t used = 0;

		for (const struct drm_job *job = drm_jobs; job != NULL; job = job->next) {
			jobs++;
			written += job->written;
		}
		for (const struct drm_merge *m = drm_merges; m != NULL; m = m->next) {
			merges++;
		}
		for (unsigned i = 0; i < FREE_LISTS; i++) {
			if (pvr_free_lists[i] != NULL) {
				lists++;
				pages += pvr_free_lists[i]->current_pages + pvr_free_lists[i]->ready_pages;
			}
		}
		for (uint32_t page = 0; page < ARENA_PAGES; page++) {
			run = arena_map[page] != 0 ? 0 : run + 1;
			largest = run > largest ? run : largest;
		}
		SAY("pvr: stats: %u clients; buffers %u (%llu KiB, %u of them surfaces), mappings %u, GPU page-table pages %u; syncobjs %u; jobs out %u "
		    "(%u kicked), merged fences %u", drm_live.files, drm_live.bos, drm_live.bo_bytes >> 10, drm_live.imported, drm_live.maps, pt_pages,
		    drm_live.syncs, jobs, written, merges);
		SAY("pvr: stats: contexts %u, render targets %u, free lists %u (%u here, %u pages); arena %u KiB of %u used, the largest free piece %u KiB; "
		    "jobs so far %u in, %u done; GPU resets %u, faults %u", drm_live.ctxs, drm_live.hwrts, drm_live.fls, lists, pages, arena_used >> 10,
		    ARENA_SIZE >> 10, largest << 2, drm_submitted, drm_finished, drm_resets, drm_faults);
		SAY("pvr: stats: MMU flushes %u taking %u us in all, %u the longest; \"busy\" from the firmware %u times; the lock held %u us at the longest",
		    mmu_flushes, mmu_flush_us, mmu_flush_most, cleanup_busy, drm_hold_most_us);
		mmu_flushes = mmu_flush_us = mmu_flush_most = cleanup_busy = drm_hold_most_us = 0;
		/* each kind of request made: how often, microseconds in all (the wait for the lock with them), the longest */
		for (unsigned i = 0; i < DRM_REQUESTS; i++) {
			if (drm_calls[i].count == 0) {
				continue;
			}
			calls++;
			used += (size_t)snprintf(line + used, sizeof(line) - used, "%s%s %u: %llu/%u", used != 0 ? "; " : "", drm_call_names[i],
			    drm_calls[i].count, drm_calls[i].total_us, drm_calls[i].most_us);
			if (used > 100) {
				SAY("pvr: stats: requests (count: us in all/longest): %s", line);
				used = 0;
			}
			drm_calls[i].count = drm_calls[i].most_us = 0;
			drm_calls[i].total_us = 0;
		}
		if (used != 0 && calls != 0) {
			SAY("pvr: stats: requests (count: us in all/longest): %s", line);
		}
	}
	if (drm_stats && (drm_irqs_found != 0 || drm_polls_found != 0)) {
		SAY("pvr: stats: %u interrupts (%u with nothing to do); jobs found done after an interrupt %u, by looking %u", drm_irqs, drm_irqs_idle,
		    drm_irqs_found, drm_polls_found);
		drm_irqs_found = drm_polls_found = 0;
	}
}

/*
 * The GPU's interrupt. Shared peripheral interrupt 259 of the SoC (the vendor's device tree,
 * mfg_doma: "interrupts = <GIC_SPI 259 IRQ_TYPE_LEVEL_LOW>"), which is the GIC's 291; active low,
 * so MediaTek's polarity register in front of the GIC must turn it over for the GIC to see a level
 * (Linux drivers/irqchip/irq-mtk-sysirq.c mtk_sysirq_set_type: the SPI's bit set in the word
 * 259 / 32 of intpol at 0x10200a80). Taken as redwood_lx.c takes its own (gic_v3_spi_attach,
 * osfmk/arm64/gic_v3_irq.c): in interrupt context it is only masked and the service thread woken;
 * the thread does what pvr_device.c's pvr_device_irq_thread_handler does and lets it in again.
 * Should it never come, or come without end with nothing behind it (the polarity wrong), the
 * thread's own looking goes on as before and the interrupt is given up: boot-arg pvr_irq=0 for
 * never taking it at all.
 */
#define PVR_SPI                 259
#define PVR_INTID               (32 + PVR_SPI)
#define SYSIRQ_INTPOL_PHYS      0x10200a80      /* dts 351-356 */
extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
extern void gic_v3_spi_mask(uint32_t intid);
extern void gic_v3_spi_unmask(uint32_t intid);
static bool drm_irq_attached, drm_irq_rest;
static uint32_t drm_irq_pending;        /* set in interrupt context, taken by the thread */

static void
drm_irq_arrived(__unused void *context, uint32_t intid)
{
	gic_v3_spi_mask(intid);
	(void)__atomic_fetch_add(&drm_irq_pending, 1, __ATOMIC_RELAXED);
	thread_wakeup((event_t)&drm_serving);
}

/* The thread, with the lock: was it woken by the interrupt, and was there anything behind it? */
static void
drm_irq_seen(void)
{
	static uint32_t idle_run;
	uint32_t came = __atomic_exchange_n(&drm_irq_pending, 0, __ATOMIC_RELAXED);

	if (came == 0) {
		return;
	}
	drm_irqs += came;
	if ((rd(rgx, CR_MIPS_WRAPPER_IRQ_STATUS) & 1u) != 0) {
		idle_run = 0;
		if (!drm_irq_works) {
			drm_irq_works = true;
			SAY("pvr: THE GPU'S INTERRUPT WORKS (SPI %u): jobs are found done by it from here on, not by looking", PVR_SPI);
		}
		drm_irq_from = true;
	} else if (++idle_run, ++drm_irqs_idle, idle_run >= 16 && drm_irq_works) {
		/* asserted with no firmware event behind it, time after time: let in again only every other look (a storm would take a processor) */
		drm_irq_rest = true;
		if (idle_run == 16) {
			SAY("pvr: the GPU's interrupt comes with nothing behind it, 16 times running: let in less often from here on while that lasts");
		}
	} else if (idle_run >= 1000 && !drm_irq_works) {
		drm_irq_attached = false;       /* masked by the handler, and never let in again */
		SAY("pvr: THE GPU'S INTERRUPT IS GIVEN UP: %u in a row with nothing behind them (its polarity, or not the GPU's line); looking instead",
		    idle_run);
	}
}

/* ... and let in again, once the thread has declared its wait. */
static void
drm_irq_listen(void)
{
	drm_irq_from = false;
	if (drm_irq_attached && !drm_irq_rest) {
		gic_v3_spi_unmask(PVR_INTID);
	}
	drm_irq_rest = false;
}

static void
drm_irq_attach(void)
{
	volatile uint32_t *intpol = (volatile uint32_t *)(ml_io_map(SYSIRQ_INTPOL_PHYS & ~0xfffu, 0x1000) + (SYSIRQ_INTPOL_PHYS & 0xfffu) +
	    4 * (PVR_SPI / 32));
	uint32_t was = *intpol, want = 1;

	(void)PE_parse_boot_argn("pvr_irq", &want, sizeof(want));
	if (want == 0) {
		SAY("pvr: s6: pvr_irq=0: the GPU's interrupt is not taken; jobs are found done by looking");
		return;
	}
	*intpol = was | (1u << (PVR_SPI % 32));
	wr(rgx, CR_MIPS_WRAPPER_IRQ_CLEAR, 1);
	if (gic_v3_spi_attach(PVR_INTID, false, drm_irq_arrived, NULL) != KERN_SUCCESS) {
		SAY("pvr: s6: the GIC would not give SPI %u: jobs are found done by looking", PVR_SPI);
		return;
	}
	drm_irq_attached = true;
	SAY("pvr: s6: the GPU's interrupt taken: SPI %u (the GIC's %u), level, its polarity word %#x -> %#x; \"WORKS\" is said at the first one",
	    PVR_SPI, PVR_INTID, was, *intpol);
}

/* With the lock held, which is let go meanwhile: asleep until a job is found done (drm_job_forget wakes), or `ns` have gone -- 5 ms at most, to look for itself. */
static void
drm_wait_for_a_job(uint64_t ns)
{
	uint64_t deadline;

	uint64_t slept = drm_slept, began = mach_absolute_time();

	clock_interval_to_deadline((uint32_t)(ns < 5 * NSEC_PER_MSEC ? ns / NSEC_PER_USEC + 1 : 5000), NSEC_PER_USEC, &deadline);
	thread_wakeup((event_t)&drm_serving);           /* the service thread, if it sleeps its idle tenth of a second */
	(void)lck_mtx_sleep_deadline(drm_lock, LCK_SLEEP_DEFAULT, (event_t)&drm_jobs, THREAD_UNINT, deadline);
	drm_slept = slept + (mach_absolute_time() - began);     /* (others ran meanwhile and used the counter: this request's own again) */
}

static void
drm_job_forget(struct drm_job *job)
{
	if (job->pair != NULL) {
		job->pair->pair = NULL;
	}
	if (job->written) {
		drm_stat_add(STAT_DONE + job->type, drm_us_since(job->kicked));
		drm_last_done = mach_absolute_time();
		if (drm_irq_from) {
			drm_irqs_found++;
		} else {
			drm_polls_found++;
		}
	}
	thread_wakeup((event_t)&drm_jobs);              /* whoever waits for a fence */
	if (drm_debug) {
		SAY("pvr: job %llu: %s %u ms after it was submitted (its timeline reads %u, its number is %u)", job->fence,
		    job->written ? "gone" : "dropped before it was started,", drm_ms_since(job->born),
		    job->ctx->queue[job->q].timeline.cpu != NULL ? fresh32(job->ctx->queue[job->q].timeline.cpu) : 0, job->seqno);
	}
	for (struct drm_job **at = &drm_jobs; *at != NULL; at = &(*at)->next) {
		if (*at == job) {
			*at = job->next;
			break;
		}
	}
	drm_finished++;
	kfree_data(job, sizeof(*job));
}

/* pvr_cccb.c: get_ccb_space, pvr_cccb_cmdseq_fits and pvr_cccb_write_command_with_header */
#define CCCB_SIZE               (1u << CCCB_SIZE_LOG2)
#define CCB_HEADER              GEN_SIZEOF_rogue_fwif_ccb_cmd_header            /* PADDING_COMMAND_SIZE is one of these */
#define CCB_WITH_HEADER(size)   (CCB_HEADER + (((size) + 7u) & ~7u))            /* pvr_cccb_get_size_of_cmd_with_hdr */

static uint32_t
cccb_space(struct pvr_queue *queue)
{
	return ((fresh32(queue->cccb_ctl.cpu + 4) - queue->write) + (CCCB_SIZE - 1)) & (CCCB_SIZE - 1);
}

static bool
cccb_fits(struct pvr_queue *queue, uint32_t size)
{
	uint32_t remaining = CCCB_SIZE - queue->write;

	if (size + CCB_HEADER > remaining) {
		size += remaining;
	}
	return cccb_space(queue) >= size;
}

static void
cccb_write(struct pvr_queue *queue, uint32_t type, const void *data, uint32_t size, uint32_t job_ref)
{
	uint32_t with = CCB_WITH_HEADER(size), remaining = CCCB_SIZE - queue->write, header[CCB_HEADER / 4] = { 0 };

	if (remaining < with + CCB_HEADER) {            /* a padding command to the end, and from the start again */
		header[GEN_rogue_fwif_ccb_cmd_header__cmd_type / 4] = GEN_ROGUE_FWIF_CCB_CMD_TYPE_PADDING;
		header[GEN_rogue_fwif_ccb_cmd_header__cmd_size / 4] = remaining - CCB_HEADER;
		memcpy(queue->cccb.cpu + queue->write, header, CCB_HEADER);
		FlushPoC_DcacheRegion((vm_offset_t)(queue->cccb.cpu + queue->write), CCB_HEADER);
		queue->write = 0;
	}
	header[GEN_rogue_fwif_ccb_cmd_header__cmd_type / 4] = type;
	header[GEN_rogue_fwif_ccb_cmd_header__cmd_size / 4] = (size + 7u) & ~7u;
	header[GEN_rogue_fwif_ccb_cmd_header__ext_job_ref / 4] = job_ref;
	header[GEN_rogue_fwif_ccb_cmd_header__int_job_ref / 4] = job_ref;
	memcpy(queue->cccb.cpu + queue->write, header, CCB_HEADER);
	memcpy(queue->cccb.cpu + queue->write + CCB_HEADER, data, size);
	FlushPoC_DcacheRegion((vm_offset_t)(queue->cccb.cpu + queue->write), with);
	queue->write += with;
}

/* pvr_queue.c's ufo_cmds_size and job_cmds_size: what a job takes of the buffer with `waits` fences before it */
static uint32_t
job_cmds_size(const struct drm_job *job, uint32_t waits)
{
	uint32_t full = waits / GEN_ROGUE_FWIF_CCB_CMD_MAX_UFOS, rest = waits % GEN_ROGUE_FWIF_CCB_CMD_MAX_UFOS;

	return full * CCB_WITH_HEADER(GEN_ROGUE_FWIF_CCB_CMD_MAX_UFOS * 8) + (rest != 0 ? CCB_WITH_HEADER(rest * 8) : 0) +
	       CCB_WITH_HEADER(job->cmd_len) + CCB_WITH_HEADER(8);
}

/* What `job` still has to wait for, as the firmware is told it: (timeline, number) pairs. How many. */
static uint32_t
job_waits(const struct drm_job *job, uint32_t ufo[][2])
{
	uint32_t waits = 0;

	for (uint32_t i = 0; i < job->deps; i++) {
		const struct drm_job *on = drm_job_of(job->dep[i]);

		if (on != NULL && on != job) {
			ufo[waits][0] = on->ctx->queue[on->q].timeline.addr;    /* struct rogue_fwif_ufo: addr, value */
			ufo[waits++][1] = on->seqno;
		}
	}
	return waits;
}

/* pvr_queue_submit_job_to_cccb: the fences it still waits on, the command, the update of the timeline to its own number */
static void
job_write(struct drm_job *job, uint32_t ufo[][2], uint32_t waits)
{
	struct pvr_queue *queue = &job->ctx->queue[job->q];
	uint32_t from = queue->write;

	for (uint32_t done = 0; done < waits; done += GEN_ROGUE_FWIF_CCB_CMD_MAX_UFOS) {
		uint32_t now = waits - done < GEN_ROGUE_FWIF_CCB_CMD_MAX_UFOS ? waits - done : GEN_ROGUE_FWIF_CCB_CMD_MAX_UFOS;

		cccb_write(queue, GEN_ROGUE_FWIF_CCB_CMD_TYPE_FENCE_PR, ufo[done], now * 8, 0);
	}
	cccb_write(queue, job->cccb_type, job->cmd, job->cmd_len, (uint32_t)job->fence);
	ufo[0][0] = queue->timeline.addr;
	ufo[0][1] = job->seqno;
	cccb_write(queue, GEN_ROGUE_FWIF_CCB_CMD_TYPE_UPDATE, ufo, 8, 0);
	job->written = true;
	job->kicked = drm_last_kick = mach_absolute_time();
	drm_stat_add(STAT_KICK + job->type, drm_us_since(job->born));
	queue->first = job->queued;
	if (queue->first == NULL) {
		queue->last = NULL;
	}
	if (drm_debug) {
		SAY("pvr: job %llu: in context %u's buffer %u at %#x..%#x behind %u fences (the first: timeline %#x at %u); its number %u on timeline %#x",
		    job->fence, job->ctx->id, job->q, from, queue->write, waits, waits != 0 ? ufo[0][0] : 0, waits != 0 ? ufo[0][1] : 0, job->seqno,
		    queue->timeline.addr);
	}
}

/* pvr_cccb.c's fill_cmd_kick_data: 8 words */
static void
kick_fill(uint32_t *kick, const struct drm_job *job, bool cleanup)
{
	const struct pvr_queue *queue = &job->ctx->queue[job->q];

	kick[GEN_rogue_fwif_kccb_cmd_kick_data__context_fw_addr / 4] = job->ctx->fw.addr + queue->at;
	kick[GEN_rogue_fwif_kccb_cmd_kick_data__client_woff_update / 4] = queue->write;
	kick[GEN_rogue_fwif_kccb_cmd_kick_data__client_wrap_mask_update / 4] = CCCB_SIZE - 1;
	if (job->hwrt != NULL && cleanup) {     /* the render target's cleanup state, so that it is not cleaned up under the job */
		kick[GEN_rogue_fwif_kccb_cmd_kick_data__num_cleanup_ctl / 4] = 1;
		kick[GEN_rogue_fwif_kccb_cmd_kick_data__cleanup_ctl_fw_addr / 4] = job->hwrt->hwrt.data[job->hwrt_index].addr +
		    GEN_rogue_fwif_hwrtdata__cleanup_state;
	}
}

/*
 * A queue's waiting jobs into its command buffer for as long as they fit, oldest first, each kicked:
 * pvr_queue.c's pvr_queue_run_job and pvr_cccb.c's pvr_cccb_send_kccb_kick -- or, for a geometry job
 * with its fragment job, both written and pvr_cccb_send_kccb_combined_kick, the geometry command
 * told where its fragment job's fence is (partial_render_geom_frag_fence: its own timeline at one
 * less than its own number).
 */
static void
drm_queue_run(struct pvr_context *ctx, unsigned q)
{
	static uint32_t ufo[DRM_JOB_DEPS + 1][2], more[DRM_JOB_DEPS + 1][2];    /* under the lock */
	struct pvr_queue *queue = &ctx->queue[q];
	struct drm_job *job;

	while ((job = queue->first) != NULL) {
		struct drm_job *frag = job->type == 0 ? job->pair : NULL;
		uint32_t waits = job_waits(job, ufo), kick[16] = { 0 };

		if (job->type == 1 && job->pair != NULL && !job->pair->written) {
			return;                 /* its geometry job brings it */
		}
		if (((fresh32(fw_kccb_ctl.cpu) + 1) & ((1u << KCCB_LOG2) - 1)) == fresh32(fw_kccb_ctl.cpu + 128) ||
		    !cccb_fits(queue, job_cmds_size(job, waits))) {
			return;                 /* no room in its buffer, or in the kernel's for the kick: next time */
		}
		if (frag != NULL) {
			uint32_t frag_waits = job_waits(frag, more);

			if (ctx->queue[1].first != frag || !cccb_fits(&ctx->queue[1], job_cmds_size(frag, frag_waits))) {
				return;         /* older fragment jobs first, or no room for it yet */
			}
			*(uint32_t *)(void *)(job->cmd + GEN_rogue_fwif_cmd_geom__partial_render_geom_frag_fence) = queue->timeline.addr;
			*(uint32_t *)(void *)(job->cmd + GEN_rogue_fwif_cmd_geom__partial_render_geom_frag_fence + 4) = job->seqno - 1;
			job_write(job, ufo, waits);
			job_write(frag, more, frag_waits);
			kick_fill(&kick[0], job, true);
			kick_fill(&kick[8], frag, frag->cccb_type != GEN_ROGUE_FWIF_CCB_CMD_TYPE_FRAG_PR);
			(void)kccb_send_words(GEN_ROGUE_FWIF_KCCB_CMD_COMBINED_GEOM_FRAG_KICK, kick, 16, false);
		} else {
			job_write(job, ufo, waits);
			kick_fill(kick, job, true);
			(void)kccb_send_words(GEN_ROGUE_FWIF_KCCB_CMD_KICK, kick, 5, false);
		}
		kccb_sent--;                    /* not one of a stage's own */
		if (drm_debug) {
			SAY("pvr: job %llu: %s: context %#x, write offset %#x, cleanup %#x%s; KCCB write %u read %u", job->fence,
			    frag != NULL ? "COMBINED kick with its fragment job" : "kick", kick[0], kick[1], kick[4], frag != NULL ? " (geometry's)" : "",
			    fresh32(fw_kccb_ctl.cpu), fresh32(fw_kccb_ctl.cpu + 128));
		}
	}
}

/*
 * pvr_device_process_active_queues with pvr_queue_process: the jobs the firmware has finished
 * forgotten, the waiting ones tried again. And the clock looked at: a job kicked longer ago than
 * pvr_timeout is a hang, for whoever holds the lock at a safe moment to act on (drm_reset).
 */
static void
drm_poll(void)
{
	struct drm_job *job, *next;
	struct drm_merge **at = &drm_merges;

	if (drm_lost) {
		return;
	}
	for (job = drm_jobs; job != NULL; job = next) {
		next = job->next;
		if (job->written && (int32_t)(fresh32(job->ctx->queue[job->q].timeline.cpu) - job->seqno) >= 0) {
			drm_job_forget(job);
		} else if (job->written && drm_reset_wanted == NULL && drm_ms_since(job->kicked) > drm_timeout_ms) {
			SAY("pvr: JOB %llu HUNG: %u ms since its kick and its timeline reads %u, short of its number %u: context %u, queue %u, "
			    "command %#x of %u bytes", job->fence, drm_ms_since(job->kicked), fresh32(job->ctx->queue[job->q].timeline.cpu), job->seqno,
			    job->ctx->id, job->q, job->cccb_type, job->cmd_len);
			drm_queue_say(job->ctx, 0);
			if (job->ctx->queues > 1) {
				drm_queue_say(job->ctx, 1);
			}
			drm_reset_wanted = "a job hung";
		}
	}
	while (*at != NULL) {           /* merges nobody waits behind any more */
		struct drm_merge *m = *at;

		if (drm_fence_done(m->fence)) {
			*at = m->next;
			kfree_data(m, sizeof(*m));
		} else {
			at = &m->next;
		}
	}
	for (unsigned i = 0; i < DRM_ALL_CTXS; i++) {
		for (unsigned q = 0; drm_contexts[i] != NULL && q < drm_contexts[i]->queues; q++) {
			if (drm_contexts[i]->queue[q].first != NULL) {
				drm_queue_run(drm_contexts[i], q);
			}
		}
	}
}

/*
 * The GPU reset and the firmware started again, with the lock held and nobody in the middle of a
 * command: pvr_power.c's pvr_power_reset(hard) with pvr_queue.c's pvr_queue_device_pre_reset and
 * _post_reset (a context with a job that did not finish is faulty; its jobs are given up) and
 * pvr_fw.c's pvr_fw_hard_reset (every firmware object zeroed and written again as it was first
 * made, the image copied in again). What the firmware said of itself goes into the log first.
 */
static void
drm_reset(const char *why)
{
	struct drm_job *job, *next;
	uint32_t given_up = 0;

	drm_reset_wanted = NULL;
	SAY("pvr: RESETTING THE GPU: %s. What the firmware says of itself:", why);
	fw_dump();
	/* check 122-129: the recovery buffer's hwr_counter, write_index, dd_req_count */
	SAY("pvr:   recoveries the firmware made on its own: %u (the next goes in place %u; %u dumps asked)", fresh32(fw_hwrinfobuf.cpu + 2176),
	    fresh32(fw_hwrinfobuf.cpu + 2180), fresh32(fw_hwrinfobuf.cpu + 2184));
	fw_trace_say(true);
	for (job = drm_jobs; job != NULL; job = job->next) {
		if (job->written) {
			job->ctx->faulty = true;
		}
	}
	for (job = drm_jobs; job != NULL; job = next) {
		next = job->next;
		if (job->written || job->ctx->faulty) {
			given_up++;
			drm_job_forget(job);
		}
	}
	for (unsigned i = 0; i < DRM_ALL_CTXS; i++) {
		if (drm_contexts[i] != NULL && drm_contexts[i]->faulty) {
			drm_contexts[i]->queue[0].first = drm_contexts[i]->queue[0].last = NULL;
			drm_contexts[i]->queue[1].first = drm_contexts[i]->queue[1].last = NULL;
			SAY("pvr:   context %u is faulty from here on: its submissions get EIO", drm_contexts[i]->id);
		}
	}
	fw_zero();
	(void)fw_image();
	fw_structures_fill();
	for (unsigned i = 0; i < DRM_ALL_VMS; i++) {
		if (drm_vms[i] != NULL) {
			pvr_vm_fill(&drm_vms[i]->vm);
		}
	}
	for (unsigned i = 0; i < DRM_ALL_CTXS; i++) {
		if (drm_contexts[i] != NULL) {
			pvr_context_fill(drm_contexts[i]);
		}
	}
	for (unsigned i = 0; i < FREE_LISTS; i++) {
		struct pvr_free_list *fl = pvr_free_lists[i];

		if (fl != NULL) {       /* as pvr_free_list_reconstruct: every node's pages listed again */
			fl->current_pages = 0;
			for (unsigned n = 0; n < fl->nodes; n++) {
				free_list_insert(fl, n);
			}
			fl->current_pages -= fl->ready_pages;
			free_list_fw_fill(fl, fl->current_pages, fl->ready_pages);
		}
	}
	for (unsigned i = 0; i < DRM_ALL_HWRTS; i++) {
		if (drm_hwrts[i] != NULL) {
			pvr_hwrt_fill(&drm_hwrts[i]->hwrt);
			memset(drm_hwrts[i]->last_geom, 0, sizeof(drm_hwrts[i]->last_geom));
			memset(drm_hwrts[i]->last_frag, 0, sizeof(drm_hwrts[i]->last_frag));
		}
	}
	FlushPoC_DcacheRegion(arena, ARENA_SIZE);
	drm_resets++;
	if (!fw_start()) {
		drm_lost = true;
		SAY("pvr: THE GPU IS LOST: its firmware did not start again; every request is EIO from here on");
		return;
	}
	mmu_flush_flags |= MMU_SYNC_2;
	(void)pvr_mmu_flush();
	if (fw_trace_mask != 0) {       /* the trace on again, for the next time */
		store32(&fw_tracectl, 0, fw_trace_mask | LOG_TYPE_TRACE);
		(void)command("LOGTYPE_UPDATE", KCCB_CMD_LOGTYPE_UPDATE, 0, 0);
	}
	SAY("pvr: the GPU is back: reset %u, %u jobs given up; the contexts not faulty go on", drm_resets, given_up);
}

/* Where a queue's command buffer stands: what the firmware has read of it, and if it sits at a fence, which, and what that timeline reads. */
static void
drm_queue_say(const struct pvr_context *ctx, unsigned q)
{
	const struct pvr_queue *queue = &ctx->queue[q];
	uint32_t read = fresh32(queue->cccb_ctl.cpu + 4) & (CCCB_SIZE - 1);
	const uint32_t *h = (const uint32_t *)(const void *)(queue->cccb.cpu + read);

	FlushPoC_DcacheRegion((vm_offset_t)h, CCB_HEADER + 16);
	SAY("pvr:   context %u queue %u: the firmware has read its buffer to %#x of %#x written (its control: write %#x, dep %#x); timeline reads %u of %u given out",
	    ctx->id, q, read, queue->write, fresh32(queue->cccb_ctl.cpu), fresh32(queue->cccb_ctl.cpu + 8), fresh32(queue->timeline.cpu), queue->seqno);
	if (read != queue->write && h[0] == GEN_ROGUE_FWIF_CCB_CMD_TYPE_FENCE_PR) {
		uint32_t now = 0;

		for (unsigned i = 0; i < DRM_ALL_CTXS; i++) {
			for (unsigned k = 0; drm_contexts[i] != NULL && k < drm_contexts[i]->queues; k++) {
				if (drm_contexts[i]->queue[k].timeline.addr == h[CCB_HEADER / 4]) {
					now = fresh32(drm_contexts[i]->queue[k].timeline.cpu);
					SAY("pvr:     it sits at a fence: context %u queue %u's timeline must reach %u and reads %u", drm_contexts[i]->id, k,
					    h[CCB_HEADER / 4 + 1], now);
				}
			}
		}
		if (now == 0) {
			SAY("pvr:     it sits at a fence on timeline %#x (of no context there is now) for %u", h[CCB_HEADER / 4], h[CCB_HEADER / 4 + 1]);
		}
	} else if (read != queue->write) {
		SAY("pvr:     the command it has not finished: type %#x, %u bytes, job %u", h[0], h[1], h[2]);
	}
}

/*
 * The firmware saying it has reset a context (FWCCB CONTEXT_RESET_NOTIFICATION): everything it gives
 * (struct rogue_fwif_fwccb_cmd_context_reset_data, as pvr_dump.c's pvr_dump_context_reset_notification
 * says it, and the newest entry of its recovery buffer, struct rogue_hwrinfo, check 102-118), and
 * for a page fault what is at that address: the heap, the mapping and its buffer or when one was
 * taken away, the page table's own entry, the free lists and the render targets' buffers. The first
 * 16 in full and every 32nd after.
 */
static void
drm_context_reset(const uint32_t *n)
{
	static const char *const reasons[17] = { "none", "guilty of a lockup", "innocent of a lockup", "guilty of overrunning", "innocent of overrunning",
		                                 "hard context switch", "WGP checksum", "TRP checksum", "GPU ECC, corrected", "GPU ECC, recovered",
		                                 "firmware ECC, corrected", "firmware ECC", "firmware watchdog", "firmware page fault",
		                                 "firmware execution error", "host watchdog", "geometry out of memory disabled" };
	uint32_t id = n[GEN_rogue_fwif_fwccb_cmd_context_reset_data__server_common_context_id / 4];
	uint32_t reason = n[GEN_rogue_fwif_fwccb_cmd_context_reset_data__reset_reason / 4];
	uint32_t flags = n[GEN_rogue_fwif_fwccb_cmd_context_reset_data__flags / 4], newest = 0;
	uint64_t pc = ((uint64_t)n[7] << 32) | n[6], fault = ((uint64_t)n[9] << 32) | n[8];     /* pc_address 24, fault_address 32 (check) */
	const struct pvr_context *ctx = NULL;
	const struct drm_vm *vm = NULL;
	const uint8_t *hwr;

	drm_faults++;
	if (drm_faults > 16 && (drm_faults & 31) != 0) {
		return;
	}
	SAY("pvr: THE FIRMWARE RESET context %u (%u such so far): %s (%u), data master %u, job %u, flags %#x%s%s; page catalogue %#llx, FAULT ADDRESS %#llx",
	    id, drm_faults, reason < 17 ? reasons[reason] : "?", reason, n[GEN_rogue_fwif_fwccb_cmd_context_reset_data__dm / 4],
	    n[GEN_rogue_fwif_fwccb_cmd_context_reset_data__reset_job_ref / 4], flags, (flags & 1) != 0 ? " PAGE FAULT" : "",
	    (flags & 2) != 0 ? " every context" : "", pc, fault);
	FlushPoC_DcacheRegion((vm_offset_t)fw_hwrinfobuf.cpu, HWRINFOBUF_SIZE);
	for (uint32_t i = 1; i < 16; i++) {             /* ROGUE_FWIF_HWINFO_MAX: the entry with the highest hwr_number */
		if (*(const uint32_t *)(const void *)(fw_hwrinfobuf.cpu + i * 136 + 60) > *(const uint32_t *)(const void *)(fw_hwrinfobuf.cpu + newest * 136 + 60)) {
			newest = i;
		}
	}
	hwr = fw_hwrinfobuf.cpu + newest * 136;
	SAY("pvr:   the firmware's record of recovery %u: type %u (3-5 a BIF fault, 6 an MMU fault, 1 an overrun), data master %u, render target data %#x, "
	    "events %#x; its fault words %#llx %#llx %#llx", *(const uint32_t *)(const void *)(hwr + 60), *(const uint32_t *)(const void *)(hwr + 72),
	    *(const uint32_t *)(const void *)(hwr + 76), *(const uint32_t *)(const void *)(hwr + 56), *(const uint32_t *)(const void *)(hwr + 64),
	    *(const uint64_t *)(const void *)hwr, *(const uint64_t *)(const void *)(hwr + 8), *(const uint64_t *)(const void *)(hwr + 16));
	for (unsigned i = 0; i < DRM_ALL_CTXS; i++) {
		if (drm_contexts[i] != NULL && drm_contexts[i]->id == id) {
			ctx = drm_contexts[i];
		}
	}
	for (unsigned i = 0; ctx != NULL && i < DRM_ALL_VMS; i++) {
		if (drm_vms[i] != NULL && &drm_vms[i]->vm == ctx->vm) {
			vm = drm_vms[i];
		}
	}
	if (ctx == NULL || vm == NULL) {
		SAY("pvr:   context %u is not one there is now: nothing to look the address up in", id);
	} else {
		struct pvr_vm *pvm = (struct pvr_vm *)(uintptr_t)ctx->vm;
		const uint64_t *pte = mmu_entry(pvm, fault & ~0xfffull, false);
		const struct drm_map *map = NULL;
		const char *heap = "NO HEAP";
		uint32_t maps = 0;

		for (unsigned i = 0; i < PVR_HEAPS; i++) {
			if (fault >= pvr_heaps[i].base && fault < pvr_heaps[i].base + pvr_heaps[i].size) {
				heap = pvr_heaps[i].name;
			}
		}
		for (const struct drm_map *m = vm->maps; m != NULL; m = m->next, maps++) {
			if (fault >= m->addr && fault < m->addr + m->size) {
				map = m;
			}
		}
		SAY("pvr:   that address is in the %s heap; this side's page catalogue is at %#llx (%s); its page table entry: %#llx; %u mappings in that address space",
		    heap, pvm->pc_phys, (pc & ~0xfffull) == pvm->pc_phys ? "the firmware's too" : "NOT WHAT THE FIRMWARE SAYS", pte != NULL ? *pte : 0, maps);
		if (map != NULL) {
			SAY("pvr:   MAPPED: %#llx..%#llx is buffer memory %p from its byte %#llx, flags %#llx, %#llx bytes long -- so the GPU's cached "
			    "translation was stale, or the page is not what the job expects", map->addr, map->addr + map->size, map->bo->memory, map->offset,
			    map->bo->flags, map->bo->size);
		} else {
			SAY("pvr:   NOT MAPPED now: no mapping of this address space holds it");
		}
		for (uint32_t i = 0; i < DRM_UNMAPPED && i < drm_unmappings; i++) {
			const struct drm_unmapped *u = &drm_unmapped[i];

			if (u->vm == ctx->vm && fault >= u->addr && fault < u->addr + u->size) {
				SAY("pvr:   it WAS mapped: %#llx..%#llx, unmapped %u ms ago -- a job used a buffer its program had already given back",
				    u->addr, u->addr + u->size, drm_ms_since(u->when));
			}
		}
		for (unsigned i = 0; i < DRM_ALL_HWRTS; i++) {
			const struct pvr_hwrt_args *a = drm_hwrts[i] != NULL ? &drm_hwrts[i]->hwrt.args : NULL;

			if (a == NULL || a->free_lists[0] == NULL) {
				continue;
			}
			for (unsigned k = 0; k < 9; k++) {
				uint64_t at = k == 0 ? a->tpc_dev_addr : k == 1 ? a->vheap_table_dev_addr : k == 2 ? a->rtc_dev_addr :
				    k < 5 ? a->rt[k - 3].pm_mlist_dev_addr : k < 7 ? a->rt[k - 5].macrotile_array_dev_addr : a->rt[k - 7].region_header_dev_addr;

				if (at != 0 && (fault & ~0xfffffull) == (at & ~0xfffffull)) {
					SAY("pvr:   within 1 MiB of a %ux%u render target's buffer %u at %#llx (0 tail pointers, 1 heap table, 2 target cache, 3-4 PM lists, "
					    "5-6 macrotiles, 7-8 region headers)", a->width, a->height, k, at);
				}
			}
		}
		drm_queue_say(ctx, 0);
		if (ctx->queues > 1) {
			drm_queue_say(ctx, 1);
		}
	}
	for (unsigned i = 0, said = 0; i < FREE_LISTS && said < 8; i++) {
		const struct pvr_free_list *fl = pvr_free_lists[i];

		if (fl != NULL) {       /* check 424-431: what the firmware has of the list */
			said++;
			FlushPoC_DcacheRegion((vm_offset_t)fl->fw.cpu, FREELIST_SIZE);
			SAY("pvr:   free list %u at GPU %#llx: this side %u pages (+%u ready) of at most %u in %u pieces; the firmware: %u pages, %u given to the PM, "
			    "%u to its page tables, stack top %u, growing %u%s", fl->id, fl->list_gpu_addr, fl->current_pages, fl->ready_pages, fl->max_pages,
			    fl->nodes, *(const uint32_t *)(const void *)(fl->fw.cpu + 28), *(const uint32_t *)(const void *)(fl->fw.cpu + 32),
			    *(const uint32_t *)(const void *)(fl->fw.cpu + 36), *(const uint32_t *)(const void *)(fl->fw.cpu + 16),
			    *(const uint32_t *)(const void *)(fl->fw.cpu + 44),
			    fault >= fl->list_gpu_addr && fault < fl->list_gpu_addr + (uint64_t)fl->max_pages * 4 ? " -- THE FAULT IS IN THIS LIST" : "");
		}
	}
	fw_dump();
	fw_trace_say(true);
}

/* With the lock held, which is let go while asleep: until the fence is signalled, or ETIME. */
static int
drm_fence_wait(uint64_t fence, int64_t timeout_ns)
{
	uint64_t began = mach_absolute_time(), ns = 0;

	for (;;) {
		fw_service();
		if (drm_fence_done(fence)) {
			drm_stats_woken();
			return 0;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		if (timeout_ns <= 0 || ns >= (uint64_t)timeout_ns) {
			return ETIME;
		}
		drm_wait_for_a_job((uint64_t)timeout_ns - ns);
	}
}

/* ---- the command streams: pvr_stream.c ---- */

static bool
stream_feature(uint32_t feature)
{
	if (feature == SF_CDM_USER_MODE_QUEUE) {
		return feature_value[F_CDM_CONTROL_STREAM_FORMAT] >= 2 && feature_value[F_CDM_CONTROL_STREAM_FORMAT] <= 4;
	}
	if (feature == SF_REQUIRES_FB_CDC_ZLS_SETUP) {
		return feature_has[F_FBCDC_ALGORITHM] && (feature_value[F_FBCDC_ALGORITHM] < 3 || feature_has[F_FB_CDC_V4]);
	}
	return feature_has[feature];
}

static bool
uapi_quirk(uint32_t quirk)              /* pvr_device_has_uapi_quirk: the five user space is told of */
{
	return quirk == 47217 ? (fw_quirks >> 1) & 1 : quirk == 48545 ? (fw_quirks >> 3) & 1 : quirk == 49927 ? (fw_quirks >> 4) & 1 :
	       quirk == 51764 ? (fw_quirks >> 6) & 1 : quirk == 62269 ? (fw_quirks >> 7) & 1 : false;
}

/* pvr_stream_process_1: the entries this core has, each copied from the stream's next bytes to its place. The new offset, or 0. */
static uint32_t
stream_part(const struct stream_def *defs, uint32_t count, const uint8_t *stream, uint32_t at, uint32_t size, uint8_t *dest, uint32_t dest_size)
{
	for (uint32_t i = 0; i < count; i++) {
		if (defs[i].feature != SF_NONE && stream_feature(defs[i].feature) == defs[i].absent) {
			continue;
		}
		if (defs[i].offset + defs[i].bytes > dest_size || defs[i].bytes > size || at > size - defs[i].bytes) {
			return 0;
		}
		memcpy(dest + defs[i].offset, stream + at, defs[i].bytes);
		at += defs[i].bytes;
	}
	return at;
}

/*
 * pvr_stream_process with pvr_stream_process_ext_stream: a length and a zero, the main part, then
 * headers each followed by the parts its bits name -- the parts for the quirks this core has must be
 * there (pvr_stream_create_musthave_masks), a part for one it has not is refused. 0 or EINVAL.
 */
static int
stream_process(const struct stream_def *main, uint32_t main_count, const struct stream_ext *ext, uint32_t ext_count, uint32_t headers,
    const uint8_t *stream, uint32_t size, uint8_t *dest, uint32_t dest_size)
{
	uint32_t length, padding, at, header, musthave[8] = { 0 };

	if (size < 8) {
		return EINVAL;
	}
	memcpy(&length, stream, 4);
	memcpy(&padding, stream + 4, 4);
	if (length < 8 || length > size || padding != 0 || (at = stream_part(main, main_count, stream, 8, length, dest, dest_size)) == 0) {
		return EINVAL;
	}
	for (uint32_t i = 0; i < ext_count; i++) {
		if ((ext[i].quirk == 47217 || ext[i].quirk == 49927) && uapi_quirk(ext[i].quirk)) {
			musthave[ext[i].header] |= ext[i].mask;
		}
	}
	if (at < size) {
		do {
			uint32_t type, bits, valid = 0;

			if (at > size - 4) {
				return EINVAL;
			}
			memcpy(&header, stream + at, 4);
			at += 4;
			type = header >> 29;            /* PVR_STREAM_EXTHDR_TYPE_SHIFT; bit 28 "another follows" */
			bits = header & 0x0fffffffu;
			for (uint32_t i = 0; i < ext_count; i++) {
				valid |= ext[i].header == type ? ext[i].valid : 0;
			}
			if (type >= headers || (bits & ~valid) != 0) {
				return EINVAL;
			}
			musthave[type] &= ~bits;
			for (uint32_t i = 0; i < ext_count; i++) {
				if (ext[i].header != type || (header & ext[i].mask) == 0) {
					continue;
				}
				if (!uapi_quirk(ext[i].quirk) || (at = stream_part(ext[i].defs, ext[i].count, stream, at, size, dest, dest_size)) == 0) {
					return EINVAL;
				}
			}
		} while ((header & (1u << 28)) != 0);
	}
	for (uint32_t i = 0; i < headers && i < 8; i++) {
		if (musthave[i] != 0) {
			return EINVAL;
		}
	}
	return 0;
}

/* ---- the requests ---- */

struct drm_pvr_obj_array {
	uint32_t stride, count;
	uint64_t array;
};
struct drm_pvr_dev_query {
	uint32_t type, size;
	uint64_t pointer;
};
struct drm_pvr_create_bo {
	uint64_t size;
	uint32_t handle, pad;
	uint64_t flags;
};
struct drm_pvr_handle {                 /* get_bo_mmap_offset; create/destroy_vm_context, destroy_context, _free_list, _hwrt_dataset; GEM_CLOSE */
	uint32_t handle, pad;
	uint64_t offset;
};
struct drm_pvr_vm_map {
	uint32_t vm, flags;
	uint64_t device_addr;
	uint32_t handle, pad;
	uint64_t offset, size;
};
struct drm_pvr_vm_unmap {
	uint32_t vm, pad;
	uint64_t device_addr, size;
};
struct drm_pvr_create_context {
	uint32_t type, flags;
	int32_t priority;
	uint32_t handle;
	uint64_t static_state;
	uint32_t static_state_len, vm;
	uint64_t callstack_addr;
};
struct drm_pvr_create_free_list {
	uint64_t gpu_addr;
	uint32_t initial, max, grow, threshold, vm, handle;
};
struct drm_pvr_create_hwrt {
	uint64_t tpc_dev_addr;
	uint32_t tpc_size, tpc_stride;
	uint64_t vheap_table_dev_addr, rtc_dev_addr;
	struct {
		uint64_t pm_mlist_dev_addr, macrotile_array_dev_addr, region_header_dev_addr;
	} rt[2];
	uint32_t free_list[2];
	uint32_t width, height, samples, layers;
	uint32_t isp_merge_lower_x, isp_merge_lower_y, isp_merge_scale_x, isp_merge_scale_y, isp_merge_upper_x, isp_merge_upper_y;
	uint32_t region_header_size, handle;
};
struct drm_pvr_sync_op {
	uint32_t handle, flags;
	uint64_t value;
};
struct drm_pvr_job {
	uint32_t type, context, flags, stream_len;
	uint64_t stream;
	struct drm_pvr_obj_array sync_ops;
	uint32_t hwrt, hwrt_index;
};
struct drm_syncobj_create {
	uint32_t handle, flags;         /* DRM_SYNCOBJ_CREATE_SIGNALED 1 */
};
struct drm_syncobj_wait {
	uint64_t handles;
	int64_t timeout_ns;             /* HERE: from now. Linux's is a time of CLOCK_MONOTONIC, which this kernel's clock is not: the caller subtracts */
	uint32_t count, flags;          /* DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL 1, _WAIT_FOR_SUBMIT 2 */
	uint32_t first_signaled, pad;
};
struct drm_syncobj_array {
	uint64_t handles;
	uint32_t count, pad;
};
struct drm_syncobj_transfer {
	uint32_t src, dst;
	uint64_t src_point, dst_point;
	uint32_t flags, pad;
};
struct drm_get_cap {
	uint64_t capability, value;
};
_Static_assert(sizeof(struct drm_pvr_vm_map) == 40 && sizeof(struct drm_pvr_create_context) == 40 && sizeof(struct drm_pvr_create_free_list) == 32 &&
    sizeof(struct drm_pvr_create_hwrt) == 136 && sizeof(struct drm_pvr_job) == 48 && sizeof(struct drm_pvr_sync_op) == 16, "pvr_drm.h");

/* a free place in a table of pointers; the handle is its number + 1 */
#define DRM_SLOT(table, count, slot) \
	for ((slot) = 0; (slot) < (count) && (table)[slot] != 0; (slot)++) { \
	}
#define DRM_GET(table, count, handle)   ((handle) >= 1 && (handle) <= (count) ? (table)[(handle) - 1] : 0)

static uint32_t
drm_free_list_min_pages(void)           /* pvr_free_list.c pvr_get_free_list_min_pages */
{
	return !feature_has[F_ROGUEXE] ? 50 : (fw_quirks & BRN_66011) != 0 ? 40 : 25;
}

/* pvr_drv.c's rogue_get_common_store_partition_space_size, .._alloc_region_size, rogue_get_max_coeffs, rogue_get_cdm_max_local_mem_size_regs */
static void
drm_runtime_info(uint32_t out[4])
{
	uint32_t partition = feature_value[F_TILE_SIZE_X] == 16 && feature_value[F_TILE_SIZE_Y] == 16 ?
	    (uint32_t)(16 * 16 * feature_value[F_MAX_PARTITIONS] * feature_value[F_USC_MIN_OUTPUT_REGISTERS_PER_PIX]) :
	    (uint32_t)feature_value[F_MAX_PARTITIONS] * 1024;
	uint32_t store = feature_has[F_COMMON_STORE_SIZE_IN_DWORDS] ? (uint32_t)feature_value[F_COMMON_STORE_SIZE_IN_DWORDS] : 512 * 4 * 4;
	uint32_t region = store - 256 * 4 - partition, phantoms = ((uint32_t)feature_value[F_NUM_CLUSTERS] + 3) / 4;
	uint32_t pixel = (((uint32_t)feature_value[F_ISP_MAX_TILES_IN_FLIGHT] + phantoms - 1) / phantoms) * GEN_ROGUE_MAX_PIXEL_SHARED_REGISTERS;
	bool brn48492 = (fw_quirks >> 2) & 1, ern38020 = (fw_enhancements >> 1) & 1, ern38748 = (fw_enhancements >> 2) & 1;
	uint32_t coeffs, local;

	if ((fw_quirks & 1) != 0 && region > 768 * 4 * 4 - 256 * 4) {          /* BRN 44079 */
		region = 768 * 4 * 4 - 256 * 4;
	}
	coeffs = region + (brn48492 && !feature_has[F_COMPUTE_OVERLAP] ? 2 * 1024 : 0) -
	    (pixel + GEN_ROGUE_MAX_VERTEX_SHARED_REGISTERS + (ern38020 ? GEN_ROGUE_MAX_COMPUTE_SHARED_REGISTERS : 0) + (ern38748 ? 0 : 2 * 1024));
	local = coeffs - (brn48492 && feature_has[F_ROGUEXE] && !feature_has[F_COMPUTE_OVERLAP] ? GEN_ROGUE_CSRM_LINE_SIZE_IN_DWORDS * 2 : 0);
	out[0] = region;
	out[1] = partition;
	out[2] = coeffs;
	out[3] = local < GEN_ROGUE_MAX_PER_KERNEL_LOCAL_MEM_SIZE_REGS ? local : GEN_ROGUE_MAX_PER_KERNEL_LOCAL_MEM_SIZE_REGS;
}

/* pvr_drv.c's pvr_ioctl_dev_query and its six answers */
static int
drm_dev_query(struct redwood_pvr_file *file, struct drm_pvr_dev_query *r)
{
	static const uint32_t sizes[6] = { 16, 32, 16, 16, 16, 16 };
	static const uint32_t quirks[5] = { 47217, 49927, 62269, 48545, 51764 };        /* the first three "must have" */
	union {
		struct { uint64_t gpu_id; uint32_t num_phantoms, pad; } gpu;
		struct { uint64_t min_pages, max_pages; uint32_t info[4]; } runtime;
		struct { uint64_t array; uint16_t count, musthave; uint32_t pad; } list;         /* quirks, and enhancements less `musthave` */
		struct drm_pvr_obj_array array;                                                 /* heaps, static data areas */
	} a;
	uint32_t found[5], count = 0, musthave = 0;
	int error = 0;

	if (r->type > 5) {
		return EINVAL;
	}
	if (r->pointer == 0) {
		r->size = sizes[r->type];
		return 0;
	}
	memset(&a, 0, sizeof(a));
	switch (r->type) {
	case 0:         /* GPU_INFO: the BVNC as the firmware's header packs it, pvr_gpu_id_to_packed_bvnc */
		a.gpu.gpu_id = gpu_bvnc;
		a.gpu.num_phantoms = ((uint32_t)feature_value[F_NUM_CLUSTERS] + 3) / 4;         /* ROGUE_REQ_NUM_PHANTOMS */
		break;
	case 1:
		a.runtime.min_pages = drm_free_list_min_pages();
		a.runtime.max_pages = GEN_ROGUE_PM_MAX_FREELIST_SIZE / GEN_ROGUE_PM_PAGE_SIZE;
		drm_runtime_info(a.runtime.info);
		break;
	case 2:
	case 3:
		if (r->size < 16 || (error = drm_in(file, r->pointer, &a, 16)) != 0 || a.list.pad != 0 || (r->type == 3 && a.list.musthave != 0)) {
			return error != 0 ? error : EINVAL;
		}
		if (r->type == 2) {
			for (unsigned i = 0; i < 5; i++) {
				if (uapi_quirk(quirks[i])) {
					found[count++] = quirks[i];
					musthave += i < 3;
				}
			}
		} else {                /* pvr_device_has_uapi_enhancement: 35421 and 42064, bits 0 and 3 */
			if ((fw_enhancements & 1) != 0) {
				found[count++] = 35421;
			}
			if ((fw_enhancements & 8) != 0) {
				found[count++] = 42064;
			}
		}
		if (a.list.array != 0) {
			if (a.list.count < count) {
				return E2BIG;
			}
			if ((error = drm_out(file, found, a.list.array, count * 4)) != 0) {
				return error;
			}
			a.list.musthave = (uint16_t)(r->type == 2 ? musthave : 0);
		}
		a.list.count = (uint16_t)count;
		break;
	case 4:
	case 5: {
		uint32_t have = r->type == 4 ? PVR_HEAPS : 5, each = r->type == 4 ? 24 : 16;

		if (r->size < 16 || (error = drm_in(file, r->pointer, &a, 16)) != 0) {
			return error != 0 ? error : EINVAL;
		}
		if (a.array.array == 0) {
			a.array.count = have;
			a.array.stride = each;
			break;
		}
		if (a.array.count > have) {
			a.array.count = have;
		}
		if (a.array.stride < each) {
			return EINVAL;
		}
		for (uint32_t i = 0; i < a.array.count && error == 0; i++) {
			struct { uint64_t base, size; uint32_t flags, page_size_log2; } heap = { pvr_heaps[i % PVR_HEAPS].base,
				i == PVR_HEAP_RGNHDR && (fw_quirks & BRN_63142) == 0 ? 0 : pvr_heaps[i % PVR_HEAPS].size, 0, 12 };
			struct { uint16_t usage, heap; uint32_t size; uint64_t offset; } area = { pvr_static_areas[i % 5].usage,
				pvr_static_areas[i % 5].heap, pvr_static_areas[i % 5].size, pvr_static_areas[i % 5].offset };

			error = r->type == 4 ? drm_answer(file, a.array.array + (uint64_t)i * a.array.stride, a.array.stride, &heap, 24) :
			    drm_answer(file, a.array.array + (uint64_t)i * a.array.stride, a.array.stride, &area, 16);
		}
		if (error != 0) {
			return error;
		}
		break;
	}
	}
	error = drm_answer(file, r->pointer, r->size, &a, sizes[r->type]);
	if (r->size > sizes[r->type]) {
		r->size = sizes[r->type];
	}
	return error;
}

static struct drm_map *
drm_map_at(struct drm_vm *vm, uint64_t addr)            /* the mapping `addr` lies in */
{
	for (struct drm_map *map = vm->maps; map != NULL; map = map->next) {
		if (addr >= map->addr && addr < map->addr + map->size) {
			return map;
		}
	}
	return NULL;
}

/* pvr_ioctl_vm_map with pvr_vm.c's pvr_vm_map: each of the buffer's 4 KiB pages, wherever it lies */
static int
drm_vm_map(struct redwood_pvr_file *file, struct drm_pvr_vm_map *r)
{
	struct drm_vm *vm = DRM_GET(file->vm, DRM_VMS, r->vm);
	struct drm_bo *bo = DRM_GET(file->bo, DRM_BOS, r->handle);
	struct drm_map *map;
	int error = 0;

	if (r->pad != 0 || r->flags != 0 || r->offset + r->size < r->offset || vm == NULL) {
		return EINVAL;
	}
	if (bo == NULL) {
		return ENOENT;
	}
	if (r->size == 0 || r->offset >= bo->size || r->offset + r->size > bo->size || (map = drm_new(sizeof(*map))) == NULL) {
		return r->size == 0 || r->offset >= bo->size || r->offset + r->size > bo->size ? EINVAL : ENOMEM;
	}
	for (uint64_t done = 0; done < r->size && error == 0; done += 4096) {
		error = pvr_vm_map(&vm->vm, r->device_addr + done, bo_phys(bo->memory, r->offset + done), 4096, bo->flags);
		if (error != 0) {
			pvr_vm_unmap(&vm->vm, r->device_addr, done);
		}
	}
	if (error != 0) {
		kfree_data(map, sizeof(*map));
		return error;
	}
	map->bo = bo;
	map->addr = r->device_addr;
	map->size = r->size;
	map->offset = r->offset;
	map->next = vm->maps;
	vm->maps = map;
	drm_live.maps++;
	bo->refs++;
	return 0;
}

static int
drm_vm_unmap(struct redwood_pvr_file *file, struct drm_pvr_vm_unmap *r)
{
	struct drm_vm *vm = DRM_GET(file->vm, DRM_VMS, r->vm);

	if (r->pad != 0 || vm == NULL) {
		return EINVAL;
	}
	for (struct drm_map **at = &vm->maps; *at != NULL; at = &(*at)->next) {
		struct drm_map *map = *at;

		if (map->addr == r->device_addr && map->size == r->size) {
			*at = map->next;
			drm_live.maps--;
			drm_unmapped[drm_unmappings % DRM_UNMAPPED] = (struct drm_unmapped) { &vm->vm, map->addr, map->size, mach_absolute_time() };
			drm_unmappings++;
			pvr_vm_unmap(&vm->vm, map->addr, map->size);
			drm_unmap_seq++;                /* its flush is the next submission's, or the service thread's: see drm_bo_put */
			drm_bo_put(map->bo);
			kfree_data(map, sizeof(*map));
			return 0;
		}
	}
	return ENOENT;
}

/* A context taken down: its waiting jobs dropped, its running ones given two seconds, then the firmware's cleanup. */
static void
drm_context_destroy(struct drm_ctx *c)
{
	struct drm_job *job, *next;

	for (unsigned ms = 0; ms < 2000; ms += 10) {
		bool running = false;

		fw_service();
		for (job = drm_jobs; job != NULL; job = job->next) {
			running = running || (job->ctx == &c->ctx && job->written);
		}
		if (!running) {
			break;
		}
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	for (job = drm_jobs; job != NULL; job = next) {
		next = job->next;
		if (job->ctx == &c->ctx) {
			drm_job_forget(job);
		}
	}
	for (unsigned i = 0; i < DRM_ALL_CTXS; i++) {
		if (drm_contexts[i] == &c->ctx) {
			drm_contexts[i] = NULL;
		}
	}
	if (!pvr_context_destroy(&c->ctx)) {
		SAY("pvr: a context the firmware will not let go of is left as it is (context %u)", c->ctx.id);
		return;
	}
	(void)pvr_mmu_flush();
	drm_live.ctxs--;
	drm_vm_put(c->vm);
	kfree_data(c, sizeof(*c));
}

/* pvr_ioctl_create_context with pvr_context.c's pvr_context_create, remap_priority and init_fw_objs */
static int
drm_create_context(struct redwood_pvr_file *file, struct drm_pvr_create_context *r)
{
	struct drm_vm *vm = DRM_GET(file->vm, DRM_VMS, r->vm);
	uint8_t stream[512];
	struct drm_ctx *c;
	uint32_t slot, all, priority = r->priority == -512 ? 0 : r->priority == 0 ? PVR_CTX_PRIORITY_MEDIUM : 2;
	int error;

	DRM_SLOT(file->ctx, DRM_CTXS, slot);
	DRM_SLOT(drm_contexts, DRM_ALL_CTXS, all);
	if (r->flags != 0 || r->type > PVR_CTX_TRANSFER_FRAG || (r->priority != -512 && r->priority != 0 && r->priority != 512) || vm == NULL ||
	    (r->type == PVR_CTX_TRANSFER_FRAG) != (r->static_state_len == 0) || r->static_state_len > sizeof(stream) ||
	    (r->type != PVR_CTX_RENDER && r->callstack_addr != 0)) {
		return EINVAL;
	}
	if (r->priority == 512 && !file->kernel) {
		return EACCES;          /* Linux: CAP_SYS_NICE or the DRM master */
	}
	if (slot == DRM_CTXS || all == DRM_ALL_CTXS || (c = drm_new(sizeof(*c))) == NULL) {
		return ENOMEM;
	}
	if ((error = drm_in(file, r->static_state, stream, r->static_state_len)) != 0 ||
	    !pvr_context_create(&c->ctx, &vm->vm, r->type, priority, r->callstack_addr, 0)) {
		kfree_data(c, sizeof(*c));
		return error != 0 ? error : ENOMEM;
	}
	if (r->type == PVR_CTX_RENDER) {
		error = stream_process(stream_static_render_main, sizeof(stream_static_render_main) / sizeof(stream_static_render_main[0]), NULL, 0, 0,
		    stream, r->static_state_len, c->ctx.static_state, stream_static_render_DEST_SIZE);
	} else if (r->type == PVR_CTX_COMPUTE) {
		error = stream_process(stream_static_compute_main, sizeof(stream_static_compute_main) / sizeof(stream_static_compute_main[0]), NULL, 0, 0,
		    stream, r->static_state_len, c->ctx.static_state, stream_static_compute_DEST_SIZE);
	}
	if (error != 0) {
		pvr_context_free(&c->ctx);
		(void)pvr_mmu_flush();
		kfree_data(c, sizeof(*c));
		return error;
	}
	pvr_context_fill(&c->ctx);
	c->vm = vm;
	vm->refs++;
	file->ctx[slot] = c;
	drm_live.ctxs++;
	drm_contexts[all] = &c->ctx;
	r->handle = slot + 1;
	return 0;
}

/* pvr_ioctl_create_free_list with free_list_create_kernel_structure: the list's buffer must be mapped there, protected, not a program's to write */
static int
drm_create_free_list(struct redwood_pvr_file *file, struct drm_pvr_create_free_list *r)
{
	struct drm_vm *vm = DRM_GET(file->vm, DRM_VMS, r->vm);
	struct drm_map *map = vm != NULL ? drm_map_at(vm, r->gpu_addr) : NULL;
	struct drm_fl *fl;
	uint32_t slot;

	DRM_SLOT(file->fl, DRM_FLS, slot);
	if (map == NULL || (map->bo->flags & 4) != 0 || (map->bo->flags & PVR_BO_PM_FW_PROTECT) == 0 || (r->gpu_addr & 3) != 0 ||
	    map->addr + map->size - r->gpu_addr < (uint64_t)r->max * 4) {
		return EINVAL;
	}
	if (slot == DRM_FLS || (fl = drm_new(sizeof(*fl))) == NULL) {
		return ENOMEM;
	}
	if (!pvr_free_list_create(&fl->fl, (uint32_t *)(void *)((uint8_t *)bo_ops->bytes(map->bo->memory) + map->offset + (r->gpu_addr - map->addr)),
	    r->gpu_addr, r->initial, r->max, r->grow, r->threshold)) {
		kfree_data(fl, sizeof(*fl));
		return EINVAL;
	}
	fl->vm = vm;
	fl->bo = map->bo;
	fl->refs = 1;
	drm_live.fls++;
	vm->refs++;
	map->bo->refs++;
	file->fl[slot] = fl;
	r->handle = slot + 1;
	return 0;
}

static void
drm_hwrt_destroy(struct drm_hwrt *h)
{
	for (unsigned i = 0; i < DRM_ALL_HWRTS; i++) {
		if (drm_hwrts[i] == h) {
			drm_hwrts[i] = NULL;
		}
	}
	for (struct drm_job *job = drm_jobs; job != NULL; job = job->next) {    /* (its jobs are the contexts' to wait for) */
		if (job->hwrt == h) {
			job->hwrt = NULL;
		}
	}
	if (!pvr_hwrt_destroy(&h->hwrt)) {
		SAY("pvr: a render target the firmware will not let go of is left as it is");
		return;
	}
	(void)pvr_mmu_flush();
	drm_live.hwrts--;
	drm_fl_put(h->fl[0]);
	drm_fl_put(h->fl[1]);
	kfree_data(h, sizeof(*h));
}

/* pvr_ioctl_create_hwrt_dataset with pvr_hwrt.c's pvr_hwrt_dataset_create */
static int
drm_create_hwrt(struct redwood_pvr_file *file, struct drm_pvr_create_hwrt *r)
{
	struct drm_fl *fl0 = DRM_GET(file->fl, DRM_FLS, r->free_list[0]), *fl1 = DRM_GET(file->fl, DRM_FLS, r->free_list[1]);
	struct pvr_hwrt_args args;
	struct drm_hwrt *h;
	uint32_t slot;

	DRM_SLOT(file->hwrt, DRM_HWRTS, slot);
	if (fl0 == NULL || fl1 == NULL) {
		return EINVAL;
	}
	if (slot == DRM_HWRTS || (h = drm_new(sizeof(*h))) == NULL) {
		return ENOMEM;
	}
	memset(&args, 0, sizeof(args));
	args.width = r->width;
	args.height = r->height;
	args.samples = r->samples;
	args.layers = r->layers;
	args.isp_merge_lower_x = r->isp_merge_lower_x;
	args.isp_merge_lower_y = r->isp_merge_lower_y;
	args.isp_merge_upper_x = r->isp_merge_upper_x;
	args.isp_merge_upper_y = r->isp_merge_upper_y;
	args.isp_merge_scale_x = r->isp_merge_scale_x;
	args.isp_merge_scale_y = r->isp_merge_scale_y;
	args.region_header_size = r->region_header_size;
	args.tpc_size = r->tpc_size;
	args.tpc_stride = r->tpc_stride;
	args.tpc_dev_addr = r->tpc_dev_addr;
	args.vheap_table_dev_addr = r->vheap_table_dev_addr;
	args.rtc_dev_addr = r->rtc_dev_addr;
	for (unsigned i = 0; i < 2; i++) {
		args.rt[i].pm_mlist_dev_addr = r->rt[i].pm_mlist_dev_addr;
		args.rt[i].macrotile_array_dev_addr = r->rt[i].macrotile_array_dev_addr;
		args.rt[i].region_header_dev_addr = r->rt[i].region_header_dev_addr;
	}
	args.free_lists[0] = &fl0->fl;
	args.free_lists[1] = &fl1->fl;
	if (!pvr_hwrt_create(&h->hwrt, &args)) {
		kfree_data(h, sizeof(*h));
		return EINVAL;
	}
	h->fl[0] = fl0;
	h->fl[1] = fl1;
	fl0->refs++;
	fl1->refs++;
	for (unsigned i = 0; i < DRM_ALL_HWRTS; i++) {
		if (drm_hwrts[i] == NULL) {
			drm_hwrts[i] = h;
			break;
		}
	}
	file->hwrt[slot] = h;
	drm_live.hwrts++;
	r->handle = slot + 1;
	return 0;
}

/* pvr_job.c's convert_*_flags */
static uint32_t
drm_job_flags(uint32_t type, uint32_t in)
{
	static const uint32_t frag[8] = { GEN_ROGUE_FRAG_FLAGS_SINGLE_CORE, GEN_ROGUE_FRAG_FLAGS_DEPTHBUFFER, GEN_ROGUE_FRAG_FLAGS_STENCILBUFFER,
		                          GEN_ROGUE_FRAG_FLAGS_PREVENT_CDM_OVERLAP, GEN_ROGUE_FRAG_FLAGS_SCRATCHBUFFER,
		                          GEN_ROGUE_FRAG_FLAGS_GET_VIS_RESULTS, 0 /* PARTIAL_RENDER: the command's type instead */,
		                          GEN_ROGUE_FRAG_FLAGS_DISABLE_PIXELMERGE };
	static const uint32_t geom[3] = { GEN_ROGUE_GEOM_FLAGS_FIRSTKICK, GEN_ROGUE_GEOM_FLAGS_LASTKICK, GEN_ROGUE_GEOM_FLAGS_SINGLE_CORE };
	static const uint32_t compute[2] = { GEN_ROGUE_COMPUTE_FLAG_PREVENT_ALL_OVERLAP, GEN_ROGUE_COMPUTE_FLAG_SINGLE_CORE };
	uint32_t out = 0;

	for (unsigned bit = 0; bit < 8; bit++) {
		if ((in >> bit) & 1) {
			out |= type == 0 ? geom[bit % 3] : type == 1 ? frag[bit] : type == 2 ? compute[bit % 2] : GEN_ROGUE_TRANSFER_FLAGS_SINGLE_CORE;
		}
	}
	return out;
}

/* pvr_job.c's create_job with pvr_job_fw_cmd_init and pvr_queue.c's pvr_queue_job_init: one job checked and its firmware command made */
static int
drm_job_create(struct redwood_pvr_file *file, const struct drm_pvr_job *a, struct drm_job **out)
{
	static const struct {
		const struct stream_def *main;
		uint32_t count;
		const struct stream_ext *ext;
		uint32_t ext_count, headers, dest_size, flag_mask, flags_at, context_type, cccb_type;
	} kinds[4] = {
		{ stream_geom_main, sizeof(stream_geom_main) / sizeof(stream_geom_main[0]), stream_geom_ext, 1, stream_geom_EXT_HEADERS,
		  stream_geom_DEST_SIZE, 0x7, GEN_rogue_fwif_cmd_geom__flags, PVR_CTX_RENDER, GEN_ROGUE_FWIF_CCB_CMD_TYPE_GEOM },
		{ stream_frag_main, sizeof(stream_frag_main) / sizeof(stream_frag_main[0]), stream_frag_ext, 2, stream_frag_EXT_HEADERS,
		  stream_frag_DEST_SIZE, 0xff, GEN_rogue_fwif_cmd_frag__flags, PVR_CTX_RENDER, GEN_ROGUE_FWIF_CCB_CMD_TYPE_FRAG },
		{ stream_compute_main, sizeof(stream_compute_main) / sizeof(stream_compute_main[0]), stream_compute_ext, 1, stream_compute_EXT_HEADERS,
		  stream_compute_DEST_SIZE, 0x3, GEN_rogue_fwif_cmd_compute__flags, PVR_CTX_COMPUTE, GEN_ROGUE_FWIF_CCB_CMD_TYPE_CDM },
		{ stream_transfer_main, sizeof(stream_transfer_main) / sizeof(stream_transfer_main[0]), NULL, 0, 0,
		  stream_transfer_DEST_SIZE, 0x1, GEN_rogue_fwif_cmd_transfer__flags, PVR_CTX_TRANSFER_FRAG, GEN_ROGUE_FWIF_CCB_CMD_TYPE_TQ_3D },
	};
	struct drm_ctx *c = DRM_GET(file->ctx, DRM_CTXS, a->context);
	struct drm_hwrt *hwrt = DRM_GET(file->hwrt, DRM_HWRTS, a->hwrt);
	uint8_t stream[1024];
	struct drm_job *job;
	int error;

	if (a->stream == 0 || a->stream_len == 0 || a->stream_len > sizeof(stream) || a->type > 3 || (a->type > 1 && (a->hwrt != 0 || a->hwrt_index != 0)) ||
	    c == NULL || (a->flags & ~kinds[a->type].flag_mask) != 0 || c->ctx.type != kinds[a->type].context_type ||
	    (a->type <= 1 && (hwrt == NULL || a->hwrt_index > 1))) {
		return EINVAL;
	}
	if (c->ctx.faulty) {
		return EIO;             /* pvr_queue_job_init */
	}
	if ((job = drm_new(sizeof(*job))) == NULL) {
		return ENOMEM;
	}
	job->born = mach_absolute_time();
	job->ctx = &c->ctx;
	job->q = a->type == 1 ? 1 : 0;
	job->type = a->type;
	job->cmd_len = kinds[a->type].dest_size;
	job->cccb_type = a->type == 1 && (a->flags & 0x40) != 0 ? GEN_ROGUE_FWIF_CCB_CMD_TYPE_FRAG_PR : kinds[a->type].cccb_type;
	if ((error = drm_in(file, a->stream, stream, a->stream_len)) == 0) {
		error = stream_process(kinds[a->type].main, kinds[a->type].count, kinds[a->type].ext, kinds[a->type].ext_count, kinds[a->type].headers,
		    stream, a->stream_len, job->cmd, job->cmd_len);
	}
	/* pvr_queue_job_init: one that could never fit its buffer (pvr_cccb_cmdseq_can_fit: half of it) */
	if (error == 0 && job_cmds_size(job, a->type == 1 ? 1 : 0) + CCB_HEADER > CCCB_SIZE / 2) {
		error = E2BIG;
	}
	if (error != 0) {
		kfree_data(job, sizeof(*job));
		return error;
	}
	*(uint32_t *)(void *)(job->cmd + kinds[a->type].flags_at) = drm_job_flags(a->type, a->flags);    /* frame_num, at 0, stays 0 */
	if (a->type <= 1) {
		job->hwrt = hwrt;
		job->hwrt_index = a->hwrt_index;
		*(uint32_t *)(void *)(job->cmd + GEN_rogue_fwif_cmd_geom__cmd_shared_hwrt_data_fw_addr) = hwrt->hwrt.data[a->hwrt_index].addr;
	}
	if (drm_debug) {
		static const char *const names[4] = { "geometry", "fragment", "compute", "transfer" };
		const uint64_t *regs = (const uint64_t *)(const void *)(job->cmd + (a->type == 1 ? 112 : 16));  /* the first register fields of each command */

		SAY("pvr: job: %s on context %u (handle %u): stream %u bytes, flags %#x -> %#x, command %#x of %u bytes, render target data %#x; "
		    "its first registers %#llx %#llx %#llx", names[a->type], c->ctx.id, a->context, a->stream_len, a->flags,
		    *(uint32_t *)(void *)(job->cmd + kinds[a->type].flags_at), job->cccb_type, job->cmd_len,
		    a->type <= 1 ? hwrt->hwrt.data[a->hwrt_index].addr : 0, regs[0], regs[1], regs[2]);
	}
	*out = job;
	return 0;
}

/* What a job must wait for, as the jobs it comes down to. False if it is more than a job can carry. */
static bool
drm_job_depend(struct drm_job *job, uint64_t fence)
{
	if (drm_job_of(fence) != NULL) {
		for (uint32_t i = 0; i < job->deps; i++) {
			if (job->dep[i] == fence) {
				return true;
			}
		}
		if (job->deps == DRM_JOB_DEPS) {
			return false;
		}
		job->dep[job->deps++] = fence;
		return true;
	}
	for (const struct drm_merge *m = drm_merges; m != NULL; m = m->next) {
		if (m->fence == fence) {
			return drm_job_depend(job, m->of[0]) && drm_job_depend(job, m->of[1]);
		}
	}
	return true;
}

/*
 * pvr_job.c's pvr_submit_jobs: every job made and checked before any is queued; a job's WAIT
 * operations become what it waits for, its SIGNAL operations make it the syncobj's fence -- seen at
 * once by the jobs after it in the same call (the "signal array") and by everybody when all are in.
 * A geometry job waits for the jobs before it on the same render target data, a fragment job for
 * the geometry jobs (Linux: the data's reservation object, written by geometry, read by fragment).
 */
#define SUBMIT_JOBS     32
#define SUBMIT_SYNCS    64
static int
drm_submit(struct redwood_pvr_file *file, const struct drm_pvr_obj_array *jobs)
{
	struct drm_job *made[SUBMIT_JOBS] = { NULL };
	struct { uint32_t handle; uint64_t fence; } staged[SUBMIT_JOBS * 4];
	uint32_t stagings = 0, count = 0;
	int error = 0;

	if (jobs->count == 0) {
		return 0;
	}
	if (jobs->count > SUBMIT_JOBS || jobs->stride < sizeof(struct drm_pvr_job)) {
		return EINVAL;
	}
	/*
	 * pvr_submit_jobs' "Flush MMU if needed - this has been deferred until now": what has been
	 * mapped since the last flush is not the GPU's to use until the firmware has been told to drop
	 * its cached translations (pvr_mmu_flush_exec) -- a new page table under a directory entry the
	 * GPU remembers as empty is a page fault. Until the window server's run this was done only on
	 * unmapping and destroying, which a test that maps little never needed.
	 */
	if (drm_stats && !drm_memory_said) {
		drm_memory_said = true;
		drm_memory_say("before the first job");
	}
	if (mmu_flush_flags != 0 && !pvr_mmu_flush()) {
		return EIO;
	}
	for (; count < jobs->count && error == 0; count++) {
		struct drm_pvr_job a;
		struct drm_pvr_sync_op op;

		if ((error = drm_in(file, jobs->array + (uint64_t)count * jobs->stride, &a, sizeof(a))) != 0 ||
		    (error = drm_job_create(file, &a, &made[count])) != 0) {
			break;
		}
		made[count]->fence = drm_fences++;
		if (a.sync_ops.count > SUBMIT_SYNCS || (a.sync_ops.count != 0 && a.sync_ops.stride < sizeof(op))) {
			error = EINVAL;
		}
		for (uint32_t i = 0; i < a.sync_ops.count && error == 0; i++) {
			uint64_t fence = 0;
			bool found = false;

			if ((error = drm_in(file, a.sync_ops.array + (uint64_t)i * a.sync_ops.stride, &op, sizeof(op))) != 0) {
				break;
			}
			/* pvr_check_sync_op: a binary syncobj has no point; a timeline's point is not kept here, so only 0 */
			if ((op.flags & ~0x8000000fu) != 0 || (op.flags & 0xf) > 1 || op.value != 0 || DRM_GET(file->sync, DRM_SYNCS, op.handle) == SYNC_FREE) {
				error = EINVAL;
			} else if ((op.flags & 0x80000000u) != 0) {
				if (stagings == SUBMIT_JOBS * 4) {
					error = EINVAL;
				} else {
					staged[stagings].handle = op.handle;
					staged[stagings++].fence = made[count]->fence;
					if (drm_debug) {
						SAY("pvr: job %llu: will be syncobj %u's fence", made[count]->fence, op.handle);
					}
				}
			} else {
				for (int s = (int)stagings - 1; s >= 0 && !found; s--) {
					if (staged[s].handle == op.handle) {
						fence = staged[s].fence;
						found = true;
					}
				}
				if (!found && file->sync[op.handle - 1] == SYNC_EMPTY) {
					error = EINVAL;         /* drm_syncobj_find_fence: nothing to wait for */
				} else if (!found) {
					fence = FENCE_OF(file->sync[op.handle - 1]);
				}
				for (uint32_t j = 0; j < count && error == 0; j++) {    /* one of this call's own, not yet in the list of jobs */
					if (made[j]->fence == fence && made[count]->deps < DRM_JOB_DEPS - 4) {
						made[count]->dep[made[count]->deps++] = fence;
					}
				}
				/* (four places kept for the render target's own order, below) */
				if (error == 0 && (!drm_job_depend(made[count], fence) || made[count]->deps > DRM_JOB_DEPS - 4)) {
					error = E2BIG;          /* more than a job can wait for: wait for some of them first */
				}
				if (drm_debug) {
					SAY("pvr: job %llu: waits for syncobj %u's fence %llu%s", made[count]->fence, op.handle, fence,
					    drm_fence_done(fence) && !found ? " (signalled already)" : "");
				}
			}
		}
		if (error != 0) {
			count++;
			break;
		}
	}
	if (error != 0) {
		for (uint32_t i = 0; i < count; i++) {
			if (made[i] != NULL) {
				kfree_data(made[i], sizeof(*made[i]));
			}
		}
		return error;
	}
	for (uint32_t i = 0; i < count; i++) {          /* push_jobs: from here nothing fails */
		struct drm_job *job = made[i];
		struct pvr_queue *queue = &job->ctx->queue[job->q];

		/* pvr_job.c's can_combine_jobs: a geometry job and the fragment job after it, same context, same render target data */
		if (job->type == 0 && i + 1 < count && made[i + 1]->type == 1 && made[i + 1]->ctx == job->ctx && made[i + 1]->hwrt == job->hwrt &&
		    made[i + 1]->hwrt_index == job->hwrt_index) {
			job->pair = made[i + 1];
			made[i + 1]->pair = job;
		}

		job->next = drm_jobs;
		drm_jobs = job;
		if (job->hwrt != NULL) {
			(void)drm_job_depend(job, job->hwrt->last_geom[job->hwrt_index]);
			if (job->type == 0) {
				(void)drm_job_depend(job, job->hwrt->last_frag[job->hwrt_index]);
				job->hwrt->last_geom[job->hwrt_index] = job->fence;
			} else {
				job->hwrt->last_frag[job->hwrt_index] = job->fence;
			}
		}
		job->seqno = ++queue->seqno;
		if (queue->last != NULL) {
			queue->last->queued = job;
		} else {
			queue->first = job;
		}
		queue->last = job;
		drm_submitted++;
	}
	for (uint32_t s = 0; s < stagings; s++) {       /* pvr_sync_signal_array_push_fences */
		file->sync[staged[s].handle - 1] = SYNC_OF(staged[s].fence);
	}
	drm_poll();
	thread_wakeup((event_t)&drm_serving);           /* the service thread: jobs are out, look often */
	return 0;
}

/* drm_syncobj.c's wait, by looking every millisecond: `timeout_ns` from now. */
static int
drm_syncobj_wait(struct redwood_pvr_file *file, struct drm_syncobj_wait *r)
{
	uint32_t handles[64];
	uint64_t began = mach_absolute_time(), ns = 0;
	int error;

	if (r->count == 0 || r->count > 64 || (r->flags & ~7u) != 0) {
		return EINVAL;
	}
	if ((error = drm_in(file, r->handles, handles, r->count * 4)) != 0) {
		return error;
	}
	for (;;) {
		uint32_t signalled = 0;

		fw_service();
		for (uint32_t i = 0; i < r->count; i++) {
			uint64_t sync = DRM_GET(file->sync, DRM_SYNCS, handles[i]);

			if (sync == SYNC_FREE || (sync == SYNC_EMPTY && (r->flags & 2) == 0)) {
				return sync == SYNC_FREE ? ENOENT : EINVAL;
			}
			if (sync != SYNC_EMPTY && drm_fence_done(FENCE_OF(sync))) {
				if (signalled++ == 0) {
					r->first_signaled = i;
				}
			}
		}
		if (signalled == r->count || (signalled != 0 && (r->flags & 1) == 0)) {
			drm_stats_woken();
			return 0;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		if (r->timeout_ns <= 0 || ns >= (uint64_t)r->timeout_ns) {
			return ETIME;
		}
		drm_wait_for_a_job((uint64_t)r->timeout_ns - ns);
	}
}

static int
drm_request(struct redwood_pvr_file *file, uint32_t number, void *data, uint32_t size)
{
	struct drm_pvr_handle *h = data;
	uint32_t slot, handles[64];
	int error;

#define NEED(type) if (size < sizeof(type)) { return EINVAL; }
	switch (number) {
	case 0x40:
		NEED(struct drm_pvr_dev_query);
		return drm_dev_query(file, data);
	case 0x41: {    /* CREATE_BO: pvr_ioctl_create_bo; the memory is the IOKit side's, in the GPU's 4 KiB pages */
		struct drm_pvr_create_bo *r = data;
		struct drm_bo *bo;

		NEED(struct drm_pvr_create_bo);
		DRM_SLOT(file->bo, DRM_BOS, slot);
		if (r->pad != 0 || r->size == 0 || r->size > DRM_BO_MAX || (r->flags & ~7ull) != 0 || (r->size & 0xfff) != 0) {
			return EINVAL;
		}
		if (slot == DRM_BOS || (bo = drm_new(sizeof(*bo))) == NULL) {
			return ENOMEM;
		}
		if ((bo->memory = drm_premade != NULL ? drm_premade : bo_make(r->size)) == NULL) {
			kfree_data(bo, sizeof(*bo));
			return ENOMEM;
		}
		drm_premade = NULL;
		bo->size = r->size;
		bo->flags = r->flags;
		bo->refs = 1;
		drm_live.bos++;
		drm_live.bo_bytes += bo->size;
		drm_live.bucket[drm_bucket(bo->size)]++;
		drm_live.bucket_bytes[drm_bucket(bo->size)] += bo->size;
		file->bo[slot] = bo;
		r->handle = slot + 1;
		return 0;
	}
	case 0x42: {    /* GET_BO_MMAP_OFFSET: the "offset" is the handle; the program maps the client memory of that type */
		struct drm_bo *bo;

		NEED(struct drm_pvr_handle);
		if (h->pad != 0) {
			return EINVAL;
		}
		if ((bo = DRM_GET(file->bo, DRM_BOS, h->handle)) == NULL) {
			return ENOENT;
		}
		h->offset = (uint64_t)h->handle << 12;
		return 0;
	}
	case 0x43: {    /* CREATE_VM_CONTEXT */
		struct drm_vm *vm;

		DRM_SLOT(file->vm, DRM_VMS, slot);
		if (size < 8 || h->pad != 0) {
			return EINVAL;
		}
		if (slot == DRM_VMS || (vm = drm_new(sizeof(*vm))) == NULL) {
			return ENOMEM;
		}
		if (!pvr_vm_create(&vm->vm)) {
			kfree_data(vm, sizeof(*vm));
			return ENOMEM;
		}
		vm->refs = 1;
		for (unsigned i = 0; i < DRM_ALL_VMS; i++) {
			if (drm_vms[i] == NULL) {
				drm_vms[i] = vm;
				break;
			}
		}
		file->vm[slot] = vm;
		h->handle = slot + 1;
		return 0;
	}
	case 0x44:      /* DESTROY_VM_CONTEXT: gone when what uses it is */
		if (size < 8 || h->pad != 0 || DRM_GET(file->vm, DRM_VMS, h->handle) == NULL) {
			return EINVAL;
		}
		drm_vm_put(file->vm[h->handle - 1]);
		file->vm[h->handle - 1] = NULL;
		return 0;
	case 0x45:
		NEED(struct drm_pvr_vm_map);
		return drm_vm_map(file, data);
	case 0x46:
		NEED(struct drm_pvr_vm_unmap);
		return drm_vm_unmap(file, data);
	case 0x47:
		NEED(struct drm_pvr_create_context);
		return drm_create_context(file, data);
	case 0x48:
		if (size < 8 || h->pad != 0 || DRM_GET(file->ctx, DRM_CTXS, h->handle) == NULL) {
			return EINVAL;
		}
		drm_context_destroy(file->ctx[h->handle - 1]);
		file->ctx[h->handle - 1] = NULL;
		return 0;
	case 0x49:
		NEED(struct drm_pvr_create_free_list);
		return drm_create_free_list(file, data);
	case 0x4a:
		if (size < 8 || h->pad != 0 || DRM_GET(file->fl, DRM_FLS, h->handle) == NULL) {
			return EINVAL;
		}
		drm_fl_put(file->fl[h->handle - 1]);
		file->fl[h->handle - 1] = NULL;
		return 0;
	case 0x4b:
		NEED(struct drm_pvr_create_hwrt);
		return drm_create_hwrt(file, data);
	case 0x4c:
		if (size < 8 || h->pad != 0 || DRM_GET(file->hwrt, DRM_HWRTS, h->handle) == NULL) {
			return EINVAL;
		}
		drm_hwrt_destroy(file->hwrt[h->handle - 1]);
		file->hwrt[h->handle - 1] = NULL;
		return 0;
	case 0x4d:
		NEED(struct drm_pvr_obj_array);
		return drm_submit(file, data);

	case 0x09:      /* DRM_IOCTL_GEM_CLOSE: the handle goes; the memory when its mappings have */
		if (size < 8 || DRM_GET(file->bo, DRM_BOS, h->handle) == NULL) {
			return EINVAL;
		}
		drm_bo_put(file->bo[h->handle - 1]);
		file->bo[h->handle - 1] = NULL;
		return 0;
	case 0x0c: {    /* DRM_IOCTL_GET_CAP: syncobjs (DRM_CAP_SYNCOBJ 0x13) yes; their timelines (0x14), PRIME (0x5) and the rest no */
		struct drm_get_cap *r = data;

		NEED(struct drm_get_cap);
		r->value = r->capability == 0x13;
		return 0;
	}
	case 0xbf: {    /* DRM_IOCTL_SYNCOBJ_CREATE */
		struct drm_syncobj_create *r = data;

		NEED(struct drm_syncobj_create);
		DRM_SLOT(file->sync, DRM_SYNCS, slot);
		if ((r->flags & ~1u) != 0) {
			return EINVAL;
		}
		if (slot == DRM_SYNCS) {
			return ENOMEM;
		}
		drm_live.syncs++;
		file->sync[slot] = (r->flags & 1) != 0 ? FENCE_SIGNALLED : SYNC_EMPTY;
		r->handle = slot + 1;
		return 0;
	}
	case 0xc0:      /* DRM_IOCTL_SYNCOBJ_DESTROY */
		if (size < 8 || DRM_GET(file->sync, DRM_SYNCS, h->handle) == SYNC_FREE) {
			return EINVAL;
		}
		drm_live.syncs--;
		file->sync[h->handle - 1] = SYNC_FREE;
		return 0;
	case 0xc3:
		NEED(struct drm_syncobj_wait);
		return drm_syncobj_wait(file, data);
	case 0xc4:      /* DRM_IOCTL_SYNCOBJ_RESET, _SIGNAL */
	case 0xc5: {
		struct drm_syncobj_array *r = data;

		NEED(struct drm_syncobj_array);
		if (r->count == 0 || r->count > 64) {
			return EINVAL;
		}
		if ((error = drm_in(file, r->handles, handles, r->count * 4)) != 0) {
			return error;
		}
		for (uint32_t i = 0; i < r->count; i++) {
			if (DRM_GET(file->sync, DRM_SYNCS, handles[i]) == SYNC_FREE) {
				return ENOENT;
			}
		}
		for (uint32_t i = 0; i < r->count; i++) {
			file->sync[handles[i] - 1] = number == 0xc4 ? SYNC_EMPTY : FENCE_SIGNALLED;
		}
		return 0;
	}
	case 0xcc: {
		/*
		 * DRM_IOCTL_SYNCOBJ_TRANSFER. Binary to binary (both points 0): the one's fence becomes the
		 * other's too. With a point on the destination (drm_syncobj_transfer_to_timeline): the
		 * source's fence is ADDED to what the destination waits for -- a timeline's later point
		 * waits for the earlier ones too, and that is all of a timeline that is kept here. With a
		 * point on the source only: what the source has gathered becomes the destination's fence.
		 */
		struct drm_syncobj_transfer *r = data;
		uint64_t source, merged;

		NEED(struct drm_syncobj_transfer);
		if (DRM_GET(file->sync, DRM_SYNCS, r->src) <= SYNC_EMPTY || DRM_GET(file->sync, DRM_SYNCS, r->dst) == SYNC_FREE) {
			return EINVAL;
		}
		source = FENCE_OF(file->sync[r->src - 1]);
		if (r->dst_point == 0 || file->sync[r->dst - 1] == SYNC_EMPTY) {
			file->sync[r->dst - 1] = file->sync[r->src - 1];
			return 0;
		}
		if ((merged = drm_fence_merge(FENCE_OF(file->sync[r->dst - 1]), source)) == 0) {
			return E2BIG;           /* too deep, or no memory: wait for one of them instead */
		}
		file->sync[r->dst - 1] = SYNC_OF(merged);
		return 0;
	}
	case REDWOOD_PVR_FENCE_MERGE: {         /* the same, said plainly: `handle`'s fence becomes itself and `fence` */
		struct redwood_pvr_fence *r = data;
		uint64_t merged;

		NEED(struct redwood_pvr_fence);
		if (DRM_GET(file->sync, DRM_SYNCS, r->handle) == SYNC_FREE) {
			return ENOENT;
		}
		merged = file->sync[r->handle - 1] == SYNC_EMPTY ? r->fence : drm_fence_merge(FENCE_OF(file->sync[r->handle - 1]), r->fence);
		if (merged == 0 && r->fence != 0) {
			return E2BIG;
		}
		file->sync[r->handle - 1] = merged == 0 ? SYNC_EMPTY : SYNC_OF(merged);
		return 0;
	}
	case REDWOOD_PVR_FENCE_EXPORT:
	case REDWOOD_PVR_FENCE_IMPORT: {
		struct redwood_pvr_fence *r = data;

		NEED(struct redwood_pvr_fence);
		if (DRM_GET(file->sync, DRM_SYNCS, r->handle) == SYNC_FREE) {
			return ENOENT;
		}
		if (number == REDWOOD_PVR_FENCE_EXPORT) {
			r->fence = file->sync[r->handle - 1] == SYNC_EMPTY ? 0 : drm_fence_done(FENCE_OF(file->sync[r->handle - 1])) ?
			    FENCE_SIGNALLED : FENCE_OF(file->sync[r->handle - 1]);
		} else {
			file->sync[r->handle - 1] = r->fence == 0 ? SYNC_EMPTY : SYNC_OF(r->fence);
		}
		return 0;
	}
	case REDWOOD_PVR_FENCE_WAIT: {
		struct redwood_pvr_fence_wait *r = data;

		NEED(struct redwood_pvr_fence_wait);
		return drm_fence_wait(r->fence, r->timeout_ns);
	}
	default:
		return ENOTTY;
	}
#undef NEED
}

/* ---- what RedwoodPVR.cpp calls ---- */

bool
redwood_pvr_ready(void)
{
	return drm_serving;
}

uint64_t
redwood_pvr_last_fence(void)
{
	return drm_serving ? drm_fences - 1 : 0;
}

/* No lock held. Every job with a fence up to `fence`, done (a merge's number is no job: up to it is all the same). */
bool
redwood_pvr_wait_upto(uint64_t fence, uint64_t ms)
{
	uint64_t began = mach_absolute_time(), ns = 0;
	bool left;

	if (!drm_serving || drm_lock == NULL) {
		return true;
	}
	lck_mtx_lock(drm_lock);
	for (;;) {
		fw_service();
		left = false;
		for (const struct drm_job *job = drm_jobs; job != NULL && !left; job = job->next) {
			left = job->fence <= fence;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		if (!left || ns >= ms * NSEC_PER_MSEC) {
			break;
		}
		drm_wait_for_a_job(ms * NSEC_PER_MSEC - ns);
	}
	lck_mtx_unlock(drm_lock);
	return !left;
}

void
redwood_pvr_provide(const struct redwood_pvr_bo_ops *ops)
{
	bo_ops = ops;
}

struct redwood_pvr_file *
redwood_pvr_open(bool kernel)
{
	struct redwood_pvr_file *file = drm_serving ? drm_new(sizeof(*file)) : NULL;

	if (file != NULL) {
		file->kernel = kernel;
		drm_live.files++;       /* (not under the lock: a count for the eye) */
	}
	return file;
}

int
redwood_pvr_request(struct redwood_pvr_file *file, uint32_t number, void *data, uint32_t size)
{
	int error;

	/* which line of the statistics: pvr_drm.h's fourteen, then the others in the order of drm_call_names */
	unsigned which = number >= 0x40 && number <= 0x4d ? number - 0x40 : number == 0x09 ? 14 : number == 0x0c ? 15 : number == 0xbf ? 16 :
	    number == 0xc0 ? 17 : number == 0xc3 ? 18 : number == 0xc4 ? 19 : number == 0xc5 ? 20 : number == 0xcc ? 21 :
	    number >= 0x7a && number <= 0x7d ? 22 + (number - 0x7a) : 27;
	uint64_t called = mach_absolute_time(), locked, ns = 0;
	uint32_t us;
	void *premade = NULL;

	if (number == 0x41 && size >= sizeof(struct drm_pvr_create_bo)) {       /* CREATE_BO's memory: made and zeroed with no lock held */
		const struct drm_pvr_create_bo *r = data;

		if (r->size != 0 && r->size <= DRM_BO_MAX && (r->size & 0xfff) == 0 && (r->flags & ~7ull) == 0 && r->pad == 0) {
			premade = bo_make(r->size);
		}
	}
	lck_mtx_lock(drm_lock);
	locked = mach_absolute_time();
	drm_slept = 0;
	drm_premade = premade;
	if (drm_reset_wanted != NULL && !drm_lost) {
		drm_reset(drm_reset_wanted);
	}
	/* pvr_device_lost: nothing more is done; what gives things back still answers, so that a program can end */
	error = drm_lost && number != 0x09 && number != 0xc0 && number != 0x44 && number != 0x48 && number != 0x4a && number != 0x4c ? EIO :
	    drm_request(file, number, data, size);
	if (drm_stats) {
		absolutetime_to_nanoseconds(mach_absolute_time() - locked - drm_slept, &ns);
		if (ns / NSEC_PER_USEC > drm_hold_most_us) {
			drm_hold_most_us = (uint32_t)(ns / NSEC_PER_USEC);
		}
		us = drm_us_since(called);
		drm_calls[which].count++;
		drm_calls[which].total_us += us;
		if (us > drm_calls[which].most_us) {
			drm_calls[which].most_us = us;
		}
	}
	premade = drm_premade;          /* not taken: the request was refused */
	drm_premade = NULL;
	if (drm_dead_count != 0) {
		thread_wakeup((event_t)&drm_serving);   /* the service thread: a flush is owed within 2 ms */
	}
	lck_mtx_unlock(drm_lock);
	if (premade != NULL) {
		bo_drop(premade);
	}
	drm_reap();
	return error;
}

/*
 * Memory made elsewhere as a buffer of this file's -- an IOSurface's pixels, where Linux has a
 * dma-buf (RedwoodPVR.cpp's request REDWOOD_PVR_SURFACE): 0 and a new handle, the buffer now holding
 * `memory` and `owner`; EEXIST and the handle it already has, for the same owner; or an errno. The
 * buffer can be mapped by the program and into a VM like any other; it cannot hold a free list.
 */
int
redwood_pvr_import(struct redwood_pvr_file *file, void *memory, uint64_t size, void *owner, uint32_t *handle)
{
	struct drm_bo *bo;
	uint32_t slot;
	int error = 0;

	lck_mtx_lock(drm_lock);
	DRM_SLOT(file->bo, DRM_BOS, slot);
	for (uint32_t i = 0; i < DRM_BOS; i++) {
		if (file->bo[i] != NULL && file->bo[i]->owner == owner) {
			*handle = i + 1;
			error = EEXIST;
		}
	}
	if (error == 0 && (size == 0 || (size & 0xfff) != 0 || size > DRM_BO_MAX)) {
		error = EINVAL;
	} else if (error == 0 && (drm_lost || slot == DRM_BOS || (bo = drm_new(sizeof(*bo))) == NULL)) {
		error = drm_lost ? EIO : ENOMEM;
	} else if (error == 0) {
		bo->memory = memory;
		bo->owner = owner;
		bo->size = size;
		bo->flags = 4;          /* DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS */
		bo->refs = 1;
		drm_live.bos++;
		drm_live.imported++;
		drm_live.imported_bytes += size;
		drm_live.bo_bytes += size;
		file->bo[slot] = bo;
		*handle = slot + 1;
	}
	lck_mtx_unlock(drm_lock);
	return error;
}

void *
redwood_pvr_memory(struct redwood_pvr_file *file, uint32_t handle)
{
	struct drm_bo *bo;
	void *memory = NULL;

	lck_mtx_lock(drm_lock);
	bo = DRM_GET(file->bo, DRM_BOS, handle);
	if (bo != NULL && (bo->flags & 4) != 0) {       /* DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS: pvr_gem.c's mmap refuses the others */
		bo_ops->keep(bo->memory);
		memory = bo->memory;
	}
	lck_mtx_unlock(drm_lock);
	return memory;
}

/*
 * A program gone, however it went: pvr_drv.c's pvr_drm_driver_postclose in its order -- contexts
 * (their jobs with them), render targets, free lists, address spaces -- then its buffers and
 * syncobjs. The firmware is told of each (the CLEANUP commands), so nothing of a crashed program's
 * stays in its tables.
 */
void
redwood_pvr_close(struct redwood_pvr_file *file)
{
	lck_mtx_lock(drm_lock);
	for (unsigned i = 0; i < DRM_CTXS; i++) {
		if (file->ctx[i] != NULL) {
			drm_context_destroy(file->ctx[i]);
		}
	}
	for (unsigned i = 0; i < DRM_HWRTS; i++) {
		if (file->hwrt[i] != NULL) {
			drm_hwrt_destroy(file->hwrt[i]);
		}
	}
	for (unsigned i = 0; i < DRM_FLS; i++) {
		if (file->fl[i] != NULL) {
			drm_fl_put(file->fl[i]);
		}
	}
	for (unsigned i = 0; i < DRM_VMS; i++) {
		if (file->vm[i] != NULL) {
			drm_vm_put(file->vm[i]);
		}
	}
	for (unsigned i = 0; i < DRM_BOS; i++) {
		if (file->bo[i] != NULL) {
			drm_bo_put(file->bo[i]);
		}
	}
	for (unsigned i = 0; i < DRM_SYNCS; i++) {
		drm_live.syncs -= file->sync[i] != SYNC_FREE;
	}
	drm_live.files--;
	(void)pvr_mmu_flush();          /* so that what it had can be given back now */
	lck_mtx_unlock(drm_lock);
	drm_reap();
	kfree_data(file, sizeof(*file));
}

/*
 * Stage 6: the service started, then every kind of request made once from inside the kernel, as a
 * program's first calls would be, and all of it closed again. No job: a job needs what only Mesa
 * makes (control streams, PDS and USC programs), and a made-up one could only fault the GPU.
 * The static context state is a stream of the right length with nothing in it, and the render
 * target's buffers one mapped buffer with sizes of 0, as in stage 5: it is never rendered to.
 */
static bool
fw_stage6(void)
{
#define T6(what, number, r) (error = redwood_pvr_request(file, number, &(r), sizeof(r)), \
	SAY("pvr: s6: %s: %s (%d)", what, error == 0 ? "ok" : "FAILED", error), error == 0)
	struct redwood_pvr_file *file;
	struct { uint64_t gpu_id; uint32_t phantoms, pad; } gpu = { 0, 0, 0 };
	struct { uint64_t min_pages, max_pages; uint32_t info[4]; } runtime;
	struct { uint64_t array; uint16_t count, musthave; uint32_t pad; } quirks = { 0, 5, 0, 0 };
	uint32_t quirk[5] = { 0 }, stream[64] = { 0 }, length = 8, handles[1];
	struct drm_pvr_dev_query query = { 0, sizeof(gpu), (uint64_t)(uintptr_t)&gpu };
	struct drm_pvr_create_bo list = { 4096, 0, 0, PVR_BO_PM_FW_PROTECT }, bo = { 16 * 4096, 0, 0, 4 };
	struct drm_pvr_handle vm = { 0, 0, 0 }, h = { 0, 0, 0 };
	struct drm_pvr_vm_map map;
	struct drm_pvr_vm_unmap unmap;
	struct drm_pvr_create_context context;
	struct drm_pvr_create_free_list fl;
	struct drm_pvr_create_hwrt hwrt;
	struct drm_syncobj_create sync = { 0, 1 }, sync2 = { 0, 0 };
	struct drm_syncobj_wait wait;
	struct drm_syncobj_transfer transfer;
	struct redwood_pvr_fence fence;
	uint32_t seconds = 0, hangtest = 0, debug = 0, stats = 0;
	struct drm_pvr_obj_array none = { sizeof(struct drm_pvr_job), 0, 0 };
	uint64_t base = pvr_heaps[PVR_HEAP_GENERAL].base + 0x100000;
	bool ok = true;
	int error;

	if (drm_lock == NULL) {
		drm_lock = lck_mtx_alloc_init(lck_grp_alloc_init("redwood-pvr", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
	}
	if (s5_vm.pc != NULL && s5_teardown() != NULL) {         /* stage 5's things, if it left them: a reset must find only what it knows how to write again */
		SAY("pvr: stage 6: NOT DONE: stage 5's objects could not be cleaned up");
		return false;
	}
	(void)PE_parse_boot_argn("pvr_debug", &debug, sizeof(debug));
	(void)PE_parse_boot_argn("pvr_hangtest", &hangtest, sizeof(hangtest));
	if (PE_parse_boot_argn("pvr_timeout", &seconds, sizeof(seconds)) && seconds != 0) {
		drm_timeout_ms = seconds * 1000;
	}
	(void)PE_parse_boot_argn("pvr_stats", &stats, sizeof(stats));
	drm_stats = stats != 0;
	drm_debug = debug != 0;
	SAY("pvr: s6: starting the RedwoodPVR service: programs can open the GPU from here on (a job has %u s; pvr_debug %u)",
	    drm_timeout_ms / 1000, debug);
	drm_serving = true;
	RedwoodPVRStart();
	if (bo_ops == NULL || (file = redwood_pvr_open(true)) == NULL) {
		SAY("pvr: stage 6: NOT DONE: the service gave no buffer memory (RedwoodPVR.cpp did not start)");
		drm_serving = false;
		return false;
	}
	ok = T6("DEV_QUERY GPU_INFO", 0x40, query) && ok;
	query = (struct drm_pvr_dev_query) { 1, sizeof(runtime), (uint64_t)(uintptr_t)&runtime };
	ok = T6("DEV_QUERY RUNTIME_INFO", 0x40, query) && ok;
	quirks.array = (uint64_t)(uintptr_t)quirk;
	query = (struct drm_pvr_dev_query) { 2, sizeof(quirks), (uint64_t)(uintptr_t)&quirks };
	ok = T6("DEV_QUERY QUIRKS", 0x40, query) && ok;
	SAY("pvr: s6:   GPU id %#llx, %u phantoms; free lists %llu..%llu pages, common store %u/%u, coefficients %u, CDM local %u; %u quirks (%u must-have), the first %u",
	    gpu.gpu_id, gpu.phantoms, runtime.min_pages, runtime.max_pages, runtime.info[0], runtime.info[1], runtime.info[2], runtime.info[3],
	    quirks.count, quirks.musthave, quirk[0]);

	ok = T6("CREATE_BO 4 KiB for a free list", 0x41, list) && ok;
	ok = T6("CREATE_BO 64 KiB", 0x41, bo) && ok;
	h.handle = bo.handle;
	ok = T6("GET_BO_MMAP_OFFSET", 0x42, h) && ok;
	ok = T6("CREATE_VM_CONTEXT", 0x43, vm) && ok;
	map = (struct drm_pvr_vm_map) { vm.handle, 0, base, list.handle, 0, 0, 4096 };
	ok = T6("VM_MAP of the list", 0x45, map) && ok;
	map = (struct drm_pvr_vm_map) { vm.handle, 0, base + 0x10000, bo.handle, 0, 0, 16 * 4096 };
	ok = T6("VM_MAP of the buffer", 0x45, map) && ok;

	/* a static render context state that says nothing: its length, a zero, and a zero for each field this core has */
	for (unsigned i = 0; i < sizeof(stream_static_render_main) / sizeof(stream_static_render_main[0]); i++) {
		const struct stream_def *d = &stream_static_render_main[i];

		length += d->feature == SF_NONE || stream_feature(d->feature) != d->absent ? d->bytes : 0;
	}
	stream[0] = length;
	context = (struct drm_pvr_create_context) { PVR_CTX_RENDER, 0, 0, 0, (uint64_t)(uintptr_t)stream, length, vm.handle, 0 };
	ok = (length <= sizeof(stream)) && T6("CREATE_CONTEXT render", 0x47, context) && ok;
	fl = (struct drm_pvr_create_free_list) { base, 64, 128, 32, 0, vm.handle, 0 };
	ok = T6("CREATE_FREE_LIST of 64 pages", 0x49, fl) && ok;
	memset(&hwrt, 0, sizeof(hwrt));
	hwrt.tpc_dev_addr = base + 0x10000;
	hwrt.vheap_table_dev_addr = base + 0x11000;
	hwrt.rtc_dev_addr = base + 0x12000;
	for (unsigned i = 0; i < 2; i++) {
		hwrt.rt[i].pm_mlist_dev_addr = base + 0x13000 + i * 0x3000;
		hwrt.rt[i].macrotile_array_dev_addr = base + 0x14000 + i * 0x3000;
		hwrt.rt[i].region_header_dev_addr = base + 0x15000 + i * 0x3000;
		hwrt.free_list[i] = fl.handle;
	}
	hwrt.width = 64;
	hwrt.height = 64;
	hwrt.samples = 1;
	hwrt.layers = 1;
	ok = T6("CREATE_HWRT_DATASET 64x64", 0x4b, hwrt) && ok;
	ok = T6("SYNCOBJ_CREATE signalled", 0xbf, sync) && ok;
	handles[0] = sync.handle;
	wait = (struct drm_syncobj_wait) { (uint64_t)(uintptr_t)handles, 0, 1, 0, 0, 0 };
	ok = T6("SYNCOBJ_WAIT", 0xc3, wait) && ok;
	ok = T6("SUBMIT_JOBS of none", 0x4d, none) && ok;
	/* fences merged: a second syncobj given the first's (signalled) fence by a TRANSFER onto a timeline point, then by the plain request */
	ok = T6("SYNCOBJ_CREATE", 0xbf, sync2) && ok;
	transfer = (struct drm_syncobj_transfer) { sync.handle, sync2.handle, 0, 1, 0, 0 };
	ok = T6("SYNCOBJ_TRANSFER onto a point", 0xcc, transfer) && ok;
	fence = (struct redwood_pvr_fence) { sync2.handle, 0, FENCE_SIGNALLED };
	ok = T6("FENCE_MERGE", REDWOOD_PVR_FENCE_MERGE, fence) && ok;
	handles[0] = sync2.handle;
	ok = T6("SYNCOBJ_WAIT on the merged one", 0xc3, wait) && ok;
	(void)pvr_mmu_flush();

	if (hangtest != 0) {
		/* boot-arg pvr_hangtest=1: the recovery itself, with everything above still open -- the firmware zeroed, loaded and started again */
		lck_mtx_lock(drm_lock);
		drm_reset("the self-test asked for it (pvr_hangtest=1)");
		lck_mtx_unlock(drm_lock);
		ok = !drm_lost && ok;
		ok = T6("SYNCOBJ_WAIT after the reset", 0xc3, wait) && ok;
		SAY("pvr: s6:   after the reset: the context's firmware structure names its buffer %#x and memory context %#x again",
		    fresh32(file->ctx[context.handle - 1]->ctx.fw.cpu + 4), fresh32(file->ctx[context.handle - 1]->ctx.fw.cpu + 136));
	}

	h = (struct drm_pvr_handle) { hwrt.handle, 0, 0 };
	ok = T6("DESTROY_HWRT_DATASET", 0x4c, h) && ok;
	h.handle = fl.handle;
	ok = T6("DESTROY_FREE_LIST", 0x4a, h) && ok;
	h.handle = context.handle;
	ok = T6("DESTROY_CONTEXT", 0x48, h) && ok;
	unmap = (struct drm_pvr_vm_unmap) { vm.handle, 0, base + 0x10000, 16 * 4096 };
	ok = T6("VM_UNMAP of the buffer", 0x46, unmap) && ok;
	h.handle = bo.handle;
	ok = T6("GEM_CLOSE of the buffer", 0x09, h) && ok;
	redwood_pvr_close(file);        /* and what is left -- the list's mapping and buffer, the VM, the syncobj -- by the close */
	fw_dump();
	if (!ok) {
		SAY("pvr: stage 6: NOT DONE: a request above failed");
		return false;
	}
	drm_irq_attach();
	fw_quiet = !drm_debug;          /* from here a command (an MMU flush for an unmapping, a cleanup) is a frame's business, not the log's */
	SAY("pvr: stage 6: every request answered, everything closed; %#x of the arena in use; RedwoodPVR serves", arena_used);
	return true;
#undef T6
}
