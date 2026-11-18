/*
 * The GPU of the rabbit r1 (MT6765), a PowerVR Rogue GE8320 (BVNC 22.87.104.18) whose registers are at
 * 0x13000000, brought as far as answering who it is. Only with boot-arg pvr=N: every address below
 * belongs to this device (the Infinix's GPU is a Mali, bsd/dev/redwood_mali.c, which this is modelled on).
 *
 *   pvr=1  reads only: the MFG power domains' status and control registers in the SPM, the bus
 *          protection in infracfg_ao, the "mfg_sel" mux in topckgen, the MFG PLL -- and, only if all
 *          of that says the GPU block has power, a bus and a clock, mfg_cfg and the GPU's identity.
 *          A block without power is not read: that can hang the bus.
 *   pvr=2  the same, then what can be read of the rails, then MediaTek's power-up: the mux ungated
 *          (on the 26 MHz crystal), MFG_ASYNC, MFG, MFG's SRAM and its bus protection released,
 *          MFG_CORE0; then mfg_cfg and the GPU's identity, and whether that is 22.87.104.18.
 *   pvr=3  the same, then the open firmware (/fw/connsys/rogue_22.87.104.18_v1.fw, a MIPS one) read once
 *          the root is mounted, checked, laid out in memory with its page table and the structures it
 *          reads while it boots, and its processor let go; five seconds of polling for its "started"
 *          flag, and BOOTED or NOT BOOTED said with what there is to tell why. "The firmware" below.
 *   pvr=4  the same, then commands: a health check, the firmware's trace turned on (boot-arg pvr_trace=
 *          its groups, 0 for off) and read back, each sent through the kernel's command buffer with
 *          the kick and waited for in its return slot; and from then on the thread stays, doing what
 *          the interrupt this kernel has not wired would have had done. "Commands" below.
 *   pvr=5  the same, then what a client's first calls make: the GPU's own page tables with ten pages
 *          mapped and the MMU cache flushed, a render context, a free list, a render target's data;
 *          with boot-arg pvr_teardown=1 all of it cleaned up again through the firmware. "The GPU's
 *          memory" below. Boot-arg pvr_wc=1: the shared memory through a write-combined mapping.
 *   pvr=6  the same, then the GPU served to programs: the RedwoodPVR service (iokit/Kernel/RedwoodPVR.cpp)
 *          started, behind it Linux's DRM interface to this driver (redwood_pvr_drm.c, which this
 *          file includes), tried once from inside the kernel, every request short of a job.
 *
 * The rails are NOT touched: the GPU runs from VCORE (haretic's mtk_mfgsys.c 52-56: stock only voted a
 * VCORE level per OPP, through DVFSRC; boot-arg r1_gpu= does that, never the buck itself) and VSRAM is an
 * LDO of the MT6357, and both are assumed left on by the bootloader. sysctl kern.redwood.pvr says how far it got: 1 the
 * state was read, 2 the domains are on, 3 the GPU said 22.87.104.18, 4 the firmware is in memory with
 * all it needs, 5 it booted, 6 it answers commands, 7 the
 * GPU has a context, a free list and a render target, 8 programs are served.
 *
 * Sources, read and named, not copied. "pg" is MediaTek's drivers/clk/mediatek/clk-mt6765-pg.c as
 * published in github.com/MiCode/Xiaomi_Kernel_OpenSource, branch dandelion-q-oss (the line numbers
 * are that file's); "dts" is arch/arm64/boot/dts/mediatek/mt6765.dts of the same tree; "mfgsys" is its
 * drivers/misc/mediatek/gpu/gpu_rgx/m1.11ED5425693/services/system/rgx_mtk/mt6765/mtk_mfgsys.c and
 * "gpufreq" its drivers/misc/mediatek/base/power/mt6765/mtk_gpufreq_core.c. "clk" is mainline Linux
 * v6.12 drivers/clk/mediatek/clk-mt6765.c, "clk-mux" its clk-mux.c, "pwrap" its
 * drivers/soc/mediatek/mtk-pmic-wrap.c, "mt6357" its drivers/regulator/mt6357-regulator.c with
 * include/linux/mfd/mt6357/registers.h, and "cr_defs" and "pvr_device" its
 * drivers/gpu/drm/imagination/pvr_rogue_cr_defs.h and pvr_device.c. The firmware (pvr=3) follows
 * Linux v7.3's drivers/gpu/drm/imagination instead (each file "GPL-2.0-only OR MIT"), named there file
 * by file and function by function, and "clk-pll" is v6.12's drivers/clk/mediatek/clk-pll.c.
 *
 * What has no source and so is only read, never written:
 *   - a clock gate inside mfg_cfg. MT6765 has no mfgcfg clock driver in either tree and mfgsys never
 *     writes one: it switches the domains on and reads the GPU. The word at offset 0 (MFG_CG_CON on
 *     the MT6768, mtkloader/mtkmali.h) is printed for whoever reads the log.
 *   - POWERON_CONFIG_EN, the SPM's "registers may be written" key: pg defines it and its project code
 *     (lines 151 and 178) and never writes it.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/redwood_input.h>
#include <sys/redwood_pvr.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void redwood_host_keep_add(const char *format, ...) __printflike(1, 2);  /* redwood_mali.c: said, and kept for the host */
#define SAY redwood_host_keep_add
extern uint64_t kvtophys(vm_offset_t va);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);       /* osfmk/arm64/caches_asm.s: written back and forgotten */
extern void delay_for_interval(uint32_t interval, uint32_t scale_factor);
extern struct vnode *rootvnode;

#define TOPCKGEN_PHYS   0x10000000      /* dts 382 */
#define INFRACFG_PHYS   0x10001000      /* infracfg_ao, the first register block of scpsys: dts 373, pg 3023 */
#define SPM_PHYS        0x10006000      /* the second: dts 374, pg 3024 */
#define APMIXED_PHYS    0x1000c000      /* dts 573 */
#define PWRAP_PHYS      0x1000d000      /* dts 584 */
#define RGX_PHYS        0x13000000      /* mfg_doma: dts 2083 */
#define MFGCFG_PHYS     0x13ffe000      /* mfg_cfg: dts 2093; mfgsys 1385 (the GPU's base + 0xffe000) */

SYSCTL_DECL(_kern_redwood);

static vm_offset_t topckgen, infracfg, spm, apmixed, pwrap, mfgcfg, rgx;
static uint32_t reached;
static uint64_t gpu_bvnc;       /* as the GPU said it, packed as the firmware's header packs it */

SYSCTL_UINT(_kern_redwood, OID_AUTO, pvr, CTLFLAG_RD | CTLFLAG_LOCKED, &reached, 0,
    "how far the PowerVR came up: 1 state read, 2 domains on, 3 it said 22.87.104.18, 4 firmware in memory, 5 firmware booted, 6 it answers commands, 7 context made, 8 serving programs");

static inline uint32_t
rd(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(base + at) = value;
	__asm__ volatile ("dsb sy");
}

/* Until (register & mask) == want, for at most `ms`. */
static bool
wait_for(vm_offset_t base, uint32_t at, uint32_t mask, uint32_t want, uint32_t ms)
{
	for (uint32_t us = 0; us < ms * 1000; us += 10) {
		if ((rd(base, at) & mask) == want) {
			return true;
		}
		delay(10);
	}
	return (rd(base, at) & mask) == want;
}

/* ---- the power controller (SPM) ---- */

#define POWERON_CONFIG_EN   0x000       /* pg 151: read only here */
#define PWR_STATUS          0x180       /* pg 154 */
#define PWR_STATUS_2ND      0x184       /* pg 155 */
#define MFG_CORE1_PWR_CON   0x310       /* pg 159: defined, but MT6765 has no such domain (pg 2921-2933) */
#define MFG_2D_PWR_CON      0x348       /* pg 172: likewise */
#define PWR_RST_B           (1u << 0)   /* pg 140 */
#define PWR_ISO             (1u << 1)   /* pg 141 */
#define PWR_ON              (1u << 2)   /* pg 142 */
#define PWR_ON_2ND          (1u << 3)   /* pg 143 */
#define PWR_CLK_DIS         (1u << 4)   /* pg 144 */
#define PWR_SRAM_PDN        (1u << 8)   /* pg 349 (MFG), 358 (MFG_CORE0) */
#define PWR_SRAM_ACK        (1u << 12)  /* pg 351, 360 */

/*
 * MT6765's three (pg 2925-2931: MFG's parent is MFG_ASYNC, MFG_CORE0's is MFG), in the order they come
 * up (pg 3063-3066, gpufreq 346-369).
 */
static const struct {
	const char *name;
	uint32_t con, status;
} domains[3] = {
	{ "MFG_ASYNC", 0x334, 1u << 23 },       /* pg 167, 330 */
	{ "MFG",       0x338, 1u << 4 },        /* pg 168, 326 */
	{ "MFG_CORE0", 0x34c, 1u << 7 },        /* pg 173, 329 */
};
#define ALL_DOMAINS         ((1u << 23) | (1u << 4) | (1u << 7))

/* ---- the bus protection (infracfg_ao) ---- */

#define TOPAXI_PROTECTEN        0x220   /* pg 181 */
#define TOPAXI_PROTECTEN_STA1   0x228   /* pg 187 */
#define TOPAXI_PROTECTEN_CLR    0x2a4   /* pg 188 */
#define MFG_PROT_STEP1          (1u << 25)                  /* pg 301 */
#define MFG_PROT_STEP2          ((1u << 21) | (1u << 22))   /* pg 303-304 */

/* ---- the clocks ---- */

#define CLK_CFG_UPDATE      0x004       /* clk 74 */
#define CLK_CFG_1           0x050       /* clk 44 */
#define CLK_CFG_1_CLR       0x058       /* clk 46 */
#define MFG_SEL_FIELD       (3u << 0)   /* clk 383-385: mfg_sel is CLK_CFG_1 bits 1..0, gated by bit 7, */
#define MFG_SEL_PDN         (1u << 7)   /* and a change of it is latched by bit 4 of CLK_CFG_UPDATE */
#define MFG_SEL_UPDATE      (1u << 4)
static const char *const mfg_parents[4] = { "clk26m", "mfgpll_ck", "syspll_d3", "univpll_d3" };    /* clk 198-203 */
#define MFGPLL_CON0         0x24c       /* clk 714; the vendor's clk-mt6765.c 230-233 names all four: read only here */

/* ---- the PMIC through its wrapper, for reading ---- */

#define PWRAP_WACS2_CMD     0xc20       /* pwrap 590 (mt6765_regs) */
#define PWRAP_WACS2_RDATA   0xc24       /* pwrap 591 */
#define PWRAP_WACS2_VLDCLR  0xc28       /* pwrap 592 */
#define PWRAP_FSM           (7u << 16)  /* pwrap 33: 0 is idle, 6 "read it, then clear" (41, 44) */
#define MT6357_HWCID                 0x0008     /* registers.h */
#define MT6357_LDO_VSRAM_OTHERS_CON0 0x19d0     /* bit 0: enabled (mt6357 95-96, 391) */
#define MT6357_LDO_VSRAM_OTHERS_DBG0 0x19ea     /* bits 14..8: the step it is at (mt6357 98-99) */
#define MT6357_LDO_VSRAM_CON1        0x19fc     /* bits 6..0: the step asked for (mt6357 392, 431-436) */
#define VSRAM_STEP_UV(step) (518750u + 6250u * ((step) & 0x7fu))    /* mt6357 325-327, 390; the step: CON1 [6:0], DBG0 [14:8] */

/* ---- the GPU's own registers ---- */

#define RGX_CR_CORE_ID          0x018   /* cr_defs 348: the old way, the configuration in bits 15..0 */
#define RGX_CR_CORE_ID_PBVNC    0x020   /* cr_defs 336: 64 bits -- B 63..48, V 47..32, N 31..16, C 15..0 (338-345) */
#define RGX_CR_CORE_REVISION    0x020   /* cr_defs 356: the old way, major in bits 23..16, minor in 15..8 */
#define EXPECT_B 22
#define EXPECT_V 87
#define EXPECT_N 104
#define EXPECT_C 18

/*
 * Everything that is safe to read whatever state the block is in, said. True if the GPU block can be
 * read after it: every domain on in both status registers, no bus protection, and its clock not gated.
 */
static bool
show(const char *when)
{
	uint32_t status = rd(spm, PWR_STATUS), second = rd(spm, PWR_STATUS_2ND);
	uint32_t prot = rd(infracfg, TOPAXI_PROTECTEN_STA1), cfg1 = rd(topckgen, CLK_CFG_1);

	SAY("pvr: %s: PWR_STATUS %#x, PWR_STATUS_2ND %#x, POWERON_CONFIG_EN %#x", when, status, second,
	    rd(spm, POWERON_CONFIG_EN));
	for (unsigned i = 0; i < 3; i++) {
		SAY("pvr:   %s: %s/%s, PWR_CON %#x", domains[i].name, (status & domains[i].status) ? "on" : "off",
		    (second & domains[i].status) ? "on" : "off", rd(spm, domains[i].con));
	}
	SAY("pvr:   no domain on MT6765: MFG_CORE1_PWR_CON %#x, MFG_2D_PWR_CON %#x", rd(spm, MFG_CORE1_PWR_CON),
	    rd(spm, MFG_2D_PWR_CON));
	SAY("pvr:   bus protection: PROTECTEN %#x, STA1 %#x: MFG step 1 %s, step 2 %s", rd(infracfg, TOPAXI_PROTECTEN), prot,
	    (prot & MFG_PROT_STEP1) ? "PROTECTED" : "released", (prot & MFG_PROT_STEP2) ? "PROTECTED" : "released");
	SAY("pvr:   CLK_CFG_1 %#x: mfg_sel on %s, %s", cfg1, mfg_parents[cfg1 & MFG_SEL_FIELD],
	    (cfg1 & MFG_SEL_PDN) ? "GATED" : "running");
	SAY("pvr:   MFGPLL_CON0..3 %#x %#x %#x %#x", rd(apmixed, MFGPLL_CON0), rd(apmixed, MFGPLL_CON0 + 4),
	    rd(apmixed, MFGPLL_CON0 + 8), rd(apmixed, MFGPLL_CON0 + 12));
	return (status & ALL_DOMAINS) == ALL_DOMAINS && (second & ALL_DOMAINS) == ALL_DOMAINS &&
	       (prot & (MFG_PROT_STEP1 | MFG_PROT_STEP2)) == 0 && (cfg1 & MFG_SEL_PDN) == 0;
}

/* One register of the PMIC, read the way pwrap's pwrap_read16 does (1447-1479), each wait its 10 ms. */
static bool
pmic_read(uint32_t reg, uint16_t *value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (wait_for(pwrap, PWRAP_WACS2_RDATA, PWRAP_FSM, 0, 10)) {
		wr(pwrap, PWRAP_WACS2_CMD, (reg >> 1) << 16);
		if (wait_for(pwrap, PWRAP_WACS2_RDATA, PWRAP_FSM, 6u << 16, 10)) {
			*value = (uint16_t)rd(pwrap, PWRAP_WACS2_RDATA);
			wr(pwrap, PWRAP_WACS2_VLDCLR, 1);
			ok = true;
		}
	}
	redwood_pwrap_unlock();
	return ok;
}

/*
 * What can be read of the two rails without driving anything. gpufreq takes "vsram_gpu" and
 * "ext_buck_vgpu" (2491, 2508); which MT6357 output vsram_gpu is on this board is not in any source
 * read here -- VSRAM_OTHERS is the candidate, and 0.875 V on it would say so.
 */
static void
rails(void)
{
	uint16_t id = 0, en = 0, asked = 0, at = 0;

	SAY("pvr: reading the PMIC through its wrapper (reads only)");
	pwrap = ml_io_map(PWRAP_PHYS, 0x1000);
	if (pmic_read(MT6357_HWCID, &id) && pmic_read(MT6357_LDO_VSRAM_OTHERS_CON0, &en) &&
	    pmic_read(MT6357_LDO_VSRAM_CON1, &asked) && pmic_read(MT6357_LDO_VSRAM_OTHERS_DBG0, &at)) {
		SAY("pvr:   PMIC HWCID %#x; VSRAM_OTHERS (vsram_gpu? not confirmed) %s, asked %u uV, at %u uV (raw %#x %#x %#x)",
		    id, (en & 1u) ? "enabled" : "DISABLED", VSRAM_STEP_UV(asked), VSRAM_STEP_UV(at >> 8), en, asked, at);
	} else {
		SAY("pvr:   the PMIC wrapper did not answer (WACS2_RDATA %#x): VSRAM not read", rd(pwrap, PWRAP_WACS2_RDATA));
	}
	SAY("pvr:   VGPU (ext_buck_vgpu, 0x55 on I2C6) not read: this kernel has no driver for that bus");
	SAY("pvr:   both rails are ASSUMED left on by the bootloader; nothing here switches or sets them");
}

/* One domain, as pg's spm_mtcmos_ctrl_mfg_async_pwr (1847-1877), _mfg_pwr (1440-1464) and _mfg_core0_pwr (1757-1787). */
static bool
domain_on(unsigned i)
{
	uint32_t con = domains[i].con, bit = domains[i].status;

	SAY("pvr: %s: PWR_ON, PWR_ON_2ND", domains[i].name);
	wr(spm, con, rd(spm, con) | PWR_ON);
	wr(spm, con, rd(spm, con) | PWR_ON_2ND);
	if (!wait_for(spm, PWR_STATUS, bit, bit, 20) || !wait_for(spm, PWR_STATUS_2ND, bit, bit, 20)) {
		SAY("pvr: %s DID NOT COME UP in 20 ms: PWR_STATUS %#x, _2ND %#x, PWR_CON %#x", domains[i].name,
		    rd(spm, PWR_STATUS), rd(spm, PWR_STATUS_2ND), rd(spm, con));
		return false;
	}
	wr(spm, con, rd(spm, con) & ~PWR_CLK_DIS);
	wr(spm, con, rd(spm, con) & ~PWR_ISO);
	wr(spm, con, rd(spm, con) | PWR_RST_B);
	return true;
}

/* Its SRAM woken: pg 1376-1383 (MFG, in spm_mtcmos_ctrl_mfg_bus_prot) and 1788-1795 (MFG_CORE0). It needs mfg_sel running. */
static bool
sram_on(unsigned i)
{
	uint32_t con = domains[i].con;

	SAY("pvr: %s: SRAM", domains[i].name);
	wr(spm, con, rd(spm, con) & ~PWR_SRAM_PDN);
	if (!wait_for(spm, con, PWR_SRAM_ACK, 0, 20)) {
		SAY("pvr: %s's SRAM DID NOT WAKE in 20 ms: PWR_CON %#x", domains[i].name, rd(spm, con));
		return false;
	}
	return true;
}

static bool
power_up(void)
{
	/*
	 * The mux first: MFG_ASYNC's domain wants mfg_sel running before it (pg 2874, 2930; gpufreq 349).
	 * On clk26m, parent 0, the one gpufreq calls clk_sub_parent (dts 2104, 2111): it needs no PLL
	 * and is slow enough for whatever voltage the rail was left at. The writes are clk-mux's
	 * mtk_clk_mux_set_parent_setclr_lock and mtk_clk_mux_enable_setclr (106-134, 33-55): the field
	 * and the gate cleared through the CLR register, then the update bit.
	 */
	SAY("pvr: mfg_sel: to clk26m, ungated");
	wr(topckgen, CLK_CFG_1_CLR, MFG_SEL_FIELD | MFG_SEL_PDN);
	wr(topckgen, CLK_CFG_UPDATE, MFG_SEL_UPDATE);

	if (!domain_on(0) || !domain_on(1) || !sram_on(1)) {
		return false;
	}
	SAY("pvr: MFG: bus protection released");
	wr(infracfg, TOPAXI_PROTECTEN_CLR, MFG_PROT_STEP2);     /* pg 1386, then 1393: step 2 first */
	wr(infracfg, TOPAXI_PROTECTEN_CLR, MFG_PROT_STEP1);
	return domain_on(2) && sram_on(2);
}

/* Inside the block: only once show() has said it can be read. */
static void
inside(void)
{
	uint32_t lo, hi, b, v, n, c;

	/*
	 * mfg_cfg. mfgsys reads 0x1c and writes 0x24 (329, 343-344, its hardware power management, which
	 * this does not start); offset 0 is where other MediaTek chips keep the block's clock gates.
	 */
	SAY("pvr: reading mfg_cfg (if nothing follows, that read hung)");
	mfgcfg = ml_io_map(MFGCFG_PHYS, 0x1000);
	SAY("pvr:   mfg_cfg +0x00 %#x (clock gates? no MT6765 source: not written), +0x1c %#x, +0x24 %#x", rd(mfgcfg, 0x00),
	    rd(mfgcfg, 0x1c), rd(mfgcfg, 0x24));

	/* Who it is, as pvr_device's pvr_load_gpu_id (324-349): the 64-bit register, and the older pair if its B is 0. */
	SAY("pvr: reading the GPU's core ID at %#x (if nothing follows, that read hung)", RGX_PHYS + RGX_CR_CORE_ID_PBVNC);
	rgx = ml_io_map(RGX_PHYS, 0x1000);
	lo = rd(rgx, RGX_CR_CORE_ID_PBVNC);
	hi = rd(rgx, RGX_CR_CORE_ID_PBVNC + 4);
	SAY("pvr:   CORE_ID %#x_%08x", hi, lo);
	if ((lo == 0 && hi == 0) || (lo == 0xffffffffu && hi == 0xffffffffu)) {
		SAY("pvr: THE CORE ID IS ALL %s: the block is not answering (no power at the rails, no clock, or a gate "
		    "in mfg_cfg) -- this is not a GPU identified", lo == 0 ? "ZEROES" : "ONES");
		return;
	}
	b = hi >> 16;
	v = hi & 0xffffu;
	n = lo >> 16;
	c = lo & 0xffffu;
	if (b == 0) {
		uint32_t id = rd(rgx, RGX_CR_CORE_ID);

		SAY("pvr:   B is 0: the older registers, CORE_REVISION %#x, CORE_ID %#x", lo, id);
		b = (lo >> 16) & 0xffu;
		v = (lo >> 8) & 0xffu;
		n = (id >> 8) & 0xffu;
		c = id & 0xffu;
	}
	if (b == EXPECT_B && v == EXPECT_V && n == EXPECT_N && c == EXPECT_C) {
		SAY("pvr: BVNC %u.%u.%u.%u -- the GE8320 this was written for", b, v, n, c);
		gpu_bvnc = ((uint64_t)b << 48) | ((uint64_t)v << 32) | ((uint64_t)n << 16) | c;
		reached = 3;
	} else {
		SAY("pvr: BVNC %u.%u.%u.%u -- NOT the %u.%u.%u.%u expected", b, v, n, c, EXPECT_B, EXPECT_V, EXPECT_N, EXPECT_C);
	}
}

/*
 * ---- the firmware (pvr=3) ----
 *
 * Linux v7.3's drivers/gpu/drm/imagination, re-expressed: what pvr_device.c's pvr_device_gpu_init and
 * pvr_fw.c's pvr_fw_init do for a MIPS firmware processor, in their order, and nothing they do for a
 * META or RISC-V one (no GPU page catalogue: pvr_fw_start skips rogue_bif_init for MIPS, 167-170).
 * The file says version 1.1 (build 6976702), open source, header version 3, and v7.3 accepts exactly
 * that (pvr_fw.c 27, pvr_fw_info.h 67: FW_MAX_SUPPORTED_MAJOR_VERSION 1, PVR_FW_INFO_VERSION 3); the
 * header carries no finer interface version than those, so the structures are v7.3's, by the offsets
 * its own pvr_rogue_fwif_check.h asserts ("check", with its line).
 *
 * Where this is not Linux:
 *   - one piece of wired, physically contiguous memory holds the page table and every firmware object,
 *     each a whole number of 4 KiB pages (Linux: a GEM object each, rounded to its own page size, placed
 *     by drm_mm). The firmware learns where each is from the structures, so only the fixed places
 *     matter: code at the bottom of the heap, data where the file says, the three at its top.
 *   - Linux maps its objects write-combined; these are the kernel's cached pages, so everything is
 *     flushed before the processor starts and each word the firmware writes is flushed before it is
 *     read (redwood_mali.c does the same for the Mali's tables, 434-438).
 *   - 64-bit registers go as two 32-bit halves, low first: see rd64.
 *   - the file's sizes and offsets are checked before they are used; Linux trusts more of them.
 *   - if it does not boot, the block is put back in soft reset (pvr_fw_start's own error path) without
 *     pvr_fw_stop's waits for idle.
 */
