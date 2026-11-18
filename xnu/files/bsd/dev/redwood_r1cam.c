/*
 * The camera of the rabbit r1 (MT6765, PMIC MT6357): a Hynix Hi846 (chip ID 0x0846, Android's "hi846_mipi_raw") on I2C2's pins, its
 * MIPI CSI-2 (4 lanes, port CSI2: [dump]) received by SENINF5 and written to memory by CAMSV_1. The design is the Infinix X6826B's
 * (redwood_mtkcam.c, docs/camera-mtk-porting.md); that phone's receiver tables came from a live dump, this one's values come from sources,
 * each named where it is used:
 *
 *   [trace]   logs/camera/cam.trace, cam.dmesg, cg0/cg1, creg.txt: the r1 under Android opening its camera
 *   [dts]     the r1's own device tree (kd_camera_hw1's pinctrl states, i2c2's scl-gpio-id/sda-gpio-id, clock names)
 *   [pg]      vendor drivers/clk/mediatek/clk-mt6765-pg.c (the CAM power domain and its bus protection; the copy redwood_pvr.c cites)
 *   [clk]     Linux drivers/clk/mediatek/clk-mt6765.c (topckgen muxes and gates, camsys and mmsys gates)
 *   [6357]    Linux include/linux/mfd/mt6357/registers.h, drivers/regulator/mt6357-regulator.c
 *   [m4u]     vendor drivers/misc/mediatek/m4u/mt6765/m4u_reg.h (the larb's MMU_EN per port)
 *   [hi846]   Linux drivers/media/i2c/hi846.c (ID, tables, exposure/gain/frame-length, stream on) -- the tables themselves are made
 *             from it by tools/r1camtable.py (they differ from [hi846v]'s preview tables in three registers: 0x021c/0x021e, the OTP's;
 *             0x0958; 0x0b12); [hi846v] the vendor's hi846mipiraw_Sensor.c (GPL, oppo-source/A5S-8.1-kernel-source, an
 *             MT6765 phone: I2C address 0x42 >> 1, MCLK 24 MHz -> PCLK 288 MHz, line 3800, frame 2526, "BLC offset 64code",
 *             "Green 1st (=GB)", margin 6, gain2reg) and that tree's imgsensor_cfg_table.c for the order of the supplies
 *   [phy]     Linux drivers/phy/mediatek/phy-mtk-mipi-csi-0-5.c (the MIPI RX analog block; MT8365's, the same "0-5" block)
 *   [seninf]  MediaTek's GPL mtk_seninf.c + mtk_seninf_reg.h for MT8183 (gitlab mediatek/aiot/bsp/linux, branch mtk-android11-5.4,
 *             drivers/media/platform/mtk-isp/seninf): the same register layout as this SENINF (CSI-2 at +0xa00, mux at +0xd00)
 *   [camsv]   the same tree's mtk-isp/camsv/mtk_camsv_hw.c: the sequence and the values; the register OFFSETS are this chip's, by name,
 *             from the vendor's drivers/misc/mediatek/cameraisp/src/mt6765/camera_isp.c (CAMSV_REG_*), whose ISP_Reset has the same
 *             software-reset sequence
 *   [dump]    logs/camera/r1camdump.txt: SENINF's first four blocks, csi0a/b, CAM_A and the idle CAMSVs out of Android while its camera
 *             streamed (tools/android/r1camdump.c). It says the port is CSI2 (top +0x24 = 0x80000200, +0x1c and +0x20 zero; mux 0's source is 4 =
 *             SENINF5; csi0a/b's bandgaps off), gives mux 0's settings, the cam-mux (identity: CAM_A takes mux 0, as on the X6826B CAMSV_1 took
 *             mux 3), CAM_A's sensor interface (two pixels a clock) and frame-buffer control. [dump2] is r1camdump2.txt, the same with SENINF5's
 *             own block (0x1a044000) and csi2a/b (0x11c14000): the lane order, the settle count, the CSI-2 control, the analog block's values
 *   [x6826b]  what redwood_mtkcam.c found on the MT6768, whose CAMSV has these same offsets: the queue (address, header, THEN the
 *             ready bit), MMU_EN off, PAK's bits, a frame is done when its last bytes are written
 *
 * NOT from a source, said in the log where it matters: which PAK value this CAMSV wants for 10 bits (three variants are tried; Android's CAMSVs
 * are idle, so no dump says). [dump2] was taken in Android's 3264x2448 mode; its settle count is used for 1632x1224 too. Android itself takes this sensor through
 * CAM_A (the ISP's first pass: 385 registers live, its command queue and statistics outputs among them); CAMSV is kept because it needs
 * twenty registers, is proven on the MT6768's same block, and differs from Android's path only in which mux is fed (top +0x08) -- everything
 * before the mux is the same.
 *
 * boot-arg r1cam=N, each stage one proof:
 *   1  reads only: rails, pins, the CAM domain, the clocks, as the bootloader left them
 *   2  power in Android's order, MCLK, the sensor's ID over I2C by hand (GPIO103/104); everything off again
 *   3  + the sensor's tables, SENINF/PHY, CAMSV_1; r1cam_frames frames (default 30) with the exposure halved and doubled on the way, a line
 *      for each, and the last one's thumbnail (tools/camthumb.py)
 *   4  /dev/redwood-camera (sys/redwood_camera.h) for iOS: a session while something reads
 * r1cam_start=S (seconds before stages 1..3, default 10), r1cam_pak (0..2, default 0), r1cam_gain (16 = 1x, default 64), r1cam_settle, r1cam_addr;
 * r1cam_mode=1 (3264x2448 instead of 1632x1224, any stage); r1cam_motor=DEGREES (turns the camera once, before the stage; see cam_motor).
 * r1cam_flip=1 (stage 4: REDWOOD_CAMERA_SENSOR 1 = back, 2 = front turns the motor; r1cam_rear= 180, r1cam_front= 0, r1cam_step_us= 360 (200..1000)).
 *
 * Standalone, not redwood_mtkcam_skel.c: the skeleton has never run, keeps its platform as a static table inside itself, and stages
 * 1..3 need this file's power, sensor and receiver code anyway; the node below is the X6826B's proven one, cut down.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/proc.h>
#include <sys/redwood_camera.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void redwood_host_keep_add(const char *format, ...) __printflike(1, 2);
extern void delay(int usec);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority, thread_t *new_thread);
extern kern_return_t thread_terminate(thread_t thread);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);       /* osfmk/arm64/caches_asm.s: written back and forgotten */

#define say(...) redwood_host_keep_add("r1cam: " __VA_ARGS__)

#if __has_include("redwood_r1cam_hi846.h")
#include "redwood_r1cam_hi846.h"

/* ---- the register windows ---- */
static struct {
	uint64_t pa;
	vm_size_t size;
	vm_offset_t va;
} cam_win[] = {
	{ 0x10000000ULL, 0x10000, 0 },          /* topckgen, infracfg_ao, GPIO, SPM, apmixed, the PMIC wrapper */
	{ 0x10219000ULL, 0x1000, 0 },           /* the memory controller (EMI): its protection unit's violation status, read only */
	{ 0x11c10000ULL, 0x6000, 0 },           /* MIPI RX analog: csi0a .. csi2b */
	{ 0x14000000ULL, 0x3000, 0 },           /* mmsys, smi_common */
	{ 0x1a000000ULL, 0x52000, 0 },          /* camsys, smi_larb3, SENINF, CAMSV_0/1 */
};
#define CAM_NWIN (sizeof(cam_win) / sizeof(cam_win[0]))

static volatile uint32_t *
cam_reg(uint64_t pa)
{
	for (unsigned i = 0; i < CAM_NWIN; i++) {
		if (pa >= cam_win[i].pa && pa + 4 <= cam_win[i].pa + cam_win[i].size) {
			return (volatile uint32_t *)(cam_win[i].va + (pa - cam_win[i].pa));
		}
	}
	{
		static volatile uint32_t nowhere;       /* (a mistake in this file, not something the phone does: said, and the access goes nowhere) */

		say("NO WINDOW COVERS %#llx", pa);
		return &nowhere;
	}
}

static uint32_t
rd32(uint64_t pa)
{
	return *cam_reg(pa);
}

static void
wr32(uint64_t pa, uint32_t value)
{
	*cam_reg(pa) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* the bits of `mask` become `value` (already in place) */
static void
set32(uint64_t pa, uint32_t mask, uint32_t value)
{
	wr32(pa, (rd32(pa) & ~mask) | (value & mask));
}

static bool
cam_poll(uint64_t pa, uint32_t mask, uint32_t want, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if ((rd32(pa) & mask) == want) {
			return true;
		}
		delay(100);
	}
	return false;
}

static uint64_t
cam_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000u;
}

/* ---- the PMIC through the wrapper's WACS2 (as redwood_mtkaudio.c reaches the same MT6357) ---- */
#define PWRAP                   0x1000d000ULL
#define PWRAP_WACS2_CMD         0xc20
#define PWRAP_WACS2_RDATA       0xc24
#define PWRAP_WACS2_VLDCLR      0xc28
#define PWRAP_FSM(r)            (((r) >> 16) & 7u)
#define MT6357_HWCID            0x0008  /* [6357] */
#define MT6357_SWCID            0x000a  /* [6357]; said only */
#define MT6357_VCAMA_CON0       0x1a6c  /* [6357] LDO_VCAMA_CON0: read only here (Android has vcama disabled: creg.txt) */
#define MT6357_VCAMD_CON0       0x1a88  /* [6357] LDO_VCAMD_CON0, bit 0 enable; [trace] "pwrap reg=1a88 val=1" */
#define MT6357_VCAMIO_CON0      0x1a9c  /* [6357] LDO_VCAMIO_CON0, bit 0 enable; [trace] "pwrap reg=1a9c val=1", 5.8 ms before VCAMD's */
#define MT6357_VCAMD_ANA_CON0   0x1c94  /* [6357] the voltage: mask 0xf00 (mt6357-regulator.c MT6357_LDO("ldo-vcamd")) */
#define MT6357_VCAMD_1V2        0x0600  /* vcamd_voltages[6] = 1200000; creg.txt: "vcamd enabled 1200000" */

static bool
pwrap_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 200000; spin++) {
		if (PWRAP_FSM(rd32(PWRAP + PWRAP_WACS2_RDATA)) == state) {
			return true;
		}
	}
	return false;
}

static bool
pmic_read(uint32_t reg, uint16_t *value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (pwrap_wait(0)) {
		wr32(PWRAP + PWRAP_WACS2_CMD, (reg >> 1) << 16);
		if (pwrap_wait(6)) {
			*value = (uint16_t)rd32(PWRAP + PWRAP_WACS2_RDATA);
			wr32(PWRAP + PWRAP_WACS2_VLDCLR, 1);
			ok = true;
		}
	}
	redwood_pwrap_unlock();
	return ok;
}

/* the bits of `mask` become `value`: one read and one write, nobody between them; then read back */
static bool
pmic_set(uint32_t reg, uint16_t mask, uint16_t value)
{
	uint16_t v = 0;
	bool ok = false;

	redwood_pwrap_lock();
	if (pwrap_wait(0)) {
		wr32(PWRAP + PWRAP_WACS2_CMD, (reg >> 1) << 16);
		if (pwrap_wait(6)) {
			v = (uint16_t)rd32(PWRAP + PWRAP_WACS2_RDATA);
			wr32(PWRAP + PWRAP_WACS2_VLDCLR, 1);
			if (pwrap_wait(0)) {
				wr32(PWRAP + PWRAP_WACS2_CMD, (1u << 31) | ((reg >> 1) << 16) | (uint16_t)((v & ~mask) | value));
				ok = pwrap_wait(0);
			}
		}
	}
	redwood_pwrap_unlock();
	delay(200);
	return ok && pmic_read(reg, &v) && (v & mask) == value;
}

/* ---- the pins (MT6765, as redwood_hyn.c: MODE 0x300 four bits a pin, DIR 0x000, DOUT 0x100, DIN 0x200, +4 set, +8 clear) ---- */
#define GPIO                    0x10005000ULL
#define PIN_PDN                 97      /* [dts] cam0_pnd0/1 = cam0@2/@3: pinmux 0x6100; cg0/cg1: output 0 -> 1 */
#define PIN_MCLK                99      /* [dts] cam0_mclk_on: pinmux 0x6301 (function 1); cg0/cg1: mode 0 -> 1 */
#define PIN_RST                 101     /* [dts] cam0_rst0/1 = cam0@0/@1: pinmux 0x6500; cg0/cg1: output 0 -> 1 */
#define PIN_SCL                 103     /* [dts] i2c2@11009000 scl-gpio-id = <0x67> */
#define PIN_SDA                 104     /* [dts] i2c2@11009000 sda-gpio-id = <0x68>; cg0: both function 1, pulled up */
#define PIN_AVDD                165     /* [dts] cam0_vcama_on/off: pinmux 0xa500; cg0/cg1: output 0 -> 1 (the 2.8 V analog supply's switch) */
static const uint8_t cam_pins[] = { PIN_PDN, PIN_MCLK, PIN_RST, PIN_SCL, PIN_SDA, PIN_AVDD };
#define CAM_NPINS               sizeof(cam_pins)

struct cam_pin_state {
	uint32_t mode, dir, dout;
};

static uint32_t
cam_pin_field(uint32_t pin, uint32_t off)
{
	return (rd32(GPIO + off + 0x10 * (pin / 32)) >> (pin % 32)) & 1u;
}

static uint32_t
cam_pin_mode_get(uint32_t pin)
{
	return (rd32(GPIO + 0x300 + 0x10 * (pin / 8)) >> (4 * (pin % 8))) & 0xfu;
}

static void
cam_pin_mode(uint32_t pin, uint32_t mode)
{
	set32(GPIO + 0x300 + 0x10 * (uint64_t)(pin / 8), 0xfu << (4 * (pin % 8)), mode << (4 * (pin % 8)));
}

/* output: the level first, then the direction */
static void
cam_pin_out(uint32_t pin, int level)
{
	wr32(GPIO + (level ? 0x104 : 0x108) + 0x10 * (pin / 32), 1u << (pin % 32));
	wr32(GPIO + 0x004 + 0x10 * (pin / 32), 1u << (pin % 32));
}

static int
cam_pin_read(uint32_t pin)
{
	return (int)cam_pin_field(pin, 0x200);
}

static void
cam_pin_save(struct cam_pin_state *s, uint32_t pin)
{
	s->mode = cam_pin_mode_get(pin);
	s->dir = cam_pin_field(pin, 0x000);
	s->dout = cam_pin_field(pin, 0x100);
}

