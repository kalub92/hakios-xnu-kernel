/*
 * CLDMA for the r1md modem driver (docs/cellular-attach-2.md 5b, 5d).
 *  - cldma_snap(): READ-ONLY register snapshots (always available), guarded by the CLDMA clock gate and MD1 power; logged only when changed.
 *  - the receive ring (SO queue 0): only with boot-arg r1_md_cldma=1 (redwood_md.c calls cldma_rx_start only then). With the arg absent
 *    not one CLDMA register is written. Polled, no interrupts. Received packets are only counted and their first bytes logged; nothing is
 *    forwarded and nothing is transmitted.
 */
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);
extern void delay(int usec);
extern void redwood_md_say(bool must, const char *format, ...) __printflike(2, 3);

#define AO_BASE         0x10014000UL
#define PD_BASE         0x1021b000UL
#define CG2_STA         0xac            /* INFRA: bit 3 = CLDMA_BCLK gated */
#define RING_N          32
#define BUF_BYTES       4096u
#define DL_MTU          3584u
#define CANARY          0xc1d3a5a5u
#define MAX_RX          2000u           /* own stop rule: a flood is not what this step is for */
#define MAX_LOG         10u

static vm_offset_t ao, pd, infra_b;

static inline uint32_t
rd(vm_offset_t b, uint32_t o)
{
	return *(volatile uint32_t *)(b + o);
}

static inline void
wr(vm_offset_t b, uint32_t o, uint32_t v)
{
	*(volatile uint32_t *)(b + o) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

void
cldma_maps(vm_offset_t infra)
{
	infra_b = infra;
	ao = ml_io_map(AO_BASE, 0x1000);
	pd = ml_io_map(PD_BASE, 0x1000);
}

/* ---- read-only snapshot ---- */

static const uint16_t ao_regs[] = { 0x400, 0x404, 0x408, 0x414, 0x418, 0x800 };
static const uint16_t pd_regs[] = { 0x28, 0x3c, 0x400, 0x410, 0x414, 0x418, 0x41c, 0x800, 0x830, 0x834, 0x838, 0x83c, 0x878 };
static uint32_t ao_last[6], pd_last[13];
static bool snap_any;

void
cldma_snap(const char *why, uint32_t ms, bool md_on)
{
	uint32_t a[6], p[13];

	if (!ao || !pd || !md_on || (rd(infra_b, CG2_STA) & (1u << 3))) {
		redwood_md_say(false, "r1md: [%u ms] cldma snap (%s): skipped (clock gated or MD1 off)", ms, why);
		return;
	}
	for (int i = 0; i < 6; i++) {
		a[i] = rd(ao, ao_regs[i]);
	}
	for (int i = 0; i < 13; i++) {
		p[i] = rd(pd, pd_regs[i]);
	}
	if (snap_any && !memcmp(a, ao_last, sizeof(a)) && !memcmp(p, pd_last, sizeof(p))) {
		return;
	}
	if (!snap_any || memcmp(a, ao_last, sizeof(a))) {
		redwood_md_say(true, "r1md: [%u ms] cldma AO (%s): SO_CFG %#x START0 %#x CUR0 %#x STATUS %#x MTU %#x L2RIMR0 %#x", ms, why, a[0], a[1], a[2], a[3], a[4], a[5]);
	}
	if (!snap_any || memcmp(p, pd_last, sizeof(p))) {
		redwood_md_say(true, "r1md: [%u ms] cldma PD (%s): UL_ST %#x UL_ERR %#x SO_ERR %#x DLD %#x %#x %#x %#x L2TISAR0 %#x L2RISAR %#x %#x L3RISAR %#x %#x DBG0 %#x", ms, why,
		    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12]);
	}
	memcpy(ao_last, a, sizeof(a));
	memcpy(pd_last, p, sizeof(p));
	snap_any = true;
}

/* ---- the receive ring (r1_md_cldma=1 only) ---- */

struct rgpd {
	uint8_t flags, non_used;
	uint16_t allow;
	uint32_t next;
	uint32_t buf;
	uint16_t len;
	uint8_t msb, non_used2;
};

static struct {
	vm_offset_t va_cached, va;      /* the cached view is never used again; va is the uncached one */
	uint64_t pa, size;
	struct rgpd *g;                 /* ring, uncached view */
	uint8_t *bufs;
	uint32_t *lo, *hi;              /* canary pages */
	uint64_t lo_pa, g_pa, bufs_pa, hi_pa;
} cl;
static volatile bool cl_started;
static uint32_t cl_cur, cl_rx, cl_resume, cl_err, cl_logged, cl_l2_last;
static uint64_t cl_bytes;

uint32_t
cldma_rx_count(void)
{
	return cl_rx;
}

uint32_t
cldma_rx_err(void)
{
	return cl_err;
}

/* cldma_plat_hw_reset (ccci_cldma_plat.c 50-98): RST0 set, a (useless) read of RST1 = the reference's only "delay", RST1 clear; the same for PD;
 * then INFRA +0xC00 |= bit 1 (CLDMA_IP_BUSY_MASK, the wakeup source mask). The reference has no udelay; we add 10 us between set and clear. */