#define FW_NAME                 "rogue_22.87.104.18_v1.fw"

/* 64-bit registers. pvr=2 read CORE_ID as two 32-bit reads and that is all that is known to work here:
 * Linux uses ioread64/iowrite64 (pvr_device.h 580, 604), one 64-bit access, which is UNTESTED on this
 * bus. Every 64-bit register goes through these two, so that is the one place to change. */
static inline uint64_t
rd64(uint32_t at)
{
	uint32_t lo = rd(rgx, at);

	return ((uint64_t)rd(rgx, at + 4) << 32) | lo;
}

static inline void
wr64(uint32_t at, uint64_t value)
{
	wr(rgx, at, (uint32_t)value);
	wr(rgx, at + 4, (uint32_t)(value >> 32));
}

/* pvr_rogue_cr_defs.h, v7.3's lines */
#define CR_SOFT_RESET                   0x0100                  /* 493 */
#define CR_SOFT_RESET_MASKFULL          0x00E7FFFFFFFFFC3DULL   /* 495 */
#define CR_SOFT_RESET_PBE2_XE_MASKFULL  0xFFEFFFFFFFFFFC3DULL   /* 494 */
#define CR_SOFT_RESET_GARTEN            0x0000000100000000ULL   /* 588 */
/* pvr_rogue_defs.h 37-50, ROGUE_CR_SOFT_RESET_RASCALDUSTS_EN: RASCAL_CORE (594) and DUST_A..H_CORE (600, 597, 579, 576, 573, 570, 567, 564) */
#define CR_SOFT_RESET_RASCALDUSTS       (0x0000000080000000ULL | 0x0000000020000000ULL | 0x0000000040000000ULL | \
	                                0x0000000800000000ULL | 0x0000001000000000ULL | 0x0000002000000000ULL | \
	                                0x0000004000000000ULL | 0x0000008000000000ULL | 0x0000010000000000ULL)
#define CR_EVENT_STATUS                 0x0130                  /* 717 */
#define CR_SIDEKICK_IDLE                0x03c8                  /* 974 */
#define CR_MIPS_WRAPPER_CONFIG          0x0810                  /* 1252 */
#define CR_MIPS_WRAPPER_CONFIG_MICROMIPS 0x00010000u            /* 1271: BOOT_ISA_MODE_MICROMIPS */
#define CR_MIPS_ADDR_REMAP1_CONFIG1     0x0818                  /* 1276; CONFIG2 is 8 on, and REMAP2, 3 and 5 are 0x10, 0x20 and 0x40 on (1298, 1320, 1364) */
#define CR_MIPS_ADDR_REMAP_ENABLE       1ULL                    /* 1282: CONFIG1_MODE_ENABLE_EN */
#define CR_MIPS_ADDR_REMAP_ADDR_OUT     0x000000FFFFFFF000ULL   /* 1288: ~CONFIG2_ADDR_OUT_CLRMSK */
#define CR_MIPS_ADDR_REMAP_UNMAPPED_STATUS 0x0868               /* 1386: bit 32 an event, 31..0 the address (1390-1392) */
#define CR_MIPS_WRAPPER_IRQ_STATUS      0x08a8                  /* 1465 */
#define CR_MIPS_DEBUG_CONFIG            0x08c8                  /* 1493 */
#define CR_MIPS_EXCEPTION_STATUS        0x08d0                  /* 1500: SLEEP 0x20, NMI_TAKEN 0x10, NEST_EXL 8, NEST_ERL 4, EXL 2, ERL 1 (1504-1519); 0x20 alone is an idle firmware: the core's SI_Sleep, in WAIT with no exception level */
#define CR_MIPS_WRAPPER_STATUS          0x08e8                  /* 1522: requests outstanding, bits 7..0 */
#define CR_MTS_GARTEN_WRAPPER_CONFIG    0x0b50                  /* 1971; IDLE_CTRL_META is 0 (1989) */
#define CR_BIF_FAULT_BANK0_MMU_STATUS   0x12b0                  /* 2619 */
#define CR_SLC_CTRL_MISC                0x3800                  /* 3941 */
#define CR_SLC_CTRL_MISC_LAZYWB_OVERRIDE 0x01000000u            /* 3947 */
#define CR_SLC_CTRL_MISC_PVR_HASH1      0x00200000u             /* 3954: ADDR_DECODE_MODE_PVR_HASH1 */
#define CR_SLC_CTRL_MISC_PSG_HAZARD     0x00000002u             /* 3967: ENABLE_PSG_HAZARD_CHECK_EN */
#define CR_SLC_CTRL_MISC_BYPASS_BURST   0x00000001u             /* 3970: BYPASS_BURST_COMBINER_EN */
#define CR_AXI_ACE_LITE_CONFIGURATION   0x38c0                  /* 4505 */
#define CR_SYS_BUS_SECURE               0xa100                  /* 5445 */
#define RGX_SIZE                        0x80000                 /* dts 2085 */

/* pvr_rogue_mips.h */
#define MIPS_PT_PAGES                   4                       /* 35: ROGUE_MIPSFW_MAX_NUM_PAGETABLE_PAGES */
#define MIPS_UNCACHED                   2u                      /* 39; every firmware object is PVR_BO_FW_FLAGS_DEVICE_UNCACHED */
#define MIPS_ENTRYLO_PFN_SHIFT          6                       /* 62 */
#define MIPS_ENTRYLO_PFN_MASK_36        0x3FFFFFC0u             /* 68: ..._PFN_MASK_ABOVE_32BIT */
#define MIPS_ENTRYLO_POLICY_SHIFT       3                       /* 73 */
#define MIPS_ENTRYLO_DIRTY              4u                      /* 78 */
#define MIPS_ENTRYLO_VALID              2u                      /* 82 */
#define MIPS_ENTRYLO_GLOBAL             1u                      /* 86 */
#define MIPS_BOOT_REMAP_IN              0x1FC00000ULL           /* 142 */
#define MIPS_DATA_REMAP_IN              0x1FC01000ULL           /* 149 */
#define MIPS_CODE_REMAP_IN              0x1FC02000ULL           /* 155 */
#define MIPS_REMAP_LOG2_SIZE            12ULL                   /* 143, 150, 156 */
#define MIPS_REGISTERS_VIRTUAL_BASE     0xCF800000u             /* 161 */
#define MIPS_BOOTLDR_CONF_OFFSET        0x0                     /* 169: struct rogue_mipsfw_boot_data, 190-198 */
#define MIPS_NMI_SHARED_DATA_BASE       0x100                   /* 173; its second word begins struct rogue_mips_state (178, 317-331) */
#define MIPS_BOOT_STAGE_OFFSET          0x400                   /* 182: "MIPS boot stage", in the boot data page */
#define MIPS_WRAPPER_REGBANK_ALIGN      16                      /* 216 */

/* pvr_fw_mips.c 14-16, pvr_fw.c 32-43 and pvr_fw_heap_info_init (889) */
#define FW_HEAP_MIPS_BASE               0xC0000000u
#define FW_HEAP_RAW                     (1u << 24)
#define FW_HEAP_MASK                    (FW_HEAP_RAW - 1)
#define FW_CONFIG_GRANULE               0x10000u
#define FW_CONFIG_OFFSET                (FW_HEAP_RAW - 3 * FW_CONFIG_GRANULE)   /* connection control, then osinit, then sysinit */
#define FW_HEAP_MAIN                    (FW_HEAP_RAW - (3 * FW_CONFIG_GRANULE + 0x100000u))
#define FW_ANYWHERE                     0xffffffffu
#define FW_BOOT_TIMEOUT_MS              5000                    /* pvr_fw.c 29: FW_BOOT_TIMEOUT_USEC */

/* pvr_rogue_heap_config.h */
#define PDSCODEDATA_HEAP_BASE           0xDA00000000ULL
#define USCCODE_HEAP_BASE               0xE000000000ULL

/* pvr_fw_info.h */
struct fw_header {
	uint32_t info_version, header_len, layout_entry_num, layout_entry_size;
	uint64_t bvnc;
	uint32_t fw_page_size, flags;
	uint16_t version_major, version_minor;
	uint32_t version_build, device_info_size, padding;
};
struct fw_entry {
	uint32_t id, type, base_addr, max_size, alloc_size, alloc_offset;
};
_Static_assert(sizeof(struct fw_header) == 48 && sizeof(struct fw_entry) == 24, "pvr_fw_info.h");
enum { MIPS_CODE = 4, MIPS_EXCEPTIONS_CODE, MIPS_BOOT_CODE, MIPS_PRIVATE_DATA, MIPS_BOOT_DATA, MIPS_STACK };  /* enum pvr_fw_section_id */
enum { FW_CODE = 1, FW_DATA };                                                                              /* enum pvr_fw_section_type */