static void
cam_pin_restore(const struct cam_pin_state *s, uint32_t pin)
{
	wr32(GPIO + (s->dout ? 0x104 : 0x108) + 0x10 * (pin / 32), 1u << (pin % 32));
	wr32(GPIO + (s->dir ? 0x004 : 0x008) + 0x10 * (pin / 32), 1u << (pin % 32));
	cam_pin_mode(pin, s->mode);
}

/* ---- I2C by hand on the camera bus's pins (redwood_mtkcam.c's, the pins pulled up inside the chip: cg0) ---- */
static void
cam_bb_line(uint32_t pin, int high)
{
	if (high) {
		wr32(GPIO + 0x008 + 0x10 * (pin / 32), 1u << (pin % 32));       /* input: the pull-up takes it high */
	} else {
		cam_pin_out(pin, 0);
	}
}

static void
cam_bb_scl(int high)
{
	cam_bb_line(PIN_SCL, high);
	for (int i = 0; high && i < 200 && !cam_pin_read(PIN_SCL); i++) {        /* a slave may hold it low */
		delay(5);
	}
	delay(5);
}

static void
cam_bb_sda(int high)
{
	cam_bb_line(PIN_SDA, high);
	delay(5);
}

static void
cam_bb_start(void)
{
	cam_bb_sda(1);
	cam_bb_scl(1);
	cam_bb_sda(0);
	cam_bb_scl(0);
}

static void
cam_bb_stop(void)
{
	cam_bb_sda(0);
	cam_bb_scl(1);
	cam_bb_sda(1);
}

/* one byte out; 1 if it was acknowledged */
static int
cam_bb_write(uint8_t byte)
{
	int ack;

	for (int i = 7; i >= 0; i--) {
		cam_bb_sda((byte >> i) & 1);
		cam_bb_scl(1);
		cam_bb_scl(0);
	}
	cam_bb_sda(1);
	cam_bb_scl(1);
	ack = !cam_pin_read(PIN_SDA);
	cam_bb_scl(0);
	return ack;
}

static uint32_t cam_addr = 0x20;        /* logs/cam2.log: the r1's answers at 0x20 and not at [hi846v]'s 0x21 (0x42 >> 1); boot-arg r1cam_addr */

/* [hi846] hi846_read_reg: the 16-bit register address, a repeated start, one byte; true if every byte was acknowledged */
static bool
cam_bb_read8(uint16_t reg, uint8_t *byte)
{
	int ok;

	*byte = 0;
	cam_bb_start();
	ok = cam_bb_write((uint8_t)(cam_addr << 1));
	ok &= cam_bb_write((uint8_t)(reg >> 8));
	ok &= cam_bb_write((uint8_t)reg);
	cam_bb_start();
	ok &= cam_bb_write((uint8_t)((cam_addr << 1) | 1));
	cam_bb_sda(1);
	for (int i = 0; i < 8; i++) {
		cam_bb_scl(1);
		*byte = (uint8_t)((*byte << 1) | cam_pin_read(PIN_SDA));
		cam_bb_scl(0);
	}
	cam_bb_scl(1);                  /* (SDA left high: no acknowledge, the last byte) */
	cam_bb_scl(0);
	cam_bb_stop();
	return ok != 0;
}

/* [hi846] hi846_write_reg_16 (width 2, high byte first) and hi846_write_reg (width 1) */
static bool
cam_bb_write_reg(uint16_t reg, uint16_t val, int width)
{
	int ok;

	cam_bb_start();
	ok = cam_bb_write((uint8_t)(cam_addr << 1));
	ok &= cam_bb_write((uint8_t)(reg >> 8));
	ok &= cam_bb_write((uint8_t)reg);
	if (width == 2) {
		ok &= cam_bb_write((uint8_t)(val >> 8));
	}
	ok &= cam_bb_write((uint8_t)val);
	cam_bb_stop();
	return ok != 0;
}

/* ---- the CAM power domain, its bus protection, the clocks ---- */
#define TOPCKGEN                0x10000000ULL
#define CLK_CFG_UPDATE          0x004   /* [clk] */
#define CLK_CFG_1               0x050   /* [clk] camtg_sel: bits 18:16, gate bit 23, update bit 6; +4 set, +8 clear */
#define CAMTG_MASK              (7u << 16)
#define CAMTG_24M               (1u << 16)      /* [clk] camtg_parents[1] = usb20_192m_d8 = 24 MHz; [dts] CLK_MCLK_24M = <topckgen 18> = CLK_TOP_USB20_192M_D8 */
#define CAMTG_PDN               (1u << 23)      /* [trace] "topckgen reg=58 val=800000" */
#define CAMTG_UPDATE            (1u << 6)
#define CLK_CFG_6               0x0a0   /* [clk] seninf_sel: bits 17:16, gate bit 23 */
#define SENINF_PDN              (1u << 23)      /* [trace] "topckgen reg=a8 val=800000" */
#define CLK_CFG_7               0x0b0   /* [clk] camtm_sel: bits 9:8, gate bit 15 */
#define CAMTM_PDN               (1u << 15)      /* [trace] "topckgen reg=b8 val=8000" */

#define INFRACFG                0x10001000ULL
#define PROT_STA1               0x228   /* [pg] INFRA_TOPAXI_PROTECTEN_STA1 */
#define PROT_STA1_1             0x258   /* [pg] INFRA_TOPAXI_PROTECTEN_STA1_1 */
#define PROT_SET                0x2a0
#define PROT_CLR                0x2a4
#define PROT_1_SET              0x2a8
#define PROT_1_CLR              0x2ac
#define CAM_PROT_STEP1          ((1u << 19) | (1u << 21))       /* [pg] CAM_PROT_STEP1_0_MASK, in PROTECTEN_1 */
#define CAM_PROT_STEP2          (1u << 20)                      /* [pg] CAM_PROT_STEP2_0_MASK, in PROTECTEN */
#define SMI_COMMON              0x14002000ULL                   /* [dts] smi_common@14002000 */
#define SMI_CLAMP               0x3c0   /* [pg] SMI_COMMON_SMI_CLAMP, +4 set, +8 clear */
#define CAM_SMI_CLAMP           (1u << 3)                       /* [pg] CAM_PROT_STEP2_1_MASK */

#define SPM                     0x10006000ULL
#define PWR_STATUS              0x180   /* [pg] */
#define PWR_STATUS_2ND          0x184
#define CAM_PWR_CON             0x344   /* [pg] */
#define DIS_STA                 (1u << 3)       /* [pg] DIS_PWR_STA_MASK: the display's domain, pg_cam's parent */
#define CAM_STA                 (1u << 25)      /* [pg] CAM_PWR_STA_MASK */
#define PWR_RST_B               (1u << 0)
#define PWR_ISO                 (1u << 1)
#define PWR_ON                  (1u << 2)
#define PWR_ON_2ND              (1u << 3)
#define PWR_CLK_DIS             (1u << 4)

#define MMSYS                   0x14000000ULL
#define MMSYS_CG_CON0           0x100   /* [clk] mm_cg_regs: +4 set (gate), +8 clear (run) */
#define MM_SMI_CAM              (1u << 25)      /* [clk] mm_smi_cam_ck; [pg] cam_cg */
#define MM_SMI_COMMON           (0xfu << 19)    /* [clk] mm_smi_common (19), mm_smi_larb0 (20), mm_smi_comm0 (21), mm_smi_comm1 (22): [pg] mm_cg2, the display domain's own */
#define CAMSYS                  0x1a000000ULL
#define CAMSYS_CG_CON           0x0     /* [clk] cam_cg_regs: +4 set, +8 clear */
#define CAMSYS_PRE              0x1043u /* [pg] cam_cg: cam_larb3 (0), cam_dfp_vad (1), cam (6), cam_ccu (12) */
#define CAMSYS_REST             0x0f80u /* [clk] camtg (7), cam_seninf (8), camsv0..2 (9..11); [trace] camsys reg=8: 1 2 40 1000 80 200 400 800 100 */
#define APMIXED_AP_PLL_CON5     0x1000c014ULL
#define APMIXED_MIPI            0x840u  /* [clk] apmixed_mipic026m (6), apmixed_mipic126m (11); [trace] "apmixed reg=14 val=323f0", "32bf0" */
#define MIPIRX_0A               0x11c10000ULL
#define MIPIRX                  0x11c14000ULL   /* csi2a ([dts] mipi_rx_ana_csi2a@11c14000): the port in use ([dump]); csi2b is +0x1000 */
#define MIPIRX_CSR              0x80    /* [trace] "mipi_rx_ana_csi0a reg=80 val=2"; [clk] CLK_MIPI0A_CSR_CSI_EN_0A, and each block's own */
static const uint64_t cam_csr[3] = { MIPIRX_0A + MIPIRX_CSR, MIPIRX + MIPIRX_CSR, MIPIRX + 0x1000 + MIPIRX_CSR };      /* Android's one, and the port's two */

static void
cam_con(uint32_t clear, uint32_t set)
{
	wr32(SPM + CAM_PWR_CON, (rd32(SPM + CAM_PWR_CON) & ~clear) | set);
}

/* [pg] pg_prepare's order: spm_mtcmos_ctrl_cam_pwr(on), the pre_clk2 gates (cam_cg), spm_mtcmos_ctrl_cam_bus_prot(on); every wait bounded */
static bool
cam_domain_on(void)
{
	say("CAM domain: PWR_ON, PWR_ON_2ND");
	cam_con(0, PWR_ON);
	cam_con(0, PWR_ON_2ND);
	if (!cam_poll(SPM + PWR_STATUS, CAM_STA, CAM_STA, 20) || !cam_poll(SPM + PWR_STATUS_2ND, CAM_STA, CAM_STA, 20)) {
		say("  THE STATUS BIT DID NOT COME UP: PWR_STATUS %#x, _2ND %#x, CAM_PWR_CON %#x", rd32(SPM + PWR_STATUS),
		    rd32(SPM + PWR_STATUS_2ND), rd32(SPM + CAM_PWR_CON));
		return false;
	}
	say("CAM domain: clock on, isolation off, reset released");
	cam_con(PWR_CLK_DIS, 0);
	cam_con(PWR_ISO, 0);
	cam_con(0, PWR_RST_B);
	say("CAM domain: the SMI's gates in mmsys (CG_CON0 %#x), then camsys's first gates (the first access to 0x1a000000)", rd32(MMSYS + MMSYS_CG_CON0));
	wr32(MMSYS + MMSYS_CG_CON0 + 8, MM_SMI_CAM | MM_SMI_COMMON);
	wr32(CAMSYS + CAMSYS_CG_CON + 8, CAMSYS_PRE);
	say("CAM domain: SRAM awake");
	cam_con(1u << 8, 0);
	if (!cam_poll(SPM + CAM_PWR_CON, 1u << 12, 0, 20)) {
		say("  SRAM 0 DID NOT ACKNOWLEDGE: CAM_PWR_CON %#x", rd32(SPM + CAM_PWR_CON));
		return false;
	}
	cam_con(1u << 9, 0);
	if (!cam_poll(SPM + CAM_PWR_CON, 1u << 13, 0, 20)) {
		say("  SRAM 1 DID NOT ACKNOWLEDGE: CAM_PWR_CON %#x", rd32(SPM + CAM_PWR_CON));
		return false;
	}
	say("CAM domain: bus protection released");
	wr32(INFRACFG + PROT_CLR, CAM_PROT_STEP2);
	wr32(SMI_COMMON + SMI_CLAMP + 8, CAM_SMI_CLAMP);
	wr32(INFRACFG + PROT_1_CLR, CAM_PROT_STEP1);
	return true;
}

/* [pg] the other way, with the clocks still running ([x6826b]: the bus acknowledges its protection with them); true if the status bit is down */
static bool
cam_domain_off(void)
{
	bool ok;

	say("CAM domain: bus protection on");
	wr32(INFRACFG + PROT_1_SET, CAM_PROT_STEP1);
	ok = cam_poll(INFRACFG + PROT_STA1_1, CAM_PROT_STEP1, CAM_PROT_STEP1, 20);
	wr32(INFRACFG + PROT_SET, CAM_PROT_STEP2);
	ok &= cam_poll(INFRACFG + PROT_STA1, CAM_PROT_STEP2, CAM_PROT_STEP2, 20);
	wr32(SMI_COMMON + SMI_CLAMP + 4, CAM_SMI_CLAMP);
	ok &= cam_poll(SMI_COMMON + SMI_CLAMP, CAM_SMI_CLAMP, CAM_SMI_CLAMP, 20);
	if (!ok) {
		say("  BUS PROTECTION DID NOT ACKNOWLEDGE (STA1 %#x, STA1_1 %#x, clamp %#x): the domain is left on", rd32(INFRACFG + PROT_STA1),
		    rd32(INFRACFG + PROT_STA1_1), rd32(SMI_COMMON + SMI_CLAMP));
		return false;
	}
	say("CAM domain: SRAM to sleep, isolation on, clock off, reset, power off");
	cam_con(0, 3u << 8);
	cam_poll(SPM + CAM_PWR_CON, 3u << 12, 3u << 12, 20);
	cam_con(0, PWR_ISO);
	cam_con(0, PWR_CLK_DIS);
	cam_con(PWR_RST_B, 0);
	cam_con(PWR_ON, 0);
	cam_con(PWR_ON_2ND, 0);
	return cam_poll(SPM + PWR_STATUS, CAM_STA, 0, 20) && cam_poll(SPM + PWR_STATUS_2ND, CAM_STA, 0, 20);
}

static bool
cam_pmic_is_mt6357(void)
{
	uint16_t hw = 0, sw = 0;
	bool ok = pmic_read(MT6357_HWCID, &hw) && pmic_read(MT6357_SWCID, &sw) && (hw >> 8) == 0x57;      /* (this r1 reads 0x5730: logs/gpu*.log) */

	say("PMIC HWCID %#x SWCID %#x: %s", hw, sw, ok ? "MT6357" : "NOT THE MT6357 THIS WAS WRITTEN FOR");
	return ok;
}