static void
cldma_hw_reset(void)
{
	wr(infra_b, 0x140, 1u << 6);    /* AO */
	(void)rd(infra_b, 0x144);
	delay(10);
	wr(infra_b, 0x144, 1u << 6);
	wr(infra_b, 0x150, 1u << 2);    /* PD */
	(void)rd(infra_b, 0x154);
	delay(10);
	wr(infra_b, 0x154, 1u << 2);
	delay(10);
	wr(infra_b, 0xc00, rd(infra_b, 0xc00) | (1u << 1));
}

/* called after clocks_on(), before the modem is released */
bool
cldma_rx_start(uint32_t ms)
{
	vm_offset_t va = 0;
	uint64_t size = (uint64_t)PAGE_SIZE * 3 + (uint64_t)RING_N * BUF_BYTES;
	uint32_t maxpn = (uint32_t)((0x100000000ULL >> PAGE_SHIFT) - 1);

	if (!ao || !pd || (rd(infra_b, CG2_STA) & (1u << 3))) {
		redwood_md_say(true, "r1md: cldma: NOT started (maps missing or clock gated)");
		return false;
	}
	/* physically contiguous, below 4 GB (the pointer fields are 32 bit, msb stays 0), the way redwood_lx.c / redwood_audio.c get theirs */
	if (kmem_alloc_contig(kernel_map, &va, size, PAGE_MASK, maxpn, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		redwood_md_say(true, "r1md: cldma: NOT started (no %llu bytes of contiguous memory below 4 GB)", size);
		return false;
	}
	cl.va_cached = va;
	cl.size = size;
	cl.pa = kvtophys(va);
	if (cl.pa + size > 0x100000000ULL) {
		redwood_md_say(true, "r1md: cldma: NOT started (memory at %#llx is above 4 GB)", cl.pa);
		return false;
	}
	bzero((void *)va, size);
	for (vm_offset_t line = va; line < va + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	cl.va = ml_io_map_unmappable(cl.pa, size, VM_WIMG_WCOMB);
	if (!cl.va) {
		redwood_md_say(true, "r1md: cldma: NOT started (no uncached mapping)");
		return false;
	}
	cl.lo = (uint32_t *)cl.va;
	cl.g = (struct rgpd *)(cl.va + PAGE_SIZE);
	cl.bufs = (uint8_t *)(cl.va + 2 * PAGE_SIZE);
	cl.hi = (uint32_t *)(cl.va + 2 * PAGE_SIZE + (uint64_t)RING_N * BUF_BYTES);
	cl.lo_pa = cl.pa;
	cl.g_pa = cl.pa + PAGE_SIZE;
	cl.bufs_pa = cl.pa + 2 * PAGE_SIZE;
	cl.hi_pa = cl.bufs_pa + (uint64_t)RING_N * BUF_BYTES;
	for (uint32_t i = 0; i < PAGE_SIZE / 4; i++) {
		cl.lo[i] = cl.hi[i] = CANARY;
	}
	for (int i = 0; i < RING_N; i++) {
		struct rgpd *g = &cl.g[i];

		g->allow = DL_MTU;
		g->next = (uint32_t)(cl.g_pa + (uint64_t)((i + 1) % RING_N) * sizeof(struct rgpd));
		g->buf = (uint32_t)(cl.bufs_pa + (uint64_t)i * BUF_BYTES);
		g->len = 0;
		g->msb = 0;
		g->non_used = 0;
		g->flags = 0x81;        /* HWO | IOC */
	}
	__asm__ volatile ("dsb sy");
	redwood_md_say(true, "r1md: cldma: ring memory pa %#llx..%#llx: canary lo %#llx, rgpd %#llx x%d, buffers %#llx (%d x %u), canary hi %#llx", cl.pa, cl.pa + size - 1,
	    cl.lo_pa, cl.g_pa, RING_N, cl.bufs_pa, RING_N, BUF_BYTES, cl.hi_pa);
	cldma_hw_reset();
	delay(100);
	wr(ao, 0x418, DL_MTU);
	wr(ao, 0x400, rd(ao, 0x400) | 1);
	wr(ao, 0x404, (uint32_t)cl.g_pa);       /* RQSAR(0) */
	wr(ao, 0x40c, rd(ao, 0x40c) & ~0xfu);   /* 4 MSB of queue 0 (reference: read-modify-write; the memory is below 4 GB) */
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(pd, 0x404, 0xff);            /* SO_START_CMD = CLDMA_BM_ALL_QUEUE, as cldma_start (ccci_hif_cldma.c 2192); gen93 has the one RX queue */
	(void)rd(pd, 0x404);
	cl_started = true;
	delay(100);
	redwood_md_say(true, "r1md: [%u ms] cldma: rx q0 started: rgpd %#llx x%d buf %#llx, INFRA C00 %#x (interrupts left masked)", ms, cl.g_pa, RING_N, cl.bufs_pa, rd(infra_b, 0xc00));
	redwood_md_say(true, "r1md: cldma after start: AO CFG %#x START %#x CUR %#x STATUS %#x MTU %#x; PD SO_ERR %#x DLD %#x %#x %#x %#x L2RISAR %#x", rd(ao, 0x400), rd(ao, 0x404),
	    rd(ao, 0x408), rd(ao, 0x414), rd(ao, 0x418), rd(pd, 0x400), rd(pd, 0x410), rd(pd, 0x414), rd(pd, 0x418), rd(pd, 0x41c), rd(pd, 0x830));
	return true;
}

static bool
canary_ok(bool full)
{
	uint32_t n = full ? PAGE_SIZE / 4 : 16;

	for (uint32_t i = 0; i < n; i++) {
		if (cl.lo[PAGE_SIZE / 4 - 1 - i] != CANARY || cl.hi[i] != CANARY) {
			return false;
		}
		if (full && (cl.lo[i] != CANARY || cl.hi[PAGE_SIZE / 4 - 1 - i] != CANARY)) {
			return false;
		}
	}
	return true;
}

/* each poll pass, after drain_rings. Returns true when a stop rule fired (the caller takes the off path). */
bool
cldma_rx_poll(uint32_t ms)
{
	uint32_t l2, l3a, l3b;

	if (!cl_started || cl_err) {
		return cl_err != 0;
	}
	while (!(((volatile struct rgpd *)&cl.g[cl_cur])->flags & 1) && cl_rx < MAX_RX + 1) {
		struct rgpd *g = &cl.g[cl_cur];
		uint32_t len;

		__asm__ volatile ("dsb ld" ::: "memory");
		len = ((volatile struct rgpd *)g)->len;
		const uint8_t *b = cl.bufs + (uint64_t)cl_cur * BUF_BYTES;

		cl_rx++;
		cl_bytes += len;
		if (cl_logged++ < MAX_LOG) {
			redwood_md_say(true, "r1md: [%u ms] cldma rx #%u len %u: %02x %02x %02x %02x %02x %02x %02x %02x", ms, cl_rx, len, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
		}
		g->len = 0;
		g->allow = DL_MTU;
		__asm__ volatile ("dsb sy" ::: "memory");
		g->flags = 0x81;
		__asm__ volatile ("dsb sy" ::: "memory");
		cl_cur = (cl_cur + 1) % RING_N;
	}
	if (!(rd(ao, 0x414) & 1)) {
		wr(pd, 0x408, 1);       /* SO_RESUME_CMD */
		cl_resume++;
	}
	l2 = rd(pd, 0x830);
	l3a = rd(pd, 0x838);
	l3b = rd(pd, 0x83c);
	if (l2 != cl_l2_last || l3a || l3b) {
		redwood_md_say(true, "r1md: [%u ms] cldma L2RISAR0 %#x L3RISAR %#x %#x (rx %u, resumes %u)", ms, l2, l3a, l3b, cl_rx, cl_resume);
		cl_l2_last = l2;
	}
	if (l2) {
		wr(pd, 0x830, l2);      /* write 1 to clear */
	}
	if ((l2 & 4) || l3a || l3b) {
		cl_err |= (l2 & 4) | (l3a ? 0x100 : 0) | (l3b ? 0x200 : 0);
	}
	if (!canary_ok(false)) {
		cl_err |= 0x400;
	}
	if (cl_rx > MAX_RX) {
		cl_err |= 0x800;
	}
	if (cl_err) {
		redwood_md_say(true, "r1md: [%u ms] cldma STOP RULE: err %#x (rx %u)", ms, cl_err, cl_rx);
	}
	return cl_err != 0;
}

/* counters line: first stat 3 / registration / end */
void
cldma_report(const char *tag, uint32_t ms)
{
	if (!cl.va) {
		return;
	}
	redwood_md_say(true, "r1md: [%u ms] cldma counters (%s): rx %u pkts %llu B, resumes %u, cur %u, err %#x, L2RISAR %#x, STATUS %#x, started %d", ms, tag, cl_rx, cl_bytes, cl_resume, cl_cur,
	    cl_err, cl_l2_last, cl_started ? rd(ao, 0x414) : 0, cl_started);
}

/* in md_power_off (every exit path): stop the queue, wait up to 10 ms, hardware reset; the memory is left allocated (nothing may be freed here) */
void
cldma_teardown(void)
{
	if (!cl_started) {
		return;
	}
	cl_started = false;
	wr(pd, 0x40c, 1);               /* SO_STOP_CMD */
	for (int i = 0; i < 100 && (rd(ao, 0x414) & 1); i++) {
		delay(100);
	}
	cldma_hw_reset();
	redwood_md_say(true, "r1md: cldma: stopped and reset; rx %u pkts %llu B, resumes %u, err %#x, canaries %s", cl_rx, cl_bytes, cl_resume, cl_err,
	    canary_ok(true) ? "intact" : "SMASHED");
	if (!canary_ok(true)) {
		cl_err |= 0x400;
	}
}
