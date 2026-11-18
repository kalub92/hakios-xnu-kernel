/*
 * CLDMA for the r1md modem driver (docs/cellular-attach-2.md 5b, 5d).
 *  - cldma_snap(): READ-ONLY register snapshots (always available), guarded by the CLDMA clock gate and MD1 power; logged only when changed.
 *  - the receive ring (SO queue 0): only with boot-arg r1_md_cldma=1 (redwood_md.c calls cldma_rx_start only then). With the arg absent
 *    not one CLDMA register is written. Polled, no interrupts. Received packets are only counted and their first bytes logged; nothing is
 *    forwarded and nothing is transmitted.
 *  - Stage 3a (boot-arg r1_md_data=1, redwood_md.c calls cldma_data_enable() before cldma_rx_start): ALSO a 32-entry TGPD ring for TX queue 0
 *    (a few packets, never reuses a slot, so no buffer is ever rewritten while the hardware may read it), received packets handed to
 *    cell_rx() in redwood_md.c, caps 20 packets / 4096 bytes sent, 200 packets received. Polled, no interrupts.
 */
#include <kern/kalloc.h>
#include <kern/locks.h>
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
#define RING_N_DEF      32
#define BUF_BYTES       4096u
#define DL_MTU          3584u
#define CANARY          0xc1d3a5a5u
#define MAX_RX          2000u           /* own stop rule: a flood is not what this step is for */
#define MAX_LOG         10u
/* Stage 3a TX (docs/cellular-stage3.md C.2; ccci_hif_cldma.c cldma_gpd_handle_tx_request, cldma_reg.h) */
#define TX_N            32              /* tgpds in the ring; only TX_MAX_PKTS are ever used, so no slot is reused */
#define TX_BUF          1536u           /* bytes per slot (Stage 3b: a packet up to the 1430 MTU; 3a's one query is 57) */
#define TX_MAX_LEN      1500u
#define TX_MAX_PKTS     20u             /* defaults (Stage 3a); Stage 3b raises them through cldma_if_enable() */
#define TX_MAX_BYTES    4096u
#define RX_MAX_DATA     200u
#define TX_STUCK_MS     2000u           /* HWO still set this long after the start command: stop rule */

extern void cell_rx(const uint8_t *b, uint32_t len, uint32_t ms);

static vm_offset_t ao, pd, infra_b;
/* Stage 3b: the TX ring is used by the ifnet's output (any thread) and reclaimed by the poll (md thread): cl_lock covers tx_* below, held briefly (log lines are said under it: the log has its own lock and never calls back);
 * not taken in teardown (which may run with the other CPUs stopped). */
static LCK_GRP_DECLARE(cl_grp, "redwood-cldma");
static LCK_MTX_DECLARE(cl_lock, &cl_grp);

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
	struct tgpd *tg;                /* TX ring (data only), then 512-byte buffers, then a canary page */
	uint8_t *tbufs;
	uint32_t *thi;
	uint64_t tg_pa, tbufs_pa, thi_pa;
} cl;
struct tgpd {
	uint8_t flags, non_used, msb, netif;
	uint32_t next;
	uint32_t buf;
	uint16_t len, psn;
};

static uint32_t rx_n = RING_N_DEF;      /* RX ring entries: 32, 128 in the service (a burst during a 20 ms idle sleep must fit) */
static bool tx_on;                      /* cldma_data_enable() was called: TX ring, hook, 200-packet cap */
static uint32_t rx_cap = MAX_RX;
/* TX ring accounting: tx_prod packets queued in all, tx_done reclaimed (the hardware cleared HWO); slot = index % TX_N; in flight = tx_prod - tx_done */
static uint32_t tx_prod, tx_done, tx_bytes, tx_started, tx_t0[TX_N], tx_l2_logged, dma_err0;
static uint32_t tx_cap_pkts = TX_MAX_PKTS, tx_cap_bytes = TX_MAX_BYTES, tx_full, tx_logged, tx_done_logged, tx_ref_logged;
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

void
cldma_data_enable(void)
{
	tx_on = true;
	rx_cap = RX_MAX_DATA - 1;       /* the 200th packet trips the rule */
}

/* Stage 4a (r1_md=10/11, the service): no per-run packet caps (the byte budget is the kernel's, redwood_md.c), a bigger RX ring. Called before cldma_rx_start. */
void
cldma_svc_enable(void)
{
	tx_cap_pkts = 0x7fffffff;
	tx_cap_bytes = 0x7fffffff;
	rx_cap = 0x7fffffff;
	rx_n = 128;
}