/* r1cam=1: nothing is written. The camera's own blocks (0x1a000000) are read only if their domain is already on. */
static void
cam_report(const char *when)
{
	uint16_t io = 0, d = 0, a = 0, vosel = 0;
	uint32_t status = rd32(SPM + PWR_STATUS), second = rd32(SPM + PWR_STATUS_2ND), cfg1 = rd32(TOPCKGEN + CLK_CFG_1);

	say("%s:", when);
	if (pmic_read(MT6357_VCAMIO_CON0, &io) && pmic_read(MT6357_VCAMD_CON0, &d) && pmic_read(MT6357_VCAMA_CON0, &a) &&
	    pmic_read(MT6357_VCAMD_ANA_CON0, &vosel)) {
		say("  rails: VCAMIO %s (%#x), VCAMD %s (%#x) voltage field %#x (0x600 = 1.2 V), VCAMA %s (%#x; Android: off)", (io & 1) ? "ON" : "off", io,
		    (d & 1) ? "ON" : "off", d, vosel & 0xf00, (a & 1) ? "ON" : "off", a);
	} else {
		say("  rails: THE PMIC WRAPPER DID NOT ANSWER (WACS2_RDATA %#x)", rd32(PWRAP + PWRAP_WACS2_RDATA));
	}
	for (unsigned i = 0; i < CAM_NPINS; i++) {
		static const char *const name[] = { "power-down", "MCLK", "reset", "SCL", "SDA", "analog supply" };
		uint32_t pin = cam_pins[i];

		say("  GPIO%u (%s): function %u, %s, out %u, reads %u", pin, name[i], cam_pin_mode_get(pin), cam_pin_field(pin, 0) ? "output" : "input",
		    cam_pin_field(pin, 0x100), cam_pin_field(pin, 0x200));
	}
	say("  PWR_STATUS %#x / %#x: display %s, CAM %s; CAM_PWR_CON %#x", status, second, (status & second & DIS_STA) ? "on" : "OFF",
	    (status & second & CAM_STA) ? "ON" : "off", rd32(SPM + CAM_PWR_CON));
	say("  bus protection: STA1 %#x (CAM bit 20 %s), STA1_1 %#x (CAM bits 19, 21 %s)", rd32(INFRACFG + PROT_STA1),
	    (rd32(INFRACFG + PROT_STA1) & CAM_PROT_STEP2) ? "protected" : "released", rd32(INFRACFG + PROT_STA1_1),
	    (rd32(INFRACFG + PROT_STA1_1) & CAM_PROT_STEP1) ? "protected" : "released");
	say("  CLK_CFG_1 %#x: camtg_sel parent %u (1 = 24 MHz), %s; CLK_CFG_6 %#x: seninf_sel parent %u, %s; CLK_CFG_7 %#x: camtm_sel parent %u, %s",
	    cfg1, (cfg1 >> 16) & 7, (cfg1 & CAMTG_PDN) ? "gated" : "running", rd32(TOPCKGEN + CLK_CFG_6), (rd32(TOPCKGEN + CLK_CFG_6) >> 16) & 3,
	    (rd32(TOPCKGEN + CLK_CFG_6) & SENINF_PDN) ? "gated" : "running", rd32(TOPCKGEN + CLK_CFG_7), (rd32(TOPCKGEN + CLK_CFG_7) >> 8) & 3,
	    (rd32(TOPCKGEN + CLK_CFG_7) & CAMTM_PDN) ? "gated" : "running");
	say("  AP_PLL_CON5 %#x (MIPI 26 MHz: bits 6, 11)", rd32(APMIXED_AP_PLL_CON5));
	if (status & second & DIS_STA) {
		uint32_t cg = rd32(MMSYS + MMSYS_CG_CON0);

		say("  mmsys CG_CON0 %#x: mm_smi_cam_ck (bit 25) %s, the SMI's own (bits 19..22) %s", cg, (cg & MM_SMI_CAM) ? "gated" : "running",
		    (cg & MM_SMI_COMMON) ? "GATED" : "running");
		if (!(cg & MM_SMI_COMMON)) {
			say("  SMI clamp %#x (CAM bit 3)", rd32(SMI_COMMON + SMI_CLAMP));
		}
	}
	if ((status & second & CAM_STA) && !(rd32(INFRACFG + PROT_STA1) & CAM_PROT_STEP2)) {
		say("  the CAM domain is on: camsys CG_CON %#x, SENINF top +0x0 %#x +0x8 %#x +0x10 %#x +0x24 %#x", rd32(CAMSYS), rd32(0x1a040000ULL),
		    rd32(0x1a040008ULL), rd32(0x1a040010ULL), rd32(0x1a040024ULL));
	}
}

/* what power_on changed, for power_off */
static struct {
	struct cam_pin_state pin[CAM_NPINS];
	uint32_t cfg1, cfg6, cfg7, mm_cg, apmix, csr[3];
	uint16_t vosel;
	bool domain, csr_saved, io, d;
} cam_pw;

static void cam_power_off(void);

/*
 * The order: [trace] the camera's clock gates, then VCAMIO, then VCAMD 5.8 ms later, the whole power-on 48 ms; [hi846v]'s table for this
 * sensor on an MT6765 phone: reset low, DOVDD 1 ms, AVDD 1 ms, DVDD 1 ms, MCLK, reset high 1 ms. The power-down pin (high while Android
 * streams: cg1) goes up before the reset, as Linux's hi846_power_on releases "shutdown" before "reset". NOT known: where in Android's
 * sequence GPIO97 moves (pin changes are not in the trace).
 */
static bool
cam_power_on(void)
{
	uint16_t io = 0, d = 0;
	uint32_t ones = 0;

	bzero(&cam_pw, sizeof(cam_pw));
	if (!cam_pmic_is_mt6357()) {
		return false;
	}
	if (!(rd32(SPM + PWR_STATUS) & rd32(SPM + PWR_STATUS_2ND) & DIS_STA)) {
		say("the display's power domain is off (PWR_STATUS %#x): the CAM domain is its child ([pg] PGATE2 pg_cam, pg_dis): stopping", rd32(SPM + PWR_STATUS));
		return false;
	}
	if (rd32(SPM + PWR_STATUS) & CAM_STA) {
		say("the CAM domain is on already: not ours, stopping");
		return false;
	}
	if (!pmic_read(MT6357_VCAMIO_CON0, &io) || !pmic_read(MT6357_VCAMD_CON0, &d) || !pmic_read(MT6357_VCAMD_ANA_CON0, &cam_pw.vosel) || ((io | d) & 1)) {
		say("a camera rail is on already or unreadable (VCAMIO %#x, VCAMD %#x): stopping", io, d);
		return false;
	}
	for (unsigned i = 0; i < CAM_NPINS; i++) {
		cam_pin_save(&cam_pw.pin[i], cam_pins[i]);
	}
	cam_pw.cfg1 = rd32(TOPCKGEN + CLK_CFG_1);
	cam_pw.cfg6 = rd32(TOPCKGEN + CLK_CFG_6);
	cam_pw.cfg7 = rd32(TOPCKGEN + CLK_CFG_7);
	cam_pw.mm_cg = rd32(MMSYS + MMSYS_CG_CON0);
	cam_pw.apmix = rd32(APMIXED_AP_PLL_CON5);

	say("power: the pins quiet (reset, power-down, analog supply low; MCLK a plain low pin)");
	cam_pin_mode(PIN_RST, 0);
	cam_pin_out(PIN_RST, 0);
	cam_pin_mode(PIN_PDN, 0);
	cam_pin_out(PIN_PDN, 0);
	cam_pin_mode(PIN_AVDD, 0);
	cam_pin_out(PIN_AVDD, 0);
	cam_pin_mode(PIN_MCLK, 0);
	cam_pin_out(PIN_MCLK, 0);

	cam_pw.domain = true;
	if (!cam_domain_on()) {
		goto fail;
	}
	say("power: the camera's gates (camsys %#x), MIPI 26 MHz, csi0a and csi2a/b CSR, seninf_sel, camtm_sel", rd32(CAMSYS + CAMSYS_CG_CON));
	wr32(CAMSYS + CAMSYS_CG_CON + 8, CAMSYS_REST);
	wr32(APMIXED_AP_PLL_CON5, cam_pw.apmix | APMIXED_MIPI);
	for (unsigned n = 0; n < 3; n++) {
		cam_pw.csr[n] = rd32(cam_csr[n]);
		cam_pw.csr_saved = true;
		wr32(cam_csr[n], cam_pw.csr[n] | 2u);
	}
	wr32(TOPCKGEN + CLK_CFG_6 + 8, SENINF_PDN);
	wr32(TOPCKGEN + CLK_CFG_7 + 8, CAMTM_PDN);
	say("power: camtg_sel to 24 MHz (parent 1; it was %u), ungated", (cam_pw.cfg1 >> 16) & 7);
	wr32(TOPCKGEN + CLK_CFG_1 + 8, CAMTG_MASK);     /* [clk] mtk_clk_mux_set_parent_setclr_lock: clear the field, set it, the update bit */
	wr32(TOPCKGEN + CLK_CFG_1 + 4, CAMTG_24M);
	wr32(TOPCKGEN + CLK_CFG_UPDATE, CAMTG_UPDATE);
	wr32(TOPCKGEN + CLK_CFG_1 + 8, CAMTG_PDN);
	say("  camsys CG_CON %#x, CLK_CFG_1 %#x, CLK_CFG_6 %#x, CLK_CFG_7 %#x, AP_PLL_CON5 %#x", rd32(CAMSYS + CAMSYS_CG_CON), rd32(TOPCKGEN + CLK_CFG_1),
	    rd32(TOPCKGEN + CLK_CFG_6), rd32(TOPCKGEN + CLK_CFG_7), rd32(APMIXED_AP_PLL_CON5));

	say("power: VCAMIO (1.8 V) on");
	if (!(cam_pw.io = pmic_set(MT6357_VCAMIO_CON0, 1, 1))) {
		say("  DOES NOT READ BACK: stopping");
		goto fail;
	}
	delay(2000);
	cam_pin_out(PIN_AVDD, 1);
	delay(2000);
	say("power: analog supply GPIO165 high (reads %d); VCAMD to 1.2 V (its field was %#x), on", cam_pin_read(PIN_AVDD), cam_pw.vosel & 0xf00);
	if (!pmic_set(MT6357_VCAMD_ANA_CON0, 0xf00, MT6357_VCAMD_1V2)) {
		say("  THE VOLTAGE DOES NOT READ BACK: VCAMD stays off, stopping");
		goto fail;
	}
	if (!(cam_pw.d = pmic_set(MT6357_VCAMD_CON0, 1, 1))) {
		say("  DOES NOT READ BACK: stopping");
		goto fail;
	}
	delay(2000);
	cam_pin_mode(PIN_MCLK, 1);
	delay(1000);
	for (unsigned i = 0; i < 2000; i++) {
		ones += (uint32_t)cam_pin_read(PIN_MCLK);
	}
	say("power: MCLK on GPIO99 (function 1): %u of 2000 reads high (a clock: neither 0 nor 2000)", ones);
	cam_pin_out(PIN_PDN, 1);
	delay(1000);
	cam_pin_out(PIN_RST, 1);
	say("power: power-down GPIO97 high (reads %d), reset GPIO101 high (reads %d); 40 ms", cam_pin_read(PIN_PDN), cam_pin_read(PIN_RST));
	delay(40000);

	cam_pin_mode(PIN_SCL, 0);
	cam_pin_mode(PIN_SDA, 0);
	cam_bb_line(PIN_SCL, 1);
	cam_bb_line(PIN_SDA, 1);
	delay(100);
	say("power: I2C by hand on GPIO103/104 (they were function %u and %u): idle SCL reads %d, SDA reads %d (1 and 1 expected)", cam_pw.pin[3].mode,
	    cam_pw.pin[4].mode, cam_pin_read(PIN_SCL), cam_pin_read(PIN_SDA));
	return true;
fail:
	cam_power_off();
	return false;
}

static void
cam_power_off(void)
{
	say("power: off");
	cam_pin_out(PIN_RST, 0);
	cam_pin_out(PIN_PDN, 0);
	cam_pin_mode(PIN_MCLK, 0);
	delay(1000);
	if (cam_pw.d) {
		say("  VCAMD off: %s", pmic_set(MT6357_VCAMD_CON0, 1, 0) ? "ok" : "NO");
		pmic_set(MT6357_VCAMD_ANA_CON0, 0xf00, cam_pw.vosel & 0xf00);
	}
	cam_pin_out(PIN_AVDD, 0);
	delay(1000);
	if (cam_pw.io) {
		say("  VCAMIO off: %s", pmic_set(MT6357_VCAMIO_CON0, 1, 0) ? "ok" : "NO");
	}
	for (unsigned i = 0; i < CAM_NPINS; i++) {
		cam_pin_restore(&cam_pw.pin[i], cam_pins[i]);
	}
	if (cam_pw.domain) {
		if (cam_pw.csr_saved) {
			for (unsigned n = 0; n < 3; n++) {
				wr32(cam_csr[n], cam_pw.csr[n]);
			}
			wr32(APMIXED_AP_PLL_CON5, cam_pw.apmix);
		}
		if (rd32(SPM + PWR_STATUS) & CAM_STA) {
			say("  %s", cam_domain_off() ? "the CAM domain is off again" : "THE CAM DOMAIN DID NOT GO DOWN");
		} else {
			cam_con(PWR_ON | PWR_ON_2ND, 0);        /* (it never came up) */
		}
		wr32(MMSYS + MMSYS_CG_CON0 + 4, cam_pw.mm_cg & (MM_SMI_CAM | MM_SMI_COMMON));
		wr32(TOPCKGEN + CLK_CFG_1 + 4, cam_pw.cfg1 & CAMTG_PDN);
		wr32(TOPCKGEN + CLK_CFG_1 + 8, CAMTG_MASK);
		wr32(TOPCKGEN + CLK_CFG_1 + 4, cam_pw.cfg1 & CAMTG_MASK);
		wr32(TOPCKGEN + CLK_CFG_UPDATE, CAMTG_UPDATE);
		wr32(TOPCKGEN + CLK_CFG_6 + 4, cam_pw.cfg6 & SENINF_PDN);
		wr32(TOPCKGEN + CLK_CFG_7 + 4, cam_pw.cfg7 & CAMTM_PDN);
	}
	bzero(&cam_pw, sizeof(cam_pw));
}

/* ---- the sensor ---- */
#define HI846_REG_FLL           0x0006  /* [hi846] the frame length in lines, 16 bits */
#define HI846_REG_EXPOSURE      0x0074  /* [hi846] 16 bits, lines */
#define HI846_REG_GAIN          0x0077  /* [hi846] 8 bits, 0..240; [hi846v] gain2reg: (gain / 4) - 16 with 64 for 1x, so 16 * (gain - 1) */
#define HI846_REG_MODE_SELECT   0x0a00  /* [hi846] 8 bits: 1 streaming, 0 standby */
#define HI846_REG_CHIP_ID_L     0x0f16  /* [hi846] 0x46 */
#define HI846_REG_CHIP_ID_H     0x0f17  /* [hi846] 0x08 */
/* the size: [hi846] mode_1632x1224 (0x0a12 = 0x0660, 0x0a14 = 0x04c8; /proc/driver/camera_info "Pre 1632x1224"), or with boot-arg r1cam_mode=1 the
 * vendor's 3264x2448 at the same 30 frames/s ([r1k] the r1's own hi846_mipi_raw_Sensor-4lane.c, github.com/rabbit-hmi-oss/android_kernel_rabbit_mt6765:
 * .cap pclk 288 MHz, line 3800, frame 2526; i2c_addr_table 0x40 >> 1). It is the mode Android was in for [dump]/[dump2] (the mux measured 3264x2448). */