/* include/uapi/linux/elf.h, as pvr_fw_util.c reads it */
struct elf32_header {
	uint8_t ident[16];
	uint16_t type, machine;
	uint32_t version, entry, phoff, shoff, flags;
	uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct elf32_program {
	uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};
#define ELF_PT_LOAD 1

/*
 * pvr_rogue_fwif_dev_info.h's feature bits in their order, a '=' before each that pvr_device_info.c's
 * features_mapping gives a value (FEATURE_MAPPING_VALUE): the values follow the mask in the file, one
 * for each such feature that is set, in this order.
 */
static const char *const feature_names[] = {
	"AXI_ACELITE", "=CDM_CONTROL_STREAM_FORMAT", "CLUSTER_GROUPING", "=COMMON_STORE_SIZE_IN_DWORDS", "COMPUTE",
	"COMPUTE_MORTON_CAPABLE", "COMPUTE_OVERLAP", "COREID_PER_OS", "DYNAMIC_DUST_POWER", "=ECC_RAMS", "=FBCDC",
	"=FBCDC_ALGORITHM", "=FBCDC_ARCHITECTURE", "=FBC_MAX_DEFAULT_DESCRIPTORS", "=FBC_MAX_LARGE_DESCRIPTORS",
	"FB_CDC_V4", "GPU_MULTICORE_SUPPORT", "GPU_VIRTUALISATION", "GS_RTA_SUPPORT", "IRQ_PER_OS",
	"=ISP_MAX_TILES_IN_FLIGHT", "=ISP_SAMPLES_PER_PIXEL", "ISP_ZLS_D24_S8_PACKING_OGL_MODE", "=LAYOUT_MARS",
	"=MAX_PARTITIONS", "=META", "=META_COREMEM_SIZE", "MIPS", "=NUM_CLUSTERS", "=NUM_ISP_IPP_PIPES", "=NUM_OSIDS",
	"=NUM_RASTER_PIPES", "PBE2_IN_XE", "PBVNC_COREID_REG", "PERFBUS", "PERF_COUNTER_BATCH", "=PHYS_BUS_WIDTH",
	"RISCV_FW_PROCESSOR", "ROGUEXE", "S7_TOP_INFRASTRUCTURE", "SIMPLE_INTERNAL_PARAMETER_FORMAT",
	"SIMPLE_INTERNAL_PARAMETER_FORMAT_V2", "=SIMPLE_PARAMETER_FORMAT_VERSION", "=SLC_BANKS",
	"=SLC_CACHE_LINE_SIZE_BITS", "SLC_SIZE_CONFIGURABLE", "=SLC_SIZE_IN_KILOBYTES", "SOC_TIMER",
	"SYS_BUS_SECURE_RESET", "TESSELLATION", "TILE_REGION_PROTECTION", "=TILE_SIZE_X", "=TILE_SIZE_Y", "TLA",
	"TPU_CEM_DATAMASTER_GLOBAL_REGISTERS", "TPU_DM_GLOBAL_REGISTERS", "TPU_FILTERING_MODE_CONTROL",
	"=USC_MIN_OUTPUT_REGISTERS_PER_PIX", "VDM_DRAWINDIRECT", "VDM_OBJECT_LEVEL_LLS", "=VIRTUAL_ADDRESS_SPACE_BITS",
	"WATCHDOG_TIMER", "WORKGROUP_PROTECTION", "=XE_ARCHITECTURE", "XE_MEMORY_HIERARCHY", "XE_TPU2",
	"=XPU_MAX_REGBANKS_ADDR_WIDTH", "=XPU_MAX_SLAVES", "=XPU_REGISTER_BROADCAST", "XT_TOP_INFRASTRUCTURE",
	"ZLS_SUBTILE",
};
#define FEATURES ((unsigned)(sizeof(feature_names) / sizeof(feature_names[0])))
_Static_assert(sizeof(feature_names) / sizeof(feature_names[0]) == 71, "PVR_FW_HAS_FEATURE_MAX");
enum {
	F_GPU_MULTICORE_SUPPORT = 16, F_MIPS = 27, F_NUM_CLUSTERS = 28, F_PBE2_IN_XE = 32, F_PHYS_BUS_WIDTH = 36,
	F_SLC_CACHE_LINE_SIZE_BITS = 44, F_SLC_SIZE_IN_KILOBYTES = 46, F_SYS_BUS_SECURE_RESET = 48, F_XE_TPU2 = 65,
	F_ISP_SAMPLES_PER_PIXEL = 21, F_NUM_ISP_IPP_PIPES = 29, F_NUM_RASTER_PIPES = 31,
	F_ROGUEXE = 38, F_SIMPLE_PARAMETER_FORMAT_VERSION = 42, F_TILE_SIZE_X = 51, F_TILE_SIZE_Y = 52, F_VIRTUAL_ADDRESS_SPACE_BITS = 60, F_XE_MEMORY_HIERARCHY = 64,
	F_CDM_CONTROL_STREAM_FORMAT = 1, F_CLUSTER_GROUPING = 2, F_COMMON_STORE_SIZE_IN_DWORDS = 3,
	F_COMPUTE_MORTON_CAPABLE = 5, F_COMPUTE_OVERLAP = 6, F_FBCDC_ALGORITHM = 11, F_FB_CDC_V4 = 15,
	F_ISP_MAX_TILES_IN_FLIGHT = 20, F_ISP_ZLS_D24_S8_PACKING_OGL_MODE = 22, F_MAX_PARTITIONS = 24,
	F_S7_TOP_INFRASTRUCTURE = 39, F_TESSELLATION = 49, F_TPU_DM_GLOBAL_REGISTERS = 55,
	F_USC_MIN_OUTPUT_REGISTERS_PER_PIX = 57, F_VDM_DRAWINDIRECT = 58, F_VDM_OBJECT_LEVEL_LLS = 59, F_ZLS_SUBTILE = 70,
};
/*
 * pvr_stream_defs.c's tables and the firmware interface's offsets that pvr=5 and pvr=6 use, made by
 * a program from Linux v7.3's headers (redwood_pvr_fwif_gen.h says how). A stream entry's feature is
 * one of the F_ above, or one of these.
 */
#define SF_NONE                         0xffffu
#define SF_CDM_USER_MODE_QUEUE          0x100u  /* pvr_device.c pvr_device_has_feature's two derived ones */
#define SF_REQUIRES_FB_CDC_ZLS_SETUP    0x101u
struct stream_def {
	uint32_t offset, bytes, feature;
	bool absent;                    /* there only on a core WITHOUT the feature */
};
struct stream_ext {
	const struct stream_def *defs;
	uint32_t count, header, mask, quirk, valid;
};
#include "redwood_pvr_fwif_gen.h"

#define BRN_NAMED       12              /* PVR_FW_HAS_BRN_MAX; the names below in the enum's order */
#define ERN_NAMED       8               /* PVR_FW_HAS_ERN_MAX */
static const uint32_t brn_numbers[BRN_NAMED] = { 44079, 47217, 48492, 48545, 49927, 50767, 51764, 62269, 63142, 63553, 66011, 71242 };
#define BRN_63553       (1ULL << 9)
#define BRN_66011       (1ULL << 10)
#define BRN_71242       (1ULL << 11)

/* The structures the firmware reads: sizes and the offsets written or read here, each from check. */
#define CCB_CTL_SIZE            144     /* check 193: write_offset 0 (187), read_offset 128 (189), wrap_mask 132 (190), cmd_size 136 (191) */
#define KCCB_CMD_SIZE           88      /* check 271 */
#define FWCCB_CMD_SIZE          88      /* check 288 */
#define KCCB_LOG2               7       /* pvr_fw.h 24: ROGUE_FWIF_KCCB_NUMCMDS_LOG2_DEFAULT */
#define FWCCB_LOG2              5       /* pvr_fw.h 22: ROGUE_FWIF_FWCCB_NUMCMDS_LOG2 */
#define HWRINFOBUF_SIZE         2336    /* check 130 */
#define SYSDATA_SIZE            3624    /* check 55: config_flags 0 (33), fault_info 80 (45, each 432: its file name 24 on, 30), fw_faults 3536 (46) */
#define GPU_UTIL_FWCB_SIZE      10280   /* check 409; last_word 10248 (407) is PVR_FWIF_GPU_UTIL_STATE_IDLE, 0 */
#define RUNTIME_CFG_SIZE        72      /* check 308: active_pm_latency_ms 0, ..._persistant 8, core_clock_speed 12, default_dusts_num_init 16, wdg_period_us 28 (297-304) */
#define TRACE_BUF_DWORDS        12000u  /* pvr_rogue_fwif.h 68 */
#define TRACE_THREADS           2       /* pvr_rogue_fwif.h 71: ROGUE_FW_THREAD_MAX */
#define TRACEBUF_SIZE           864     /* check 24: log_type 0 (20), tracebuf[] 8 (21) each 424 (18), tracebuf_size_in_dwords 856 (22) */
#define TRACEBUF_SPACE_SIZE     424     /* check 18: trace_buffer_fw_addr 4 (15), assert_buf 16 (17): path 0, info 200, line_num 400 (9-11) */
#define OSDATA_SIZE             584     /* check 75: power_sync_fw_addr 572 (73) */
#define OSINIT_SIZE             104     /* check 344, the offsets 333-343; rogue_comp_checks 40: hw_bvnc 0, fw_bvnc 16, ddk_version 36, ddk_build 40, updated 56 (323-330) */
#define SYSINIT_SIZE            544     /* check 402, the offsets 370-401 */
#define SYSINIT_FIRMWARE_STARTED 352    /* check 393: a bool, one byte */
#define CONNECTION_CTL_SIZE     16      /* check 314: connection_fw_state 0 (310): 0 offline, 1 ready, 2 active (pvr_rogue_fwif.h 165-175) */
#define COMPCHECKS_LAYOUT_VERSION 3     /* pvr_rogue_fwif.h 1627 */

struct fw_obj {
	uint8_t *cpu;           /* in the kernel */
	uint64_t phys;
	uint32_t addr;          /* in the firmware: pvr_mips_get_fw_addr_with_offset */
	uint32_t bytes;         /* whole pages */
};

/* the page table's 16 KiB, the file's 112, some 46 pages of structures; and from pvr=5 the GPU's page tables, client CCBs and free-list pages */
#define ARENA_SIZE      0x400000u       /* wired for good: 8 MiB was a guess; the window server with a second client used 884 KiB of it */
#define ARENA_PAGES     (ARENA_SIZE >> 12)
static uint8_t arena_map[ARENA_PAGES];          /* a byte a page: taken */
#define CLEANUP_TRIES   80
static uint32_t cleanup_busy;                   /* times the firmware called something busy */
static uint32_t pt_pages;                       /* GPU page-table pages there are */
static uint32_t mmu_flush_flags;                /* pvr_dev->mmu_flush_cache_flags: what the next MMUCACHE command is to say (pvr=5) */
#define MMUCACHE_PT             0x1u            /* pvr_rogue_fwif.h 821-837: ROGUE_FWIF_MMUCACHEDATA_FLAGS_* */
#define MMUCACHE_PD             0x2u
#define MMUCACHE_PC             0x4u
#define MMUCACHE_TLB            0x18u
#define MMUCACHE_INTERRUPT      0x4000000u
#define MMU_SYNC_0              (MMUCACHE_PT | MMUCACHE_INTERRUPT | MMUCACHE_TLB)       /* pvr_mmu.c 72-76: PVR_MMU_SYNC_LEVEL_n_FLAGS */
#define MMU_SYNC_1              (MMU_SYNC_0 | MMUCACHE_PD)
#define MMU_SYNC_2              (MMU_SYNC_1 | MMUCACHE_PC)

static uint8_t *fw_file;
static uint32_t fw_size;
static const struct fw_header *fw_hdr;
static const struct fw_entry *fw_layout;
static uint64_t fw_quirks, fw_enhancements, feature_value[71];
static bool feature_has[71];
static vm_offset_t arena;
static uint64_t arena_phys;
static uint32_t arena_used, heap_next, clock_hz;
static uint32_t *mips_pt;
static struct fw_obj fw_code, fw_data, fw_connection, fw_sysdata, fw_tracectl, fw_osinit, fw_sysinit;
static struct fw_obj fault_page, gpu_util, runtime_cfg, trace[TRACE_THREADS];
static struct fw_obj fw_kccb_ctl, fw_kccb, fw_fwccb_ctl, fw_fwccb, fw_rtn, fw_power_sync, fw_osdata, fw_trace0, fw_hwrinfobuf, fw_mmucache_sync;      /* what the commands use */
static uint64_t boot_code_phys, boot_data_phys, exception_code_phys;
static bool rgx_whole;
static uint8_t *boot_data;

static inline void
put32(const struct fw_obj *o, uint32_t at, uint32_t value)
{
	*(uint32_t *)(void *)(o->cpu + at) = value;
}

static inline void
put64(const struct fw_obj *o, uint32_t at, uint64_t value)
{
	*(uint64_t *)(void *)(o->cpu + at) = value;
}

/* A word the firmware may have written since: out of memory, not out of this CPU's cache. */
static uint32_t
fresh32(const uint8_t *p)
{
	FlushPoC_DcacheRegion((vm_offset_t)p, 4);
	return *(volatile const uint32_t *)(const void *)p;
}

static const struct fw_entry *
fw_section(uint32_t id)         /* pvr_fw_find_layout_entry */
{
	for (uint32_t i = 0; i < fw_hdr->layout_entry_num; i++) {
		if (fw_layout[i].id == id) {
			return &fw_layout[i];
		}
	}
	return NULL;
}

/*
 * Step 1. Does the firmware need more than the 26 MHz pvr=2 left the block on? Nothing read says so:
 * Linux only refuses a rate of 0 (pvr_fw.c fw_sysinit_init and fw_runtime_cfg_init: WARN_ON(!clock_speed_hz))
 * and hands the firmware whatever the clock is, and gpufreq itself parks a running GPU on this very
 * parent while it reprograms the PLL (__mt_gpufreq_switch_to_clksrc, 1495-1515; clk_sub_parent, 2422).
 * It will boot slower -- which is why the wait is Linux's whole five seconds. So the default stays on
 * clk26m, and the firmware is told 26 MHz.
 *
 * That was the first runs' choice. The device has since rendered on the PLL as the bootloader leaves
 * it (460 MHz), so the PLL is now the default and boot-arg pvr_pll=0 is what keeps the 26 MHz.
 * The other rates gpufreq knows (400, 500, 600..730 MHz) it reaches by parking mfg_sel on clk26m,
 * writing MFGPLL_CON1 with the new divider and its "changed" bit 31, waiting 20 us and switching
 * back (__mt_gpufreq_clock_switch, 1441 on) -- always after raising, or before lowering, VGPU to
 * that rate's voltage, which is why none of that is done here: VGPU is not driven by this kernel.
 *
 * mfg_sel goes to mfgpll_ck, as gpufreq always has it while the GPU is on
 * (1506), but only if the PLL's own registers say it is powered, out of isolation and enabled
 * (clk-pll 23-25, mtk_pll_prepare 215-238) and set for no more than gpufreq's fastest (730 MHz,
 * mtk_gpufreq_core.h 59). MediaTek's PLLs have no lock bit to read: Linux waits 20 us after enabling
 * one and calls it locked (clk-pll 238), and this one has been enabled since the bootloader. Its rate
 * is 26 MHz * CON1[21:0] / 2^14 / 2^CON1[26:24] (clk 714-715: 22 bits of which 8 whole, the divider
 * at 24; clk-pll __mtk_pll_recalc_rate; gpufreq __mt_gpufreq_get_cur_freq 1966-1988 agrees): the
 * 0x211b13b pvr=2 printed is 460 MHz. What is NOT known is whether VGPU is high enough for it --
 * gpufreq wants 0.65 V at 400 MHz and 0.70 V at 500 -- which is why this is not the default. If the
 * GPU stops saying who it is on the faster clock, the mux goes back.
 */
#define CLK26M_HZ           26000000u   /* dts 546; gpufreq's GPUPLL_FIN */
#define CLK_CFG_1_SET       0x054       /* clk 45 */
#define MFGPLL_PWR_CON      0x258       /* clk 714 */
#define PLL_EN              (1u << 0)   /* clk-pll 23: CON0_BASE_EN */
#define PLL_PWR_ON          (1u << 0)   /* clk-pll 24 */
#define PLL_ISO             (1u << 1)   /* clk-pll 25 */
#define MFGPLL_MAX_HZ       730000000u

/*
 * Boot-arg r1_gpu=500|680 (the r1 only): the GPU's VCORE asked for through the DVFSRC (SW_REQ2's VCORE level: 2 is
 * 0.70 V, 3 is 0.80 V -- mtk-dvfsrc-regulator.c mt6765_voltages, mtk_mfgsys.c mtk_mfg_opps), what VCORE then reads
 * checked, and only then MFGPLL set (clk-pll.c: postdiv and pcw, the CHG bit, 20 us) while mfg_sel is still on clk26m.
 * SW_REQ2 goes back on the way to the loader (redwood_r1_gpu_restore()). True if the PLL was retuned.
 */
#define R1_DVFSRC_PHYS     0x10012000UL
#define R1_DV_SW_REQ2      0x08
#define R1_DV_LEVEL        0xdc
#define CLK_CFG_5          0x090        /* clk 444-446: spm_sel, which the DVFSRC's registers need running, is gated by bit 31 */
static vm_offset_t r1_dv;
static uint32_t r1_sw_req2;
static bool r1_vote_saved;

static bool
r1_gpu(uint32_t mhz, uint32_t pll_khz)
{
	static const uint8_t bits[6] = { 0, 1, 3, 4, 8, 9 }, vcore[6] = { 0, 1, 2, 2, 3, 3 };
	uint32_t level = mhz == 500 ? 2 : 3, value = mhz == 500 ? 0x02133b13 : 0x021a2762, need = mhz == 500 ? 700000 : 800000;
	uint32_t con1 = rd(apmixed, MFGPLL_CON0 + 4), old = 0, uv = 0;
	uint16_t dbg = 0;
	bool ok = false;

	if (mhz != 500 && mhz != 680) {
		SAY("r1gpu: %u refused: 500 or 680 only", mhz);
		return false;
	}
	if ((rd(topckgen, CLK_CFG_1) & MFG_SEL_FIELD) != 0 || (rd(topckgen, CLK_CFG_5) & (1u << 31)) != 0) {
		SAY("r1gpu: mfg_sel is not on clk26m, or spm_sel is gated: refused");
		return false;
	}
	r1_dv = ml_io_map(R1_DVFSRC_PHYS, 0x1000);
	for (int i = 0; i < 6; i++) {
		ok |= (rd(r1_dv, R1_DV_LEVEL) >> 16) == 1u << bits[i];
	}
	if (!ok) {              /* DVFSRC not started: no vote to make; VCORE as it sits has to be enough */
		ok = pmic_read(0x1522, &dbg) && (uv = 518750u + 6250u * (dbg & 0x7fu)) >= need;
		if (ok) {
			SAY("r1gpu: DVFSRC not started, VCORE %u uV is enough, no vote", uv);
		}
	} else {
		old = rd(r1_dv, R1_DV_SW_REQ2);
		r1_sw_req2 = old;
		r1_vote_saved = true;
		wr(r1_dv, R1_DV_SW_REQ2, (old & ~0xcu) | level << 2);
		delay(1);
		ok = false;
		for (int i = 0; i < 1000 && !ok; i++) {
			uint32_t lv = rd(r1_dv, R1_DV_LEVEL);

			for (int j = 0; j < 6; j++) {
				ok |= (lv & 0xffff) == 0 && (lv >> 16) == 1u << bits[j] && vcore[j] >= level;
			}
			if (!ok) {
				delay(10);
			}
		}
		ok = ok && pmic_read(0x1522, &dbg) && (uv = 518750u + 6250u * (dbg & 0x7fu)) >= need;
		if (!ok) {
			wr(r1_dv, R1_DV_SW_REQ2, old);
			r1_vote_saved = false;
		}
	}
	if (!ok) {
		SAY("r1gpu: VCORE vote failed, staying at %u kHz", pll_khz);
		return false;
	}
	wr(apmixed, MFGPLL_CON0 + 4, (con1 & ~(7u << 24) & ~0x3fffffu) | value);
	wr(apmixed, MFGPLL_CON0 + 4, rd(apmixed, MFGPLL_CON0 + 4) | 1u << 31);
	delay(20);
	if ((rd(apmixed, MFGPLL_CON0 + 4) & 0x073fffffu) != value) {
		wr(apmixed, MFGPLL_CON0 + 4, (rd(apmixed, MFGPLL_CON0 + 4) & ~(7u << 24) & ~0x3fffffu) | (con1 & 0x073fffffu));
		wr(apmixed, MFGPLL_CON0 + 4, rd(apmixed, MFGPLL_CON0 + 4) | 1u << 31);
		delay(20);
		if (r1_vote_saved) {
			wr(r1_dv, R1_DV_SW_REQ2, old);
			r1_vote_saved = false;
		}
		SAY("r1gpu: MFGPLL CON1 reads %#x, not %#x: put back, any VCORE vote taken away, staying at %u kHz", rd(apmixed, MFGPLL_CON0 + 4), value, pll_khz);
		return false;
	}
	SAY("r1gpu: VCORE %u uV (level %u), MFGPLL CON1 %#x = %u kHz", uv, level, rd(apmixed, MFGPLL_CON0 + 4), mhz * 1000);
	return true;
}

/* On the way back to the loader (loader_return.c): the VCORE request taken away. An MMIO write only: no wait, no pwrap, no log. */
void redwood_r1_gpu_restore(void);
void
redwood_r1_gpu_restore(void)
{
	if (r1_vote_saved) {
		r1_vote_saved = false;
		wr(r1_dv, R1_DV_SW_REQ2, r1_sw_req2);
	}
}

static uint32_t
gpu_clock(void)
{
	uint32_t con0 = rd(apmixed, MFGPLL_CON0), con1 = rd(apmixed, MFGPLL_CON0 + 4), pwr = rd(apmixed, MFGPLL_PWR_CON);
	uint32_t pll_hz = (uint32_t)((((uint64_t)CLK26M_HZ * (con1 & 0x3fffffu)) >> 14) >> ((con1 >> 24) & 7u));
	bool up = (con0 & PLL_EN) != 0 && (pwr & PLL_PWR_ON) != 0 && (pwr & PLL_ISO) == 0;
	uint32_t want = 1, r1 = 0, pll_khz = (pll_hz + 500) / 1000;      /* the PLL unless pvr_pll=0: the device has rendered on it at 460 MHz */

	(void)PE_parse_boot_argn("pvr_pll", &want, sizeof(want));
	SAY("pvr: 1: the clock: mfg_sel is on %s; MFGPLL CON0 %#x CON1 %#x PWR_CON %#x: %s, set for %u kHz",
	    mfg_parents[rd(topckgen, CLK_CFG_1) & MFG_SEL_FIELD], con0, con1, pwr, up ? "powered and enabled" : "NOT RUNNING", pll_khz);
	if (want == 0) {
		SAY("pvr: 1: THE GPU'S CLOCK: clk26m, 26000 kHz, because pvr_pll=0 says so");
		return CLK26M_HZ;
	}
	if (!up || pll_hz < CLK26M_HZ || pll_hz > MFGPLL_MAX_HZ) {
		SAY("pvr: 1: THE GPU'S CLOCK: clk26m, 26000 kHz: the PLL is not running, or not set for 26..730 MHz");
		return CLK26M_HZ;
	}
	if (PE_parse_boot_argn("r1_gpu", &r1, sizeof(r1)) && r1 != 0 && r1_gpu(r1, pll_khz)) {
		con1 = rd(apmixed, MFGPLL_CON0 + 4);
		pll_hz = (uint32_t)((((uint64_t)CLK26M_HZ * (con1 & 0x3fffffu)) >> 14) >> ((con1 >> 24) & 7u));
		pll_khz = (pll_hz + 500) / 1000;
	}
	SAY("pvr: 1: mfg_sel: to mfgpll_ck, %u kHz as the bootloader left it set (if nothing follows, the GPU did not take it: pvr_pll=0)", pll_khz);
	wr(topckgen, CLK_CFG_1_CLR, MFG_SEL_FIELD);     /* clk-mux mtk_clk_mux_set_parent_setclr_lock, 106-134 */
	wr(topckgen, CLK_CFG_1_SET, 1);
	wr(topckgen, CLK_CFG_UPDATE, MFG_SEL_UPDATE);
	delay(100);
	if ((rd(topckgen, CLK_CFG_1) & MFG_SEL_FIELD) == 1 && rd64(RGX_CR_CORE_ID_PBVNC) == gpu_bvnc) {
		SAY("pvr: 1: THE GPU'S CLOCK: mfgpll_ck, %u kHz, and the GPU still says who it is", pll_khz);
		return pll_hz;
	}
	SAY("pvr: 1: on the PLL THE GPU DID NOT ANSWER RIGHT (CLK_CFG_1 %#x): back to clk26m", rd(topckgen, CLK_CFG_1));
	wr(topckgen, CLK_CFG_1_CLR, MFG_SEL_FIELD);
	wr(topckgen, CLK_CFG_UPDATE, MFG_SEL_UPDATE);
	delay(100);
	return CLK26M_HZ;
}

/* The file's set features said, a few to a line (a line holds 200 characters). */
static void
say_features(void)
{
	char line[150];
	size_t used = 0;

	for (unsigned i = 0; i < FEATURES; i++) {
		char one[64];
		size_t length;

		if (!feature_has[i]) {
			continue;
		}
		if (feature_names[i][0] == '=') {
			snprintf(one, sizeof(one), "%s=%llu", feature_names[i] + 1, feature_value[i]);
		} else {
			snprintf(one, sizeof(one), "%s", feature_names[i]);
		}
		length = strlen(one);
		if (used != 0 && used + length + 2 >= sizeof(line)) {
			SAY("pvr:   features: %s", line);
			used = 0;
		}
		used += (size_t)snprintf(line + used, sizeof(line) - used, "%s%s", used != 0 ? ", " : "", one);
	}
	if (used != 0) {
		SAY("pvr:   features: %s", line);
	}
}

/*
 * Step 2: the file read and checked as pvr_fw.c's pvr_fw_validate (90-165) checks it, then what it says
 * of this core taken as pvr_fw_get_device_info (168-189) and pvr_device_info.c's
 * pvr_device_info_set_quirks, _set_enhancements and _set_features take it. The root is waited for as
 * redwood_connsys.c's step 5 waits (2230-2233); the buffer is redwood_connsys_file's kalloc_data, kept.
 */
static bool
fw_read(void)
{
	const uint64_t *info, *words, *params;
	uint64_t brn, ern, mask_words, param_words, unknown;
	uint32_t at, param = 0;
	char named[100];
	size_t used = 0;

	SAY("pvr: 2: waiting for the root, then reading /fw/connsys/%s", FW_NAME);
	for (unsigned s = 0; s < 240 && rootvnode == NULL; s++) {
		delay_for_interval(1, NSEC_PER_SEC);
	}
	delay_for_interval(5, NSEC_PER_SEC);
	fw_file = redwood_connsys_file(FW_NAME, &fw_size);
	if (fw_file == NULL) {
		SAY("pvr: 2: /fw/connsys/%s IS NOT THERE (or the root never came: rootvnode %s)", FW_NAME, rootvnode == NULL ? "unset" : "set");
		return false;
	}
	if (fw_size < 2 * 4096 || (fw_size % 4096) != 0) {
		SAY("pvr: 2: NOT A FIRMWARE: %u bytes, not a whole number of 4 KiB blocks", fw_size);
		return false;
	}
	at = fw_size - 4096;
	fw_hdr = (const struct fw_header *)(const void *)(fw_file + at);
	SAY("pvr: 2: %u bytes; header version %u, length %u, %u layout entries of %u bytes, flags %#x, firmware %u.%u build %u, BVNC %#llx",
	    fw_size, fw_hdr->info_version, fw_hdr->header_len, fw_hdr->layout_entry_num, fw_hdr->layout_entry_size, fw_hdr->flags,
	    fw_hdr->version_major, fw_hdr->version_minor, fw_hdr->version_build, fw_hdr->bvnc);
	if (fw_hdr->info_version != 3 || fw_hdr->header_len != sizeof(struct fw_header) ||
	    fw_hdr->layout_entry_size != sizeof(struct fw_entry) || fw_hdr->layout_entry_num > 8) {
		SAY("pvr: 2: REFUSED: not the header Linux v7.3 reads (version 3, 48 bytes, at most 8 entries of 24)");
		return false;
	}
	if ((fw_hdr->flags & 1u) == 0 || fw_hdr->version_major != 1) {
		SAY("pvr: 2: REFUSED: Linux v7.3 takes only open-source firmware (flag 1) of major version 1");
		return false;
	}
	if (fw_hdr->bvnc != gpu_bvnc) {
		SAY("pvr: 2: REFUSED: built for BVNC %#llx, and this GPU said %#llx", fw_hdr->bvnc, gpu_bvnc);
		return false;
	}
	fw_layout = (const struct fw_entry *)(const void *)(fw_hdr + 1);
	for (uint32_t i = 0; i < fw_hdr->layout_entry_num; i++) {
		const struct fw_entry *e = &fw_layout[i];

		SAY("pvr:   section %u type %u: at %#x, %#x bytes (most %#x), offset %#x in its allocation", e->id, e->type, e->base_addr,
		    e->alloc_size, e->max_size, e->alloc_offset);
		if (e->base_addr >= e->base_addr + e->alloc_size) {
			SAY("pvr: 2: REFUSED: that section is empty or wraps");
			return false;
		}
	}

	/* the device information: four counts of 64-bit words, then the quirks, enhancements, features and the features' values */
	if (fw_hdr->device_info_size < 32 || (fw_hdr->device_info_size % 8) != 0 || fw_hdr->device_info_size > at) {
		SAY("pvr: 2: REFUSED: a device information block of %u bytes", fw_hdr->device_info_size);
		return false;
	}
	info = (const uint64_t *)(const void *)(fw_file + at - fw_hdr->device_info_size);
	brn = info[0];
	ern = info[1];
	mask_words = info[2];
	param_words = info[3];
	if (brn == 0 || brn > 16 || ern > 16 || mask_words > 16 || param_words > 256 ||
	    (4 + brn + ern + mask_words + param_words) * 8 > fw_hdr->device_info_size) {
		SAY("pvr: 2: REFUSED: the device information counts %llu, %llu, %llu, %llu do not fit its %u bytes", brn, ern, mask_words,
		    param_words, fw_hdr->device_info_size);
		return false;
	}
	words = info + 4;
	fw_quirks = words[0];
	fw_enhancements = ern != 0 ? words[brn] : 0;
	named[0] = 0;
	for (unsigned i = 0; i < BRN_NAMED; i++) {
		if ((fw_quirks >> i) & 1u) {
			used += (size_t)snprintf(named + used, sizeof(named) - used, " %u", brn_numbers[i]);
		}
	}
	unknown = fw_quirks & ~((1ULL << BRN_NAMED) - 1);
	SAY("pvr:   quirks (BRN) %#llx:%s%s; enhancements (ERN) %#llx", fw_quirks, named,
	    unknown != 0 ? ", and bits Linux v7.3 has no name for (it warns and goes on)" : "", ern != 0 ? words[brn] : 0ULL);
	words += brn + ern;
	params = words + mask_words;
	for (unsigned i = 0; i < FEATURES && i < mask_words * 64; i++) {
		if (((words[i >> 6] >> (i & 63)) & 1u) == 0) {
			continue;
		}
		feature_has[i] = true;
		if (feature_names[i][0] == '=') {
			if (param >= param_words) {
				SAY("pvr: 2: REFUSED: more features with a value than the %llu values in the file", param_words);
				return false;
			}
			feature_value[i] = params[param++];
		}
	}
	say_features();
	SAY("pvr: 2: the file is what Linux v7.3 would load for this GPU: %u of its %llu feature values used%s", param, param_words,
	    (mask_words > 1 && (words[1] >> (FEATURES - 64)) != 0) ? "; feature bits past its 71 are set too" : "");
	if (!feature_has[F_MIPS] || feature_value[F_PHYS_BUS_WIDTH] <= 32 || feature_has[F_XE_TPU2]) {
		SAY("pvr: 2: REFUSED: this was written for a MIPS firmware processor behind a bus wider than 32 bits, without "
		    "XE_TPU2's second reset register (pvr_fw_mips.c 109-110, pvr_fw_startstop.c 107)");
		return false;
	}
	return true;
}

/* The arena's pages, first fit: the first of `pages` in a row, or -1. */
static int
arena_alloc(uint32_t pages)
{
	uint32_t run = 0;

	for (uint32_t page = 0; page < ARENA_PAGES && pages != 0; page++) {
		run = arena_map[page] != 0 ? 0 : run + 1;
		if (run == pages) {
			memset(&arena_map[page + 1 - pages], 1, pages);
			arena_used += pages << 12;
			return (int)(page + 1 - pages);
		}
	}
	return -1;
}

static void
arena_free(uint32_t first, uint32_t pages)
{
	memset(&arena_map[first], 0, pages);
	arena_used -= pages << 12;
}

/*
 * One firmware object: `size` bytes, whole 4 KiB pages of the arena, mapped into the firmware's heap
 * at offset `at`, or at the next free place of its main part. pvr_fw.c's
 * pvr_fw_object_create_and_map_common and pvr_fw_object_fw_map, with pvr_vm_mips.c's pvr_vm_mips_map
 * (145-207) and get_mips_pte_flags (116-133) for the page table: readable and writable is "dirty",
 * with valid and global, and the cache policy "uncached". A page already mapped is an overlap.
 */
static struct fw_obj
fw_object(const char *name, uint32_t size, uint32_t at)
{
	struct fw_obj o = { NULL, 0, 0, 0 };
	uint32_t bytes = (size + 0xfffu) & ~0xfffu, pages = bytes >> 12, run = 0;
	int first = -1;

	if (at == FW_ANYWHERE) {        /* the lowest free run of the main heap: drm_mm's part, first fit */
		for (uint32_t page = heap_next >> 12; page < FW_HEAP_MAIN >> 12 && run < pages; page++) {
			run = mips_pt[page] != 0 ? 0 : run + 1;
			at = (page + 1 - run) << 12;
		}
		if (run < pages) {
			bytes = 0;
		}
	}
	for (uint32_t page = 0; bytes != 0 && (at & 0xfffu) == 0 && at < FW_HEAP_RAW && bytes <= FW_HEAP_RAW - at && page < pages; page++) {
		if (mips_pt[(at >> 12) + page] != 0) {
			SAY("pvr: %s OVERLAPS what is already at heap offset %#x", name, at + (page << 12));
			return o;
		}
	}
	if (bytes == 0 || (at & 0xfffu) != 0 || at >= FW_HEAP_RAW || bytes > FW_HEAP_RAW - at || (first = arena_alloc(pages)) < 0) {
		SAY("pvr: NO ROOM for %s: %#x bytes at heap offset %#x, %#x of the arena's %#x used", name, size, at, arena_used, ARENA_SIZE);
		return o;
	}
	o.cpu = (uint8_t *)arena + ((uint32_t)first << 12);
	o.phys = arena_phys + ((uint64_t)(uint32_t)first << 12);
	o.addr = (at & FW_HEAP_MASK) | FW_HEAP_MIPS_BASE;
	o.bytes = bytes;
	memset(o.cpu, 0, bytes);
	FlushPoC_DcacheRegion((vm_offset_t)o.cpu, bytes);
	for (uint32_t page = 0; page < pages; page++) {
		uint32_t *pte = &mips_pt[(at >> 12) + page];

		*pte = ((uint32_t)((o.phys >> 12) + page) << MIPS_ENTRYLO_PFN_SHIFT) & MIPS_ENTRYLO_PFN_MASK_36;
		*pte |= MIPS_ENTRYLO_DIRTY | (MIPS_UNCACHED << MIPS_ENTRYLO_POLICY_SHIFT) | MIPS_ENTRYLO_VALID | MIPS_ENTRYLO_GLOBAL;
	}
	FlushPoC_DcacheRegion((vm_offset_t)&mips_pt[at >> 12], pages * 4);
	return o;
}

/*
 * pvr_fw_object_destroy with pvr_vm_mips.c's pvr_vm_mips_unmap: out of the firmware's page table, the
 * memory back. Linux has the firmware drop its cached translations at once (pvr_mmu_flush_exec); here
 * that is asked for and the caller's next pvr_mmu_flush sends it -- before anything else is made.
 */
static void
fw_object_free(struct fw_obj *o)
{
	uint32_t at = (o->addr & FW_HEAP_MASK) >> 12;

	if (o->cpu == NULL) {
		return;
	}
	memset(&mips_pt[at], 0, (o->bytes >> 12) * 4);
	FlushPoC_DcacheRegion((vm_offset_t)&mips_pt[at], (o->bytes >> 12) * 4);
	arena_free((uint32_t)((o->phys - arena_phys) >> 12), o->bytes >> 12);
	mmu_flush_flags |= MMU_SYNC_2;          /* pvr_mmu_flush_request_all */
	memset(o, 0, sizeof(*o));
}

/*
 * The image into its memory, and the bootloader's configuration: the part of step 3 that a reset of
 * the GPU does again (pvr_fw.c's pvr_fw_reinit_code_data, from Linux's kept copy; here from the file).
 */
static uint32_t fw_code_size, fw_data_size, fw_segments;

static bool
fw_image(void)
{
	const struct fw_entry *boot_code = fw_section(MIPS_BOOT_CODE), *boot_entry = fw_section(MIPS_BOOT_DATA);
	const struct fw_entry *exception = fw_section(MIPS_EXCEPTIONS_CODE), *stack = fw_section(MIPS_STACK);
	const struct elf32_header *elf = (const struct elf32_header *)(const void *)fw_file;
	const struct elf32_program *program;
	uint32_t image = fw_size - 4096 - fw_hdr->device_info_size;
	uint64_t *config;

	fw_segments = 0;
	/* the ELF's loadable segments, each to where the layout puts its address */
	if (memcmp(elf->ident, "\177ELF\1\1", 6) != 0 || elf->phentsize != sizeof(*program) || elf->phoff > image ||
	    (uint64_t)elf->phoff + (uint64_t)elf->phnum * sizeof(*program) > image) {
		SAY("pvr: 3: REFUSED: the image is not a 32-bit little-endian ELF with its program headers inside it");
		return false;
	}
	program = (const struct elf32_program *)(const void *)(fw_file + elf->phoff);
	for (uint32_t i = 0; i < elf->phnum; i++, program++) {
		const struct fw_entry *e = NULL;
		const struct fw_obj *to;
		uint32_t end = program->vaddr + program->memsz, offset;

		if (program->type != ELF_PT_LOAD) {
			continue;
		}
		for (uint32_t j = 0; j < fw_hdr->layout_entry_num && end > program->vaddr; j++) {
			uint32_t from = fw_layout[j].base_addr, until = from + fw_layout[j].alloc_size;

			if (program->vaddr >= from && program->vaddr < until && end > from && end <= until) {
				e = &fw_layout[j];
				break;
			}
		}
		if (e == NULL || program->filesz > program->memsz || program->offset > image || program->filesz > image - program->offset) {
			SAY("pvr: 3: REFUSED: segment %u (%#x, %#x bytes, %#x of them at %#x in the file) is in no section, or not in the file",
			    i, program->vaddr, program->memsz, program->filesz, program->offset);
			return false;
		}
		to = e->type == FW_CODE ? &fw_code : &fw_data;
		offset = program->vaddr - e->base_addr + e->alloc_offset;
		if ((uint64_t)offset + program->memsz > (e->type == FW_CODE ? fw_code_size : fw_data_size)) {
			SAY("pvr: 3: REFUSED: segment %u ends past its allocation", i);
			return false;
		}
		memcpy(to->cpu + offset, fw_file + program->offset, program->filesz);
		memset(to->cpu + offset + program->filesz, 0, program->memsz - program->filesz);
		fw_segments++;
	}

	/* what the MIPS wrapper's remaps point at, and the bootloader's configuration at the head of the boot data */
	boot_code_phys = fw_code.phys + boot_code->alloc_offset;
	exception_code_phys = fw_code.phys + exception->alloc_offset;
	boot_data_phys = fw_data.phys + boot_entry->alloc_offset;
	boot_data = fw_data.cpu + boot_entry->alloc_offset;
	if (boot_code->alloc_offset + 4096 > fw_code_size || exception->alloc_offset + 4096 > fw_code_size ||
	    boot_entry->alloc_offset + 4096 > fw_data_size || stack->alloc_offset + 4096 > fw_data_size) {
		SAY("pvr: 3: REFUSED: a boot section lies outside its allocation");
		return false;
	}
	config = (uint64_t *)(void *)(boot_data + MIPS_BOOTLDR_CONF_OFFSET);
	config[0] = fw_data.phys + stack->alloc_offset;         /* stack_phys_addr */
	config[1] = RGX_PHYS;                                   /* reg_base */
	for (unsigned i = 0; i < MIPS_PT_PAGES; i++) {
		config[2 + i] = arena_phys + i * 4096;          /* pt_phys_addr[] */
	}
	((uint32_t *)(void *)&config[6])[0] = 12;               /* pt_log2_page_size */
	((uint32_t *)(void *)&config[6])[1] = MIPS_PT_PAGES;    /* pt_num_pages */
	config[7] = 0;                                          /* reserved1, reserved2 */

	FlushPoC_DcacheRegion((vm_offset_t)fw_code.cpu, fw_code.bytes);
	FlushPoC_DcacheRegion((vm_offset_t)fw_data.cpu, fw_data.bytes);
	return true;
}

/* pvr_fw.c's pvr_fw_hard_reset, its first half: every page the firmware can see, zeroed (Linux spares its trace buffers; nothing here reads an old trace after a reset). */
static void
fw_zero(void)
{
	for (uint32_t page = 0; page < FW_HEAP_RAW >> 12; page++) {
		if ((mips_pt[page] & MIPS_ENTRYLO_VALID) != 0) {
			uint8_t *cpu = (uint8_t *)arena + ((((uint64_t)(mips_pt[page] & MIPS_ENTRYLO_PFN_MASK_36) >> MIPS_ENTRYLO_PFN_SHIFT) << 12) - arena_phys);

			memset(cpu, 0, 4096);
			FlushPoC_DcacheRegion((vm_offset_t)cpu, 4096);
		}
	}
}

/*
 * Step 3: memory, the image, the page table, the boot configuration. pvr_fw.c's pvr_fw_process
 * (631-792) with layout_get_sizes, pvr_fw_mips.c's pvr_mips_init and pvr_mips_fw_process (33-94),
 * pvr_fw_util.c's pvr_fw_process_elf_command_stream with pvr_fw.c's pvr_fw_find_mmu_segment (227-279),
 * pvr_vm_mips.c's pvr_vm_mips_init. The memory as redwood_ufshc.c takes its buffers (1255-1262):
 * kmem_alloc_contig, kvtophys, FlushPoC_DcacheRegion.
 */
static bool
fw_load(void)
{
	const struct fw_entry *private = fw_section(MIPS_PRIVATE_DATA), *boot_code = fw_section(MIPS_BOOT_CODE);
	const struct fw_entry *boot_entry = fw_section(MIPS_BOOT_DATA), *exception = fw_section(MIPS_EXCEPTIONS_CODE);
	const struct fw_entry *stack = fw_section(MIPS_STACK);
	uint32_t code_size = 0, data_size = 0, wc = 0;
	const uint64_t *config;

	for (uint32_t i = 0; i < fw_hdr->layout_entry_num; i++) {
		if (fw_layout[i].alloc_size >= ARENA_SIZE || fw_layout[i].alloc_offset >= ARENA_SIZE) {
			code_size = ARENA_SIZE;
		} else if (fw_layout[i].type == FW_CODE) {
			code_size += fw_layout[i].alloc_size;
		} else if (fw_layout[i].type == FW_DATA) {
			data_size += fw_layout[i].alloc_size;
		} else {
			code_size = ARENA_SIZE;         /* core memory sections: a META's, not laid out here */
		}
	}
	if (private == NULL || boot_code == NULL || boot_entry == NULL || exception == NULL || stack == NULL ||
	    code_size >= ARENA_SIZE || data_size >= ARENA_SIZE) {
		SAY("pvr: 3: REFUSED: the layout lacks one of the five MIPS sections this needs, or has core memory, or is too large");
		return false;
	}
	SAY("pvr: 3: taking %u KiB of contiguous wired memory for the page table, %#x bytes of code and %#x of data", ARENA_SIZE >> 10,
	    code_size, data_size);
	if (kmem_alloc_contig(kernel_map, &arena, ARENA_SIZE, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		SAY("pvr: 3: NO MEMORY");
		return false;
	}
	arena_phys = kvtophys(arena);
	if (kvtophys(arena + ARENA_SIZE - 4096) != arena_phys + ARENA_SIZE - 4096 || (arena_phys & 0xfffu) != 0 ||
	    (arena_phys + ARENA_SIZE) >> feature_value[F_PHYS_BUS_WIDTH] != 0) {
		SAY("pvr: 3: THAT MEMORY IS NOT ONE PIECE the GPU can reach: physical %#llx, its last page %#llx", arena_phys,
		    kvtophys(arena + ARENA_SIZE - 4096));
		return false;
	}

	/* the page table first: 4 bytes for each 4 KiB of the 16 MiB heap, not itself in the heap */
	(void)PE_parse_boot_argn("pvr_wc", &wc, sizeof(wc));
	if (wc != 0) {
		/* a second mapping, write-combined, used from here on in place of the cached one (see "the GPU's memory" below) */
		FlushPoC_DcacheRegion(arena, ARENA_SIZE);
		arena = ml_io_map_wcomb((vm_offset_t)arena_phys, ARENA_SIZE);
		SAY("pvr: 3: pvr_wc=1: the arena through a write-combined mapping at %#lx", (unsigned long)arena);
	}
	mips_pt = (uint32_t *)arena;
	(void)arena_alloc(MIPS_PT_PAGES);       /* the arena's first pages */

	/* the code must be at the bottom of the heap, the data where the file's private data says (pvr_fw_process 651-683) */
	fw_code = fw_object("the code", code_size, 0);
	fw_data = fw_object("the data", data_size, private->base_addr & FW_HEAP_MASK);
	if (fw_code.cpu == NULL || fw_data.cpu == NULL) {
		return false;
	}
	heap_next = (private->base_addr & FW_HEAP_MASK) + ((data_size + 0xfffu) & ~0xfffu);
	if (heap_next < code_size) {
		heap_next = (code_size + 0xfffu) & ~0xfffu;
	}

	fw_code_size = code_size;
	fw_data_size = data_size;
	if (!fw_image()) {
		return false;
	}
	config = (uint64_t *)(void *)(boot_data + MIPS_BOOTLDR_CONF_OFFSET);
	SAY("pvr: 3: %u segments loaded; memory at physical %#llx: page table there, code %#llx (firmware %#x), data %#llx (%#x)", fw_segments,
	    arena_phys, fw_code.phys, fw_code.addr, fw_data.phys, fw_data.addr);
	SAY("pvr: 3: boot code %#llx, boot data %#llx, exceptions %#llx, stack %#llx; first words of boot code %#x, of code %#x",
	    boot_code_phys, boot_data_phys, exception_code_phys, config[0], *(uint32_t *)(void *)(fw_code.cpu + boot_code->alloc_offset),
	    *(uint32_t *)(void *)fw_code.cpu);
	return true;
}

/* What step 4 writes into the structures it has made: again after a reset of the GPU, when all of them have been zeroed (pvr_fw_hard_reset). */
static void
fw_structures_fill(void)
{
	/* ccb_ctrl_init: both offsets 0 */
	put32(&fw_kccb_ctl, 132, (1u << KCCB_LOG2) - 1);           /* wrap_mask */
	put32(&fw_kccb_ctl, 136, KCCB_CMD_SIZE);                   /* cmd_size */
	put32(&fw_fwccb_ctl, 132, (1u << FWCCB_LOG2) - 1);
	put32(&fw_fwccb_ctl, 136, FWCCB_CMD_SIZE);

	/* fw_sysdata_init: config_flags is 0 unless the SLC is under 4 KiB, then ROGUE_FWIF_INICFG_DISABLE_DM_OVERLAP (pvr_rogue_fwif.h 492) */
	put32(&fw_sysdata, 0, feature_value[F_SLC_SIZE_IN_KILOBYTES] < 4 ? 1u << 19 : 0);

	/* fw_fault_page_init */
	for (unsigned i = 0; i < 4096; i += 4) {
		put32(&fault_page, i, 0xdeadbee0);
	}

	/* fw_gpu_util_fwcb_init writes last_word = PVR_FWIF_GPU_UTIL_STATE_IDLE: 0, as it is */

	/* fw_runtime_cfg_init */
	put32(&runtime_cfg, 8, 1);                              /* active_pm_latency_persistant: true */
	put32(&runtime_cfg, 12, clock_hz);                      /* core_clock_speed */
	put32(&runtime_cfg, 16, (uint32_t)feature_value[F_NUM_CLUSTERS]);       /* default_dusts_num_init */

	/* tracebuf_ctrl_init: log_type ROGUE_FWIF_LOG_TYPE_NONE (0); trace_buffer, the host's own pointer, left 0 */
	for (unsigned i = 0; i < TRACE_THREADS; i++) {
		put32(&fw_tracectl, 8 + i * TRACEBUF_SPACE_SIZE + 4, trace[i].addr);
	}
	put32(&fw_tracectl, 856, TRACE_BUF_DWORDS);

	/* fw_osdata_init */
	put32(&fw_osdata, 572, fw_power_sync.addr);

	/* fw_osinit_init */
	put32(&fw_osinit, 0, fw_kccb_ctl.addr);                    /* kernel_ccbctl_fw_addr */
	put32(&fw_osinit, 4, fw_kccb.addr);                        /* kernel_ccb_fw_addr */
	put32(&fw_osinit, 8, fw_rtn.addr);                         /* kernel_ccb_rtn_slots_fw_addr */
	put32(&fw_osinit, 12, fw_fwccb_ctl.addr);                  /* firmware_ccbctl_fw_addr */
	put32(&fw_osinit, 16, fw_fwccb.addr);                      /* firmware_ccb_fw_addr */
	put32(&fw_osinit, 28, fw_hwrinfobuf.addr);                 /* rogue_fwif_hwr_info_buf_ctl_fw_addr */
	put32(&fw_osinit, 36, fw_osdata.addr);                     /* fw_os_data_fw_addr */
	put32(&fw_osinit, 40, COMPCHECKS_LAYOUT_VERSION);       /* rogue_comp_checks.hw_bvnc.layout_version */
	put32(&fw_osinit, 40 + 16, COMPCHECKS_LAYOUT_VERSION);  /* ....fw_bvnc.layout_version */

	/* fw_sysinit_init */
	put64(&fw_sysinit, 0, fault_page.phys);                 /* fault_phys_addr */
	put64(&fw_sysinit, 8, PDSCODEDATA_HEAP_BASE);           /* pds_exec_base */
	put64(&fw_sysinit, 16, USCCODE_HEAP_BASE);              /* usc_exec_base */
	put32(&fw_sysinit, 316, runtime_cfg.addr);              /* runtime_cfg_fw_addr */
	put32(&fw_sysinit, 320, fw_tracectl.addr);              /* trace_buf_ctl_fw_addr */
	put32(&fw_sysinit, 324, fw_sysdata.addr);               /* fw_sys_data_fw_addr */
	put32(&fw_sysinit, 328, gpu_util.addr);                 /* gpu_util_fw_cb_ctl_fw_addr */
	put32(&fw_sysinit, 344, clock_hz);                      /* initial_core_clock_speed */
	put32(&fw_sysinit, 356, 1);                             /* marker_val */
}

/*
 * Step 4: everything the firmware is handed before it will say it has started, made in the order
 * pvr_fw_init makes it (940-1009): the connection control (pvr_fw_create_fwif_connection_ctl), the
 * kernel's and the firmware's command buffers with their controls (pvr_ccb.c pvr_kccb_init,
 * pvr_fwccb_init, pvr_ccb_init, ccb_ctrl_init), the return slots, and pvr_fw_create_structures (455-598)
 * with its fw_*_init functions and pvr_fw_trace.c's pvr_fw_trace_init and tracebuf_ctrl_init. All are
 * zero but for what those functions write.
 */
static bool
fw_structures(void)
{

	SAY("pvr: 4: the structures the firmware reads as it boots (heap offsets %#x on, and three at %#x)", heap_next, FW_CONFIG_OFFSET);
	fw_connection = fw_object("the connection control", CONNECTION_CTL_SIZE, FW_CONFIG_OFFSET);
	fw_kccb_ctl = fw_object("the kernel CCB's control", CCB_CTL_SIZE, FW_ANYWHERE);
	fw_kccb = fw_object("the kernel CCB", KCCB_CMD_SIZE << KCCB_LOG2, FW_ANYWHERE);
	fw_fwccb_ctl = fw_object("the firmware CCB's control", CCB_CTL_SIZE, FW_ANYWHERE);
	fw_fwccb = fw_object("the firmware CCB", FWCCB_CMD_SIZE << FWCCB_LOG2, FW_ANYWHERE);
	fw_rtn = fw_object("the return slots", 4u << KCCB_LOG2, FW_ANYWHERE);              /* pvr_fw_init 949: a u32 each */
	fw_power_sync = fw_object("the power sync", 4, FW_ANYWHERE);                       /* pvr_fw.h 371: a u32 */
	fw_hwrinfobuf = fw_object("the recovery info", HWRINFOBUF_SIZE, FW_ANYWHERE);
	fw_mmucache_sync = fw_object("the MMU cache sync", 4, FW_ANYWHERE);                /* pvr_fw.c 46: PVR_SYNC_OBJ_SIZE */
	fw_sysdata = fw_object("sysdata", SYSDATA_SIZE, FW_ANYWHERE);
	fault_page = fw_object("the fault page", 4096, FW_ANYWHERE);                    /* pvr_fw.c 44 */
	gpu_util = fw_object("the GPU utilisation", GPU_UTIL_FWCB_SIZE, FW_ANYWHERE);
	runtime_cfg = fw_object("the runtime configuration", RUNTIME_CFG_SIZE, FW_ANYWHERE);
	for (unsigned i = 0; i < TRACE_THREADS; i++) {
		trace[i] = fw_object("a trace buffer", TRACE_BUF_DWORDS * 4, FW_ANYWHERE);
	}
	fw_tracectl = fw_object("the trace control", TRACEBUF_SIZE, FW_ANYWHERE);
	fw_osdata = fw_object("osdata", OSDATA_SIZE, FW_ANYWHERE);
	fw_osinit = fw_object("osinit", OSINIT_SIZE, FW_CONFIG_OFFSET + FW_CONFIG_GRANULE);
	fw_sysinit = fw_object("sysinit", SYSINIT_SIZE, FW_CONFIG_OFFSET + 2 * FW_CONFIG_GRANULE);
	fw_trace0 = trace[0];
	if (fw_sysinit.cpu == NULL || fw_osinit.cpu == NULL || fw_osdata.cpu == NULL || fw_tracectl.cpu == NULL || trace[0].cpu == NULL ||
	    trace[TRACE_THREADS - 1].cpu == NULL || runtime_cfg.cpu == NULL || gpu_util.cpu == NULL || fault_page.cpu == NULL ||
	    fw_sysdata.cpu == NULL || fw_mmucache_sync.cpu == NULL || fw_hwrinfobuf.cpu == NULL || fw_power_sync.cpu == NULL || fw_rtn.cpu == NULL ||
	    fw_fwccb.cpu == NULL || fw_fwccb_ctl.cpu == NULL || fw_kccb.cpu == NULL || fw_kccb_ctl.cpu == NULL || fw_connection.cpu == NULL) {
		return false;
	}
	/* (the MMU cache sync is made here as Linux makes it; nothing names it to the firmware before a later command does) */

	fw_structures_fill();

	/* all of it out of this CPU's cache, where the firmware processor cannot see it */
	FlushPoC_DcacheRegion(arena, ARENA_SIZE);
	SAY("pvr: 4: made, %#x of the arena's %#x used: sysinit %#x, osinit %#x, connection %#x, KCCB %#x (control %#x), FWCCB %#x (%#x)",
	    arena_used, ARENA_SIZE, fw_sysinit.addr, fw_osinit.addr, fw_connection.addr, fw_kccb.addr, fw_kccb_ctl.addr, fw_fwccb.addr, fw_fwccb_ctl.addr);
	SAY("pvr: 4: clock told %u Hz; page table entries: code %#x, data %#x, sysinit %#x", clock_hz, mips_pt[0],
	    mips_pt[(fw_data.addr & FW_HEAP_MASK) >> 12], mips_pt[(fw_sysinit.addr & FW_HEAP_MASK) >> 12]);
	return true;
}

/* What can be read of a firmware that is booting, or is not. */
static void
fw_state(uint32_t ms)
{
	uint64_t unmapped = rd64(CR_MIPS_ADDR_REMAP_UNMAPPED_STATUS);
	const uint8_t *nmi = boot_data + MIPS_NMI_SHARED_DATA_BASE + 4;         /* struct rogue_mips_state */

	SAY("pvr:   %4u ms: started %u, boot stage %#x, connection %u; EXCEPTION_STATUS %#x, UNMAPPED_STATUS %#x_%08x, IRQ_STATUS %#x", ms,
	    fresh32(fw_sysinit.cpu + SYSINIT_FIRMWARE_STARTED) & 0xffu, fresh32(boot_data + MIPS_BOOT_STAGE_OFFSET), fresh32(fw_connection.cpu),
	    rd(rgx, CR_MIPS_EXCEPTION_STATUS), (uint32_t)(unmapped >> 32), (uint32_t)unmapped, rd(rgx, CR_MIPS_WRAPPER_IRQ_STATUS));
	if (fresh32(nmi) != 0) {
		SAY("pvr:   the firmware's own exception record: error_state %#x error_epc %#x status %#x cause %#x badvaddr %#x epc %#x",
		    fresh32(nmi), fresh32(nmi + 4), fresh32(nmi + 8), fresh32(nmi + 12), fresh32(nmi + 16), fresh32(nmi + 20));
	}
}

/* More of the same, once, at the end: the registers, and whatever the firmware wrote about itself. */
static void
fw_verdict(void)
{
	const uint8_t *assert_buf = fw_tracectl.cpu + 8 + 16, *fault = fw_sysdata.cpu + 80;
	const uint8_t *checks = fw_osinit.cpu + 40;
	uint64_t reset = rd64(CR_SOFT_RESET);

	SAY("pvr:   SOFT_RESET %#x_%08x, SIDEKICK_IDLE %#x, MIPS_WRAPPER_STATUS %#x, EVENT_STATUS %#x, BIF_FAULT_BANK0_MMU_STATUS %#x",
	    (uint32_t)(reset >> 32), (uint32_t)reset, rd(rgx, CR_SIDEKICK_IDLE), rd(rgx, CR_MIPS_WRAPPER_STATUS), rd(rgx, CR_EVENT_STATUS),
	    rd(rgx, CR_BIF_FAULT_BANK0_MMU_STATUS));
	FlushPoC_DcacheRegion((vm_offset_t)fw_tracectl.cpu, 4096);
	FlushPoC_DcacheRegion((vm_offset_t)fw_sysdata.cpu, 4096);
	FlushPoC_DcacheRegion((vm_offset_t)fw_osinit.cpu, 4096);
	SAY("pvr:   sysinit: marker %u, started at its time %u; osinit's checks: updated %u, firmware BVNC %#llx, DDK %#x build %u",
	    fresh32(fw_sysinit.cpu + 356), fresh32(fw_sysinit.cpu + 360), *(const uint32_t *)(const void *)(checks + 56),
	    *(const uint64_t *)(const void *)(checks + 16 + 8), *(const uint32_t *)(const void *)(checks + 36),
	    *(const uint32_t *)(const void *)(checks + 40));
	if (assert_buf[0] != 0 || assert_buf[200] != 0) {
		SAY("pvr:   THE FIRMWARE ASSERTED: %.70s:%u: %.90s", (const char *)assert_buf, *(const uint32_t *)(const void *)(assert_buf + 400),
		    (const char *)assert_buf + 200);
	}
	if (*(const uint32_t *)(const void *)(fw_sysdata.cpu + 3536) != 0) {
		SAY("pvr:   the firmware counts %u faults of its own, the first: %.70s:%u: %.60s", *(const uint32_t *)(const void *)(fw_sysdata.cpu + 3536),
		    (const char *)fault + 24, *(const uint32_t *)(const void *)(fault + 24 + 400), (const char *)fault + 24 + 200);
	}
}

/*
 * Step 5: pvr_fw_startstop.c's pvr_fw_start (105-194) with rogue_slc_init (63-94), pvr_fw_mips.c's
 * pvr_mips_wrapper_init (97-162) and rogue_axi_ace_list_init (20-34), then pvr_fw.c's
 * pvr_wait_for_fw_boot (869-880) as pvr_power.c's pvr_power_fw_enable has it -- asleep a millisecond
 * at a time where Linux spins, by the clock, and saying what it sees twice a second. No interrupt is wanted or taken.
 */
static bool
fw_start(void)
{
	uint64_t mask = feature_has[F_PBE2_IN_XE] ? CR_SOFT_RESET_PBE2_XE_MASKFULL : CR_SOFT_RESET_MASKFULL;
	uint32_t slc;
	uint64_t began, ns = 0;
	uint32_t ms, said = 0;
	bool started = false;

	SAY("pvr: 5: the start: soft reset (if nothing follows, a write to the GPU hung)");
	if (!rgx_whole) {       /* (fw_start runs again at every reset of the GPU) */
		rgx = ml_io_map(RGX_PHYS, RGX_SIZE);    /* more than pvr=2's one page: SYS_BUS_SECURE is at 0xa100 */
		rgx_whole = true;
	}
	if (feature_has[F_SYS_BUS_SECURE_RESET]) {
		wr(rgx, CR_SYS_BUS_SECURE, 0);
		(void)rd(rgx, CR_SYS_BUS_SECURE);
	}
	wr64(CR_SOFT_RESET, mask);                      /* everything in reset */
	(void)rd64(CR_SOFT_RESET);
	wr64(CR_SOFT_RESET, mask ^ CR_SOFT_RESET_RASCALDUSTS);  /* the rascal and the dusts out */
	(void)rd64(CR_SOFT_RESET);
	wr64(CR_SOFT_RESET, CR_SOFT_RESET_GARTEN);      /* everything out but the firmware processor */
	SAY("pvr: 5: all but the firmware processor out of reset: SOFT_RESET reads %#llx; now the SLC, the MIPS wrapper, AXI", rd64(CR_SOFT_RESET));

	/* rogue_slc_init: only the low half written, as Linux */
	slc = (rd(rgx, CR_SLC_CTRL_MISC) & CR_SLC_CTRL_MISC_PSG_HAZARD) | CR_SLC_CTRL_MISC_PVR_HASH1;
	if (feature_value[F_SLC_CACHE_LINE_SIZE_BITS] < 1024) {
		slc |= CR_SLC_CTRL_MISC_BYPASS_BURST;
	}
	if ((fw_quirks & BRN_71242) != 0 && !feature_has[F_GPU_MULTICORE_SUPPORT]) {
		slc |= CR_SLC_CTRL_MISC_LAZYWB_OVERRIDE;
	}
	wr(rgx, CR_SLC_CTRL_MISC, slc);

	/* pvr_mips_wrapper_init: where the firmware finds the registers and that it starts in microMIPS; then
	 * the three pages its fixed addresses are remapped to, and the same for address 0 on a 36-bit core */
	wr(rgx, CR_MIPS_WRAPPER_CONFIG, (MIPS_REGISTERS_VIRTUAL_BASE >> MIPS_WRAPPER_REGBANK_ALIGN) | CR_MIPS_WRAPPER_CONFIG_MICROMIPS);
	wr64(CR_MIPS_ADDR_REMAP1_CONFIG1, MIPS_BOOT_REMAP_IN | CR_MIPS_ADDR_REMAP_ENABLE);
	wr64(CR_MIPS_ADDR_REMAP1_CONFIG1 + 8, (boot_code_phys & CR_MIPS_ADDR_REMAP_ADDR_OUT) | MIPS_REMAP_LOG2_SIZE);
	if ((fw_quirks & BRN_63553) != 0) {
		wr64(CR_MIPS_ADDR_REMAP1_CONFIG1 + 0x40, CR_MIPS_ADDR_REMAP_ENABLE);
		wr64(CR_MIPS_ADDR_REMAP1_CONFIG1 + 0x48, (boot_code_phys & CR_MIPS_ADDR_REMAP_ADDR_OUT) | MIPS_REMAP_LOG2_SIZE);
	}
	wr64(CR_MIPS_ADDR_REMAP1_CONFIG1 + 0x10, MIPS_DATA_REMAP_IN | CR_MIPS_ADDR_REMAP_ENABLE);
	wr64(CR_MIPS_ADDR_REMAP1_CONFIG1 + 0x18, (boot_data_phys & CR_MIPS_ADDR_REMAP_ADDR_OUT) | MIPS_REMAP_LOG2_SIZE);
	wr64(CR_MIPS_ADDR_REMAP1_CONFIG1 + 0x20, MIPS_CODE_REMAP_IN | CR_MIPS_ADDR_REMAP_ENABLE);
	wr64(CR_MIPS_ADDR_REMAP1_CONFIG1 + 0x28, (exception_code_phys & CR_MIPS_ADDR_REMAP_ADDR_OUT) | MIPS_REMAP_LOG2_SIZE);
	wr64(CR_MTS_GARTEN_WRAPPER_CONFIG, 0);          /* IDLE_CTRL_META: "Garten IDLE bit controlled by MIPS" */
	wr(rgx, CR_MIPS_DEBUG_CONFIG, 0);               /* the EJTAG probe on */

	/* rogue_axi_ace_list_init: "everything to outer cache" -- the shifts are cr_defs 4523-4541 */
	wr64(CR_AXI_ACE_LITE_CONFIGURATION, (3u << 8) | (3u << 10) | (2u << 16) | (2u << 14) | (2u << 12) | (2u << 22) | (2u << 26) |
	    (2u << 30));
	SAY("pvr: 5: SLC_CTRL_MISC %#x; MIPS_WRAPPER_CONFIG %#x; remaps read back: boot %#llx, data %#llx, exceptions %#llx%s",
	    rd(rgx, CR_SLC_CTRL_MISC), rd(rgx, CR_MIPS_WRAPPER_CONFIG), rd64(CR_MIPS_ADDR_REMAP1_CONFIG1 + 8),
	    rd64(CR_MIPS_ADDR_REMAP1_CONFIG1 + 0x18), rd64(CR_MIPS_ADDR_REMAP1_CONFIG1 + 0x28),
	    (fw_quirks & BRN_63553) != 0 ? "; BRN 63553's fifth set" : "");

	SAY("pvr: 5: letting the firmware processor go, and polling sysinit's \"started\" for %u ms", FW_BOOT_TIMEOUT_MS);
	delay(3);                                       /* "at least 16 cycles" before and after: at 26 MHz that is 1 us */
	wr64(CR_SOFT_RESET, 0);
	(void)rd64(CR_SOFT_RESET);
	delay(3);

	began = mach_absolute_time();
	for (;;) {
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		ms = (uint32_t)(ns / NSEC_PER_MSEC);
		started = (fresh32(fw_sysinit.cpu + SYSINIT_FIRMWARE_STARTED) & 0xffu) != 0;
		if (started || ms >= said || ms >= FW_BOOT_TIMEOUT_MS) {
			fw_state(ms);
			said = ms + 500;
		}
		if (started || ms >= FW_BOOT_TIMEOUT_MS) {
			break;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	fw_verdict();
	if (started) {
		SAY("pvr: 5: BOOTED: the firmware set its started flag; it is left running");
		return true;
	}
	SAY("pvr: 5: NOT BOOTED: no started flag in %u ms; everything back into soft reset", FW_BOOT_TIMEOUT_MS);
	wr64(CR_SOFT_RESET, mask);
	return false;
}

/*
 * ---- commands (pvr=4) ----
 *
 * Linux v7.3 again. What it does NOT do after the boot flag: there is no handshake -- nothing in the
 * driver writes or reads the connection control once it is made (the firmware itself put "active", 2,
 * there on the device) -- and pvr_fw_init sends no command; nor has v7.3 a command that asks the
 * firmware about itself (what it says is in osinit's checks, printed at boot). The first commands of a
 * running Linux are the watchdog's HEALTH_CHECK twice a second, sent without a return slot (pvr_power.c pvr_watchdog_worker,
 * pvr_watchdog_kccb_stalled, 166), LOGTYPE_UPDATE when someone turns the trace on (pvr_fw_trace.c
 * update_logtype, 203), and the power requests, which are the way DOWN (pvr_power_fw_disable:
 * forced idle, then off) and so are not sent here but for boot-arg pvr_idle=1, the first of the two.
 *
 * No interrupt: Linux's threaded handler (pvr_device.c pvr_device_irq_thread_handler, 203-237) clears
 * the MIPS wrapper's interrupt, drains the firmware's command buffer and wakes whoever waits on a
 * return slot. Here fw_service does the first two whenever it is called -- every millisecond while a
 * command is waited for, ten times a second otherwise -- and the waiters poll their slot. What that
 * costs: up to 100 ms before a firmware request (a free list to grow, a context reset) is seen when
 * nothing is waiting, a wakeup ten times a second for ever, and an interrupt line that stays asserted
 * between polls. Its safety events are not polled: Linux enables them only with ECC_RAMS or
 * WATCHDOG_TIMER (pvr_device_safety_irq_init, 253-274) and this core's file sets neither.
 *
 * ponytail: the shared memory is still the kernel's cached pages, flushed around each access. The one
 * place both sides write a cache line is the return slots, and only one slotted command is ever out
 * at a time, so that holds; many at once (jobs) want an uncached mapping of the arena instead.
 */
#define CR_EVENT_CLEAR                  0x0138                  /* cr_defs 831 */
#define CR_MIPS_WRAPPER_IRQ_CLEAR       0x08b0                  /* cr_defs 1472; bit 0 here and in IRQ_STATUS is the event (1469, 1476) */
#define CR_MTS_SCHEDULE                 0x0b00                  /* cr_defs 1683; the data master in bits 3..0 (1704), PVR_FWIF_DM_GP is 0 */

#define CMD_MAGIC                       (0x2ABCu << 16)         /* pvr_rogue_fwif.h 798-802 */
#define KCCB_CMD_POW                    (107u | CMD_MAGIC)      /* 1168 */
#define KCCB_CMD_HEALTH_CHECK           (115u | CMD_MAGIC)      /* 1188 */
#define KCCB_CMD_LOGTYPE_UPDATE         (206u | CMD_MAGIC)      /* 1212 */
#define POW_FORCED_IDLE_REQ             2u                      /* 930: enum rogue_fwif_power_type */
#define POWER_FORCE_IDLE                1u                      /* 936 */
#define KCCB_RTN_SLOT_CMD_EXECUTED      1u                      /* 1769; CLEANUP_BUSY 2, POLL_FAILURE 4 */
#define LOG_TYPE_TRACE                  1u                      /* 23 */
#define LOG_TYPE_GROUP_MASK             0x80007FFEu             /* 39 */
#define KCCB_TIMEOUT_MS                 1000                    /* the HZ every caller of pvr_kccb_wait_for_completion gives; pvr_ccb.c 21, pvr_power.c 28 */
#define WATCHDOG_TIME_MS                500                     /* pvr_power.c 30 */

static uint32_t kccb_sent, kccb_acked, fwccb_seen, health_checks;
static uint32_t last_rtn;               /* the return slot of the last command() */
static uint32_t mmu_flushes, mmu_flush_us, mmu_flush_most;      /* since the last statistics line */
static uint32_t drm_unmap_seq, drm_flushed_seq; /* unmappings there have been; those an answered flush covers (pvr=6) */
static uint32_t drm_dead_count;
static uint64_t drm_dead_since;
static void drm_reap(void);
static bool fw_quiet;                   /* programs are being served: a command a frame is not said in the log (pvr=6) */
static uint32_t fw_trace_mask;          /* the trace groups turned on, to turn on again after a reset */
static bool drm_lost;                   /* the GPU did not come back from a reset: nothing is sent any more (pvr=6) */
static const char *drm_reset_wanted;
static int kccb_send4(uint32_t type, uint32_t data0, uint32_t data1, uint32_t data2, uint32_t data3, bool slot);
static int kccb_send_words(uint32_t type, const uint32_t *data, uint32_t words, bool slot);
static void pvr_free_list_grow_request(uint32_t id);    /* pvr=5 */
static void pvr_free_list_reconstruct_request(const uint32_t *request);
static void drm_context_reset(const uint32_t *notification);    /* pvr=6 */
static void drm_poll(void);
static struct drm_job *drm_jobs;
static lck_mtx_t *drm_lock;
static bool drm_serving;
static uint32_t drm_submitted, drm_finished, drm_resets;
static void drm_reset(const char *why);
static void drm_irq_seen(void);
static void drm_irq_listen(void);
static void drm_stats_say(void);
static uint32_t drm_ms_since(uint64_t then);
static bool drm_irq_works;
static uint64_t drm_last_kick;

/* A word of ours where the firmware reads it: the line fresh from memory, changed, and back. */
static void
store32(const struct fw_obj *o, uint32_t at, uint32_t value)
{
	FlushPoC_DcacheRegion((vm_offset_t)(o->cpu + at), 4);
	put32(o, at, value);
	FlushPoC_DcacheRegion((vm_offset_t)(o->cpu + at), 4);
}

/* pvr_ccb.c's process_fwccb_command (138) and pvr_fwccb_process (185): each said; the free list to grow is grown (pvr=5), the rest only said. */
static void
fwccb_process(void)
{
	for (unsigned n = 0; n < (1u << FWCCB_LOG2); n++) {
		uint32_t read = fresh32(fw_fwccb_ctl.cpu + 128), type;
		const uint8_t *cmd = fw_fwccb.cpu + (read & ((1u << FWCCB_LOG2) - 1)) * FWCCB_CMD_SIZE;

		if (read == fresh32(fw_fwccb_ctl.cpu)) {
			return;
		}
		FlushPoC_DcacheRegion((vm_offset_t)cmd, FWCCB_CMD_SIZE);
		type = *(const uint32_t *)(const void *)cmd;
		store32(&fw_fwccb_ctl, 128, (read + 1) & ((1u << FWCCB_LOG2) - 1));
		fwccb_seen++;
		if (type == (103u | CMD_MAGIC)) {       /* ROGUE_FWIF_FWCCB_CMD_FREELIST_GROW: its data is the list's id (pvr_rogue_fwif.h 1332) */
			pvr_free_list_grow_request(*(const uint32_t *)(const void *)(cmd + 8));
			continue;
		}
		if (type == GEN_ROGUE_FWIF_FWCCB_CMD_FREELISTS_RECONSTRUCTION) {
			pvr_free_list_reconstruct_request((const uint32_t *)(const void *)(cmd + 8));
			continue;
		}
		if (type == (109u | CMD_MAGIC) && drm_serving) {         /* REQUEST_GPU_RESTART: process_fwccb_command's pvr_power_reset */
			drm_reset_wanted = "the firmware asked for a restart";
		}
		if (type == GEN_ROGUE_FWIF_FWCCB_CMD_CONTEXT_RESET_NOTIFICATION) {
			drm_context_reset((const uint32_t *)(const void *)(cmd + 8));
			continue;
		}
		SAY("pvr: FWCCB[%u]: %s type %u%s, data %#x %#x %#x %#x", read, (type & 0xffff0000u) == CMD_MAGIC ? "command" : "BAD MAGIC,",
		    type & 0xffffu, (type & 0xffffu) == 109 ? " (REQUEST_GPU_RESTART: not done here)" : (type & 0xffffu) == 107 ? " (UPDATE_STATS)" :
		    (type & 0xffffu) == 105 ? " (CONTEXT_RESET_NOTIFICATION)" : "", *(const uint32_t *)(const void *)(cmd + 8),
		    *(const uint32_t *)(const void *)(cmd + 12), *(const uint32_t *)(const void *)(cmd + 16), *(const uint32_t *)(const void *)(cmd + 20));
	}
}

/* What the interrupt would have had done: pvr_fw_mips.c pvr_mips_irq_pending and pvr_mips_irq_clear, then the firmware's buffer. */
static void
fw_service(void)
{
	for (unsigned n = 0; n < 8 && (rd(rgx, CR_MIPS_WRAPPER_IRQ_STATUS) & 1u) != 0; n++) {
		wr(rgx, CR_MIPS_WRAPPER_IRQ_CLEAR, 1);
		fwccb_process();
	}
	fwccb_process();        /* and once without: a poller loses nothing by looking */
	drm_poll();             /* pvr_device_process_active_queues: jobs done, jobs to start (pvr=6) */
}

/*
 * One command into the kernel's buffer and the kick: pvr_ccb.c's pvr_kccb_send_cmd_powered (the wait
 * for room: a second, pvr_kccb_reserve_slot_sync, 340) and pvr_kccb_send_cmd_reserved_powered (262),
 * pvr_fw.c's pvr_fw_mts_schedule (1076-1085). The slot it went into, or -1. With `slot` the return
 * slot is cleared first, as Linux does only for a caller that will wait.
 */
static int
kccb_send_words(uint32_t type, const uint32_t *data, uint32_t words, bool slot)
{
	uint32_t old = 0, new = 0, cmd[KCCB_CMD_SIZE / 4] = { type, 0 };        /* cmd_type, kccb_flags, then cmd_data (check 268-270) */
	bool room = false;

	for (unsigned ms = 0; ms < KCCB_TIMEOUT_MS && !room; ms++) {
		old = fresh32(fw_kccb_ctl.cpu) & ((1u << KCCB_LOG2) - 1);
		new = (old + 1) & ((1u << KCCB_LOG2) - 1);
		room = fresh32(fw_kccb_ctl.cpu + 128) != new;
		if (!room) {
			delay_for_interval(1, NSEC_PER_MSEC);
		}
	}
	if (!room || words > KCCB_CMD_SIZE / 4 - 2) {
		return -1;
	}
	memcpy(&cmd[2], data, words * 4);
	memcpy(fw_kccb.cpu + old * KCCB_CMD_SIZE, cmd, KCCB_CMD_SIZE);
	FlushPoC_DcacheRegion((vm_offset_t)(fw_kccb.cpu + old * KCCB_CMD_SIZE), KCCB_CMD_SIZE);
	if (slot) {
		store32(&fw_rtn, old * 4, 0);           /* ROGUE_FWIF_KCCB_RTN_SLOT_NO_RESPONSE */
	}
	store32(&fw_kccb_ctl, 0, new);                  /* write_offset */
	wr(rgx, CR_MTS_SCHEDULE, 0);                    /* PVR_FWIF_DM_GP & ~ROGUE_CR_MTS_SCHEDULE_DM_CLRMSK; wr's dsb is Linux's mb() */
	kccb_sent++;
	return (int)old;
}

static int
kccb_send4(uint32_t type, uint32_t data0, uint32_t data1, uint32_t data2, uint32_t data3, bool slot)
{
	const uint32_t data[4] = { data0, data1, data2, data3 };

	return kccb_send_words(type, data, 4, slot);
}

static int
kccb_send(uint32_t type, uint32_t data0, uint32_t data1, bool slot)
{
	return kccb_send4(type, data0, data1, 0, 0, slot);
}

/* A command sent and its return slot polled as pvr_kccb_wait_for_completion waits on it, the interrupt's work done meanwhile. */
static bool
command(const char *name, uint32_t type, uint32_t data0, uint32_t data1)
{
	uint32_t rtn = 0, ms = 0, irq;
	uint64_t began = mach_absolute_time(), ns = 0;
	int slot;

	if (!fw_quiet) {
		SAY("pvr: 6: sending %s (type %#x, data %#x %#x)", name, type, data0, data1);
	}
	slot = kccb_send(type, data0, data1, true);
	if (slot < 0) {
		SAY("pvr: 6: %s NOT SENT: the kernel's buffer stayed full for %u ms (write %u, read %u)", name, KCCB_TIMEOUT_MS,
		    fresh32(fw_kccb_ctl.cpu), fresh32(fw_kccb_ctl.cpu + 128));
		return false;
	}
	/* looked at every 20 us for the first half millisecond (the firmware answers in less), then four times a millisecond */
	for (irq = 0;;) {
		irq |= rd(rgx, CR_MIPS_WRAPPER_IRQ_STATUS);     /* seen at any poll: fw_service clears it */
		rtn = fresh32(fw_rtn.cpu + (uint32_t)slot * 4);
		fw_service();
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		ms = (uint32_t)(ns / NSEC_PER_MSEC);
		if ((rtn & KCCB_RTN_SLOT_CMD_EXECUTED) != 0 || ms >= KCCB_TIMEOUT_MS) {
			break;
		}
		if (ns < 500 * NSEC_PER_USEC) {
			delay(20);
		} else {
			delay_for_interval(250, NSEC_PER_USEC);
		}
	}
	if ((rtn & KCCB_RTN_SLOT_CMD_EXECUTED) == 0) {
		SAY("pvr: 6: %s in slot %d NOT ACKNOWLEDGED in %u ms: return %#x, KCCB write %u read %u, executed %u, IRQ_STATUS seen %#x, "
		    "EXCEPTION_STATUS %#x", name, slot, KCCB_TIMEOUT_MS, rtn, fresh32(fw_kccb_ctl.cpu), fresh32(fw_kccb_ctl.cpu + 128),
		    fresh32(fw_osdata.cpu + 568), irq, rd(rgx, CR_MIPS_EXCEPTION_STATUS));
		return false;
	}
	kccb_acked++;
	last_rtn = rtn;
	if (!fw_quiet) {
		SAY("pvr: 6: %s in slot %d: return %#x after %u us (IRQ_STATUS seen %#x); the firmware counts %u commands executed, %u interrupts",
		    name, slot, rtn, (uint32_t)(ns / NSEC_PER_USEC), irq, fresh32(fw_osdata.cpu + 568), fresh32(fw_osdata.cpu + 560));
	}
	return true;
}

/*
 * The firmware's trace, thread 0's, from its write pointer on as pvr_fw_trace.c reads it (read_fw_trace,
 * fw_trace_get_next, fw_trace_seq_show): an id word (pvr_rogue_fwif_sf.h 57-67: 0x7 on top, the number
 * of parameters in bits 19..16, the group in 15..12, the message in 11..0), two of time (the low 48
 * bits), the parameters. Linux turns the id into text with a table of 779 format strings; that is not
 * carried here, so group, number and parameters are said raw -- look the id up in pvr_rogue_fwif_sf.h.
 */
#define TRACE_SAID 12
static void
fw_trace_say(bool newest)
{
	static const char *const groups[16] = { "NULL", "MAIN", "CLEANUP", "CSW", "PM", "RTD", "SPM", "MTS", "BIF", "MISC", "POW", "HWR",
		                                "HWP", "RPM", "DMA", "DBG" };       /* enum rogue_fw_log_sfgroups */
	const uint32_t *buffer = (const uint32_t *)(const void *)fw_trace0.cpu;
	uint32_t start = fresh32(fw_tracectl.cpu + 8) % TRACE_BUF_DWORDS, said = 0, entries = 0, skip = 0;
#define TRACE_WORD(i) buffer[(start + (i)) % TRACE_BUF_DWORDS]

	FlushPoC_DcacheRegion((vm_offset_t)fw_trace0.cpu, TRACE_BUF_DWORDS * 4);
	SAY("pvr: 6: the firmware's trace (log_type %#x, its write pointer at word %u), the %s entries:", fresh32(fw_tracectl.cpu), start,
	    newest ? "newest" : "oldest");
	for (uint32_t i = 0; newest && i < TRACE_BUF_DWORDS; i++) {      /* how many there are, to say the last of them */
		uint32_t id = TRACE_WORD(i);

		if ((id & 0xfff00000u) == 0x70000000u && (id & 0xffffu) != 0) {
			skip++;
			i += 2 + ((id >> 16) & 0xfu);
		}
	}
	skip = skip > TRACE_SAID ? skip - TRACE_SAID : 0;
	for (uint32_t i = 0; i < TRACE_BUF_DWORDS; i++) {
		uint32_t id = TRACE_WORD(i), parameters = (id >> 16) & 0xfu;

		if ((id & 0xfff00000u) != 0x70000000u || (id & 0xffffu) == 0) {         /* ROGUE_FW_LOG_VALIDID; ROGUE_FW_SF_FIRST */
			continue;
		}
		entries++;
		if (entries > skip && said < TRACE_SAID) {
			said++;
			SAY("pvr:   [%llu] %s %u (id %#x): %#x %#x %#x %#x", (((uint64_t)TRACE_WORD(i + 1) << 32) | TRACE_WORD(i + 2)) & 0xffffffffffffULL,
			    groups[(id >> 12) & 0xfu], id & 0xfffu, id, parameters > 0 ? TRACE_WORD(i + 3) : 0, parameters > 1 ? TRACE_WORD(i + 4) : 0,
			    parameters > 2 ? TRACE_WORD(i + 5) : 0, parameters > 3 ? TRACE_WORD(i + 6) : 0);
		}
		i += 2 + parameters;
	}
#undef TRACE_WORD
	SAY("pvr: 6: %u trace entries in all, %u said", entries, said);
}

/* The watchdog's half second: pvr_watchdog_kccb_stalled. True once the firmware has stopped taking commands. */
static bool
fw_watchdog(void)
{
	static uint32_t old_executed, stalls;
	uint32_t executed = fresh32(fw_osdata.cpu + 568);
	bool idle = fresh32(fw_kccb_ctl.cpu) == fresh32(fw_kccb_ctl.cpu + 128);

	if (executed != old_executed) {
		old_executed = executed;
		stalls = 0;
	} else if (!idle) {
		return ++stalls >= 2;
	} else if (drm_jobs == NULL && kccb_send(KCCB_CMD_HEALTH_CHECK, 0, 0, false) >= 0) {    /* no context is active (no job is out): every time */
		health_checks++;
		kccb_sent--;            /* the stage's count is of its own commands, not the watchdog's */
	}
	return false;
}

/*
 * A health check as Linux sends it: with no return slot (pvr_watchdog_kccb_stalled passes NULL), and
 * known to have been taken only by the firmware's count of commands executed moving on -- the
 * watchdog's own test. Run 4 on the device showed why: this firmware executes one and writes nothing
 * into its return slot (ten executed, the first's slot still 0), so waiting on the slot, which was
 * this file's idea and not Linux's, never ends. What the slot reads afterwards is said all the same.
 */
static bool
health_check(void)
{
	uint32_t before = fresh32(fw_osdata.cpu + 568), now = before, ms;
	int slot;

	SAY("pvr: 6: sending HEALTH_CHECK (type %#x), no return slot asked: its sign is the firmware's count of commands, now %u",
	    KCCB_CMD_HEALTH_CHECK, before);
	slot = kccb_send(KCCB_CMD_HEALTH_CHECK, 0, 0, false);
	for (ms = 0; slot >= 0 && ms < KCCB_TIMEOUT_MS && (now = fresh32(fw_osdata.cpu + 568)) == before; ms++) {
		fw_service();
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	SAY("pvr: 6: HEALTH_CHECK in slot %d: %s after %u polls: the count is %u; KCCB write %u read %u; its return slot reads %#x", slot,
	    now != before ? "taken" : "NOT TAKEN", ms, now, fresh32(fw_kccb_ctl.cpu), fresh32(fw_kccb_ctl.cpu + 128),
	    slot >= 0 ? fresh32(fw_rtn.cpu + (uint32_t)slot * 4) : 0);
	kccb_acked += now != before;
	return now != before;
}

/* Everything the two sides share about commands, raw, and what the firmware counts against itself. */
static void
fw_dump(void)
{
	uint64_t unmapped = rd64(CR_MIPS_ADDR_REMAP_UNMAPPED_STATUS);

	FlushPoC_DcacheRegion((vm_offset_t)fw_rtn.cpu, 16);
	FlushPoC_DcacheRegion((vm_offset_t)fw_kccb_ctl.cpu, CCB_CTL_SIZE);
	FlushPoC_DcacheRegion((vm_offset_t)fw_fwccb_ctl.cpu, CCB_CTL_SIZE);
	SAY("pvr:   return slots 0..3: %#x %#x %#x %#x (at firmware %#x, physical %#llx, page table entry %#x)",
	    *(uint32_t *)(void *)fw_rtn.cpu, *(uint32_t *)(void *)(fw_rtn.cpu + 4), *(uint32_t *)(void *)(fw_rtn.cpu + 8),
	    *(uint32_t *)(void *)(fw_rtn.cpu + 12), fw_rtn.addr, fw_rtn.phys, mips_pt[(fw_rtn.addr & FW_HEAP_MASK) >> 12]);
	SAY("pvr:   KCCB control: write %u read %u wrap %#x size %u; FWCCB control: write %u read %u wrap %#x size %u",
	    *(uint32_t *)(void *)fw_kccb_ctl.cpu, *(uint32_t *)(void *)(fw_kccb_ctl.cpu + 128), *(uint32_t *)(void *)(fw_kccb_ctl.cpu + 132),
	    *(uint32_t *)(void *)(fw_kccb_ctl.cpu + 136), *(uint32_t *)(void *)fw_fwccb_ctl.cpu, *(uint32_t *)(void *)(fw_fwccb_ctl.cpu + 128),
	    *(uint32_t *)(void *)(fw_fwccb_ctl.cpu + 132), *(uint32_t *)(void *)(fw_fwccb_ctl.cpu + 136));
	/* check 46, 51, 122, 72, 71: sysdata's fw_faults and hwr_state_flags, the recovery buffer's hwr_counter, osdata's counts */
	SAY("pvr:   the firmware's own: faults %u, HWR state %#x, HWR count %u, commands executed %u, interrupts %u; UNMAPPED_STATUS %#x_%08x, "
	    "EXCEPTION_STATUS %#x", fresh32(fw_sysdata.cpu + 3536), fresh32(fw_sysdata.cpu + 3576), fresh32(fw_hwrinfobuf.cpu + 2176),
	    fresh32(fw_osdata.cpu + 568), fresh32(fw_osdata.cpu + 560), (uint32_t)(unmapped >> 32), (uint32_t)unmapped,
	    rd(rgx, CR_MIPS_EXCEPTION_STATUS));
}

/* Stage 4: every step taken whatever the one before did. True if every command sent was acknowledged. */
static bool
fw_commands(void)
{
	uint32_t mask = 0x7ffe, idle = 0;       /* every trace group but DEBUG; boot-arg pvr_trace=0 leaves the trace off, as Linux by default */

	FlushPoC_DcacheRegion((vm_offset_t)fw_connection.cpu, 16);
	SAY("pvr: 6: commands. The connection control reads firmware %u, OS %u (Linux v7.3 writes neither: no handshake); "
	    "EXCEPTION_STATUS %#x", *(uint32_t *)(void *)fw_connection.cpu, *(uint32_t *)(void *)(fw_connection.cpu + 4),
	    rd(rgx, CR_MIPS_EXCEPTION_STATUS));
	wr(rgx, CR_MIPS_WRAPPER_IRQ_CLEAR, 1);          /* pvr_device_irq_init clears what booting raised (299) */
	fw_service();
	(void)health_check();
	(void)PE_parse_boot_argn("pvr_trace", &mask, sizeof(mask));
	mask &= LOG_TYPE_GROUP_MASK;
	fw_trace_mask = mask;
	if (mask != 0) {
		store32(&fw_tracectl, 0, mask | LOG_TYPE_TRACE);        /* build_log_type */
		(void)command("LOGTYPE_UPDATE", KCCB_CMD_LOGTYPE_UPDATE, 0, 0);       /* this one Linux does wait for in its slot */
		(void)health_check();
		fw_trace_say(false);
	}
	(void)PE_parse_boot_argn("pvr_idle", &idle, sizeof(idle));
	if (idle != 0) {
		/* pvr_power.c's pvr_power_request_idle through pvr_power_send_command (48-64): answered in the power sync, not the slot */
		uint32_t answer = 0, ms;

		SAY("pvr: 6: pvr_idle=1: POW FORCED_IDLE_REQ, the first half of Linux's power-off; nothing in v7.3 undoes it short of a restart");
		store32(&fw_power_sync, 0, 0);
		if (kccb_send(KCCB_CMD_POW, POW_FORCED_IDLE_REQ, POWER_FORCE_IDLE, true) >= 0) {
			for (ms = 0; ms < 1000 && (answer = fresh32(fw_power_sync.cpu)) == 0; ms++) {   /* POWER_SYNC_TIMEOUT_US */
				fw_service();
				delay_for_interval(1, NSEC_PER_MSEC);
			}
			kccb_acked += answer != 0;
			SAY("pvr: 6: POW in the power sync: %#x after %u ms%s; sysdata's pow_state %u", answer, ms, answer != 0 ? "" : " -- NOT ANSWERED",
			    fresh32(fw_sysdata.cpu + 8));
		}
	}
	fw_dump();
	return kccb_sent == kccb_acked;
}

/*
 * ---- the GPU's memory, a context, a free list, a render target (pvr=5) ----
 *
 * Linux v7.3 again: pvr_mmu.c, pvr_vm.c, pvr_context.c with pvr_queue.c and pvr_cccb.c, pvr_free_list.c,
 * pvr_hwrt.c. One thing first, because it decides what "acknowledged" can mean here: in v7.3 none of
 * these objects is created by a command. A VM context, a render context, a free list and a render
 * target's data are structures written into firmware memory, which the firmware first reads when a
 * job naming them is kicked. The commands of this stage are the MMU cache flush (pvr_mmu_flush_exec)
 * and, taking things down again, the cleanup requests (pvr_fw.c pvr_fw_structure_cleanup) -- the
 * latter being the only time before a job that the firmware looks at these structures at all, which
 * is what boot-arg pvr_teardown=1 is for.
 *
 * The functions below take what the ioctls of pvr_drv.c take, in the kernel's terms -- a VM, GPU
 * addresses, physical ranges, counts -- so that a user client can be put over them as RedwoodMali.cpp
 * is over redwood_mali.c: pvr_vm_create/destroy (CREATE_VM_CONTEXT), pvr_vm_map/unmap (VM_MAP, with
 * the physical segments of a buffer the client owns: CREATE_BO is the client's own
 * IOBufferMemoryDescriptor), pvr_context_create/destroy, pvr_free_list_create/destroy,
 * pvr_hwrt_create/destroy, and pvr_heaps and pvr_static_areas for DEV_QUERY. What they are NOT yet:
 * safe from two threads (one lock around the lot will do to begin with), and Linux's bookkeeping of
 * which buffer is mapped where (pvr_vm_find_gem_object), which here is the caller's.
 *
 * Left out of Linux, on purpose: the scheduler and fences of pvr_queue.c (nothing is submitted yet),
 * the static context state a render context gets from user space through pvr_stream.c (zero here;
 * Mesa's goes in when Mesa is above), page tables freed as they empty (they go with the VM), GPU
 * pages other than 4 KiB, and runtime power management.
 *
 * The arena stays the kernel's cached pages with the flushes of fresh32 and store32: still only one
 * slotted command is ever outstanding, the GPU's own page tables are written by this side alone and
 * flushed before the command that makes the GPU look at them, and four device runs stand behind it.
 * Boot-arg pvr_wc=1 uses a second, write-combined mapping of the same memory instead (ml_io_map_wcomb,
 * as MiniIOMobileFramebuffer.cpp maps its second picture, 510) -- what jobs with many slots out at
 * once will want; it is not the default because two mappings of one page with different cache
 * attributes have not been tried on this kernel.
 */
#define KCCB_CMD_MMUCACHE               (102u | CMD_MAGIC)      /* pvr_rogue_fwif.h 1157 */
#define KCCB_CMD_CLEANUP                (106u | CMD_MAGIC)      /* 1166 */
#define KCCB_CMD_FREELIST_GROW_UPDATE   (110u | CMD_MAGIC)      /* 1176 */
#define KCCB_RTN_SLOT_CLEANUP_BUSY      2u                      /* 1771 */
enum { CLEANUP_FWCOMMONCONTEXT, CLEANUP_HWRTDATA, CLEANUP_FREELIST };                           /* 902-908: enum rogue_fwif_cleanup_type */

/* include/uapi/drm/pvr_drm.h: the buffer flags a mapping is made with (536-537), the heaps (267-277), the static areas (344-383) */
#define PVR_BO_BYPASS_DEVICE_CACHE      1ull
#define PVR_BO_PM_FW_PROTECT            2ull
enum { PVR_HEAP_GENERAL, PVR_HEAP_PDS_CODE_DATA, PVR_HEAP_USC_CODE, PVR_HEAP_RGNHDR, PVR_HEAP_VIS_TEST, PVR_HEAP_TRANSFER_FRAG, PVR_HEAPS };
enum { PVR_AREA_EOT, PVR_AREA_FENCE, PVR_AREA_VDM_SYNC, PVR_AREA_YUV_CSC };

/* pvr_vm.c's pvr_heaps (947-984) from pvr_rogue_heap_config.h; the region headers' only with BRN 63142 (pvr_find_heap_containing, 1110) */
static const struct pvr_heap {
	const char *name;
	uint64_t base, size;
} pvr_heaps[PVR_HEAPS] = {
	{ "general", 0x8000000000ULL, 128ULL << 30 },
	{ "PDS code and data", PDSCODEDATA_HEAP_BASE, 4ULL << 30 },
	{ "USC code", USCCODE_HEAP_BASE, 4ULL << 30 },
	{ "region headers", 0xDBF0000000ULL, 256ULL << 20 },
	{ "visibility test", 0xF200000000ULL, 2ULL << 20 },
	{ "transfer fragment", 0xE400000000ULL, 16ULL << 30 },
};
#define BRN_63142       (1ULL << 8)

/* pvr_vm.c's static_data_areas (912-943): places at the bottom of two heaps that user space fills and DEV_QUERY tells it of; the kernel keeps clear of them */
static const struct pvr_static_area {
	uint16_t usage, heap;
	uint32_t offset, size;
} pvr_static_areas[5] = {
	{ PVR_AREA_FENCE, PVR_HEAP_GENERAL, 0, 128 },
	{ PVR_AREA_YUV_CSC, PVR_HEAP_GENERAL, 128, 1024 },
	{ PVR_AREA_VDM_SYNC, PVR_HEAP_PDS_CODE_DATA, 0, 128 },
	{ PVR_AREA_EOT, PVR_HEAP_PDS_CODE_DATA, 128, 128 },
	{ PVR_AREA_VDM_SYNC, PVR_HEAP_USC_CODE, 0, 128 },
};


/*
 * pvr_mmu.c's pvr_mmu_flush_exec (125): the firmware told which of the GPU's cached translations to
 * drop, answered in its slot. Linux resets the GPU if it is not; this says so.
 */
static bool
pvr_mmu_flush(void)
{
	uint32_t flags = mmu_flush_flags, covers = drm_unmap_seq;
	uint64_t began, ns = 0;
	bool done;

	if (flags == 0) {
		drm_flushed_seq = covers;       /* nothing has been unmapped that the GPU could still hold */
		return true;
	}
	mmu_flush_flags = 0;
	began = mach_absolute_time();
	done = command("MMUCACHE", KCCB_CMD_MMUCACHE, flags, fw_mmucache_sync.addr);    /* cache_flags, mmu_cache_sync_fw_addr, ..._update_value 0 */
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
	mmu_flushes++;
	mmu_flush_us += (uint32_t)(ns / NSEC_PER_USEC);
	if (ns / NSEC_PER_USEC > mmu_flush_most) {
		mmu_flush_most = (uint32_t)(ns / NSEC_PER_USEC);
	}
	if (done) {
		drm_flushed_seq = covers;       /* the unmappings before it: their buffers' memory may be used again (drm_reap) */
	}
	return done;
}

/* pvr_fw.c's pvr_fw_structure_cleanup (1101): 0 done, 1 the firmware says it is still in use, -1 no answer */
static int
pvr_cleanup(const char *what, uint32_t type, uint32_t fw_addr)
{
	if (drm_lost) {
		return 0;               /* nobody to ask, and nobody left to be confused by it */
	}
	if (!command(what, KCCB_CMD_CLEANUP, type, fw_addr)) {
		return -1;
	}
	return (last_rtn & KCCB_RTN_SLOT_CLEANUP_BUSY) != 0;
}

/*
 * The GPU's page tables, pvr_mmu.c with pvr_rogue_mmu_defs.h: 40 bits of address; a page catalogue of
 * 1024 4-byte entries (bits 39..30), page directories of 512 8-byte entries (29..21), page tables of
 * 512 (20..12), each one 4 KiB page of the arena (pvr_mmu_backing_page_init), zero until used.
 */
struct pvr_vm {
	uint32_t *pc;
	uint64_t pc_phys;
	struct fw_obj memctx;           /* struct rogue_fwif_fwmemcontext: what a context names its address space by */
};
#define FWMEMCONTEXT_SIZE       32      /* check 138: pc_dev_paddr 0 (132), page_cat_base_reg_set 8 (133) */

static uint64_t *
mmu_table(uint64_t phys)
{
	return (uint64_t *)(void *)((uint8_t *)arena + (phys - arena_phys));
}

static uint64_t
mmu_page(void)          /* a zeroed table page; its physical address, 0 if there is none */
{
	int page = arena_alloc(1);

	if (page < 0) {
		static bool said;

		if (!said) {
			said = true;
			SAY("pvr: NO MEMORY LEFT FOR A GPU PAGE TABLE: %#x of the arena's %#x used; mappings fail from here on (said once)", arena_used, ARENA_SIZE);
		}
		return 0;
	}
	memset((uint8_t *)arena + ((uint32_t)page << 12), 0, 4096);
	FlushPoC_DcacheRegion(arena + ((uint32_t)page << 12), 4096);
	pt_pages++;
	return arena_phys + ((uint64_t)(uint32_t)page << 12);
}

/* The page table entry for `addr`, the tables above it made if `make`; NULL if there is none (or no memory). */
static uint64_t *
mmu_entry(struct pvr_vm *vm, uint64_t addr, bool make)
{
	uint32_t *pce = &vm->pc[(addr >> 30) & 0x3ffu];
	uint64_t *pde, phys;

	if ((*pce & 1u) == 0) {
		if (!make || (phys = mmu_page()) == 0) {
			return NULL;
		}
		*pce = (uint32_t)((phys >> 12) << 4) | 1u;      /* pvr_page_table_l2_entry_raw_set: PD_BASE (bits 31..4 of the address over 4 KiB), VALID */
		FlushPoC_DcacheRegion((vm_offset_t)pce, 4);
		mmu_flush_flags |= MMU_SYNC_2;
	}
	pde = &mmu_table((uint64_t)(*pce >> 4) << 12)[(addr >> 21) & 0x1ffu];
	if ((*pde & 1u) == 0) {
		if (!make || (phys = mmu_page()) == 0) {
			return NULL;
		}
		*pde = (phys & 0xFFFFFFF000ULL) | 1u;           /* pvr_page_table_l1_entry_raw_set: PT_BASE, PAGE_SIZE_4KB (0), VALID */
		FlushPoC_DcacheRegion((vm_offset_t)pde, 8);
		mmu_flush_flags |= MMU_SYNC_1;
	}
	return &mmu_table(*pde & 0xFFFFFFF000ULL)[(addr >> 12) & 0x1ffu];
}

/* pvr_vm_unmap, pvr_mmu_unmap: whatever of the range is mapped, unmapped. The flush is the caller's, as in Linux. */
static void
pvr_vm_unmap(struct pvr_vm *vm, uint64_t addr, uint64_t size)
{
	for (uint64_t at = addr; at < addr + size; at += 4096) {
		uint64_t *pte = mmu_entry(vm, at, false);

		if (pte != NULL && (*pte & 1u) != 0) {
			*pte = 0;
			FlushPoC_DcacheRegion((vm_offset_t)pte, 8);
			mmu_flush_flags |= MMU_SYNC_0;
		}
	}
}

/*
 * pvr_vm.c's pvr_vm_map with pvr_vm_bind_op_map_init's checks, and pvr_mmu.c's pvr_mmu_map and
 * pvr_page_create: `size` bytes of physical memory at `phys` to appear at GPU address `addr`, which
 * must lie in one heap and be free. `flags` are the buffer's: PVR_BO_BYPASS_DEVICE_CACHE, PVR_BO_PM_FW_PROTECT.
 * 0, or EINVAL, EEXIST, ENOMEM with nothing left mapped.
 */
static int
pvr_vm_map(struct pvr_vm *vm, uint64_t addr, uint64_t phys, uint64_t size, uint64_t flags)
{
	uint64_t bits = 1u | ((flags & PVR_BO_BYPASS_DEVICE_CACHE) ? 8u : 0) | ((flags & PVR_BO_PM_FW_PROTECT) ? 1ULL << 62 : 0);
	int error = EINVAL;

	/* pvr_page_flags_raw_create: SLC_BYPASS_CTRL bit 3, PM_META_PROTECT bit 62; never read-only or cache-coherent */
	if (size == 0 || ((addr | phys | size) & 0xfffu) != 0 || addr + size < addr || (phys + size) >> 40 != 0) {
		return EINVAL;
	}
	for (unsigned heap = 0; heap < PVR_HEAPS && error != 0; heap++) {
		if ((heap != PVR_HEAP_RGNHDR || (fw_quirks & BRN_63142) != 0) && addr >= pvr_heaps[heap].base &&
		    addr + size <= pvr_heaps[heap].base + pvr_heaps[heap].size) {
			error = 0;
		}
	}
	for (uint64_t done = 0; error == 0 && done < size; done += 4096) {
		uint64_t *pte = mmu_entry(vm, addr + done, true);

		if (pte == NULL || (*pte & 1u) != 0) {
			error = pte == NULL ? ENOMEM : EEXIST;
			pvr_vm_unmap(vm, addr, done);
			break;
		}
		*pte = ((phys + done) & 0xFFFFFFF000ULL) | bits;        /* pvr_page_table_l0_entry_raw_set */
		FlushPoC_DcacheRegion((vm_offset_t)pte, 8);
		mmu_flush_flags |= MMU_SYNC_0;
	}
	return error;
}

/* fw_mem_context_init (520): at creation, and again after a reset */
static void
pvr_vm_fill(struct pvr_vm *vm)
{
	put64(&vm->memctx, 0, vm->pc_phys);             /* pc_dev_paddr */
	put32(&vm->memctx, 8, 0xFFFFFFFFu);             /* page_cat_base_reg_set: ROGUE_FW_BIF_INVALID_PCSET (595) */
	FlushPoC_DcacheRegion((vm_offset_t)vm->memctx.cpu, FWMEMCONTEXT_SIZE);
}

/* pvr_vm_create_context for user space (545) with pvr_mmu_context_create and fw_mem_context_init (520). False if there is no memory. */
static bool
pvr_vm_create(struct pvr_vm *vm)
{
	if (feature_value[F_VIRTUAL_ADDRESS_SPACE_BITS] != 40 || (vm->pc_phys = mmu_page()) == 0) {
		return false;
	}
	vm->pc = (uint32_t *)(void *)mmu_table(vm->pc_phys);
	vm->memctx = fw_object("a memory context", FWMEMCONTEXT_SIZE, FW_ANYWHERE);
	if (vm->memctx.cpu == NULL) {
		arena_free((uint32_t)((vm->pc_phys - arena_phys) >> 12), 1);
		return false;
	}
	pvr_vm_fill(vm);
	return true;
}

/* pvr_vm_context_release: the firmware's object, then every table. Whatever is still mapped goes with them. */
static void
pvr_vm_destroy(struct pvr_vm *vm)
{
	fw_object_free(&vm->memctx);
	for (unsigned i = 0; i < 1024; i++) {
		if ((vm->pc[i] & 1u) != 0) {
			uint64_t pd_phys = (uint64_t)(vm->pc[i] >> 4) << 12, *pd = mmu_table(pd_phys);

			for (unsigned j = 0; j < 512; j++) {
				if ((pd[j] & 1u) != 0) {
					arena_free((uint32_t)(((pd[j] & 0xFFFFFFF000ULL) - arena_phys) >> 12), 1);
					pt_pages--;
				}
			}
			arena_free((uint32_t)((pd_phys - arena_phys) >> 12), 1);
			pt_pages--;
		}
	}
	arena_free((uint32_t)((vm->pc_phys - arena_phys) >> 12), 1);
	pt_pages--;
	mmu_flush_flags |= MMU_SYNC_2;
	vm->pc = NULL;
}

/*
 * A context: pvr_context.c's pvr_context_create (282) -- render, compute or transfer -- and for each
 * of its queues (a render context has two: geometry, fragment) pvr_queue.c's pvr_queue_create (1258)
 * less Linux's scheduler: a client command buffer with its control (pvr_cccb.c pvr_cccb_init,
 * cccb_ctrl_init), the register state the firmware keeps across a context switch (reg_state_init), a
 * timeline word the firmware writes a job's number into when it is done, and the queue's common
 * context filled as init_fw_context (1087) fills it inside the context's firmware structure. The
 * static state user space gives a render or compute context is decoded into it by the caller.
 */
#define FWCOMMONCONTEXT_SIZE    152     /* check 185, the offsets written below 158-183 */
#define CCCB_CTL_SIZE           32      /* pvr_rogue_fwif_shared_check.h 28: read_offset 4 (22), wrap_mask 12 (24) */
#define CCCB_SIZE_LOG2          15      /* pvr_queue.c 22-25 */
#define FRAG_CTX_STATE_SIZE(stores) (12 + 4 * (stores))        /* check 152, pvr_rogue_fwif.h 658; pvr_queue.c get_frag_ctx_state_size */
#define MAX_DEADLINE_MS         30000   /* pvr_queue.c 19 */
#define PVR_FWIF_DM_GEOM        2       /* pvr_rogue_fwif_common.h 33 */
#define PVR_FWIF_DM_FRAG        3       /* 34 */
#define PVR_FWIF_DM_CDM         4       /* 35 */
#define PVR_CTX_PRIORITY_MEDIUM 1       /* pvr_context.h 25: what DRM_PVR_CTX_PRIORITY_NORMAL becomes; LOW 0, HIGH 2 */
enum { PVR_CTX_RENDER, PVR_CTX_COMPUTE, PVR_CTX_TRANSFER_FRAG };        /* enum drm_pvr_ctx_type */

struct drm_job;
struct pvr_context {
	struct pvr_vm *vm;
	uint32_t id, type, queues, priority, pid;
	uint64_t callstack_addr;
	bool faulty;                    /* pvr_context.faulty: a job of its was lost; no more are taken */
	uint8_t static_state[stream_static_render_DEST_SIZE];   /* as decoded; a compute context's is shorter */
	struct fw_obj fw;               /* rogue_fwif_fwrendercontext, ..computecontext or ..transfercontext */
	struct pvr_queue {
		struct fw_obj cccb_ctl, cccb, state, timeline;
		uint32_t at;            /* of its common context in `fw` */
		uint32_t write;         /* pvr_cccb.write_offset: where the next command goes */
		uint32_t seqno;         /* the last job number given out on its timeline */
		struct drm_job *first, *last;   /* jobs not yet in the command buffer, oldest first (pvr=6) */
	} queue[2];
};
static uint32_t pvr_context_ids = 1;    /* XA_FLAGS_ALLOC1 (pvr_context.c 477): from 1 */

static void
pvr_context_free(struct pvr_context *ctx)
{
	for (unsigned q = 0; q < 2; q++) {
		fw_object_free(&ctx->queue[q].timeline);
		fw_object_free(&ctx->queue[q].state);
		fw_object_free(&ctx->queue[q].cccb);
		fw_object_free(&ctx->queue[q].cccb_ctl);
	}
	fw_object_free(&ctx->fw);
}

/* init_fw_context and reg_state_init for each queue, and the static state: at creation, and again after a reset has zeroed everything */
static void
pvr_context_fill(struct pvr_context *ctx)
{
	if (ctx->type == PVR_CTX_RENDER) {
		put64(&ctx->queue[0].state, 8, ctx->callstack_addr);    /* geom_core[0].geom_reg_vdm_call_stack_pointer_init (check 141) */
	}
	for (unsigned q = 0; q < ctx->queues; q++) {
		uint32_t at = ctx->queue[q].at;
		uint32_t dm = ctx->type == PVR_CTX_COMPUTE ? PVR_FWIF_DM_CDM : ctx->type == PVR_CTX_RENDER && q == 0 ? PVR_FWIF_DM_GEOM : PVR_FWIF_DM_FRAG;

		put32(&ctx->queue[q].cccb_ctl, 12, (1u << CCCB_SIZE_LOG2) - 1);         /* wrap_mask */
		put32(&ctx->fw, at + 0, ctx->queue[q].cccb_ctl.addr);                   /* ccbctl_fw_addr */
		put32(&ctx->fw, at + 4, ctx->queue[q].cccb.addr);                       /* ccb_fw_addr */
		put32(&ctx->fw, at + 24, ctx->queue[q].state.addr);                     /* context_state_addr */
		put32(&ctx->fw, at + 32, ctx->priority);                                     /* priority; priority_seq_num (36) 0 */
		put32(&ctx->fw, at + 60, dm);                                           /* dm (get_dm_type) */
		put32(&ctx->fw, at + 104, MAX_DEADLINE_MS);                             /* max_deadline_ms */
		put32(&ctx->fw, at + 136, ctx->vm->memctx.addr);                             /* fw_mem_context_fw_addr */
		put32(&ctx->fw, at + 140, ctx->id);                                     /* server_common_context_id */
		put32(&ctx->fw, at + 144, ctx->pid);                                         /* pid */
		FlushPoC_DcacheRegion((vm_offset_t)ctx->queue[q].cccb_ctl.cpu, CCCB_CTL_SIZE);
		FlushPoC_DcacheRegion((vm_offset_t)ctx->queue[q].state.cpu, 16);
		ctx->queue[q].write = 0;
		ctx->queue[q].seqno = 0;
	}
	if (ctx->type == PVR_CTX_RENDER) {
		memcpy(ctx->fw.cpu + GEN_rogue_fwif_fwrendercontext__static_render_context_state_ctxswitch_regs, ctx->static_state,
		    stream_static_render_DEST_SIZE);
	} else if (ctx->type == PVR_CTX_COMPUTE) {
		memcpy(ctx->fw.cpu + GEN_rogue_fwif_fwcomputecontext__static_compute_context_state_ctxswitch_regs, ctx->static_state,
		    stream_static_compute_DEST_SIZE);
	}
	FlushPoC_DcacheRegion((vm_offset_t)ctx->fw.cpu, ctx->fw.bytes);
}

static bool
pvr_context_create(struct pvr_context *ctx, struct pvr_vm *vm, uint32_t type, uint32_t priority, uint64_t callstack_addr, uint32_t pid)
{
	/* get_frag_ctx_state_size and get_xfer_ctx_state_size */
	uint32_t stores = (uint32_t)(!feature_has[F_XE_MEMORY_HIERARCHY] ? feature_value[F_NUM_ISP_IPP_PIPES] :
	    type == PVR_CTX_RENDER ? feature_value[F_NUM_RASTER_PIPES] : 1);
	uint32_t size = type == PVR_CTX_RENDER ? GEN_SIZEOF_rogue_fwif_fwrendercontext : type == PVR_CTX_COMPUTE ?
	    GEN_SIZEOF_rogue_fwif_fwcomputecontext : GEN_SIZEOF_rogue_fwif_fwtransfercontext;
	bool made = true;

	memset(ctx, 0, sizeof(*ctx));
	if (type > PVR_CTX_TRANSFER_FRAG || (type == PVR_CTX_RENDER && feature_has[F_XE_MEMORY_HIERARCHY] && feature_has[F_GPU_MULTICORE_SUPPORT])) {
		return false;           /* get_frag_ctx_state_size's multicore case: not this core's */
	}
	ctx->vm = vm;
	ctx->type = type;
	ctx->queues = type == PVR_CTX_RENDER ? 2 : 1;
	ctx->id = pvr_context_ids++;
	ctx->queue[1].at = GEN_rogue_fwif_fwrendercontext__frag_context;
	for (unsigned q = 0; q < ctx->queues; q++) {
		uint32_t state = type == PVR_CTX_COMPUTE ? GEN_SIZEOF_rogue_fwif_compute_ctx_state : type == PVR_CTX_RENDER && q == 0 ?
		    GEN_SIZEOF_rogue_fwif_geom_ctx_state : FRAG_CTX_STATE_SIZE(stores);

		ctx->queue[q].cccb_ctl = fw_object("a client CCB's control", CCCB_CTL_SIZE, FW_ANYWHERE);
		ctx->queue[q].cccb = fw_object("a client CCB", 1u << CCCB_SIZE_LOG2, FW_ANYWHERE);
		ctx->queue[q].state = fw_object("a context's register state", state, FW_ANYWHERE);
		ctx->queue[q].timeline = fw_object("a timeline", 4, FW_ANYWHERE);
		made = made && ctx->queue[q].cccb_ctl.cpu != NULL && ctx->queue[q].cccb.cpu != NULL && ctx->queue[q].state.cpu != NULL &&
		    ctx->queue[q].timeline.cpu != NULL;
	}
	ctx->fw = fw_object("a context", size, FW_ANYWHERE);
	if (!made || ctx->fw.cpu == NULL) {
		pvr_context_free(ctx);
		return false;
	}
	ctx->priority = priority;
	ctx->callstack_addr = callstack_addr;
	ctx->pid = pid;
	pvr_context_fill(ctx);
	return true;
}

/*
 * pvr_context_release through pvr_queue_destroy: the firmware asked to let go of each common context,
 * the last queue first, then the memory. A context the firmware calls busy is asked again, four
 * times a millisecond for 20 ms (Linux asks once; this holds everybody's lock meanwhile, so not
 * the second it was until a window server's frames showed what that costs); one it will not let go
 * of is left where it is, and false said.
 */
static bool
pvr_context_destroy(struct pvr_context *ctx)
{
	bool clean = true;

	for (int q = (int)ctx->queues - 1; q >= 0; q--) {
		int busy = 1;

		for (unsigned tries = 0; tries < CLEANUP_TRIES && busy == 1; tries++) {
			busy = pvr_cleanup(q == 0 && ctx->type == PVR_CTX_RENDER ? "CLEANUP of the geometry context" : ctx->type == PVR_CTX_RENDER ?
			    "CLEANUP of the fragment context" : "CLEANUP of the context", CLEANUP_FWCOMMONCONTEXT, ctx->fw.addr + ctx->queue[q].at);
			if (busy == 1) {
				cleanup_busy++;
				delay_for_interval(250, NSEC_PER_USEC);
			}
		}
		clean = clean && busy == 0;
	}
	if (clean) {
		pvr_context_free(ctx);
	}
	return clean;
}

/*
 * A free list, pvr_free_list.c: the pages the GPU's parameter manager takes its memory from, listed
 * as page numbers, 4 bytes each, filled from the top down in a buffer that the caller has mapped
 * (with PVR_BO_PM_FW_PROTECT) at `list_gpu_addr`; and struct rogue_fwif_freelist telling the firmware
 * of it. Each growth's pages are one piece of memory (Linux: a GEM object, pvr_free_list_node): the
 * buffer provider's if there is one (pvr=6), else the arena's; all given back when the list goes.
 */
#define FREELIST_SIZE           64      /* check 434, the offsets below 421-431 */
#define FREE_LIST_ALIGNMENT     3u      /* pvr_free_list.c 18: counts are multiples of 16 bytes of entries (pvr_rogue_defs.h 74) */
#define FREE_LIST_NODES         64      /* ponytail: growths a list can have here; Linux's list of nodes has no end */
#define FREE_LIST_HWRTS         16      /* render target datas using a list, for its reconstruction */
#define FREE_LISTS              256

struct redwood_pvr_bo_ops;
static const struct redwood_pvr_bo_ops *bo_ops;         /* iokit/Kernel/RedwoodPVR.cpp's, once it has started */
static void *bo_make(uint64_t bytes);
static void bo_drop(void *memory);
static uint64_t bo_phys(void *memory, uint64_t offset);

struct pvr_free_list {
	struct fw_obj fw;
	uint32_t *list;                 /* the buffer's kernel mapping */
	uint64_t list_gpu_addr;
	uint32_t id, max_pages, grow_pages, grow_threshold, initial_pages, current_pages, ready_pages;
	struct {
		void *memory;           /* the provider's, or NULL: `first` is a page of the arena */
		uint32_t first, pages;
	} node[FREE_LIST_NODES];
	uint32_t nodes;
	struct fw_obj *hwrt[FREE_LIST_HWRTS];
};
static struct pvr_free_list *pvr_free_lists[FREE_LISTS];
static uint32_t free_list_said;        /* by id - 1: pvr_dev->free_list_ids, from 1 (pvr_drv.c 1446) */

static uint32_t
free_list_ready(const struct pvr_free_list *fl, uint32_t pages)         /* calculate_free_list_ready_pages_locked */
{
	uint32_t ready = pages * fl->grow_threshold / 100;

	return (ready < fl->grow_pages ? ready : fl->grow_pages) & ~FREE_LIST_ALIGNMENT;
}

/* pvr_free_list_insert_node_locked (270) with pvr_free_list_insert_pages_locked: a node's page numbers below those already there */
static void
free_list_insert(struct pvr_free_list *fl, unsigned n)
{
	uint32_t pages = fl->node[n].pages, start = ((fl->max_pages - fl->current_pages - pages) * 4 & ~15u) / 4;

	for (uint32_t i = 0; i < pages; i++) {
		fl->list[start + i] = fl->node[n].memory != NULL ? (uint32_t)(bo_phys(fl->node[n].memory, (uint64_t)i << 12) >> 12) :
		    (uint32_t)((arena_phys >> 12) + fl->node[n].first + i);     /* ROGUE_BIF_PM_PHYSICAL_PAGE_ALIGNSHIFT 12 */
	}
	FlushPoC_DcacheRegion((vm_offset_t)&fl->list[start], pages * 4);
	fl->current_pages += pages;
}

/* pvr_free_list_grow (298) */
static bool
pvr_free_list_grow(struct pvr_free_list *fl, uint32_t pages)
{
	unsigned n = fl->nodes;
	int first = -1;

	if ((pages & FREE_LIST_ALIGNMENT) != 0 || pages == 0 || fl->current_pages + pages > fl->max_pages || n == FREE_LIST_NODES) {
		return false;
	}
	fl->node[n].memory = bo_ops != NULL ? bo_make((uint64_t)pages << 12) : NULL;
	if (fl->node[n].memory == NULL && (bo_ops != NULL || (first = arena_alloc(pages)) < 0)) {
		return false;
	}
	fl->node[n].first = (uint32_t)first;
	fl->node[n].pages = pages;
	fl->nodes++;
	free_list_insert(fl, n);
	fl->ready_pages = free_list_ready(fl, fl->current_pages);
	fl->current_pages -= fl->ready_pages;
	return true;
}

static void
free_list_drop_nodes(struct pvr_free_list *fl)
{
	for (unsigned n = 0; n < fl->nodes; n++) {
		if (fl->node[n].memory != NULL) {
			bo_drop(fl->node[n].memory);
		} else {
			arena_free(fl->node[n].first, fl->node[n].pages);
		}
	}
	fl->nodes = 0;
}

/* free_list_fw_init (170): the firmware's structure for a list with `current` pages in use and `ready` held back */
static void
free_list_fw_fill(struct pvr_free_list *fl, uint32_t current, uint32_t ready)
{
	put64(&fl->fw, 0, fl->list_gpu_addr);                                   /* freelist_dev_addr */
	put64(&fl->fw, 8, (fl->list_gpu_addr + (uint64_t)(fl->max_pages - current) * 4) & ~15ULL);     /* current_dev_addr */
	put32(&fl->fw, 16, current - 1);                                        /* current_stack_top */
	put32(&fl->fw, 20, fl->max_pages);                                      /* max_pages */
	put32(&fl->fw, 24, fl->grow_pages);                                     /* grow_pages */
	put32(&fl->fw, 28, current);                                            /* current_pages */
	put32(&fl->fw, 40, fl->id);                                             /* freelist_id */
	put32(&fl->fw, 48, ready);                                              /* ready_pages; grow_pending (44) false */
	FlushPoC_DcacheRegion((vm_offset_t)fl->fw.cpu, FREELIST_SIZE);
}

/* pvr_free_list_create (412) with free_list_create_kernel_structure's rules and free_list_fw_init (170) */
static bool
pvr_free_list_create(struct pvr_free_list *fl, uint32_t *list, uint64_t list_gpu_addr, uint32_t initial_pages, uint32_t max_pages,
    uint32_t grow_pages, uint32_t grow_threshold)
{
	uint32_t id;

	memset(fl, 0, sizeof(*fl));
	for (id = 0; id < FREE_LISTS && pvr_free_lists[id] != NULL; id++) {
	}
	if (id == FREE_LISTS || grow_threshold > 100 || initial_pages > max_pages || grow_pages > max_pages || max_pages == 0 ||
	    (initial_pages < max_pages && grow_pages == 0) || (initial_pages == max_pages && grow_pages != 0) ||
	    ((initial_pages | max_pages | grow_pages) & FREE_LIST_ALIGNMENT) != 0) {
		return false;
	}
	fl->list = list;
	fl->list_gpu_addr = list_gpu_addr;
	fl->max_pages = max_pages;
	fl->grow_pages = grow_pages;
	fl->grow_threshold = grow_threshold;
	fl->initial_pages = initial_pages;
	fl->id = id + 1;
	fl->fw = fw_object("a free list", FREELIST_SIZE, FW_ANYWHERE);
	if (fl->fw.cpu == NULL) {
		return false;
	}
	free_list_fw_fill(fl, initial_pages - free_list_ready(fl, initial_pages), free_list_ready(fl, initial_pages));
	if (!pvr_free_list_grow(fl, initial_pages)) {
		fw_object_free(&fl->fw);
		return false;
	}
	pvr_free_lists[id] = fl;
	return true;
}

/*
 * The firmware asking for more (FWCCB FREELIST_GROW): pvr_free_list_process_grow_req (360). The answer
 * goes without a return slot, as Linux's.
 */
static void
pvr_free_list_grow_request(uint32_t id)
{
	struct pvr_free_list *fl = id >= 1 && id <= FREE_LISTS ? pvr_free_lists[id - 1] : NULL;
	uint32_t grown = 0;

	if (fl == NULL) {
		SAY("pvr: the firmware asks free list %u, which is not one of ours, to grow", id);
		return;
	}
	fl->current_pages += fl->ready_pages;
	fl->ready_pages = 0;
	if (pvr_free_list_grow(fl, fl->grow_pages)) {
		grown = fl->grow_pages;
	}
	if (++free_list_said <= 24 || (free_list_said & 63) == 0 || grown == 0) {
		SAY("pvr: free list %u %s %u pages at the firmware's asking: %u of at most %u now, %u held ready, in %u pieces (%u such lines so far)", id,
		    grown != 0 ? "grown by" : "COULD NOT GROW by", fl->grow_pages, fl->current_pages, fl->max_pages, fl->ready_pages, fl->nodes, free_list_said);
	}
	(void)kccb_send4(KCCB_CMD_FREELIST_GROW_UPDATE, fl->fw.addr, grown, fl->current_pages + fl->ready_pages, fl->ready_pages, false);
	kccb_sent--;            /* not one of a stage's own */
}

/*
 * The firmware having lost track of its lists after a hardware recovery (FWCCB
 * FREELISTS_RECONSTRUCTION): pvr_free_list_process_reconstruct_req and pvr_free_list_reconstruct --
 * each list named filled again from its nodes, its firmware counts reset, the render targets on it
 * marked as recovered; then the same ids sent back.
 */
static void
pvr_free_list_reconstruct_request(const uint32_t *request)      /* freelist_count, then (at 8 bytes) freelist_ids[] */
{
	uint32_t words[1 + 1 + GEN_ROGUE_FWIF_MAX_FREELISTS_TO_RECONSTRUCT] = { 0 };
	uint32_t count = request[0] < GEN_ROGUE_FWIF_MAX_FREELISTS_TO_RECONSTRUCT ? request[0] : GEN_ROGUE_FWIF_MAX_FREELISTS_TO_RECONSTRUCT;

	for (uint32_t i = 0; i < count; i++) {
		uint32_t id = request[2 + i];
		struct pvr_free_list *fl = id >= 1 && id <= FREE_LISTS ? pvr_free_lists[id - 1] : NULL;

		words[2 + i] = id;
		if (fl == NULL) {
			continue;
		}
		fl->current_pages = 0;
		for (unsigned n = 0; n < fl->nodes; n++) {
			free_list_insert(fl, n);
		}
		fl->current_pages -= fl->ready_pages;
		store32(&fl->fw, GEN_rogue_fwif_freelist__current_stack_top, fresh32(fl->fw.cpu + 28) - 1);
		store32(&fl->fw, GEN_rogue_fwif_freelist__allocated_page_count, 0);
		store32(&fl->fw, GEN_rogue_fwif_freelist__allocated_mmu_page_count, 0);
		for (unsigned h = 0; h < FREE_LIST_HWRTS; h++) {
			if (fl->hwrt[h] != NULL) {
				store32(fl->hwrt[h], GEN_rogue_fwif_hwrtdata__state, GEN_ROGUE_FWIF_RTDATA_STATE_HWR);
				store32(fl->hwrt[h], GEN_rogue_fwif_hwrtdata__hwrt_data_flags,
				    fresh32(fl->hwrt[h]->cpu + GEN_rogue_fwif_hwrtdata__hwrt_data_flags) & ~(uint32_t)GEN_HWRTDATA_HAS_LAST_GEOM);
			}
		}
	}
	words[0] = count;
	if (++free_list_said <= 24 || (free_list_said & 63) == 0) {
		SAY("pvr: %u free lists put back together at the firmware's asking after a recovery (the first %u)", count, request[2]);
	}
	(void)kccb_send_words(GEN_ROGUE_FWIF_KCCB_CMD_FREELISTS_RECONSTRUCTION_UPDATE, words, 2 + count, false);
	kccb_sent--;
}

/* pvr_free_list_release: the cleanup, once more after the firmware's requests have been read if it says busy; then the memory and the pages. */
static bool
pvr_free_list_destroy(struct pvr_free_list *fl)
{
	int busy = pvr_cleanup("CLEANUP of the free list", CLEANUP_FREELIST, fl->fw.addr);

	if (busy == 1) {
		fwccb_process();
		busy = pvr_cleanup("CLEANUP of the free list, again", CLEANUP_FREELIST, fl->fw.addr);
	}
	if (busy != 0) {
		return false;
	}
	pvr_free_lists[fl->id - 1] = NULL;
	free_list_drop_nodes(fl);
	fw_object_free(&fl->fw);
	return true;
}

/*
 * A render target's data, pvr_hwrt.c: what pvr_hwrt_dataset_create makes from
 * drm_pvr_ioctl_create_hwrt_dataset_args, which struct pvr_hwrt_args repeats. The GPU addresses in it
 * are buffers user space has made and mapped; the firmware reads none of this before a geometry job.
 */
#define HWRTDATA_COMMON_SIZE    88      /* check 455, the offsets below 436-454 */
#define HWRTDATA_SIZE           384     /* check 480, the offsets below 457-476; rta_ctl 208 with max_rts 24 in (417) and the two addresses 16, 20 */

struct pvr_hwrt_args {
	uint32_t width, height, samples, layers;
	uint32_t isp_merge_lower_x, isp_merge_lower_y, isp_merge_upper_x, isp_merge_upper_y, isp_merge_scale_x, isp_merge_scale_y;
	uint32_t region_header_size, tpc_size, tpc_stride;
	uint64_t tpc_dev_addr, vheap_table_dev_addr, rtc_dev_addr;
	struct {
		uint64_t pm_mlist_dev_addr, macrotile_array_dev_addr, region_header_dev_addr;
	} rt[2];                        /* ROGUE_FWIF_NUM_RTDATAS */
	struct pvr_free_list *free_lists[2];    /* ROGUE_FWIF_NUM_RTDATA_FREELISTS; [0] the local one */
};
struct pvr_hwrt {
	struct fw_obj common, data[2], srtc[2], raa[2];
	uint32_t layers;
	struct pvr_free_list *local;
	struct pvr_hwrt_args args;      /* what it was made from: written again after a reset */
};

static void pvr_hwrt_fill(struct pvr_hwrt *hwrt);

static uint64_t
hwrt_multisamplectl(uint32_t samples, bool y_flip)      /* get_cr_multisamplectl_val; samples is 1, 2, 4 or 8 */
{
	static const uint8_t positions[4][2][8] = {
		{ { 8 }, { 8 } }, { { 12, 4 }, { 12, 4 } }, { { 6, 14, 2, 10 }, { 2, 6, 10, 14 } },
		{ { 9, 7, 13, 5, 3, 1, 11, 15 }, { 5, 11, 9, 3, 13, 7, 15, 1 } },
	};
	unsigned idx = samples >= 8 ? 3 : samples >= 4 ? 2 : samples >= 2 ? 1 : 0;
	uint64_t value = 0;

	for (unsigned i = 0; i < 8; i++) {
		uint64_t y = positions[idx][1][i];

		value |= (uint64_t)positions[idx][0][i] << (i * 8);
		value |= (y_flip ? (16 - y) & 0xf : y) << (i * 8 + 4);
	}
	return value;
}

static void
pvr_hwrt_free(struct pvr_hwrt *hwrt)
{
	for (int i = 1; i >= 0; i--) {
		for (unsigned h = 0; hwrt->local != NULL && h < FREE_LIST_HWRTS; h++) {        /* pvr_free_list_remove_hwrt */
			if (hwrt->local->hwrt[h] == &hwrt->data[i]) {
				hwrt->local->hwrt[h] = NULL;
			}
		}
		if (hwrt->layers > 1) {
			fw_object_free(&hwrt->raa[i]);
			fw_object_free(&hwrt->srtc[i]);
		}
		fw_object_free(&hwrt->data[i]);
	}
	fw_object_free(&hwrt->common);
}

/* hwrt_init_kernel_structure, hwrt_init_common_fw_structure (229), hwrt_data_init_fw_structure (354) */
static bool
pvr_hwrt_create(struct pvr_hwrt *hwrt, const struct pvr_hwrt_args *args)
{
	uint32_t min_pages = !feature_has[F_ROGUEXE] ? 50 : (fw_quirks & BRN_66011) != 0 ? 40 : 25;    /* pvr_get_free_list_min_pages, pvr_free_list.c 21-23 */

	memset(hwrt, 0, sizeof(*hwrt));
	/* written for what this core is: one ISP sample a pixel, and a "simple parameter format" -- for whose version, 1 here,
	 * Linux v7.3 itself warns (WARN_ON(parameter_format != 2), pvr_hwrt.c) and goes on with the same arithmetic */
	if (args->free_lists[0] == NULL || args->free_lists[1] == NULL || args->free_lists[0]->current_pages < min_pages ||
	    args->width == 0 || args->height == 0 || args->layers == 0 || (args->samples != 1 && args->samples != 2 &&
	    args->samples != 4 && args->samples != 8) || feature_value[F_ISP_SAMPLES_PER_PIXEL] != 1 ||
	    !feature_has[F_SIMPLE_PARAMETER_FORMAT_VERSION] || feature_value[F_TILE_SIZE_X] == 0 || feature_value[F_TILE_SIZE_Y] == 0) {
		return false;
	}
	hwrt->layers = args->layers;
	hwrt->args = *args;
	hwrt->common = fw_object("the render target's common data", HWRTDATA_COMMON_SIZE, FW_ANYWHERE);
	if (hwrt->common.cpu == NULL) {
		return false;
	}
	for (unsigned i = 0; i < 2; i++) {
		struct fw_obj *data = &hwrt->data[i];

		*data = fw_object("a render target's data", HWRTDATA_SIZE, FW_ANYWHERE);
		if (args->layers > 1) {         /* SRTC_ENTRY_SIZE, RAA_ENTRY_SIZE: a u32 a layer */
			hwrt->srtc[i] = fw_object("a shadow render target cache", args->layers * 4, FW_ANYWHERE);
			hwrt->raa[i] = fw_object("a render target array's counts", args->layers * 4, FW_ANYWHERE);
		}
		if (data->cpu == NULL || (args->layers > 1 && (hwrt->srtc[i].cpu == NULL || hwrt->raa[i].cpu == NULL))) {
			pvr_hwrt_free(hwrt);
			return false;
		}
		for (unsigned h = 0; h < FREE_LIST_HWRTS; h++) {        /* pvr_free_list_add_hwrt: on its local list */
			if (args->free_lists[0]->hwrt[h] == NULL) {
				args->free_lists[0]->hwrt[h] = data;
				break;
			}
		}
	}
	hwrt->local = args->free_lists[0];
	pvr_hwrt_fill(hwrt);
	return true;
}

/* hwrt_init_common_fw_structure's and hwrt_data_init_fw_structure's writing: at creation, and again after a reset */
static void
pvr_hwrt_fill(struct pvr_hwrt *hwrt)
{
	const struct pvr_hwrt_args *args = &hwrt->args;
	uint32_t tiles_x, tiles_y, mtile_x, mtile_y, isp_x, isp_y, teaa = 0;

	tiles_x = (args->width + (uint32_t)feature_value[F_TILE_SIZE_X] - 1) / (uint32_t)feature_value[F_TILE_SIZE_X];
	tiles_y = (args->height + (uint32_t)feature_value[F_TILE_SIZE_Y] - 1) / (uint32_t)feature_value[F_TILE_SIZE_Y];
	bool v1 = feature_value[F_SIMPLE_PARAMETER_FORMAT_VERSION] == 1;

	/*
	 * Version 1 of the simple parameter format (the r1's GE8320) has one macrotile, the whole render
	 * (Mesa's rogue_get_num_macrotiles_xy gives 1 x 1 for anything but version 2), and its tile count
	 * is not rounded to even. With version 2's sum a 64 x 64 target got a 2 x 2-tile macrotile and
	 * only its first tiles were drawn (seen on the device).
	 */
	mtile_x = v1 ? tiles_x : (tiles_x + 7) / 8 * 2;
	mtile_y = v1 ? tiles_y : (tiles_y + 7) / 8 * 2;
	isp_x = args->samples >= 4 ? mtile_x << 1 : mtile_x;            /* get_cr_isp_mtile_size_val, samples_per_pixel 1 */
	isp_y = args->samples >= 2 ? mtile_y << 1 : mtile_y;
	teaa |= args->samples >= 2 ? 4u : 0;                            /* get_cr_te_aa_val: ROGUE_CR_TE_AA_Y_EN, _X_EN (pvr_rogue_cr_defs_client.h 45, 51) */
	teaa |= args->samples >= 4 ? 2u : 0;

	/* the register fields are pvr_rogue_cr_defs_client.h's (67-153) */
	put32(&hwrt->common, 4, ((args->width - 1) & 0x7fffu) | (((args->height - 1) << 16) & 0x7fff0000u));     /* screen_pixel_max */
	put64(&hwrt->common, 8, hwrt_multisamplectl(args->samples, false));     /* multi_sample_ctl */
	put64(&hwrt->common, 16, hwrt_multisamplectl(args->samples, true));     /* flipped_multi_sample_ctl */
	put32(&hwrt->common, 24, args->tpc_stride);
	put32(&hwrt->common, 28, args->tpc_size);
	put32(&hwrt->common, 32, v1 ? ((tiles_x - 1) & 0x1ffu) | (((tiles_y - 1) << 12) & 0x1ff000u) :
	    ((((tiles_x + 1) & ~1u) - 1) & 0x1ffu) | (((((tiles_y + 1) & ~1u) - 1) << 12) & 0x1ff000u));       /* te_screen */
	put32(&hwrt->common, 36, mtile_x * mtile_y);                            /* mtile_stride */
	put32(&hwrt->common, 40, teaa);
	put32(&hwrt->common, 44, (mtile_x << 18) & 0x07fc0000u);                /* te_mtile1: X1; X2 and X3 are 0 in this format */
	put32(&hwrt->common, 48, (mtile_y << 18) & 0x07fc0000u);                /* te_mtile2 */
	put32(&hwrt->common, 52, args->isp_merge_lower_x);
	put32(&hwrt->common, 56, args->isp_merge_lower_y);
	put32(&hwrt->common, 60, args->isp_merge_upper_x);
	put32(&hwrt->common, 64, args->isp_merge_upper_y);
	put32(&hwrt->common, 68, args->isp_merge_scale_x);
	put32(&hwrt->common, 72, args->isp_merge_scale_y);
	put32(&hwrt->common, 76, args->region_header_size);
	put32(&hwrt->common, 80, ((isp_x << 16) & 0x03ff0000u) | (isp_y & 0x3ffu));     /* isp_mtile_size */
	FlushPoC_DcacheRegion((vm_offset_t)hwrt->common.cpu, HWRTDATA_COMMON_SIZE);

	for (unsigned i = 0; i < 2; i++) {
		struct fw_obj *data = &hwrt->data[i];

		put64(data, 0, args->rt[i].pm_mlist_dev_addr);
		put32(data, 164, hwrt->common.addr);                    /* hwrt_data_common_fw_addr */
		put32(data, 176, args->free_lists[0]->fw.addr);         /* freelists_fw_addr[] */
		put32(data, 180, args->free_lists[1]->fw.addr);
		put64(data, 200, args->vheap_table_dev_addr);
		put32(data, 208 + 16, args->layers > 1 ? hwrt->srtc[i].addr : 0);       /* rta_ctl.valid_render_targets_fw_addr */
		put32(data, 208 + 20, args->layers > 1 ? hwrt->raa[i].addr : 0);        /* rta_ctl.rta_num_partial_renders_fw_addr */
		put32(data, 208 + 24, args->layers);                    /* rta_ctl.max_rts */
		put64(data, 240, args->tpc_dev_addr);                   /* tail_ptrs_dev_addr */
		put64(data, 248, args->rt[i].macrotile_array_dev_addr);
		put64(data, 256, args->rt[i].region_header_dev_addr);
		put64(data, 264, args->rtc_dev_addr);
		FlushPoC_DcacheRegion((vm_offset_t)data->cpu, HWRTDATA_SIZE);
	}
}

/* pvr_hwrt_dataset_release (500): each data's cleanup, the second first, then the memory */
static bool
pvr_hwrt_destroy(struct pvr_hwrt *hwrt)
{
	bool clean = true;

	for (int i = 1; i >= 0; i--) {
		int busy = 1;

		for (unsigned tries = 0; tries < CLEANUP_TRIES && busy == 1; tries++) { /* busy while a job on it runs: asked again, for 20 ms */
			busy = pvr_cleanup(i == 1 ? "CLEANUP of the render target's second data" : "CLEANUP of the render target's first data",
			    CLEANUP_HWRTDATA, hwrt->data[i].addr);
			if (busy == 1) {
				cleanup_busy++;
				delay_for_interval(250, NSEC_PER_USEC);
			}
		}
		clean = clean && busy == 0;
	}
	if (clean) {
		pvr_hwrt_free(hwrt);
	}
	return clean;
}

/*
 * Stage 5: one of each, as a client's first calls would make them. The buffers a client would bring
 * are ten pages of the arena here, mapped in a row in the general heap above its static areas'
 * 64 KiB: [0] the free list's page numbers, then the tail pointer cache, the virtual heap table, the
 * render target cache, and for each of the two render target datas its PM list, macrotile array and
 * region headers. Their SIZES and the tail pointer cache's stride, the region header size and the
 * ISP merge values are what Mesa's pvr works out for a real target: here the buffers are a page each
 * and those numbers 0, which nothing reads before a geometry job -- so this target is for the
 * firmware's bookkeeping only and must not be rendered to.
 */
#define S5_PAGES        10
#define S5_BASE         (pvr_heaps[PVR_HEAP_GENERAL].base + 0x10000)
#define S5_FREE_PAGES   64u     /* at least pvr_get_free_list_min_pages' 40 for this core */
#define S5_WIDTH        64u
#define S5_HEIGHT       64u

static struct pvr_vm s5_vm;
static struct pvr_context s5_context;
static struct pvr_free_list s5_free_list;
static struct pvr_hwrt s5_hwrt;

static int s5_bo = -1;

/* What stage 5 made, cleaned up through the firmware, last made first. NULL, or what stopped it. */
static const char *
s5_teardown(void)
{
	const char *failed = NULL;

	SAY("pvr: s5: the cleanups, last made first -- the firmware reads each structure now");
	if (!pvr_hwrt_destroy(&s5_hwrt)) {
		failed = "the render target's cleanup";
	} else if (!pvr_free_list_destroy(&s5_free_list)) {
		failed = "the free list's cleanup";
	} else if (!pvr_context_destroy(&s5_context)) {
		failed = "the context's cleanup";
	} else {
		pvr_vm_unmap(&s5_vm, S5_BASE, S5_PAGES * 4096);
		pvr_vm_destroy(&s5_vm);
		arena_free((uint32_t)s5_bo, S5_PAGES);
		if (!pvr_mmu_flush()) {
			failed = "the last MMU cache flush";
		}
	}
	SAY("pvr: s5: teardown %s; %#x of the arena in use", failed == NULL ? "done, every cleanup answered and none busy" : "STOPPED", arena_used);
	return failed;
}

static bool
fw_stage5(void)
{
	struct pvr_hwrt_args args;
	uint32_t teardown = 0;
	const char *failed = NULL;
	int bo, error;

	(void)PE_parse_boot_argn("pvr_teardown", &teardown, sizeof(teardown));
	SAY("pvr: s5: a VM context (its page catalogue and the firmware's memory context)");
	if (!pvr_vm_create(&s5_vm)) {
		failed = "the VM context";
		goto out;
	}
	SAY("pvr: s5: VM: page catalogue at physical %#llx, memory context at firmware %#x; %u heaps, the general one at %#llx", s5_vm.pc_phys,
	    s5_vm.memctx.addr, (fw_quirks & BRN_63142) != 0 ? PVR_HEAPS : PVR_HEAPS - 1, pvr_heaps[PVR_HEAP_GENERAL].base);
	SAY("pvr: s5: %u static areas are user space's to fill, the general heap's up to %#x: the kernel only tells of them",
	    (unsigned)(sizeof(pvr_static_areas) / sizeof(pvr_static_areas[0])), pvr_static_areas[1].offset + pvr_static_areas[1].size);

	SAY("pvr: s5: mapping %u pages at GPU %#llx, the first protected for the PM; then the MMU cache flush", S5_PAGES, S5_BASE);
	if ((bo = s5_bo = arena_alloc(S5_PAGES)) < 0) {
		failed = "memory for the buffers";
		goto out;
	}
	memset((uint8_t *)arena + ((uint32_t)bo << 12), 0, S5_PAGES * 4096);
	FlushPoC_DcacheRegion(arena + ((uint32_t)bo << 12), S5_PAGES * 4096);
	error = pvr_vm_map(&s5_vm, S5_BASE, arena_phys + ((uint64_t)(uint32_t)bo << 12), 4096, PVR_BO_PM_FW_PROTECT);
	if (error == 0) {
		error = pvr_vm_map(&s5_vm, S5_BASE + 4096, arena_phys + ((uint64_t)(uint32_t)bo << 12) + 4096, (S5_PAGES - 1) * 4096, 0);
	}
	/* the checks themselves: a second mapping of a mapped page, and one outside every heap, must both be refused */
	if (error != 0 || pvr_vm_map(&s5_vm, S5_BASE, arena_phys, 4096, 0) != EEXIST || pvr_vm_map(&s5_vm, 0x1000, arena_phys, 4096, 0) != EINVAL) {
		SAY("pvr: s5: MAPPING FAILED or let through what it must refuse (error %d)", error);
		failed = "the mapping";
		goto out;
	}
	if (!pvr_mmu_flush()) {
		failed = "the MMU cache flush";
		goto out;
	}
	SAY("pvr: s5: mapped: catalogue entry %#x, the first page's entry %#llx, the second's %#llx; flush acknowledged (%#x)",
	    s5_vm.pc[(S5_BASE >> 30) & 0x3ffu], *mmu_entry(&s5_vm, S5_BASE, false), *mmu_entry(&s5_vm, S5_BASE + 4096, false), last_rtn);

	SAY("pvr: s5: a render context (geometry and fragment common contexts, a %u KiB client CCB each)", 1u << (CCCB_SIZE_LOG2 - 10));
	if (!pvr_context_create(&s5_context, &s5_vm, PVR_CTX_RENDER, PVR_CTX_PRIORITY_MEDIUM, 0, 0)) {
		failed = "the render context";
		goto out;
	}
	SAY("pvr: s5: context %u at firmware %#x (fragment's at %#x), client CCBs %#x and %#x: written; v7.3 has no command that creates one",
	    s5_context.id, s5_context.fw.addr, s5_context.fw.addr + s5_context.queue[1].at, s5_context.queue[0].cccb.addr, s5_context.queue[1].cccb.addr);

	SAY("pvr: s5: a free list of %u pages (at most %u, growing by %u), its page numbers at GPU %#llx", S5_FREE_PAGES, 2 * S5_FREE_PAGES,
	    S5_FREE_PAGES / 2, S5_BASE);
	if (!pvr_free_list_create(&s5_free_list, (uint32_t *)(void *)((uint8_t *)arena + ((uint32_t)bo << 12)), S5_BASE, S5_FREE_PAGES,
	    2 * S5_FREE_PAGES, S5_FREE_PAGES / 2, 0)) {
		failed = "the free list";
		goto out;
	}
	SAY("pvr: s5: free list %u at firmware %#x: %u pages, the first page number %#x at entry %u: written, no command",
	    s5_free_list.id, s5_free_list.fw.addr, s5_free_list.current_pages, s5_free_list.list[2 * S5_FREE_PAGES - S5_FREE_PAGES],
	    2 * S5_FREE_PAGES - S5_FREE_PAGES);

	SAY("pvr: s5: render target data for %ux%u, one layer, one sample", S5_WIDTH, S5_HEIGHT);
	memset(&args, 0, sizeof(args));
	args.width = S5_WIDTH;
	args.height = S5_HEIGHT;
	args.samples = 1;
	args.layers = 1;
	args.tpc_dev_addr = S5_BASE + 1 * 4096;
	args.vheap_table_dev_addr = S5_BASE + 2 * 4096;
	args.rtc_dev_addr = S5_BASE + 3 * 4096;
	for (unsigned i = 0; i < 2; i++) {
		args.rt[i].pm_mlist_dev_addr = S5_BASE + (4 + 3 * i) * 4096;
		args.rt[i].macrotile_array_dev_addr = S5_BASE + (5 + 3 * i) * 4096;
		args.rt[i].region_header_dev_addr = S5_BASE + (6 + 3 * i) * 4096;
		args.free_lists[i] = &s5_free_list;
	}
	if (!pvr_hwrt_create(&s5_hwrt, &args)) {
		failed = "the render target data";
		goto out;
	}
	SAY("pvr: s5: render target data at firmware %#x and %#x, common %#x (TE screen %#x, macrotiles %#x): written, no command",
	    s5_hwrt.data[0].addr, s5_hwrt.data[1].addr, s5_hwrt.common.addr, *(uint32_t *)(void *)(s5_hwrt.common.cpu + 32),
	    *(uint32_t *)(void *)(s5_hwrt.common.cpu + 44));

	/* the new objects' pages in the firmware's own page table: Linux asks for this flush on every mapping there (pvr_vm_mips_map) */
	mmu_flush_flags |= MMU_SYNC_2;
	if (!pvr_mmu_flush() || !health_check()) {
		failed = "the firmware, after all of it";
		goto out;
	}

	if (teardown != 0) {
		failed = s5_teardown();
	}
out:
	fw_dump();
	if (failed != NULL) {
		SAY("pvr: stage 5: NOT DONE: %s", failed);
		return false;
	}
	SAY("pvr: stage 5: context %u, free list %u pages, HWRT %ux%u: all acknowledged%s", s5_context.id, S5_FREE_PAGES, S5_WIDTH, S5_HEIGHT,
	    teardown != 0 ? ", and taken down again" : " (the MMU flushes; the objects are written, not sent)");
	return true;
}

#include "redwood_pvr_drm.c"

static void
firmware(uint32_t stage)
{
	clock_hz = gpu_clock();
	if (fw_read() && fw_load() && fw_structures()) {
		reached = 4;
		if (fw_start()) {
			reached = 5;
			if (stage >= 4) {
				if (fw_commands()) {
					reached = 6;
				}
				SAY("pvr: stage 4: %u commands sent, %u acknowledged; %u from the firmware", kccb_sent, kccb_acked, fwccb_seen);
				if (stage >= 5 && reached == 6 && fw_stage5()) {
					reached = 7;
				}
				if (stage >= 6 && reached == 7 && fw_stage6()) {
					reached = 8;
				}
			}
		}
	}
}

/*
 * For as long as the firmware runs: the interrupt's work ten times a second and the watchdog's twice,
 * so that what later stages send is answered and what the firmware asks is seen. Linux resets a
 * stalled firmware (pvr_power_reset); this only says so, once, and goes on looking.
 */
static void
fw_keep(void)
{
	bool stalled = false;
	uint64_t began = mach_absolute_time(), watched = 0, said = 0, now = 0, deadline;

	for (;;) {
		uint32_t nap_us;

		if (drm_lock == NULL) {         /* before programs are served (pvr=4, 5): ten times a second will do */
			delay_for_interval(100, NSEC_PER_MSEC);
		} else {
			drm_reap();             /* (here, with no lock and no wait declared: it takes the lock itself) */
			lck_mtx_lock(drm_lock);
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &now);
		if (!drm_lost) {
			drm_irq_seen();
			fw_service();
		}
		if (drm_reset_wanted != NULL && !drm_lost) {
			drm_reset(drm_reset_wanted);
		}
		/* buffers given back wait for a flush: the next submission sends it, or this, 2 ms after the first of them */
		if (!drm_lost && drm_lock != NULL && drm_dead_count != 0 && drm_ms_since(drm_dead_since) >= 2) {
			(void)pvr_mmu_flush();
		}
		if (!drm_lost && now - watched >= (uint64_t)WATCHDOG_TIME_MS * NSEC_PER_MSEC) {
			watched = now;
			if (fw_watchdog() && drm_serving && !drm_lost) {
				drm_reset_wanted = "the firmware stopped taking commands";      /* pvr_watchdog_worker's pvr_power_reset(hard) */
			} else if (fw_watchdog() && !stalled) {
				stalled = true;
				SAY("pvr: THE FIRMWARE HAS STALLED: a command waits and its count of executed stays %u; KCCB write %u read %u, "
				    "EXCEPTION_STATUS %#x", fresh32(fw_osdata.cpu + 568), fresh32(fw_kccb_ctl.cpu), fresh32(fw_kccb_ctl.cpu + 128),
				    rd(rgx, CR_MIPS_EXCEPTION_STATUS));
			}
		}
		if (now - said >= 10ull * NSEC_PER_SEC) {
			if (said == 0) {
				SAY("pvr: ten seconds on: %u health checks sent, the firmware counts %u commands executed and %u interrupts; %s", health_checks,
				    fresh32(fw_osdata.cpu + 568), fresh32(fw_osdata.cpu + 560), stalled ? "STALLED" : "alive");
			}
			said = now;
			drm_stats_say();
		}
		if (drm_lock == NULL) {
			continue;
		}
		/*
		 * Asleep until the interrupt (drm_irq_arrived), a kick (drm_submit) or the time is up: a
		 * tenth of a second with nothing out; with jobs out, 2 ms if the interrupt has been seen to
		 * work -- it is what wakes this -- and else a tenth of a millisecond for the first 3 ms
		 * after a kick (jobs here take a few) and a quarter after. The wait is declared before the
		 * interrupt is let in again, so one that comes at once is not slept through.
		 */
		nap_us = drm_jobs != NULL ? (drm_irq_works ? 2000 : drm_ms_since(drm_last_kick) < 3 ? 100 : 250) : drm_dead_count != 0 ? 2000 : 100000;
		clock_interval_to_deadline(nap_us, NSEC_PER_USEC, &deadline);
		(void)assert_wait_deadline((event_t)&drm_serving, THREAD_UNINT, deadline);
		drm_irq_listen();
		lck_mtx_unlock(drm_lock);
		(void)thread_block(THREAD_CONTINUE_NULL);
	}
}

static void
pvr_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t stage = (uint32_t)(uintptr_t)parameter;
	bool readable;

	topckgen = ml_io_map(TOPCKGEN_PHYS, 0x1000);
	infracfg = ml_io_map(INFRACFG_PHYS, 0x1000);
	spm = ml_io_map(SPM_PHYS, 0x1000);
	apmixed = ml_io_map(APMIXED_PHYS, 0x1000);

	readable = show("as the bootloader left it");
	reached = 1;
	if (stage >= 2) {
		rails();
		if (power_up()) {
			reached = 2;
		}
		readable = show("after the power-up");
	}
	if (readable) {
		inside();
	} else {
		SAY("pvr: mfg_cfg and the GPU's registers SKIPPED: not every domain is on with its bus and clock, "
		    "and reading a block without power can hang the bus%s", stage < 2 ? " (pvr=2 switches them on)" : "");
	}
	if (stage >= 3) {
		if (reached == 3) {
			firmware(stage);
		} else {
			SAY("pvr: the firmware SKIPPED: the GPU did not say %u.%u.%u.%u", EXPECT_B, EXPECT_V, EXPECT_N, EXPECT_C);
		}
	}
	SAY("pvr: stage %u asked, reached %u of %u", stage, reached, stage >= 6 ? 8 : stage >= 5 ? 7 : stage >= 4 ? 6 : stage >= 3 ? 5 : 3);
	if (stage >= 4 && reached >= 5) {
		fw_keep();              /* never returns */
	}
	thread_terminate(current_thread());
}

void
redwood_pvr_init(void)
{
	uint32_t stage = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("pvr", &stage, sizeof(stage)) || stage == 0) {
		return;
	}
	if (kernel_thread_start(pvr_thread, (void *)(uintptr_t)stage, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