/* Stage 3b (r1_md_if=1): the ifnet sends interactively. Backstops a little above the caps redwood_cell.c enforces (those are what end the run cleanly). */
void
cldma_if_enable(uint32_t pkts_cap, uint32_t bytes_cap)
{
	tx_cap_pkts = pkts_cap + 50;
	tx_cap_bytes = bytes_cap + 20000;
	rx_cap = pkts_cap + 150;
}

uint32_t
cldma_tx_pkts(void)
{
	return tx_prod;
}

uint32_t
cldma_tx_bytes(void)
{
	return tx_bytes;
}

uint32_t
cldma_tx_completed(void)
{
	return tx_done;
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
	uint64_t rxsize = (uint64_t)PAGE_SIZE * 3 + (uint64_t)rx_n * BUF_BYTES;
	uint64_t tbytes = ((uint64_t)TX_N * TX_BUF + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
	uint64_t size = rxsize + (tx_on ? (uint64_t)PAGE_SIZE * 2 + tbytes : 0);     /* + TX ring page, the TX buffers (whole pages), TX canary page */
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
	cl.hi = (uint32_t *)(cl.va + 2 * PAGE_SIZE + (uint64_t)rx_n * BUF_BYTES);
	cl.lo_pa = cl.pa;
	cl.g_pa = cl.pa + PAGE_SIZE;
	cl.bufs_pa = cl.pa + 2 * PAGE_SIZE;
	cl.hi_pa = cl.bufs_pa + (uint64_t)rx_n * BUF_BYTES;
	for (uint32_t i = 0; i < PAGE_SIZE / 4; i++) {
		cl.lo[i] = cl.hi[i] = CANARY;
	}
	if (tx_on) {
		cl.tg = (struct tgpd *)(cl.va + rxsize);
		cl.tbufs = (uint8_t *)(cl.va + rxsize + PAGE_SIZE);
		cl.thi = (uint32_t *)(cl.va + rxsize + PAGE_SIZE + tbytes);
		cl.tg_pa = cl.pa + rxsize;
		cl.tbufs_pa = cl.tg_pa + PAGE_SIZE;
		cl.thi_pa = cl.tbufs_pa + tbytes;
		for (uint32_t i = 0; i < PAGE_SIZE / 4; i++) {
			cl.thi[i] = CANARY;
		}
		for (int i = 0; i < TX_N; i++) {
			struct tgpd *t = &cl.tg[i];

			t->flags = 0x80;        /* IOC, HWO clear (cldma_tx_ring_init) */
			t->non_used = 0;
			t->msb = 0;
			t->netif = 0;
			t->next = (uint32_t)(cl.tg_pa + (uint64_t)((i + 1) % TX_N) * sizeof(struct tgpd));
			t->buf = (uint32_t)(cl.tbufs_pa + (uint64_t)i * TX_BUF);
			t->len = 0;
			t->psn = 0;
		}
	}
	for (uint32_t i = 0; i < rx_n; i++) {
		struct rgpd *g = &cl.g[i];

		g->allow = DL_MTU;
		g->next = (uint32_t)(cl.g_pa + (uint64_t)((i + 1) % rx_n) * sizeof(struct rgpd));
		g->buf = (uint32_t)(cl.bufs_pa + (uint64_t)i * BUF_BYTES);
		g->len = 0;
		g->msb = 0;
		g->non_used = 0;
		g->flags = 0x81;        /* HWO | IOC */
	}
	__asm__ volatile ("dsb sy");
	redwood_md_say(true, "r1md: cldma: ring memory pa %#llx..%#llx: canary lo %#llx, rgpd %#llx x%u, buffers %#llx (%u x %u), canary hi %#llx", cl.pa, cl.pa + size - 1,
	    cl.lo_pa, cl.g_pa, rx_n, cl.bufs_pa, rx_n, BUF_BYTES, cl.hi_pa);
	cldma_hw_reset();
	delay(100);
	wr(ao, 0x418, DL_MTU);
	wr(ao, 0x400, rd(ao, 0x400) | 1);
	wr(ao, 0x404, (uint32_t)cl.g_pa);       /* RQSAR(0) */
	wr(ao, 0x40c, rd(ao, 0x40c) & ~0xfu);   /* 4 MSB of queue 0 (reference: read-modify-write; the memory is below 4 GB) */
	if (tx_on) {
		/* cldma_start: TQSAR(0) + its 4 MSB nibble, and the power-on-domain backup copy; the queue itself is started on the first send */
		wr(pd, 0x00, (uint32_t)cl.tg_pa);
		wr(pd, 0x10, rd(pd, 0x10) & ~0xfu);
		wr(ao, 0x04, (uint32_t)cl.tg_pa);
		wr(ao, 0x14, rd(ao, 0x14) & ~0xfu);
		dma_err0 = rd(pd, 0x870);
		redwood_md_say(true, "r1md: cldma: TX q0 ring %#llx x%d (%u-byte buffers %#llx, canary %#llx), TQSAR0 %#x BK %#x, DMA_ERR %#x", cl.tg_pa, TX_N, TX_BUF, cl.tbufs_pa, cl.thi_pa,
		    rd(pd, 0x00), rd(ao, 0x04), dma_err0);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(pd, 0x404, 0xff);            /* SO_START_CMD = CLDMA_BM_ALL_QUEUE, as cldma_start (ccci_hif_cldma.c 2192); gen93 has the one RX queue */
	(void)rd(pd, 0x404);
	cl_started = true;
	delay(100);
	redwood_md_say(true, "r1md: [%u ms] cldma: rx q0 started: rgpd %#llx x%u buf %#llx, INFRA C00 %#x (interrupts left masked)", ms, cl.g_pa, rx_n, cl.bufs_pa, rd(infra_b, 0xc00));
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
		if (tx_on && cl.thi && cl.thi[i] != CANARY) {
			return false;
		}
	}
	return true;
}

/* cl_lock held: a tgpd whose HWO the hardware cleared is done; its slot is free again (len 0) */
static void
tx_reclaim(uint32_t ms)
{
	while (tx_done < tx_prod) {
		struct tgpd *t = &cl.tg[tx_done % TX_N];

		if (((volatile struct tgpd *)t)->flags & 1) {
			break;
		}
		__asm__ volatile ("dsb ld" ::: "memory");
		if (tx_done_logged++ < 8) {
			redwood_md_say(true, "r1md: [%u ms] cldma tx #%u done (HWO clear) %u ms after the start", ms, tx_done + 1, ms - tx_t0[tx_done % TX_N]);
		}
		t->len = 0;
		tx_done++;
	}
}

/* each poll pass, after drain_rings. Returns true when a stop rule fired (the caller takes the off path). */
bool
cldma_rx_poll(uint32_t ms)
{
	uint32_t l2, l3a, l3b;

	if (!cl_started || cl_err) {
		return cl_err != 0;
	}
	while (!(((volatile struct rgpd *)&cl.g[cl_cur])->flags & 1) && cl_rx < rx_cap + 1) {
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
		if (tx_on) {
			cell_rx(b, len < BUF_BYTES ? len : BUF_BYTES, ms);      /* the buffer is still ours: it is re-armed below */
		}
		g->len = 0;
		g->allow = DL_MTU;
		__asm__ volatile ("dsb sy" ::: "memory");
		g->flags = 0x81;
		__asm__ volatile ("dsb sy" ::: "memory");
		cl_cur = (cl_cur + 1) % rx_n;
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
	if (tx_on && tx_prod) {
		uint32_t l2t, ule, de;

		lck_mtx_lock(&cl_lock);
		l2t = rd(pd, 0x800);
		ule = rd(pd, 0x3c);
		de = rd(pd, 0x870);
		tx_reclaim(ms);
		if (tx_done < tx_prod) {
			if (ms - tx_t0[tx_done % TX_N] > TX_STUCK_MS) {
				cl_err |= 0x1000;
			}
			if (!(rd(pd, 0x28) & 1)) {
				wr(pd, 0x34, 1);        /* UL_RESUME_CMD: idle with a slot still owned by the hardware = the queue went idle just before HWO was set */
			}
		}
		if (l2t || ule) {
			if (tx_l2_logged++ < 6) {
				redwood_md_say(true, "r1md: [%u ms] cldma tx L2TISAR0 %#x UL_ERROR %#x L3TISAR %#x %#x UL_STATUS %#x", ms, l2t, ule, rd(pd, 0x810), rd(pd, 0x814), rd(pd, 0x28));
			}
			if (l2t) {
				wr(pd, 0x800, l2t);     /* write 1 to clear */
			}
		}
		if ((l2t & 0xf00) || ule) {
			cl_err |= 0x2000;
		}
		if (de != dma_err0) {
			cl_err |= 0x4000;
		}
		lck_mtx_unlock(&cl_lock);
	}
	if (!canary_ok(false)) {
		cl_err |= 0x400;
	}
	if (cl_rx > rx_cap) {
		cl_err |= 0x800;
	}
	if (cl_err) {
		redwood_md_say(true, "r1md: [%u ms] cldma STOP RULE: err %#x (rx %u)", ms, cl_err, cl_rx);
	}
	return cl_err != 0;
}

/*
 * Send one raw IP packet on TX queue 0, netif 0 (ccmni0; ccci_h->data[0] = ccmni index becomes tgpd.netif, the ccci header itself is stripped on
 * gen93: ccci_hif_cldma.c cldma_gpd_handle_tx_request, MD_GENERATION >= 6293). Ownership: the slot's buffer and tgpd are written first, HWO is set LAST
 * (after a barrier), from then on the hardware owns them and nothing writes them again (slots are never reused: at most TX_MAX_PKTS < TX_N sends).
 * The memory is never freed. Returns false (nothing sent) on a cap, an error, a missing ring or a bad length.
 */
bool
cldma_tx(const uint8_t *ip, uint32_t len, uint32_t ms)
{
	struct tgpd *t;
	uint8_t *buf;
	uint32_t slot;

	if (!tx_on || !cl_started || !cl.tg || cl_err) {
		return false;
	}
	lck_mtx_lock(&cl_lock);
	tx_reclaim(ms);
	if (len < 20 || len > TX_MAX_LEN || tx_prod >= tx_cap_pkts || tx_bytes + len > tx_cap_bytes || tx_prod - tx_done >= TX_N - 1) {
		if (tx_ref_logged++ < 6) {
			redwood_md_say(true, "r1md: cldma tx REFUSED: len %u, %u pkts %u B sent so far (caps %u pkts %u B), %u in flight", len, tx_prod, tx_bytes, tx_cap_pkts, tx_cap_bytes, tx_prod - tx_done);
		}
		tx_full += tx_prod - tx_done >= TX_N - 1;
		lck_mtx_unlock(&cl_lock);
		return false;
	}
	slot = tx_prod % TX_N;
	t = &cl.tg[slot];
	buf = cl.tbufs + (uint64_t)slot * TX_BUF;
	if ((((volatile struct tgpd *)t)->flags & 1) || ((volatile struct tgpd *)t)->len != 0) {
		redwood_md_say(true, "r1md: cldma tx REFUSED: slot %u not free", slot);
		lck_mtx_unlock(&cl_lock);
		return false;
	}
	memcpy(buf, ip, len);
	t->len = (uint16_t)len;
	t->msb = 0;
	t->netif = 0;
	t->non_used = 1;
	__asm__ volatile ("dsb sy" ::: "memory");
	t->flags = 0x81;                /* IOC | HWO: the hardware owns the slot from here until the reclaim */
	__asm__ volatile ("dsb sy" ::: "memory");
	tx_t0[slot] = ms;
	tx_prod++;
	tx_bytes += len;
	if (!tx_started) {
		wr(pd, 0x30, 1);        /* UL_START_CMD, queue 0 only (the reference writes all four; only q0 has a ring here) */
		(void)rd(pd, 0x30);
		tx_started = 1;
	} else if (!(rd(pd, 0x28) & 1)) {
		wr(pd, 0x34, 1);        /* UL_RESUME_CMD: the queue went idle on an empty tgpd */
		(void)rd(pd, 0x34);
	}
	if (tx_logged++ < 8) {
		redwood_md_say(true, "r1md: [%u ms] cldma tx #%u len %u netif 0 queued (HWO set), UL_STATUS %#x CUR %#x", ms, tx_prod, len, rd(pd, 0x28), rd(pd, 0x14));
	}
	lck_mtx_unlock(&cl_lock);
	return true;
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
	if (tx_on) {
		redwood_md_say(true, "r1md: [%u ms] cldma tx (%s): %u pkts %u B queued, %u completed (HWO cleared), UL_STATUS %#x", ms, tag, tx_prod, tx_bytes, tx_done, cl_started ? rd(pd, 0x28) : 0);
	}
}

/* in md_power_off (every exit path): stop the queue, wait up to 10 ms, hardware reset; the memory is left allocated (nothing may be freed here) */
void
cldma_teardown(void)
{
	if (!cl_started) {
		return;
	}
	cl_started = false;
	if (tx_on && tx_started) {
		wr(pd, 0x38, 1);        /* UL_STOP_CMD: q0 stops before the reset; the TX memory is never freed */
		for (int i = 0; i < 100 && (rd(pd, 0x28) & 1); i++) {
			delay(100);
		}
		redwood_md_say(true, "r1md: cldma: tx stopped: %u queued %u completed, %u refused (ring full), UL_STATUS %#x", tx_prod, tx_done, tx_full, rd(pd, 0x28));
	}
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