static uint32_t cam_w = 1632, cam_h = 1224, cam_mode;
#define CAM_W                   cam_w
#define CAM_H                   cam_h
#define CAM_FRAME_MIN           2526u   /* [hi846] .frame_len, 0x0006 = 0x09de; [hi846v] pre.framelength */
#define CAM_FRAME_MAX           0xffffu /* [hi846] HI846_FLL_MAX */
#define CAM_EXP_MIN             6u      /* [hi846] HI846_EXPOSURE_MIN */
#define CAM_EXP_MARGIN          6u      /* [hi846] hi846_set_ctrl "margin"; [hi846v] .margin */
#define CAM_EXP_TABLE           0x09d8u /* [hi846] the mode's 0x0074 */
#define CAM_LINE_NS             13194u  /* [hi846v] 3800 PCLKs a line at 288 MHz (MCLK 24 MHz); 2526 of them are the 33.3 ms between Android's frames (cam.dmesg int_us) */
#define CAM_GAIN_UNITY          16u     /* the node's gain is register + 16: linear, 16 for 1x */
#define CAM_GAIN_MAX            256u    /* [hi846] HI846_ANAL_GAIN_MAX 240 + 16: 16x */
#define CAM_BLACK10             64u     /* [hi846v] "BLC offset : 64code"; the init table's 0x0c10..0x0c16 = 0x0040 */
#define CAM_BAYER               2u      /* GBRG: [hi846] MEDIA_BUS_FMT_SGBRG10_1X10; camera_info "output_format 1 (Gb)" */

static uint32_t cam_gain_arg = 64;      /* boot-arg r1cam_gain: the gain a session starts with (4x: a room is dark at 1x and 33 ms) */

static bool
cam_sensor_id(void)
{
	static const uint8_t addrs[] = { 0, 0x20, 0x21 };       /* r1cam_addr; this r1's 0x20; the chip's other address */
	uint8_t hi = 0, lo = 0;

	for (unsigned i = cam_addr == 0x20 ? 1 : 0; i < sizeof(addrs); i++) {
		cam_addr = addrs[i] ? addrs[i] : cam_addr;
		say("sensor: reading the chip ID (0x0f17, 0x0f16) at I2C address %#x", cam_addr);
		if (cam_bb_read8(HI846_REG_CHIP_ID_H, &hi) && cam_bb_read8(HI846_REG_CHIP_ID_L, &lo)) {
			say("sensor ID %#06x%s", (hi << 8) | lo, hi == 0x08 && lo == 0x46 ? " = Hi846" : " (NOT 0x0846)");
			return hi == 0x08 && lo == 0x46;
		}
		say("  no acknowledge at %#x (SCL reads %d, SDA reads %d)", cam_addr, cam_pin_read(PIN_SCL), cam_pin_read(PIN_SDA));
	}
	return false;
}

static bool
cam_sensor_list(const char *name, const struct r1cam_reg *t, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		if (!cam_bb_write_reg(t[i].reg, t[i].val, 2)) {
			say("sensor: %s: write %u of %u (register %#x) NOT ACKNOWLEDGED", name, i, n, t[i].reg);
			return false;
		}
	}
	return true;
}

static uint32_t cam_cur_exposure, cam_cur_gain, cam_cur_frame;

/* [hi846] hi846_set_ctrl: frame length, exposure; the gain 8 bits */
static void
cam_sensor_exposure(uint32_t lines, uint32_t gain, uint32_t frame)
{
	frame = frame ? frame : CAM_FRAME_MIN;
	frame = frame < CAM_FRAME_MIN ? CAM_FRAME_MIN : frame > CAM_FRAME_MAX ? CAM_FRAME_MAX : frame;
	lines = lines < CAM_EXP_MIN ? CAM_EXP_MIN : lines > CAM_FRAME_MAX - CAM_EXP_MARGIN ? CAM_FRAME_MAX - CAM_EXP_MARGIN : lines;
	if (lines + CAM_EXP_MARGIN > frame) {
		frame = lines + CAM_EXP_MARGIN;         /* (a longer frame, as Linux and the vendor do; the caller's 0 means this too) */
	}
	gain = gain < CAM_GAIN_UNITY ? CAM_GAIN_UNITY : gain > CAM_GAIN_MAX ? CAM_GAIN_MAX : gain;
	cam_bb_write_reg(HI846_REG_FLL, (uint16_t)frame, 2);
	cam_bb_write_reg(HI846_REG_EXPOSURE, (uint16_t)lines, 2);
	cam_bb_write_reg(HI846_REG_GAIN, (uint16_t)(gain - CAM_GAIN_UNITY), 1);
	cam_cur_frame = frame;
	cam_cur_exposure = lines;
	cam_cur_gain = gain;
}

/* [hi846] hi846_start_streaming, up to (not with) the stream-on write */
static bool
cam_sensor_tables(void)
{
#define N(t) ((unsigned)(sizeof(t) / sizeof(struct r1cam_reg)))
#ifdef R1CAM_HI846_FULL
	if (cam_mode) {
		say("sensor: the vendor's tables (init %u, 3264x2448 %u writes)", N(r1cam_hi846_vinit), N(r1cam_hi846_full));
		if (!cam_sensor_list("init", r1cam_hi846_vinit, N(r1cam_hi846_vinit)) || !cam_sensor_list("full", r1cam_hi846_full, N(r1cam_hi846_full))) {
			return false;
		}
	} else
#endif
	{
		say("sensor: the tables (init %u, mode %u, mipi %u writes)", N(r1cam_hi846_init), N(r1cam_hi846_mode), N(r1cam_hi846_mipi));
		if (!cam_sensor_list("init", r1cam_hi846_init, N(r1cam_hi846_init)) || !cam_sensor_list("mode", r1cam_hi846_mode, N(r1cam_hi846_mode)) ||
		    !cam_sensor_list("mipi", r1cam_hi846_mipi, N(r1cam_hi846_mipi))) {
			return false;
		}
	}
#undef N
	cam_sensor_exposure(CAM_EXP_TABLE, cam_gain_arg, CAM_FRAME_MIN);
	say("  all acknowledged; exposure %u lines, gain %u/16, frame %u lines", cam_cur_exposure, cam_cur_gain, cam_cur_frame);
	return true;
}

static void
cam_sensor_stream(bool on)
{
	uint8_t mode = 0xff;

	say("sensor: stream %s (0x0a00)", on ? "on" : "off");
	cam_bb_write_reg(HI846_REG_MODE_SELECT, on ? 1 : 0, 1);
	delay(10000);                   /* [hi846v] streaming_control: 10 ms */
	cam_bb_read8(HI846_REG_MODE_SELECT, &mode);
	say("  mode register reads %#x", mode);
}

/* ---- the receiver: the analog block, SENINF1 and its mux, CAMSV_1 ---- */
#define SENINF_TOP              0x1a040000ULL   /* the top registers, and seninf1's block */
#define SENINF                  0x1a044000ULL   /* SENINF5: CSI port 2's ([seninf] port_to_seninf_id, base + 0x1000 * 4; [dump] mux 0's source is 4) */
#define SENINF_MUX              0x1a043000ULL   /* the block whose mux (+0xd00) is mux 3: the one CAMSV_1 takes ([dump] cam-mux 0x76543210; [x6826b]) */
#define SENINF_TOP_MUX_CTRL     0x008   /* [seninf] a nibble for each mux: which SENINF feeds it; [dump] 0x00043214 */
#define SENINF_TOP_CAM_MUX_CTRL 0x010   /* [seninf] a nibble for each consumer (CAM_A, CAM_B, CAMSV_0, CAMSV_1, ..): which mux feeds it; [dump] 0x76543210, not written */
#define SENINF_TOP_PHY_CSI2     0x024   /* [seninf] SENINF_TOP_PHY_SENINF_CTL_CSI2; [dump] 0x80000200 */
#define SENINF_CTRL             0x200
#define SENINF_CTRL_EXT         0x204
#define SENINF_TG1_TM_STP       0x614
#define MIPI_RX_CON24_CSI0      0x824   /* [seninf] the data lanes' order, two bits each from bit 24 */
#define SENINF_CSI2_CTL         0xa00
#define SENINF_CSI2_LNRD_TIMING 0xa08
#define SENINF_CSI2_INT_EN      0xa10
#define SENINF_CSI2_INT_STATUS  0xa14
#define SENINF_CSI2_DGB_SEL     0xa18
#define SENINF_CSI2_SPARE0      0xa20
#define SENINF_CSI2_LNRC_FSM    0xa28
#define SENINF_CSI2_HS_TRAIL    0xa40
#define SENINF_CSI2_RESYNC      0xa74   /* SENINF_CSI2_RESYNC_MERGE_CTL */
#define SENINF_CSI2_MODE        0xae8
#define SENINF_CSI2_DPHY_SYNC   0xb20
#define SENINF_MUX_CTRL         0xd00
#define SENINF_MUX_INTSTA       0xd08
#define SENINF_MUX_DEBUG_2      0xd14   /* reads the frame the mux measured, width << 16 | height: [dump] 0x0cc00990 = 3264x2448 ([x6826b] the same way) */
#define SENINF_MUX_CTRL_EXT     0xd3c

#define CAMSV                   0x1a051000ULL   /* CAMSV_1: [x6826b]'s; vendor camera_isp.h CAMSV_1_BASE_HW */
#define SV_MODULE_EN            0x010   /* the vendor's CAMSV_REG_* offsets for this chip */
#define SV_FMT_SEL              0x014
#define SV_INT_EN               0x018
#define SV_INT_STATUS           0x01c
#define SV_SW_CTL               0x020
#define SV_IMGO_FBC             0x02c
#define SV_CLK_EN               0x030
#define SV_PAK                  0x04c
#define SV_FBC_IMGO_CTL1        0x110
#define SV_FBC_IMGO_CTL2        0x114
#define SV_FBC_IMGO_ENQ_ADDR    0x118
#define SV_SPECIAL_FUN_EN       0x218
#define SV_IMGO_BASE_ADDR       0x220
#define SV_IMGO_XSIZE           0x230
#define SV_IMGO_YSIZE           0x234
#define SV_IMGO_STRIDE          0x238
#define SV_IMGO_CON             0x23c
#define SV_IMGO_CON2            0x240
#define SV_IMGO_CON3            0x244
#define SV_DMA_ERR_CTRL         0x440
#define SV_IMGO_ERR_STAT        0x444
#define SV_DMA_RSV1             0x470
#define SV_DMA_RSV6             0x484
#define SV_TG_SEN_MODE          0x500
#define SV_TG_VF_CON            0x504
#define SV_TG_SEN_GRAB_PXL      0x508
#define SV_TG_SEN_GRAB_LIN      0x50c
#define SV_TG_PATH_CFG          0x510
#define SV_TG_FRMSIZE_ST        0x538   /* [x6826b] reads the frame the CAMSV's own sensor interface measured */
#define SV_DMA_FH_EN            0xe00
#define SV_IMGO_FH_BASE_ADDR    0xe04
#define LARB3                   0x1a002000ULL   /* [dts] smi_larb3@1a002000 */
#define LARB_NON_SEC_CON(port)  (0x380 + 4 * (port))    /* [m4u] SMI_LARB_NON_SEC_CONx, bit 0 F_SMI_MMU_EN; 21 ports (mt6765-larb-port.h M4U_LARB3_ID) */

/*
 * How the CAMSV packs: the three ways there is a reason for.
 *   0  [camsv]'s 10-bit setting to the letter (FMT_SEL 1, PAK 0x11): a line is width * 10 / 8 bytes, the pixels a little-endian bit stream
 *      (V4L2_PIX_FMT_MTISP_SGBRG10; [x6826b] read the same layout)
 *   1  the same with [x6826b]'s PAK bits 5:4 = 2 (0x21): on the MT6768 those bits choose which bits of the bus are the pixel, and 2 was right
 *   2  [x6826b]'s proven setting: a byte a pixel, the top 8 of the 10 bits (PAK 0x20)
 */
static const struct {
	uint32_t pak, bits;
	const char *name;
} cam_variants[] = {
	{ 0x11, 10, "10 bits, PAK 0x11" },
	{ 0x21, 10, "10 bits, PAK 0x21" },
	{ 0x20, 8, "8 bits, PAK 0x20" },
};
#define CAM_NVARIANTS           (sizeof(cam_variants) / sizeof(cam_variants[0]))
static uint32_t cam_variant, cam_pitch;
static uint32_t cam_settle = 0x15;      /* [dump2] +0x4a08 = 0x00001500 (and [seninf]'s SENINF_SETTLE_DELAY); boot-arg r1cam_settle */

/*
 * The frame memory: three images and one piece for the three frame headers, each wired and in one piece below 4 GB (the DMA writes an image from the
 * address it is given, 32 bits, with no translation), but not next to one another: IMGO_BASE_ADDR is written for every frame (cam_arm), and a piece of
 * 2.5 MB is found where one of 8 MB is not. Taken ONCE, when the driver's thread starts (logs/camgl1.log: taken at the first open, twenty minutes after
 * boot with 11 MB free, there was no such piece and the camera stayed black), and kept for the life of the system.
 */
#define CAM_NBUF                3u
#define CAM_IMG_BYTES           (cam_mode ? 0x990000u : 0x280000u)      /* 4080 x 2448 = 9 987 840; 2040 x 1224 = 2 496 960 */
#define CAM_HDR_BYTES           0x10000u
static vm_offset_t cam_piece[CAM_NBUF + 1];     /* the images, then the headers */
static uint64_t cam_piece_pa[CAM_NBUF + 1];
static bool cam_buf_have;

static volatile uint8_t *
cam_img(unsigned k)
{
	return (volatile uint8_t *)cam_piece[k];
}

static bool
cam_buf_make(void)
{
	if (cam_buf_have) {
		return true;
	}
	for (unsigned k = 0; k <= CAM_NBUF; k++) {
		vm_size_t size = k < CAM_NBUF ? CAM_IMG_BYTES : CAM_NBUF * CAM_HDR_BYTES;
		uint64_t pa;

		if (cam_piece[k]) {
			continue;
		}
		/* (the limit first; without it if this kernel's page numbers are not what the limit assumes, and then the address is checked) */
		if (kmem_alloc_contig(kernel_map, &cam_piece[k], size, PAGE_MASK, (ppnum_t)(atop_64(0x100000000ULL) - 1), 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS &&
		    kmem_alloc_contig(kernel_map, &cam_piece[k], size, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			say("NO MEMORY for the frames: piece %u of %u (%u KiB in one piece)", k, CAM_NBUF + 1, (unsigned)(size >> 10));
			cam_piece[k] = 0;
			return false;           /* (what was taken is kept: the next try takes the rest) */
		}
		pa = kvtophys(cam_piece[k]);
		if (kvtophys(cam_piece[k] + size - 4096) != pa + size - 4096 || pa + size > 0x100000000ULL) {
			say("FRAME MEMORY NOT IN ONE PIECE BELOW 4 GB: piece %u at physical %#llx, its last page %#llx", k, pa, kvtophys(cam_piece[k] + size - 4096));
			kmem_free(kernel_map, cam_piece[k], size);
			cam_piece[k] = 0;
			return false;
		}
		FlushPoC_DcacheRegion(cam_piece[k], size);
		cam_piece_pa[k] = pa;
	}
	cam_buf_have = true;
	say("frame memory reserved: 3 images of %u KiB at physical %#llx, %#llx, %#llx; headers at %#llx; kept until the system stops", CAM_IMG_BYTES >> 10,
	    cam_piece_pa[0], cam_piece_pa[1], cam_piece_pa[2], cam_piece_pa[3]);
	return true;
}

/*
 * The DMA's next frame goes into image `k`. logs/cam5.log: on this chip the frame-buffer queue counts ([x6826b]'s address, header, THEN the ready bit:
 * FBC CTL2 goes 0x303, then "written" 3, then "dropped" rising) but does not give the DMA its address: the frames went where IMGO_BASE_ADDR pointed,
 * which was 0 until it was written by hand, and then a full frame was in memory. So: the address into IMGO_BASE_ADDR (and the queue's registers, as
 * before), then ONE count (IMGO_FBC bit 0). One count at a time: a frame that starts before this was called finds the count at 0 and is dropped whole,
 * it cannot overwrite the image just finished. The tail is zeroed first: its bytes say when the frame is whole.
 */
static unsigned cam_cur;                /* the image the DMA has */

static void
cam_arm(unsigned k)
{
	volatile uint64_t *tail = (volatile uint64_t *)(cam_img(k) + (uint64_t)cam_pitch * CAM_H - 64);
	uint32_t pa = (uint32_t)cam_piece_pa[k];

	for (unsigned i = 0; i < 8; i++) {
		tail[i] = 0;
	}
	FlushPoC_DcacheRegion((vm_offset_t)tail, 64);
	wr32(CAMSV + SV_IMGO_BASE_ADDR, pa);
	wr32(CAMSV + SV_FBC_IMGO_ENQ_ADDR, pa);
	wr32(CAMSV + SV_IMGO_FH_BASE_ADDR, (uint32_t)(cam_piece_pa[CAM_NBUF] + (uint64_t)k * CAM_HDR_BYTES));
	wr32(CAMSV + SV_IMGO_FBC, 1);
	cam_cur = k;
}

static bool
cam_tail_written(unsigned k)
{
	volatile uint64_t *tail = (volatile uint64_t *)(cam_img(k) + (uint64_t)cam_pitch * CAM_H - 64);
	uint64_t any = 0;

	FlushPoC_DcacheRegion((vm_offset_t)tail, 64);
	for (unsigned i = 0; i < 8; i++) {
		any |= tail[i];
	}
	return any != 0;
}

/*
 * csi2a and csi2b as [dump2] has them while Android streams, by [phy] mtk_mipi_phy_power_on's steps: ANA00 0x1a4e / 0x124e (the clock on csi2a's
 * lane 2: CKMODE_EN bit 11 there only; CKSEL bits 6, 9, 12; the bandgap's bits 3, 2 last), ANA18 0x88518851, ANA1C 0x8851, ANA24 0x40000000 (RESERVE
 * 0x40); and what [phy] does not write: the lanes' delays ANA08 / ANA0C and ANA48 (this phone's own values). +0x80 and +0xa8 already are as [phy] sets them.
 */
static void
cam_phy(bool on)
{
	static const struct {
		uint16_t off;
		uint32_t val[2];
	} live[] = { { 0x08, { 0x0c0c0d0d, 0x0d0d0c0c } }, { 0x0c, { 0x0c0c, 0x1414 } }, { 0x18, { 0x88518851, 0x88518851 } }, { 0x1c, { 0x8851, 0x8851 } },
		     { 0x24, { 0x40000000, 0x40000000 } }, { 0x48, { 0x438, 0x238 } } };

	for (unsigned n = 0; n < 2; n++) {
		uint64_t b = MIPIRX + 0x1000 * n;

		if (!on) {
			set32(b + 0x00, (1u << 3) | (1u << 2), 0);      /* mtk_mipi_phy_power_off: BG_CORE_EN, BG_LPF_EN */
			continue;
		}
		set32(b + 0x00, (1u << 5) | (1u << 6) | (1u << 8) | (1u << 9) | (1u << 11) | (1u << 12), (1u << 6) | (1u << 9) | (n == 0 ? 1u << 11 : 0) | (1u << 12));
		for (unsigned i = 0; i < sizeof(live) / sizeof(live[0]); i++) {
			wr32(b + live[i].off, live[i].val[n]);
		}
	}
	if (on) {
		set32(MIPIRX + 0x00, 1u << 3, 1u << 3);                 /* BG_CORE_EN, both; 20..40 us; then BG_LPF_EN */
		set32(MIPIRX + 0x1000, 1u << 3, 1u << 3);
		delay(30);
		set32(MIPIRX + 0x00, 1u << 2, 1u << 2);
		set32(MIPIRX + 0x1000, 1u << 2, 1u << 2);
	}
}

/* [seninf] mtk_seninf_power_on for CSI_PORT_2 in 4D1C: csi2_setup_phy, (the PHY), csi2_rx_config, csi2_set_mipi, set_mux; where [dump] has Android's value, that */
static void
cam_seninf_start(void)
{
	uint32_t v;

	/* [dump] 0x80000200: DPHY_MODE (bit 0) 0 = 4D1C, CK_SEL_1 (10:8) = lane 2, the lane mux enabled (31) */
	set32(SENINF_TOP + SENINF_TOP_PHY_CSI2, 1u | (7u << 8) | (1u << 31), (2u << 8) | (1u << 31));
	cam_phy(true);
	wr32(SENINF_TOP + SENINF_TG1_TM_STP, 0x67);                             /* [dump] +0x614 = 0x67, in the first block; TG1_PH_CNT, TG1_SEN_CK: Android has their reset values */
	set32(SENINF_TOP + SENINF_CTRL, 1u, 1u);                                /* [dump] +0x200 = 1, +0x204 = 0x40: Android has seninf1 enabled too */
	set32(SENINF_TOP + SENINF_CTRL_EXT, (1u << 6) | (1u << 5), 1u << 6);
	set32(SENINF + SENINF_CTRL, 1u | (0xfu << 12) | (7u << 28), 1u);        /* SENINF_EN; SRC_SEL 0; PAD2CAM_DATA_SEL 0 (10 bits) */
	set32(SENINF + SENINF_CTRL_EXT, (1u << 6) | (1u << 5), 1u << 6);        /* CSI2_IP_EN, not NCSI2_IP_EN */
	/* from here every value is [dump2]'s (SENINF5 +0x824 0xc9000000, +0xa00 0x0209617f, +0xa08 0x1500, +0xa10 0xfdffffff, +0xa18 0x8000001a, +0xa20 0xfffffffe,
	 * +0xa28 0x10, +0xa40 0x1b, +0xa74 3, +0xb10 0x1e, +0xb20 0x001dff00; +0xa0c is 0 there and is not written), in [seninf]'s order */
	set32(SENINF + MIPI_RX_CON24_CSI0, 0xffu << 24, 0xc9u << 24);          /* the sensor's lanes 0..3 on 1, 2, 0, 3: [seninf]'s example, [phy]'s mapping */
	set32(SENINF + SENINF_CSI2_LNRD_TIMING, 0xffu << 8, cam_settle << 8);  /* DATA_SETTLE_PARAMETER */
	wr32(SENINF + SENINF_CSI2_LNRC_FSM, 0x10);
	wr32(SENINF + SENINF_CSI2_CTL, rd32(SENINF + SENINF_CSI2_CTL) | (1u << 16) | 0x10 | 0xf);        /* header order, clock lane, four data lanes: reset 0x00086160 -> 0x0209617f with bit 25 below */
	set32(SENINF + SENINF_CSI2_RESYNC, 7u | (1u << 10) | (1u << 11), 3u);  /* CPHY_LANE_RESYNC_CNT 3; no bypass; D-PHY */
	set32(SENINF + SENINF_CSI2_MODE, 0xffu | (7u << 8), 0);
	wr32(SENINF + SENINF_CSI2_DPHY_SYNC, (0x001du << 16) | 0xff00u);        /* SYNC_SEQ_PAT_0, SYNC_SEQ_MASK_0 ([dump]: the reset value too) */
	set32(SENINF + SENINF_CSI2_CTL, (1u << 27) | (1u << 7) | (1u << 25), 1u << 25);        /* not CLOCK_HS_OPTION, not HSRX_DET_EN; HS_TRAIL_EN */
	set32(SENINF + SENINF_CSI2_HS_TRAIL, 0xffu, 0x1b);
	set32(SENINF + SENINF_CSI2_DGB_SEL, (1u << 31) | 0xffu, (1u << 31) | 0x1a);
	wr32(SENINF + SENINF_CSI2_SPARE0, 0xfffffffe);
	wr32(SENINF + SENINF_CSI2_INT_EN, 0xfdffffff);
	wr32(SENINF + 0xb10, 0x1e);                                             /* SENINF_CSI2_INT_EN_EXT */
	wr32(SENINF + SENINF_CSI2_INT_STATUS, 0xffffffff);
	set32(SENINF + SENINF_CTRL, 1u << 7, 1u << 7);                          /* CSI2_SW_RST, 1 us */
	delay(1);
	set32(SENINF + SENINF_CTRL, 1u << 7, 0);

	/* mux 3 as Android has mux 0 ([dump] +0xd00 = 0xa6df8180, +0xd3c = 0x101; an idle mux has 0x06df0080 and 0): enabled (31), source 8 = the
	 * CSI-2 sensor (15:12), two pixels a clock (PIX_SEL bit 8; CAM_A's sensor interface has DBL_DATA_BUS with it, as CAMSV_1's below),
	 * FIFO_FULL_WR_EN 2 (29:28); +0xd2c is not written (Android has its reset value 0xe2000) */
	set32(SENINF_MUX + SENINF_MUX_CTRL, (1u << 31) | (0xfu << 12) | (7u << 8) | (3u << 28) | (0x3fu << 22) | (0x3fu << 16),
	    (1u << 31) | (8u << 12) | (1u << 8) | (2u << 28) | (0x1bu << 22) | (0x1fu << 16));
	wr32(SENINF_MUX + SENINF_MUX_CTRL_EXT, 0x101);
	v = rd32(SENINF_MUX + SENINF_MUX_CTRL);
	wr32(SENINF_MUX + SENINF_MUX_CTRL, v | 3u);                             /* the mux's reset pulse */
	wr32(SENINF_MUX + SENINF_MUX_CTRL, v & ~3u);
	/* [dump] 0x00043214: Android feeds mux 0 from SENINF5 (4) for CAM_A; the same for mux 3 (nibble 3), which CAMSV_1 takes. This nibble is the
	 * whole difference between Android's path and this one. */
	set32(SENINF_TOP + SENINF_TOP_MUX_CTRL, 0xfu << 12, 4u << 12);
	say("SENINF5: CSI2_CTL %#x, lanes %#x, settle %#x; mux 3 %#x, EXT %#x; top mux %#x, cam mux %#x, PHY ctl %#x; csi2a ANA00 %#x, csi2b %#x",
	    rd32(SENINF + SENINF_CSI2_CTL), rd32(SENINF + MIPI_RX_CON24_CSI0), rd32(SENINF + SENINF_CSI2_LNRD_TIMING), rd32(SENINF_MUX + SENINF_MUX_CTRL),
	    rd32(SENINF_MUX + SENINF_MUX_CTRL_EXT), rd32(SENINF_TOP + SENINF_TOP_MUX_CTRL), rd32(SENINF_TOP + SENINF_TOP_CAM_MUX_CTRL),
	    rd32(SENINF_TOP + SENINF_TOP_PHY_CSI2), rd32(MIPIRX), rd32(MIPIRX + 0x1000));
}

/* the sensor streams: does mux 3 measure the frame (300 ms at most)? */
static bool
cam_seninf_lock(void)
{
	for (unsigned ms = 10; ms <= 300; ms += 10) {
		delay(10000);
		if (rd32(SENINF_MUX + SENINF_MUX_DEBUG_2) == ((CAM_W << 16) | CAM_H)) {
			say("SENINF: mux 3 measures %ux%u after %u ms", CAM_W, CAM_H, ms);
			return true;
		}
	}
	say("SENINF: MUX 3 MEASURES %#x, NOT THE FRAME (CSI2 INT %#x, CSI2_CTL %#x): the sensor does not stream, or the receiver does not lock (r1cam_settle)",
	    rd32(SENINF_MUX + SENINF_MUX_DEBUG_2), rd32(SENINF + SENINF_CSI2_INT_STATUS), rd32(SENINF + SENINF_CSI2_CTL));
	return false;
}

/* CAMSV_1's registers and the larb's ports, a few to a line (the kept log cuts a line at about 200 characters) */
static void
cam_sv_dump(const char *when)
{
	say("SV %s: MODULE_EN %#x FMT_SEL %#x INT_EN %#x SW_CTL %#x IMGO_FBC %#x CLK_EN %#x PAK %#x", when, rd32(CAMSV + SV_MODULE_EN), rd32(CAMSV + SV_FMT_SEL),
	    rd32(CAMSV + SV_INT_EN), rd32(CAMSV + SV_SW_CTL), rd32(CAMSV + SV_IMGO_FBC), rd32(CAMSV + SV_CLK_EN), rd32(CAMSV + SV_PAK));
	say("SV %s: FBC CTL1 %#x CTL2 %#x (count 6:0, queued 13:8, written 21:16, dropped 31:24) ENQ %#x CUR %#x", when, rd32(CAMSV + SV_FBC_IMGO_CTL1),
	    rd32(CAMSV + SV_FBC_IMGO_CTL2), rd32(CAMSV + SV_FBC_IMGO_ENQ_ADDR), rd32(CAMSV + 0x11c));
	say("SV %s: IMGO BASE %#x OFST %#x XSIZE %#x YSIZE %#x STRIDE %#x CON %#x %#x %#x CROP %#x", when, rd32(CAMSV + SV_IMGO_BASE_ADDR), rd32(CAMSV + 0x228),
	    rd32(CAMSV + SV_IMGO_XSIZE), rd32(CAMSV + SV_IMGO_YSIZE), rd32(CAMSV + SV_IMGO_STRIDE), rd32(CAMSV + SV_IMGO_CON), rd32(CAMSV + SV_IMGO_CON2),
	    rd32(CAMSV + SV_IMGO_CON3), rd32(CAMSV + 0x248));
	say("SV %s: DMA RSTSTAT %#x SPECIAL %#x ERR_CTRL %#x IMGO_ERR %#x DEBUG_ADDR %#x RSV1 %#x RSV6 %#x FH_EN %#x FH_BASE %#x", when, rd32(CAMSV + 0x200),
	    rd32(CAMSV + SV_SPECIAL_FUN_EN), rd32(CAMSV + SV_DMA_ERR_CTRL), rd32(CAMSV + SV_IMGO_ERR_STAT), rd32(CAMSV + 0x46c), rd32(CAMSV + SV_DMA_RSV1),
	    rd32(CAMSV + SV_DMA_RSV6), rd32(CAMSV + SV_DMA_FH_EN), rd32(CAMSV + SV_IMGO_FH_BASE_ADDR));
	say("SV %s: TG SEN_MODE %#x VF %#x PXL %#x LIN %#x PATH %#x MEMIN %#x FRMSIZE %#x INTER %#x", when, rd32(CAMSV + SV_TG_SEN_MODE), rd32(CAMSV + SV_TG_VF_CON),
	    rd32(CAMSV + SV_TG_SEN_GRAB_PXL), rd32(CAMSV + SV_TG_SEN_GRAB_LIN), rd32(CAMSV + SV_TG_PATH_CFG), rd32(CAMSV + 0x514), rd32(CAMSV + SV_TG_FRMSIZE_ST),
	    rd32(CAMSV + 0x53c));
	say("SV %s: TOP_DEBUG %#x DBG_SET %#x DBG_PORT %#x DCM_DIS %#x DCM_STATUS %#x DMA_DEBUG_SEL %#x; TG SOF_CNT %#x DAT_NO %#x FRM_CNT %#x", when, rd32(CAMSV),
	    rd32(CAMSV + 0x34), rd32(CAMSV + 0x38), rd32(CAMSV + 0x44), rd32(CAMSV + 0x48), rd32(CAMSV + 0x488), rd32(CAMSV + 0x520), rd32(CAMSV + 0x530),
	    rd32(CAMSV + 0x534));
	/* vendor emi/mt6765/mt_emi.h, emi/submodule/mpu_v1.c: EMI_MPUS (master 15:0, region 20:16, domain 24:21, write violation 30:29), EMI_MPUT the address less 0x40000000 */
	{
		uint64_t at = (uint64_t)rd32(0x102191f8ULL) + 0x40000000ULL;
		bool ours = false;

		for (unsigned k = 0; k <= CAM_NBUF; k++) {
			ours = ours || (at >= cam_piece_pa[k] && at < cam_piece_pa[k] + (k < CAM_NBUF ? CAM_IMG_BYTES : CAM_NBUF * CAM_HDR_BYTES));
		}
		say("SV %s: EMI protection: MPUS %#x, the refused access at %#llx: %s", when, rd32(0x102191f0ULL), at,
		    ours ? "IN THE FRAME MEMORY" : "not in the frame memory (an old record)");
	}
	say("SV %s: larb3 NON_SEC_CON ports 0, 12, 13, 14: %#x %#x %#x %#x; SEC_CON: %#x %#x %#x %#x; frames at %#llx", when, rd32(LARB3 + LARB_NON_SEC_CON(0)),
	    rd32(LARB3 + LARB_NON_SEC_CON(12)), rd32(LARB3 + LARB_NON_SEC_CON(13)), rd32(LARB3 + LARB_NON_SEC_CON(14)), rd32(LARB3 + 0xf80), rd32(LARB3 + 0xf80 + 48),
	    rd32(LARB3 + 0xf80 + 52), rd32(LARB3 + 0xf80 + 56), cam_piece_pa[0]);
}

/* [camsv] mtk_camsv_setup (camsv_conf, the 10-bit sparams), the buffers, then mtk_camsv_cmos_vf_hw_enable; false if its reset never finished */
static bool
cam_camsv_start(void)
{
	cam_pitch = CAM_W * cam_variants[cam_variant].bits / 8;
	say("smi_larb3: MMU_EN off for its 21 ports (port 0 had %#x): no IOMMU tables here, the DMA's addresses are physical", rd32(LARB3 + LARB_NON_SEC_CON(0)));
	for (unsigned port = 0; port < 21; port++) {
		set32(LARB3 + LARB_NON_SEC_CON(port), 1u, 0);
	}
	say("CAMSV_1: %s, %u bytes a line, %u lines", cam_variants[cam_variant].name, cam_pitch, CAM_H);
	wr32(CAMSV + SV_TG_SEN_MODE, 0x00010002);               /* TIME_STP_EN, DBL_DATA_BUS (two pixels a clock, as the mux); with CMOS_EN below 0x00010003: [dump] CAM_A's +0x500 */
	wr32(CAMSV + SV_TG_SEN_GRAB_PXL, CAM_W << 16);          /* ([x6826b]: 0x0ff00000 for 4080) */
	wr32(CAMSV + SV_TG_SEN_GRAB_LIN, CAM_H << 16);
	wr32(CAMSV + SV_SW_CTL, 4);                             /* SW_RST; then the DMA's reset and its acknowledge (the vendor's ISP_Reset has the same) */
	wr32(CAMSV + SV_SW_CTL, 0);
	wr32(CAMSV + SV_SW_CTL, 1);
	if (!cam_poll(CAMSV + SV_SW_CTL, 3, 3, 20)) {
		say("  CAMSV_1's RESET DID NOT FINISH: SW_CTL %#x", rd32(CAMSV + SV_SW_CTL));
		wr32(CAMSV + SV_SW_CTL, 0);
		return false;
	}
	wr32(CAMSV + SV_SW_CTL, 0);
	wr32(CAMSV + SV_INT_EN, 0x001b04ff);                    /* INT_ST_MASK_CAMSV and the errors and drops (bits 4, 5, 6, 16, 17, 19): nothing takes the interrupt here, the status is read */
	wr32(CAMSV + SV_MODULE_EN, 0x40000001 | 4);             /* double buffer, TG; PAK */
	set32(CAMSV + SV_FMT_SEL, 7u, 1u);                      /* 10 bits in ([x6826b]: 0, 1 and 2 gave the same bytes); [dump] the idle value 0x100 keeps its bit 8 */
	wr32(CAMSV + SV_PAK, cam_variants[cam_variant].pak);
	/* [camsv]'s other writes, all needed or harmless here (logs/cam4.log: without them and CLK_EN the sensor interface is dead; logs/cam5.log: with them it works) */
	wr32(CAMSV + SV_TG_PATH_CFG, 0x1000);
	set32(CAMSV + SV_DMA_RSV1, 1u << 31, 0);
	wr32(CAMSV + SV_DMA_RSV6, 0xffffffff);
	wr32(CAMSV + SV_SPECIAL_FUN_EN, 0x61000000);
	wr32(CAMSV + SV_FBC_IMGO_CTL1, 0);
	wr32(CAMSV + SV_FBC_IMGO_CTL1, 0x00010000);
	wr32(CAMSV + SV_IMGO_XSIZE, cam_pitch - 1);
	wr32(CAMSV + SV_IMGO_YSIZE, CAM_H - 1);
	wr32(CAMSV + SV_IMGO_STRIDE, 0x01030000 | cam_pitch);
	wr32(CAMSV + SV_IMGO_CON, 0x80000080);
	wr32(CAMSV + SV_IMGO_CON2, 0x00020002);
	wr32(CAMSV + SV_IMGO_CON3, 0x00020002);
	set32(CAMSV + SV_TG_SEN_MODE, 1u, 1u);                  /* CMOS_EN first */
	set32(CAMSV + SV_FBC_IMGO_CTL1, 0x00008000, 0x00008000);        /* FBC_EN: 0x00018000 in all, what [dump] has in CAM_A's (+0x110) and [x6826b]'s HAL
	                                                                 * gives a CAMSV (bits 15, 16); [camsv] sets bit 22 too, which neither has */
	set32(CAMSV + SV_MODULE_EN, 0x10, 0x10);                /* IMGO_EN */
	wr32(CAMSV + SV_DMA_FH_EN, 1);                          /* a frame header, as the HAL of [x6826b] and [camsv]'s enableFH */
	cam_arm(0);                                             /* (never the view-finder without an address: cam_arm writes IMGO_BASE_ADDR) */
	wr32(CAMSV + SV_CLK_EN, (1u << 0) | (1u << 2) | (1u << 15));   /* [camsv] TG, PAK, DMA: needed (logs/cam4.log) */
	set32(CAMSV + SV_TG_VF_CON, 1u, 1u);                    /* the view-finder: frames are taken from here on */
	cam_sv_dump("programmed");
	return true;
}

static void
cam_rx_stop(void)
{
	say("receiver: stop");
	set32(CAMSV + SV_TG_VF_CON, 1u, 0);                     /* [camsv] mtk_camsv_cmos_vf_hw_disable */
	set32(CAMSV + SV_TG_SEN_MODE, 1u, 0);
	delay(40000);                                           /* (a frame's time: the DMA ends the one it is in) */
	wr32(CAMSV + SV_MODULE_EN, 0);
	wr32(CAMSV + SV_FBC_IMGO_CTL1, 0);
	wr32(CAMSV + SV_SW_CTL, 4);
	wr32(CAMSV + SV_SW_CTL, 0);
	set32(SENINF + SENINF_CSI2_CTL, 0x1f, 0);               /* [seninf] mtk_seninf_power_off */
	set32(SENINF_MUX + SENINF_MUX_CTRL, 1u << 31, 0);
	set32(SENINF_TOP + SENINF_TOP_MUX_CTRL, 0xfu << 12, 3u << 12);  /* ([dump]: mux 3's own source) */
	cam_phy(false);
}

/* what the receiver says of the stream: the first lines to read when no frame arrives. CAMSV_1's INT (read clears it): 0 vertical sync, 1 2 TG, 3 exposure
 * done, 4 TG error, 5 grab error, 6 drop, 7 start of frame, 10 pass done, 16 IMGO error, 17 IMGO overrun, 19 IMGO drop, 20 pass done with a buffer ([camsv]) */
static void
cam_rx_status(const char *when)
{
	say("%s: SENINF CSI2 INT %#x, mux INT %#x, mux measured %#x (0x066004c8 = 1632x1224)", when, rd32(SENINF + SENINF_CSI2_INT_STATUS),
	    rd32(SENINF_MUX + SENINF_MUX_INTSTA), rd32(SENINF_MUX + SENINF_MUX_DEBUG_2));
	say("%s: CAMSV_1 INT %#x (bit 20: a frame into a buffer; 16, 17, 19: IMGO error, overrun, drop)", when, rd32(CAMSV + SV_INT_STATUS));
	cam_sv_dump(when);
}

/* ---- what a buffer holds ---- */

/* pixel x of a line as the DMA wrote it, 10 bits */
static uint32_t
cam_pixel(const volatile uint8_t *line, uint32_t x)
{
	if (cam_pitch == CAM_W) {
		return (uint32_t)line[x] << 2;
	}
	return ((((uint32_t)line[x * 10 / 8] | ((uint32_t)line[x * 10 / 8 + 1] << 8)) >> (x * 10 % 8)) & 0x3ff);
}

/* `lines` lines from `first`, as MIPI RAW10 (four pixels' high 8 bits, then a byte of their low 2 bits) */
static void
cam_read_lines(unsigned k, uint32_t first, uint32_t lines, uint8_t *d)
{
	for (uint32_t y = first; y < first + lines; y++) {
		const volatile uint8_t *s = cam_img(k) + (uint64_t)y * cam_pitch;

		for (uint32_t i = 0; i < CAM_W / 4; i++, s += cam_pitch == CAM_W ? 4 : 5, d += 5) {
			if (cam_pitch == CAM_W) {
				d[0] = s[0];
				d[1] = s[1];
				d[2] = s[2];
				d[3] = s[3];
				d[4] = 0;
			} else {
				uint64_t v = (uint64_t)s[0] | ((uint64_t)s[1] << 8) | ((uint64_t)s[2] << 16) | ((uint64_t)s[3] << 24) | ((uint64_t)s[4] << 32);

				d[0] = (uint8_t)(v >> 2);
				d[1] = (uint8_t)(v >> 12);
				d[2] = (uint8_t)(v >> 22);
				d[3] = (uint8_t)(v >> 32);
				d[4] = (uint8_t)((v & 3) | ((v >> 10 & 3) << 2) | ((v >> 20 & 3) << 4) | ((v >> 30 & 3) << 6));
			}
		}
	}
}

/* the four colours of the 2x2 cell (every 8th cell of every 8th pair of lines) and the least and most of the first colour */
struct cam_means {
	uint32_t c[4], mn, mx;
};

static void
cam_sample(unsigned k, struct cam_means *m)
{
	volatile uint8_t *base = cam_img(k);
	uint64_t sum[4] = { 0, 0, 0, 0 }, n = 0;

	FlushPoC_DcacheRegion((vm_offset_t)base, (size_t)cam_pitch * CAM_H);
	m->mn = 1023;
	m->mx = 0;
	for (uint32_t y = 0; y < CAM_H; y += (y & 1) ? 15 : 1) {
		volatile uint8_t *p = base + (uint64_t)y * cam_pitch;

		for (uint32_t x = 0; x < CAM_W; x += 16) {
			uint32_t a = cam_pixel(p, x), b = cam_pixel(p, x + 1);

			sum[(y & 1) * 2] += a;
			sum[(y & 1) * 2 + 1] += b;
			m->mn = a < m->mn ? a : m->mn;
			m->mx = a > m->mx ? a : m->mx;
			n += (y & 1) == 0;
		}
	}
	for (unsigned i = 0; i < 4; i++) {
		m->c[i] = (uint32_t)(sum[i] / (n ? n : 1));
	}
}

/* lines written, a few raw bytes, and a 64x48 thumbnail for tools/camthumb.py */
static uint32_t
cam_stats(unsigned k)
{
	volatile uint8_t *base = cam_img(k);
	uint32_t written = 0, last = 0;
	static const char hex[] = "0123456789abcdef";
	char row[64 * 2 + 1];

	FlushPoC_DcacheRegion((vm_offset_t)base, (size_t)cam_pitch * CAM_H);
	for (uint32_t y = 0; y < CAM_H; y++) {
		volatile uint8_t *p = base + (uint64_t)y * cam_pitch;

		for (uint32_t x = 0; x < cam_pitch; x += 8) {
			if (*(volatile uint64_t *)(p + x)) {
				written++;
				last = y;
				break;
			}
		}
	}
	say("image %u: %u of %u lines written (last %u)", k, written, CAM_H, last);
	for (unsigned r = 0; r < 3; r++) {
		const uint32_t rows[3] = { 0, CAM_H / 2, CAM_H - 1 };
		volatile uint8_t *p = base + (uint64_t)rows[r] * cam_pitch + 800;

		say("  line %u [800..809]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", rows[r], p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]);
	}
	say("image %u: thumbnail 64x48 follows", k);
	for (uint32_t cy = 0; cy < 48; cy++) {
		for (uint32_t cx = 0; cx < 64; cx++) {
			uint32_t t = 0, v;

			for (uint32_t i = 0; i < 16; i++) {     /* 4x4 samples of one colour (even x, even y: green) inside the 25x25 block */
				t += cam_pixel(base + (uint64_t)((cy * CAM_H / 48 + (i / 4) * 6) & ~1u) * cam_pitch, (cx * CAM_W / 64 + (i % 4) * 6) & ~1u);
			}
			v = t / 16 >> 2;
			row[cx * 2] = hex[v >> 4];
			row[cx * 2 + 1] = hex[v & 15];
		}
		row[128] = 0;
		say("THUMB %02u: %s", cy, row);
	}
	return written;
}

/* power is on and the ID read: tables, SENINF, the stream until the mux measures the frame, then standby, CAMSV_1,
 * and the stream again: the DMA is ready before the sensor's first frame, as [camsv] and [x6826b] have it. On false everything this started is stopped. */
static bool
cam_stream_start(void)
{
	if (!cam_buf_make() || !cam_sensor_tables()) {
		return false;
	}
	cam_seninf_start();
	cam_sensor_stream(true);
	(void)cam_seninf_lock();        /* (it goes on either way: the capture's status lines say more) */
	cam_sensor_stream(false);
	if (!cam_camsv_start()) {
		cam_rx_stop();
		return false;
	}
	cam_sensor_stream(true);
	return true;
}

static void
cam_stream_stop(void)
{
	cam_sensor_stream(false);
	cam_rx_stop();
}

/* the frame in the DMA's image is whole: the image's number, and the DMA has the next one; -1 if it is not whole yet. `keep` (or -1) is not given to the DMA. */
static int
cam_frame_done(int keep)
{
	unsigned done = cam_cur, next = (done + 1) % CAM_NBUF;

	if (!cam_tail_written(done)) {
		return -1;
	}
	delay(1000);                    /* ([x6826b]: the tail is 64 bytes, the DMA may be in them still) */
	if ((int)next == keep) {
		next = (next + 1) % CAM_NBUF;
	}
	cam_arm(next);
	return (int)done;
}

/*
 * r1cam=3: r1cam_frames frames (default 30), each re-armed as the node does it; a line for each (the time since the one before, the cell's means), the
 * exposure halved for the second third and doubled for the last (the green mean above black should follow: about half, about double the first third's),
 * then the last frame's thumbnail.
 */
static void
cam_capture(void)
{
	uint32_t frames = 30, got = 0, green[3] = { 0, 0, 0 }, count[3] = { 0, 0, 0 };
	uint64_t t0, last, now;
	int done = -1;

	(void)PE_parse_boot_argn("r1cam_frames", &frames, sizeof(frames));
	frames = frames < 3 ? 3 : frames > 300 ? 300 : frames;
	say("capture: %u frames, %s", frames, cam_variants[cam_variant].name);
	if (!cam_stream_start()) {
		return;
	}
	cam_sensor_exposure(1200, cam_gain_arg, CAM_FRAME_MIN);
	t0 = last = cam_ms();
	while (got < frames && cam_ms() - last < 1000) {
		struct cam_means m;
		int k = cam_frame_done(-1);
		uint32_t third = got * 3 / frames;

		if (k < 0) {
			delay(1000);
			continue;
		}
		done = k;
		now = cam_ms();
		cam_sample((unsigned)k, &m);
		say("frame %u: +%llu ms, image %d, exposure %u: min %u max %u, means %u %u / %u %u; FBC %#x INT %#x", got, now - last, k, cam_cur_exposure, m.mn, m.mx,
		    m.c[0], m.c[1], m.c[2], m.c[3], rd32(CAMSV + SV_FBC_IMGO_CTL2), rd32(CAMSV + SV_INT_STATUS));
		last = now;
		if (got * 3 >= third * frames + 9) {            /* (not a third's first three frames: a new exposure is a frame or two late) */
			green[third] += m.c[0];
			count[third]++;
		}
		got++;
		if (got * 3 / frames != third && got < frames) {
			cam_sensor_exposure(third == 0 ? 600 : 2400, cam_gain_arg, CAM_FRAME_MIN);
		}
	}
	if (got < frames) {
		cam_rx_status("THE FRAMES STOPPED");
	}
	say("capture: %u frames in %llu ms (30 a second: %u ms); dropped by the DMA: %u (FBC CTL2 31:24)", got, last - t0, got * 1000 / 30,
	    rd32(CAMSV + SV_FBC_IMGO_CTL2) >> 24);
	for (unsigned i = 0; i < 3; i++) {
		say("exposure %u lines: green mean above black %d (%u frames)", i == 0 ? 1200 : i == 1 ? 600 : 2400,
		    count[i] ? (int)(green[i] / count[i]) - (int)CAM_BLACK10 : 0, count[i]);
	}
	cam_stream_stop();
	if (done >= 0) {
		uint32_t w = cam_stats((unsigned)done);

		say("RESULT: %s (%u of %u lines, %u frames)", w >= CAM_H - 1 && got == frames ? "FRAMES reach memory" : "NOT ALL OF IT", w, CAM_H, got);
	} else {
		say("RESULT: NOTHING in memory");
	}
}

/* ---- /dev/redwood-camera (r1cam=4): redwood_mtkcam.c's node ---- */
#define CAM_IDLE_MS     4000u           /* no read for this long: the stream stops and everything is switched off */
#define CAM_STRIDE      (CAM_W / 4 * 5)
#define CAM_STRIDE_MAX  (3264u / 4 * 5)

static LCK_GRP_DECLARE(cam_lck_grp, "redwood-r1cam");
static LCK_MTX_DECLARE(cam_lock, &cam_lck_grp);
enum { SLOT_FILL, SLOT_READY, SLOT_READING, SLOT_FREE };
static struct {
	int             state;
	uint64_t        sequence, time;
	uint32_t        exposure, gain, frame_lines;
} cam_slot[CAM_NBUF];
static bool cam_dev_wanted, cam_dev_reading, cam_broken, cam_streaming;
static int cam_dev_opens;
static uint64_t cam_seq, cam_given, cam_last_read_ms;
static struct {
	bool            pending;
	uint32_t        exposure, gain, frame_lines;
} cam_ask;
static uint8_t cam_chunk[CAM_STRIDE_MAX * 8];
/* boot-arg r1cam_flip=1: the motor turns the camera to the face a SENSOR write asks for (1 = back, 2 = front; 0 = unknown/none yet) */
static bool cam_flip, cam_moving;
static uint32_t cam_face_want, cam_face_at;
static uint64_t cam_face_moved_ms;      /* when the last face move finished; the first frame after it says what the camera sees (0: said) */

/* the first frame after a face move: mean raw luma of the top and bottom half, from 16 rows x 4 columns (64 pixels) */
static void
cam_face_first_frame(unsigned k)
{
	volatile uint8_t *base = cam_img(k);
	uint32_t sum[2] = { 0, 0 };

	for (uint32_t r = 0; r < 16; r++) {
		uint32_t y = (r * CAM_H / 16) & ~1u;
		volatile uint8_t *p = base + (uint64_t)y * cam_pitch;

		FlushPoC_DcacheRegion((vm_offset_t)p, cam_pitch);
		for (uint32_t c = 0; c < 4; c++) {
			sum[r >= 8] += cam_pixel(p, (c * CAM_W / 4 + CAM_W / 8) & ~1u);
		}
	}
	say("r1cam: face %u first frame at %llu ms (move done at %llu); luma top/bottom half %u/%u", cam_face_at, cam_ms(), cam_face_moved_ms,
	    sum[0] / 32, sum[1] / 32);
	cam_face_moved_ms = 0;
}

/* the stream is on: the frame the DMA finishes becomes "ready" (the newest; an older one nobody read is the DMA's next), until nobody reads; false if no frame ever came */
static bool
cam_serve_loop(void)
{
	uint64_t last_frame_ms = cam_ms();
	bool stop = false;

	lck_mtx_lock(&cam_lock);
	cam_seq = cam_given = 0;
	for (unsigned k = 0; k < CAM_NBUF; k++) {
		cam_slot[k].state = k == cam_cur ? SLOT_FILL : SLOT_FREE;
	}
	cam_streaming = true;
	lck_mtx_unlock(&cam_lock);
	say("serving: the stream is on");
	while (!stop) {
		bool ask;
		uint32_t exposure, gain, frame;
		int reading = -1, k;

		lck_mtx_lock(&cam_lock);
		for (unsigned i = 0; i < CAM_NBUF; i++) {
			if (cam_slot[i].state == SLOT_READING) {
				reading = (int)i;
			}
		}
		if ((k = cam_frame_done(reading)) >= 0) {
			for (unsigned i = 0; i < CAM_NBUF; i++) {      /* (three images: the DMA's, the newest, and one a reader may hold) */
				if (cam_slot[i].state == SLOT_READY) {
					cam_slot[i].state = SLOT_FREE;
				}
			}
			cam_slot[cam_cur].state = SLOT_FILL;
			cam_slot[k].state = SLOT_READY;
			cam_slot[k].sequence = ++cam_seq;
			cam_slot[k].time = mach_absolute_time();
			cam_slot[k].exposure = cam_cur_exposure;
			cam_slot[k].gain = cam_cur_gain;
			cam_slot[k].frame_lines = cam_cur_frame;
			last_frame_ms = cam_ms();
			if (cam_face_moved_ms) {
				cam_face_first_frame((unsigned)k);
			}
			if (cam_seq <= 3) {
				say("serving: frame %llu whole", cam_seq);
			}
			wakeup(&cam_seq);
		}
		ask = cam_ask.pending;
		exposure = cam_ask.exposure;
		gain = cam_ask.gain;
		frame = cam_ask.frame_lines;
		cam_ask.pending = false;
		stop = cam_dev_opens == 0 || (cam_ms() - cam_last_read_ms > CAM_IDLE_MS && !cam_dev_reading) ||
		    (cam_flip && cam_face_want != cam_face_at);
		lck_mtx_unlock(&cam_lock);
		if (ask) {
			cam_sensor_exposure(exposure, gain, frame);
		}
		if (cam_ms() - last_frame_ms > 3000) {
			cam_rx_status("serving: NO FRAME FOR 3 s");
			break;
		}
		delay_for_interval(2, NSEC_PER_MSEC);
	}
	lck_mtx_lock(&cam_lock);
	cam_streaming = false;
	lck_mtx_unlock(&cam_lock);
	say("serving: %s: the stream stops after %llu frames (the DMA dropped %u)", stop ? "nobody reads" : "the frames stopped", cam_seq,
	    rd32(CAMSV + SV_FBC_IMGO_CTL2) >> 24);
	return cam_seq != 0;
}

static int
camdev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	if (cam_broken) {
		return EIO;
	}
	lck_mtx_lock(&cam_lock);
	cam_dev_opens++;
	lck_mtx_unlock(&cam_lock);
	return 0;
}

static int
camdev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	lck_mtx_lock(&cam_lock);
	cam_dev_opens = 0;              /* (close is the last close) */
	lck_mtx_unlock(&cam_lock);
	return 0;
}

static int
camdev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct redwood_camera_frame f;
	uint64_t since = cam_ms();
	int b = -1, error = 0;

	lck_mtx_lock(&cam_lock);
	if (cam_dev_reading) {
		lck_mtx_unlock(&cam_lock);
		return EBUSY;
	}
	cam_dev_reading = true;
	cam_last_read_ms = cam_ms();
	for (;;) {
		struct timespec ts = { 0, 50 * NSEC_PER_MSEC };

		if (cam_broken) {
			error = EIO;
			break;
		}
		b = -1;
		for (int i = 0; i < (int)CAM_NBUF; i++) {
			if (cam_slot[i].state == SLOT_READY && cam_slot[i].sequence > cam_given && (b < 0 || cam_slot[i].sequence > cam_slot[b].sequence)) {
				b = i;
			}
		}
		if (b >= 0 && cam_streaming) {
			break;
		}
		if (!cam_streaming && !cam_dev_wanted) {
			cam_dev_wanted = true;
			wakeup(&cam_dev_wanted);
		}
		if (cam_ms() - since > (cam_streaming ? 3000u : 20000u)) {
			error = EAGAIN;
			break;
		}
		error = msleep(&cam_seq, &cam_lock, PZERO | PCATCH, "redwood-r1cam", &ts);
		if (error && error != EWOULDBLOCK) {
			break;
		}
		error = 0;
		cam_last_read_ms = cam_ms();
	}
	if (error) {
		cam_dev_reading = false;
		lck_mtx_unlock(&cam_lock);
		return error;
	}
	cam_slot[b].state = SLOT_READING;
	cam_given = cam_slot[b].sequence;
	bzero(&f, sizeof(f));
	memcpy(f.magic, REDWOOD_CAMERA_MAGIC, 8);
	f.header_bytes = sizeof(f);
	f.width = CAM_W;
	f.height = CAM_H;
	f.stride = CAM_STRIDE;
	f.format = 1;
	f.bayer = CAM_BAYER;
	f.black = CAM_BLACK10;
	f.sequence = cam_slot[b].sequence;
	f.time = cam_slot[b].time;
	f.exposure_lines = cam_slot[b].exposure;
	f.line_ns = CAM_LINE_NS;
	f.gain = cam_slot[b].gain;
	f.gain_unity = CAM_GAIN_UNITY;
	f.gain_max = CAM_GAIN_MAX;
	f.frame_lines = cam_slot[b].frame_lines;
	f.frame_lines_min = CAM_FRAME_MIN;
	f.frame_lines_max = CAM_FRAME_MAX;
	f.margin = CAM_EXP_MARGIN;
	f.sensor = cam_flip && cam_face_at == 2 ? 2 : 1;
	lck_mtx_unlock(&cam_lock);

	error = uiomove((caddr_t)&f, (int)MIN(sizeof(f), (size_t)uio_resid(uio)), uio);
	if (error == 0 && uio_resid(uio) > 0) {
		FlushPoC_DcacheRegion((vm_offset_t)cam_img((unsigned)b), (size_t)cam_pitch * CAM_H);
		for (uint32_t y = 0; y < CAM_H && error == 0 && uio_resid(uio) > 0; y += 8) {
			cam_read_lines((unsigned)b, y, 8, cam_chunk);      /* (1224 and 2448 are multiples of 8) */
			error = uiomove((caddr_t)cam_chunk, (int)MIN((user_ssize_t)(CAM_STRIDE * 8), uio_resid(uio)), uio);
		}
	}
	lck_mtx_lock(&cam_lock);
	cam_slot[b].state = SLOT_FREE;
	cam_dev_reading = false;
	cam_last_read_ms = cam_ms();
	lck_mtx_unlock(&cam_lock);
	return error;
}

static int
camdev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct redwood_camera_command c;

	if (uio_resid(uio) < (user_ssize_t)sizeof(c) || uiomove((caddr_t)&c, sizeof(c), uio) != 0) {
		return EINVAL;
	}
	switch (c.command) {
	case REDWOOD_CAMERA_EXPOSURE:
		lck_mtx_lock(&cam_lock);
		cam_ask.exposure = c.a;
		cam_ask.gain = c.b;
		cam_ask.frame_lines = c.c;
		cam_ask.pending = true;
		lck_mtx_unlock(&cam_lock);
		return 0;
	case REDWOOD_CAMERA_KEEP:
		return ENOSPC;                  /* (no memory in one piece is kept for the host here) */
	case REDWOOD_CAMERA_SENSOR:
		if (cam_flip && (c.a == 1 || c.a == 2)) {       /* r1cam_flip=1: the motor's two faces; the thread moves it before the next session */
			int busy;

			lck_mtx_lock(&cam_lock);
			busy = cam_moving;
			if (!busy) {
				cam_face_want = c.a;
			}
			lck_mtx_unlock(&cam_lock);
			return busy ? EBUSY : 0;
		}
		return c.a == 1 ? 0 : EINVAL;   /* the r1 has the one camera */
	default:
		return EINVAL;
	}
}

static const struct cdevsw camdev_cdevsw = {
	.d_open = camdev_open, .d_close = camdev_close, .d_read = camdev_read, .d_write = camdev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* ---- what else is on the camera's bus: read only (r1cam=2, 3) ---- */

/* does anything acknowledge its address (no register is read or written)? [dts] i2c2 has camera_main_af@0c and camera_main_eeprom@50 */
static void
cam_bus_probe(void)
{
	static const uint8_t addrs[] = { 0x0c, 0x50 };

	for (unsigned i = 0; i < sizeof(addrs); i++) {
		int ack;

		cam_bb_start();
		ack = cam_bb_write((uint8_t)(addrs[i] << 1));
		cam_bb_stop();
		say("bus: address %#x (%s): %s", addrs[i], i ? "the module's EEPROM" : "a lens driver, if the module has autofocus",
		    ack ? "ACKNOWLEDGED" : "nothing answers");
	}
}

/*
 * ---- the motor that turns the camera (boot-arg r1cam_motor=DEGREES, 0..180; nothing without it) ----
 * [r1k] drivers/misc/step_motor_ms35774.c, the whole of it: a step/direction driver on seven pins ([dts] step_motor_ms35774's pinctrl states:
 * VM GPIO163, PDN 164, ENN 166, DIR 167, STEP 168, MS2 170, MS1 171). Idle: VM low, ENN high, DIR low, STEP low, PDN low, MS1 high, MS2 low. A turn:
 * DIR (low towards 0), VM high, 10 ms, ENN low, 6 steps a degree (STEP high 50 us, low 360 us), 50 us, ENN high, VM low. Where the camera is cannot be
 * read: Android's driver, at its start, turns 180 degrees towards 0 (into the stop) and then goes to 90; this does the same and then goes to DEGREES.
 */
#define MOTOR_VM        163
#define MOTOR_PDN       164
#define MOTOR_ENN       166
#define MOTOR_DIR       167
#define MOTOR_STEP      168
#define MOTOR_MS2       170
#define MOTOR_MS1       171

static uint32_t cam_step_us = 360;      /* (r1cam_step_us=, with r1cam_flip=1 only) */

static void
cam_motor_turn(int up, uint32_t degrees)
{
	cam_pin_out(MOTOR_DIR, up);
	cam_pin_out(MOTOR_VM, 1);
	delay(10000);
	cam_pin_out(MOTOR_ENN, 0);
	for (uint32_t step = 0; step < 6 * degrees; step++) {
		cam_pin_out(MOTOR_STEP, 1);
		delay(50);
		cam_pin_out(MOTOR_STEP, 0);
		delay(cam_step_us);
	}
	delay(50);
	cam_pin_out(MOTOR_ENN, 1);
	cam_pin_out(MOTOR_VM, 0);
}

static void
cam_motor_pins(void)
{
	static const struct {
		uint8_t pin, level;
	} idle[] = { { MOTOR_VM, 0 }, { MOTOR_ENN, 1 }, { MOTOR_DIR, 0 }, { MOTOR_STEP, 0 }, { MOTOR_PDN, 0 }, { MOTOR_MS1, 1 }, { MOTOR_MS2, 0 } };

	for (unsigned i = 0; i < sizeof(idle) / sizeof(idle[0]); i++) {
		cam_pin_mode(idle[i].pin, 0);
		cam_pin_out(idle[i].pin, idle[i].level);
	}
}

static void
cam_motor(uint32_t degrees)
{
	say("motor: to %u degrees: the pins idle, 180 degrees towards 0 (1080 steps, 0.45 s), 2 s, then %u steps up", degrees, 6 * degrees);
	cam_motor_pins();
	cam_motor_turn(0, 180);
	delay_for_interval(2000, NSEC_PER_MSEC);        /* (the vendor's wait between its two turns) */
	cam_motor_turn(1, degrees);
	say("motor: done");
}

/* r1cam_flip=1: turn to the wanted face (r1cam_rear= / r1cam_front= degrees); the first move after boot homes into the stop at 0 */
static uint32_t cam_rear_deg = 180, cam_front_deg;
static int cam_deg = -1;        /* where the camera is, in degrees; -1 unknown */

static void
cam_face_move(void)
{
	static bool pins;
	uint32_t to_face = cam_face_want, to = to_face == 2 ? cam_front_deg : cam_rear_deg, steps = 0;
	uint64_t t0 = cam_ms();
	int from = cam_deg;

	lck_mtx_lock(&cam_lock);
	cam_moving = true;
	lck_mtx_unlock(&cam_lock);
	if (!pins) {
		cam_motor_pins();
		pins = true;
	}
	if (from < 0) {
		cam_motor_turn(0, 180);         /* (home: into the stop at 0) */
		steps += 6 * 180;
		from = 0;
	}
	if ((int)to > from) {
		cam_motor_turn(1, to - (uint32_t)from);
		steps += 6 * (to - (uint32_t)from);
	} else if ((int)to < from) {
		cam_motor_turn(0, (uint32_t)from - to);
		steps += 6 * ((uint32_t)from - to);
	}
	say("motor: face %u -> %u (%s): %d -> %u degrees, %u steps at %u us, %llu ms; supply (VM) and enable (ENN) off", cam_face_at, to_face,
	    to_face == 2 ? "front" : "back", cam_deg, to, steps, cam_step_us, cam_ms() - t0);
	cam_deg = (int)to;
	cam_face_moved_ms = cam_ms();
	lck_mtx_lock(&cam_lock);
	cam_face_at = to_face;
	cam_moving = false;
	lck_mtx_unlock(&cam_lock);
}

/* ---- the stages ---- */
static uint32_t cam_level;

static void
r1cam_thread(__unused void *parameter, __unused wait_result_t wr)
{
	uint32_t start = 10, failed = 0;
	int major;

	(void)PE_parse_boot_argn("r1cam_start", &start, sizeof(start));
	(void)PE_parse_boot_argn("r1cam_pak", &cam_variant, sizeof(cam_variant));
	cam_variant %= CAM_NVARIANTS;
	(void)PE_parse_boot_argn("r1cam_gain", &cam_gain_arg, sizeof(cam_gain_arg));
	(void)PE_parse_boot_argn("r1cam_settle", &cam_settle, sizeof(cam_settle));
	(void)PE_parse_boot_argn("r1cam_addr", &cam_addr, sizeof(cam_addr));
	(void)PE_parse_boot_argn("r1cam_mode", &cam_mode, sizeof(cam_mode));
#ifdef R1CAM_HI846_FULL
	if (cam_mode == 1) {
		cam_w = 3264;
		cam_h = 2448;
	} else
#endif
	{
		cam_mode = 0;           /* (or the vendor's tables are not in this build: tools/r1camtable.py's third argument) */
	}
	cam_settle &= 0xff;
	cam_addr &= 0x7f;
	for (uint32_t s = 0; s < (cam_level >= 4 ? 0 : start); s++) {
		delay_for_interval(1000, NSEC_PER_MSEC);
	}
	for (unsigned i = 0; i < CAM_NWIN; i++) {
		cam_win[i].va = ml_io_map((vm_offset_t)cam_win[i].pa, cam_win[i].size);
		if (!cam_win[i].va) {
			say("cannot map %#llx: stopping", cam_win[i].pa);
			thread_terminate(current_thread());
			return;
		}
	}
	say("stage %u, %ux%u", cam_level, cam_w, cam_h);
	{
		uint32_t flip = 0;

		if (PE_parse_boot_argn("r1cam_flip", &flip, sizeof(flip)) && flip == 1 && cam_level >= 4) {
			(void)PE_parse_boot_argn("r1cam_rear", &cam_rear_deg, sizeof(cam_rear_deg));
			(void)PE_parse_boot_argn("r1cam_front", &cam_front_deg, sizeof(cam_front_deg));
			(void)PE_parse_boot_argn("r1cam_step_us", &cam_step_us, sizeof(cam_step_us));
			cam_rear_deg = MIN(cam_rear_deg, 180);
			cam_front_deg = MIN(cam_front_deg, 180);
			cam_step_us = MIN(MAX(cam_step_us, 200), 1000);
			cam_flip = true;
			say("motor: flip on: back at %u degrees, front at %u, %u us a step", cam_rear_deg, cam_front_deg, cam_step_us);
		}
	}
	{
		uint32_t degrees = 0;

		if (PE_parse_boot_argn("r1cam_motor", &degrees, sizeof(degrees)) && degrees <= 180) {
			cam_motor(degrees);
		}
	}
	if (cam_level >= 3 && !cam_buf_make() && cam_mode) {    /* (now, while there are such pieces; never at an open) */
		for (unsigned k = 0; k < CAM_NBUF; k++) {       /* (the pieces of the big size go back before the size changes) */
			if (cam_piece[k]) {
				kmem_free(kernel_map, cam_piece[k], CAM_IMG_BYTES);
				cam_piece[k] = 0;
			}
		}
		cam_mode = 0;
		cam_w = 1632;
		cam_h = 1224;
		say("no memory for 3264x2448: 1632x1224");
		(void)cam_buf_make();
	}
	if (cam_level < 4) {
		cam_pmic_is_mt6357();
		cam_report("as the bootloader left it");
		if (cam_level == 2 && cam_power_on()) {
			cam_report("powered");
			if (cam_sensor_id()) {
				cam_bus_probe();
			}
			cam_power_off();
		}
		if (cam_level >= 3 && cam_power_on()) {
			if (cam_sensor_id()) {
				cam_bus_probe();
				cam_capture();
			}
			cam_power_off();
		}
		if (cam_level >= 2) {
			cam_report("off again");
		}
		say("stage %u: done", cam_level);
		thread_terminate(current_thread());
		return;
	}
	/* r1cam=4: a session whenever a read wants frames; after three that never gave a frame the camera is given up (reads fail with EIO) */
	major = cdevsw_add(-1, &camdev_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-camera") == NULL) {
		say("/dev/redwood-camera CANNOT BE MADE");
		thread_terminate(current_thread());
		return;
	}
	say("/dev/redwood-camera made (%s)", cam_variants[cam_variant].name);
	while (!cam_broken) {
		struct timespec ts = { 1, 0 };
		bool ok = false;

		lck_mtx_lock(&cam_lock);
		while (!cam_dev_wanted) {
			(void)msleep(&cam_dev_wanted, &cam_lock, PZERO, "redwood-r1cam-idle", &ts);
		}
		cam_dev_wanted = false;
		if (cam_dev_opens == 0) {       /* (a read that came while the last session was ending) */
			lck_mtx_unlock(&cam_lock);
			continue;
		}
		lck_mtx_unlock(&cam_lock);
		if (cam_flip && cam_face_want != 0 && cam_face_want != cam_face_at) {
			cam_face_move();        /* (the only mover, and never with the stream on) */
		}
		if (cam_power_on()) {
			if (cam_sensor_id() && cam_stream_start()) {
				ok = cam_serve_loop();
				cam_stream_stop();
			}
			cam_power_off();
		}
		if (cam_flip && cam_face_want != cam_face_at) {
			ok = true;              /* (stopped to turn, not a failure) */
		}
		failed = ok ? 0 : failed + 1;
		if (failed >= 3) {
			lck_mtx_lock(&cam_lock);
			cam_broken = true;
			wakeup(&cam_seq);
			lck_mtx_unlock(&cam_lock);
			say("the camera could not be brought up three times: given up");
		}
		delay_for_interval(500, NSEC_PER_MSEC);
	}
	thread_terminate(current_thread());
}

void
redwood_r1cam_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("r1cam", &cam_level, sizeof(cam_level)) || cam_level < 1) {
		return;
	}
	if (kernel_thread_start_priority(r1cam_thread, NULL, 81 /* BASEPRI_KERNEL */, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

#else /* no tables */

void
redwood_r1cam_init(void)
{
	uint32_t level = 0;

	if (PE_parse_boot_argn("r1cam", &level, sizeof(level)) && level >= 1) {
		say("the sensor's tables are not in this build (redwood_r1cam_hi846.h: tools/r1camtable.py): nothing done");
	}
}

#endif
