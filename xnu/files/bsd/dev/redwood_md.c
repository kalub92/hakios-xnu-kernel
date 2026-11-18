/*
 * The Infinix's MD1 modem (docs/cellular-plan.md).
 *   r1_md=1  stage 0: reads, and says in the kept log, the power, bus-protection and clock-gate state of MD1. Nothing is written.
 *            A gated block hangs the bus when read, so the AP CCIF is read only with its clock ungated (INFRA CG1 bit 23) and
 *            the MD boot status only with MD1 on in both PWR_STATUS registers.
 *   r1_md=2  stage 0, then stage 1: power MD1 on and release it to its first handshake (HS1), as haretic's r1_md_probe.c stages
 *            1-2 do (docs/ref/haretic-modem/), in the same order: MD clock mode, VMODEM, the MD1 power domain (the legacy modem
 *            sequence of mtk-pm-domains.c, bus protection released MD2PERI, masters, PERI2MD), pre-access, SRCCLKENA, MD PLLs, MD
 *            watchdog off, CCIF/CLDMA clock ungate, shared memory and ring buffers, CCIF reset, boot vector, post-access, then
 *            up to 10 s of watching the boot status and CCIF. It ENDS AT HS1: no runtime-data reply, no AT, nothing past it.
 *            Then MD1 is powered down again and what was found put back (redwood_md_quiesce does the same on the way back to
 *            the loader if the thread did not finish). The loader hands the shared memory, CCB and image placement from LK's
 *            tags as boot-args r1_md_smem= r1_md_smem_sz= r1_md_ccb= r1_md_ccb_sz= r1_md_img= r1_md_img_sz= r1_md_dfd=.
 *   r1_md=3  stage 1, then sub-stage 2a (docs/cellular-stage2.md section 5): before the release the CCB control page and the DBM
 *            pattern; on HS1 the runtime-data reply (feature table in smem +0xe800, header and v2_1 struct in the CCIF SRAM, BUSY,
 *            TCHNUM); then up to 10 s of watching: interrupt acks, the RX rings of queues 0-6 drained and decoded (logged, never
 *            answered), the debug regions dumped on a modem exception. Nothing is sent back beyond the reply and the acks.
 *   r1_md=4  2a, then sub-stage 2b: a TX path on the CCIF rings (ring_tx) and the answers the open kernel code gives itself: the
 *            kernel RPC requests (channel 32, answers on 33, queue 1, docs/cellular-stage2.md 1.8) and the system messages 0x105 /
 *            0x114 / 0x116 (queue 0). FS requests and the userspace RPCs (0x400f-0x4015 apart from 0x4012/0x4013, which the spec lists
 *            as kernel answers) are logged and NOT answered. Watch: 10 s after the last received packet, 30 s at most.
 *   r1_md=5  2b, then sub-stage 2c (docs/cellular-stage2.md 2.1, 4): the FS channel (14 rx / 15 tx, queue 4) is served by redwood_mdfs.c: a
 *            read-only ext4 reader over nvdata, nvcfg, protect1, protect2 (the eMMC driver refuses every write to them) and an in-RAM
 *            overlay (16 MB) for everything the modem writes. Nothing is sent on the AT channel; unsolicited text on it is logged by
 *            length, and its text only for lines starting "+E". Ends at HS2 + 3 s, 20 s after the last FS request, or 60 s in all.
 *   r1_md=6  2c, then sub-stage 2d: after HS2 the FS and kernel RPC keep being served; the AT channel (ch 10 rx / 12 tx, queue 5, plain AT, no CMUX)
 *            is read for its unsolicited lines; once "+EIND: 128" has come, a WHITELIST of read-only queries is sent one at a time (at_cmds):
 *            AT, ATE0, AT+CFUN?, AT+EFUN?, AT+CPIN?, AT+ESIMS?, AT+CGSN, AT+CGMR. Identifiers are never logged (digit counts only). If CFUN? or EFUN?
 *            shows the radio ON nothing more is sent and the run ends at once.
 *   r1_md=7  2d, then sub-stage 2e-1 (docs/cellular-stage3.md A.8 phase 1): the SIM only, RF stays OFF. After +EIND: 128: AT, ATE0, AT+CMEE=1,
 *            AT+ESIMS=1, AT+ESIMS, AT+EFUN=0 (SIM powered, radio off; the modem's +ESIMS/+EUSIM/+ICCID lines are collected), AT+CPIN? polled up to
 *            20 s (no PIN is ever sent), then AT+EFUN?, AT+CFUN?, AT+CGSN (a hash only), AT+CGMR, AT+ERAT?, AT+CEMODE?. Identifiers (ICCID, IMSI, IMEI,
 *            numbers) are logged as a length or a FNV-32 hash. At the end the ten gates of A.10 are logged ("gate N name: pass|FAIL"). 2e-2, the
 *            radio-on step (AT+EFUN=1, registration, hold, AT+EFUN=0), is coded but runs only with r1_md_radio=1 AND all ten gates passed (gate 9 needs
 *            r1_md_imei_h=HASH): without the boot-arg "radio-on NOT requested: stopping at 2e-1". The AT whitelist is enforced in at_send.
 * No storage is written. The MD side (0x2000xxxx) is read or written only with MD1 on and PERI2MD open.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void redwood_host_keep_add(const char *format, ...) __printflike(1, 2);
extern void delay_for_interval(uint32_t interval, uint32_t scale_factor);
extern void thread_terminate_self(void);
extern void read_random(void *buffer, u_int numbytes);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
/* redwood_mdfs.c (r1_md=5) */
extern bool mdfs_init(void);
extern uint32_t mdfs_handle(const uint8_t *req, uint32_t len, uint32_t *out);
extern void mdfs_summary(void);
extern const char *mdfs_jnl(void);
extern const char *mdfs_last(void);
extern void mdfs_counts(uint32_t *ops, uint32_t *missing, uint32_t *ovl);
extern void mdfs_done(void);
extern void mdfs_phase(int p);
/* r1_md=7 gates 2-4: the same struct as in redwood_mdfs.c */
struct mdfs_gate_info {
	uint32_t unimpl, bad_req, cap_ops, cap_hits, errors, errors_other, prot_reads, prot_short, prot_mut, crc_checked, crc_bad, sweep_nodes, sweep_bad, prot_same, max_req, oth_mut;
	uint64_t prot_bytes;
	char oth_files[96];
};
extern void mdfs_gate(struct mdfs_gate_info *g);
extern uint64_t redwood_emmc_nv_refused(void);
extern bool redwood_battery_get(int *soc, int *charger);
extern bool redwood_r1_temp(int *celsius);

/* md_quiet: on the way back to the loader (the other CPUs stopped, the log's lock may be held by one) nothing is said */
static volatile bool md_quiet;
#define SAY(...) do { if (!md_quiet) redwood_host_keep_add(__VA_ARGS__); } while (0)

/* SPM */
#define SPM_PWR_STATUS          0x180
#define SPM_PWR_STATUS_2ND      0x184
#define SPM_MD1_PWR_CON         0x320
#define SPM_MD_SRAM_ISO_CON     0x394
#define SPM_MD_EXTRA_PWR_CON    0x398
/* MD1_PWR_CON bits */
#define PWR_RST_B               (1u << 0)
#define PWR_ISO                 (1u << 1)
#define PWR_ON                  (1u << 2)
#define PWR_ON_2ND              (1u << 3)
#define PWR_CLK_DIS             (1u << 4)
#define SRAM_PDN                (1u << 8)
/* INFRACFG_AO */
#define INFRA_RST0              0x150
#define INFRA_RST1              0x154
#define INFRA_PROTECTEN         0x220
#define INFRA_PROTECTEN_STA1    0x228
#define INFRA_PROTECTEN_1       0x250
#define INFRA_PROTECTEN_STA1_1  0x258
#define INFRA_PROTECTEN_SET     0x2a0
#define INFRA_PROTECTEN_CLR     0x2a4
#define INFRA_PROTECTEN_1_SET   0x2a8
#define INFRA_PROTECTEN_1_CLR   0x2ac
#define INFRA_AP2MD_DUMMY       0x370
#define INFRA_MD_SRCCLKENA      0xf0c
#define PERI2MD_PROT            (1u << 7)
#define MDMASTER_PROT           ((1u << 3) | (1u << 4))
#define MD2PERI_PROT            (1u << 6)
#define CCIF0_RST               (1u << 8)
/* the MD side, AP view */
#define MD_BOOT_VECTOR_EN       0x20000024UL
#define MD_RGU_BASE             0x200f0100UL
#define MD_WDT_MODE_KEY         0x55000030u
#define MDTOP_PLLMIXED_BASE     0x20140000UL
#define MDTOP_CLKSW_BASE        0x20150000UL
/* CCIF */
#define APCCIF_BUSY             0x04
#define APCCIF_RCHNUM           0x10
#define APCCIF_ACK              0x14
#define APCCIF_CHDATA           0x100
#define CCIF_SRAM_SIZE          512
#define CCIF_RX_SRAM            15
#define CCIF_RX_EXCEPTION_INIT  16
/* shared memory, gen93 layout */
#define SMEM_MD_VIEW_BASE       0x40000000u
#define SMEM_MDSS_DBG_OFF       (2u * 1024)
#define SMEM_MDSS_DBG_SIZE      (10u * 1024)
#define SMEM_CCB_CTRL_OFF       (96u * 1024)
#define SMEM_NETD_OFF           (100u * 1024)
#define SMEM_AUDIO_OFF          (108u * 1024)
#define SMEM_CCISM_MCU_OFF      (160u * 1024)
#define SMEM_CCISM_MCU_SIZE     (721u * 1024)
#define SMEM_CCISM_MCU_EXP_OFF  (881u * 1024)
#define SMEM_CCISM_MCU_EXP_SIZE (121u * 1024)
#define SMEM_DFD_OFF            (1024u * 1024)
/* CCIF ring buffers */
#define CCIF_QUEUE_NUM          8
#define RBF_HEADER              0xee0000eeu
#define RBF_FOOTER              0xff0000ffu
#define RBF_CTL_LEN             (8 + 24 + 8)
#define RBF_RX_READ             0x8
#define RBF_RX_WRITE            0xc
#define RBF_RX_LENGTH           0x10
#define RBF_TX_LENGTH           0x1c
/* HS1 */
#define CCCI_HEADER_LEN         16
#define MD_INIT_START_BOOT      0x0
#define MD_INIT_CHK_ID          0x5555ffffu
#define MD_FEATURE_COUNT        64
#define MD_FEATURE_QUERY_PATTERN 0x49434343u
#define SRAM_HS1_LEN            (CCCI_HEADER_LEN + 4 + MD_FEATURE_COUNT + 4 + 152)
/* MT6357 VMODEM (a buck) through the PMIC wrapper */
#define PWRAP_BASE              0x1000d000UL
#define WACS2_CMD               0xc20
#define WACS2_RDATA             0xc24
#define WACS2_VLDCLR            0xc28
#define VMODEM_CON0             0x1588  /* bit 0: enable */
#define VMODEM_DBG0             0x15a2  /* what it is at, [6:0] */
#define VMODEM_ELR0             0x15aa  /* the selector, [6:0]: 500 mV + 6.25 mV * sel */
#define VMODEM_SEL_800MV        0x30

static const uint32_t rbf_rx_size[CCIF_QUEUE_NUM] = { 80 * 1024, 80 * 1024, 40 * 1024, 80 * 1024, 20 * 1024, 20 * 1024, 64 * 1024, 0 };
static const uint32_t rbf_tx_size[CCIF_QUEUE_NUM] = { 128 * 1024, 40 * 1024, 8 * 1024, 40 * 1024, 20 * 1024, 20 * 1024, 64 * 1024, 0 };
static const uint32_t rbf_exp_rx_size[CCIF_QUEUE_NUM] = { 12 * 1024, 32 * 1024, 8 * 1024, 0, 0, 0, 8 * 1024, 0 };
static const uint32_t rbf_exp_tx_size[CCIF_QUEUE_NUM] = { 12 * 1024, 32 * 1024, 8 * 1024, 0, 0, 0, 8 * 1024, 0 };

static vm_offset_t spm, infra, top, apmix, ccif, mdccif, bsta, pwrap, nc_smem;

/* from the loader (boot-args) */
static uint32_t h_smem, h_smem_sz, h_ccb, h_ccb_sz, h_img, h_img_sz, h_dfd;

/* the state as found, to put back */
static uint32_t found_clk_mode, found_srcclkena, found_ap2md, found_vm_con0, found_vm_elr0;
static bool found_vm_valid;
/* 0 not started, 1 powering on / running, 2 powered down again */
static volatile int md_state;
static uint32_t md_level;       /* the r1_md value: 3 = stage 1 + 2a */
static uint64_t md_t0;

static inline uint32_t
rd(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
wr(vm_offset_t base, uint32_t at, uint32_t v)
{
	*(volatile uint32_t *)(base + at) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline void
setb(vm_offset_t base, uint32_t at, uint32_t bits)
{
	wr(base, at, rd(base, at) | bits);
}

static inline void
clrb(vm_offset_t base, uint32_t at, uint32_t bits)
{
	wr(base, at, rd(base, at) & ~bits);
}

static const char *
md_cg(uint32_t sta, int bit)
{
	return (sta & (1u << bit)) ? "gated" : "ungated";
}

static uint32_t
md_ms(void)
{
	uint64_t ns = 0;

	absolutetime_to_nanoseconds(mach_absolute_time() - md_t0, &ns);
	return (uint32_t)(ns / NSEC_PER_MSEC);
}

static bool
md1_is_on(void)
{
	return (rd(spm, SPM_PWR_STATUS) & 1) && (rd(spm, SPM_PWR_STATUS_2ND) & 1);
}

/* ---- PMIC wrapper (as redwood_haptic.c does) ---- */

static bool
pw_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 200000; spin++) {
		if (((rd(pwrap, WACS2_RDATA) >> 16) & 7u) == state) {
			return true;
		}
	}
	return false;
}

static bool
pw_read(uint32_t reg, uint32_t *value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (pw_wait(0)) {
		wr(pwrap, WACS2_CMD, (reg >> 1) << 16);
		if (pw_wait(6)) {
			*value = rd(pwrap, WACS2_RDATA) & 0xffff;
			wr(pwrap, WACS2_VLDCLR, 1);
			ok = true;
		}
	}
	redwood_pwrap_unlock();
	return ok;
}

static bool
pw_write(uint32_t reg, uint32_t value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (pw_wait(0)) {
		wr(pwrap, WACS2_CMD, (1u << 31) | ((reg >> 1) << 16) | (value & 0xffff));
		ok = pw_wait(0);
	}
	redwood_pwrap_unlock();
	return ok;
}

/* ---- polls (bounded) ---- */

static bool
poll_infra(uint32_t reg, uint32_t mask, uint32_t want, const char *what)
{
	uint32_t v = 0;

	for (int i = 0; i < 100000; i++) {      /* 1 s at 10 us, as MTK_POLL_TIMEOUT */
		v = rd(infra, reg);
		if ((v & mask) == want) {
			SAY("r1md: [%u ms] %s: infra %#x = %#x (wanted %#x under %#x): ok", md_ms(), what, reg, v, want, mask);
			return true;
		}
		delay(10);
	}
	SAY("r1md: [%u ms] %s: infra %#x = %#x, wanted %#x under %#x: TIMED OUT", md_ms(), what, reg, v, want, mask);
	return false;
}

static void
dump_status(const char *when)
{
	SAY("r1md: %s: SPM PWR_STATUS %#x/%#x (MD1 %s) MD1_PWR_CON %#x SRAM_ISO %#x EXTRA %#x", when, rd(spm, SPM_PWR_STATUS),
	    rd(spm, SPM_PWR_STATUS_2ND), md1_is_on() ? "on" : "off", rd(spm, SPM_MD1_PWR_CON), rd(spm, SPM_MD_SRAM_ISO_CON),
	    rd(spm, SPM_MD_EXTRA_PWR_CON));
	SAY("r1md: %s: INFRA PROTECTEN %#x STA1 %#x PROTECTEN_1 %#x STA1_1 %#x AP2MD %#x SRCCLKENA %#x; TOPCKGEN CLK_MODE %#x", when,
	    rd(infra, INFRA_PROTECTEN), rd(infra, INFRA_PROTECTEN_STA1), rd(infra, INFRA_PROTECTEN_1), rd(infra, INFRA_PROTECTEN_STA1_1),
	    rd(infra, INFRA_AP2MD_DUMMY), rd(infra, INFRA_MD_SRCCLKENA), rd(top, 0));
}

/* ---- stage 1 ---- */

/* VMODEM at 0.8 V: enabled if it was not, the selector set if it was not (regulator_enable + regulator_set_voltage) */
static bool
vmodem_on(void)
{
	uint32_t con0 = 0, elr0 = 0, dbg0 = 0;

	if (!pw_read(VMODEM_CON0, &con0) || !pw_read(VMODEM_ELR0, &elr0) || !pw_read(VMODEM_DBG0, &dbg0)) {
		SAY("r1md: VMODEM: the PMIC wrapper did not answer");
		return false;
	}
	found_vm_con0 = con0;
	found_vm_elr0 = elr0;
	found_vm_valid = true;
	SAY("r1md: VMODEM before: CON0 %#x (%s) ELR0 %#x (sel %u = %u uV) DBG0 %#x", con0, (con0 & 1) ? "enabled" : "disabled", elr0,
	    elr0 & 0x7f, 500000 + 6250 * (elr0 & 0x7f), dbg0);
	if ((elr0 & 0x7f) != VMODEM_SEL_800MV) {
		pw_write(VMODEM_ELR0, (elr0 & ~0x7fu) | VMODEM_SEL_800MV);
	}
	if (!(con0 & 1)) {
		pw_write(VMODEM_CON0, con0 | 1);
	}
	delay_for_interval(2, NSEC_PER_MSEC);
	con0 = elr0 = dbg0 = 0;
	(void)pw_read(VMODEM_CON0, &con0);
	(void)pw_read(VMODEM_ELR0, &elr0);
	(void)pw_read(VMODEM_DBG0, &dbg0);
	SAY("r1md: VMODEM after: CON0 %#x ELR0 %#x (sel %u = %u uV) DBG0 %#x (sel %u = %u uV)", con0, elr0, elr0 & 0x7f,
	    500000 + 6250 * (elr0 & 0x7f), dbg0, dbg0 & 0x7f, 500000 + 6250 * (dbg0 & 0x7f));
	return (con0 & 1) && (elr0 & 0x7f) == VMODEM_SEL_800MV && (dbg0 & 0x7f) == VMODEM_SEL_800MV;
}

/* scpsys_power_on() for MD1: scpsys_modem_legacy_pwrseq_on, then the bus protection released in reverse order of bp_cfg */
static bool
md1_domain_on(void)
{
	bool on = false;

	clrb(spm, SPM_MD1_PWR_CON, PWR_RST_B);
	setb(spm, SPM_MD1_PWR_CON, PWR_ON);
	setb(spm, SPM_MD1_PWR_CON, PWR_ON_2ND);
	for (int i = 0; i < 100000 && !(on = md1_is_on()); i++) {
		delay(10);
	}
	SAY("r1md: [%u ms] MD1 power ack: %s, PWR_STATUS %#x/%#x MD1_PWR_CON %#x", md_ms(), on ? "on" : "NOT ON", rd(spm, SPM_PWR_STATUS),
	    rd(spm, SPM_PWR_STATUS_2ND), rd(spm, SPM_MD1_PWR_CON));
	if (!on) {
		return false;
	}
	/* scpsys_sram_enable: SRAM_PDN (bit 8) cleared (no ack bits), then the external SRAM isolation register's bit 0 SET */
	clrb(spm, SPM_MD1_PWR_CON, SRAM_PDN);
	setb(spm, SPM_MD_SRAM_ISO_CON, 1);
	SAY("r1md: SRAM: MD1_PWR_CON %#x MD_SRAM_ISO_CON %#x", rd(spm, SPM_MD1_PWR_CON), rd(spm, SPM_MD_SRAM_ISO_CON));
	clrb(spm, SPM_MD1_PWR_CON, PWR_ISO);
	clrb(spm, SPM_MD1_PWR_CON, PWR_CLK_DIS);
	setb(spm, SPM_MD1_PWR_CON, PWR_RST_B);
	clrb(spm, SPM_MD_EXTRA_PWR_CON, 1);
	SAY("r1md: legacy sequence done: MD1_PWR_CON %#x MD_SRAM_ISO_CON %#x MD_EXTRA_PWR_CON %#x", rd(spm, SPM_MD1_PWR_CON),
	    rd(spm, SPM_MD_SRAM_ISO_CON), rd(spm, SPM_MD_EXTRA_PWR_CON));
	/* bus protection released, last bp_cfg entry first; the ack is ignored on release (BUS_PROT_WR_IGN): the CLR register is written */
	wr(infra, INFRA_PROTECTEN_1_CLR, MD2PERI_PROT);
	SAY("r1md: release MD2PERI (CLR %#x <- %#x): PROTECTEN_1 %#x STA1_1 %#x", INFRA_PROTECTEN_1_CLR, MD2PERI_PROT,
	    rd(infra, INFRA_PROTECTEN_1), rd(infra, INFRA_PROTECTEN_STA1_1));
	wr(infra, INFRA_PROTECTEN_CLR, MDMASTER_PROT);
	SAY("r1md: release MD masters (CLR %#x <- %#x): PROTECTEN %#x STA1 %#x", INFRA_PROTECTEN_CLR, MDMASTER_PROT,
	    rd(infra, INFRA_PROTECTEN), rd(infra, INFRA_PROTECTEN_STA1));
	wr(infra, INFRA_PROTECTEN_CLR, PERI2MD_PROT);
	SAY("r1md: release PERI2MD (CLR %#x <- %#x): PROTECTEN %#x STA1 %#x", INFRA_PROTECTEN_CLR, PERI2MD_PROT,
	    rd(infra, INFRA_PROTECTEN), rd(infra, INFRA_PROTECTEN_STA1));
	/* md1a: STA1 still showed bits 3 and 7 straight after the CLR writes (the release acks are ignored, as in the reference); the
	 * reference's md_reachable() only ran after printk lines, i.e. later. Wait here (bounded, 100 ms) for the status to follow. */
	for (int i = 0; i < 10000; i++) {
		if (!(rd(infra, INFRA_PROTECTEN_STA1) & (PERI2MD_PROT | MDMASTER_PROT))) {
			SAY("r1md: [%u ms] release acked after %d us: STA1 %#x STA1_1 %#x", md_ms(), i * 10, rd(infra, INFRA_PROTECTEN_STA1),
			    rd(infra, INFRA_PROTECTEN_STA1_1));
			return true;
		}
		delay(10);
	}
	SAY("r1md: [%u ms] after 100 ms STA1 %#x STA1_1 %#x (PERI2MD/masters bits not clear)", md_ms(), rd(infra, INFRA_PROTECTEN_STA1),
	    rd(infra, INFRA_PROTECTEN_STA1_1));
	return true;
}

/* r1md_md_reachable: MD registers only with MD1 on and PERI2MD open */
static bool
md_reachable(void)
{
	uint32_t sta1 = rd(infra, INFRA_PROTECTEN_STA1);

	if (!md1_is_on()) {
		SAY("r1md: MD1 is not powered, not touching MD registers");
		return false;
	}
	if (sta1 & PERI2MD_PROT) {
		SAY("r1md: PERI2MD protection is active (STA1 %#x), not touching MD registers", sta1);
		return false;
	}
	return true;
}

static bool
pre_access(void)
{
	clrb(infra, INFRA_AP2MD_DUMMY, 1);
	wr(infra, INFRA_PROTECTEN_1_SET, MD2PERI_PROT);
	return poll_infra(INFRA_PROTECTEN_STA1_1, MD2PERI_PROT, MD2PERI_PROT, "pre-access MD2PERI protect");
}

static bool
post_access(void)
{
	wr(infra, INFRA_PROTECTEN_SET, PERI2MD_PROT);
	if (!poll_infra(INFRA_PROTECTEN_STA1, PERI2MD_PROT, PERI2MD_PROT, "post-access PERI2MD protect")) {
		return false;
	}
	wr(infra, INFRA_PROTECTEN_1_CLR, MD2PERI_PROT);
	if (!poll_infra(INFRA_PROTECTEN_STA1_1, MD2PERI_PROT, 0, "post-access MD2PERI release")) {
		return false;
	}
	setb(infra, INFRA_AP2MD_DUMMY, 1);
	SAY("r1md: post access: PROTECTEN %#x PROTECTEN_1 %#x AP2MD %#x", rd(infra, INFRA_PROTECTEN), rd(infra, INFRA_PROTECTEN_1),
	    rd(infra, INFRA_AP2MD_DUMMY));
	return true;
}

/* md1_pll_init(), every loop bounded, as r1md_pll_init */
static bool
pll_init(void)
{
	vm_offset_t pll = ml_io_map(MDTOP_PLLMIXED_BASE, 0x1000), clksw = ml_io_map(MDTOP_CLKSW_BASE, 0x1000);
	uint32_t val = 0;
	int i;

	for (i = 0; i < 51; i++) {      /* readl_poll_timeout(pll, val, val, 20 ms, 1 s) */
		val = rd(pll, 0);
		if (val) {
			break;
		}
		delay_for_interval(20, NSEC_PER_MSEC);
	}
	SAY("r1md: [%u ms] pll: version %#x (%d polls)", md_ms(), val, i);
	if (!val) {
		SAY("r1md: pll: MD PLL block reads 0 after 1 s; MD clock/power path is not up");
		return false;
	}
	/* clock square1 low-pass filter for 26 MHz quality */
	setb(apmix, 0, 1u << 1);
	delay(150);
	SAY("r1md: pll: APMIXED AP_PLL_CON0 %#x", rd(apmix, 0));
	/* writes of the reference, in its order (the registers it does not read back are not read back here either) */
	wr(pll, 0x4, 0x02020e88);       /* md_srclkena_ack settle time: 136T of 32 kHz */
	wr(pll, 0x60, 0x801713b1);
	wr(pll, 0x58, 0x80171400);
	wr(pll, 0x50, 0x80229e00);
	wr(pll, 0x48, 0x80204e00);
	wr(pll, 0x40, 0x80213c00);
	for (i = 0; i < 10001; i++) {   /* readl_poll_timeout(pll + 0xc00, !(val & BIT(14)), 10 us, 100 ms) */
		val = rd(pll, 0xc00);
		if (!(val & (1u << 14))) {
			break;
		}
		delay(10);
	}
	SAY("r1md: [%u ms] pll: +0xc00 %#x (bit 14 %s)", md_ms(), val, (val & (1u << 14)) ? "STAYS SET" : "clear");
	if (val & (1u << 14)) {
		return false;
	}
	wr(pll, 0x64, rd(pll, 0x64) & ~0x80u);
	SAY("r1md: pll: +0x64 %#x", rd(pll, 0x64));
	wr(pll, 0x10, 0x100010);
	for (i = 1; i <= 50; i++) {
		val = rd(pll, 0x10);
		if (val == 0x100010) {
			break;
		}
		if (!(i % 5)) {
			SAY("r1md: pll: rewrite 0x100010 (%d)", i);
			wr(pll, 0x10, 0x100010);
		}
		delay_for_interval(20, NSEC_PER_MSEC);
	}
	SAY("r1md: [%u ms] pll: +0x10 %#x after %d tries", md_ms(), val, i);
	if (val != 0x100010) {
		return false;
	}
	for (i = 0; i < 50; i++) {
		val = rd(clksw, 0x84);
		if (val & (1u << 15)) {
			break;
		}
		delay_for_interval(20, NSEC_PER_MSEC);
	}
	SAY("r1md: [%u ms] pll: clksw +0x84 %#x", md_ms(), val);
	if (!(val & (1u << 15))) {
		SAY("r1md: pll: MD clock switch never reported ready");
		return false;
	}
	wr(clksw, 0x24, rd(clksw, 0x24) | 0x3);
	wr(clksw, 0x24, rd(clksw, 0x24) | 0x58103fc);
	wr(clksw, 0x28, rd(clksw, 0x28) | 0x10);
	wr(clksw, 0x20, 0x1);
	SAY("r1md: pll: clksw +0x24 %#x +0x28 %#x", rd(clksw, 0x24), rd(clksw, 0x28));
	wr(pll, 0x314, 0xffff);
	wr(pll, 0x318, 0xffff);
	wr(pll, 0xf00, 0x62930000);     /* record that the MD PLLs are initialised */
	SAY("r1md: pll: init done");
	return true;
}

static void
md_wdt_off(void)
{
	vm_offset_t rgu = ml_io_map(MD_RGU_BASE & ~0xfffUL, 0x1000) + (MD_RGU_BASE & 0xfff);

	wr(rgu, 0, MD_WDT_MODE_KEY);
	SAY("r1md: MD WDT mode %#x", rd(rgu, 0));
}

/* the CCIF, CLDMA clocks (the clk_bulk_prepare_enable of the stage-2 probe): clear the gate bits through the CLR registers */
static void
clocks_on(void)
{
	wr(infra, 0x8c, (1u << 12) | (1u << 13) | (1u << 23) | (1u << 26));     /* CG1: CCIF1_AP, CCIF1_MD, CCIF_AP, CCIF_MD */
	wr(infra, 0xa8, 1u << 3);                                               /* CG2: CLDMA_BCLK */
	wr(infra, 0xc4, (1u << 18) | (1u << 19));                               /* CG3: CCIF2_AP, CCIF2_MD */
	SAY("r1md: clocks ungated: CG1 %#x CG2 %#x CG3 %#x", rd(infra, 0x94), rd(infra, 0xac), rd(infra, 0xc8));
}

static void
zero_range(uint32_t off, uint32_t len)
{
	volatile uint64_t *p;

	if (off >= h_smem_sz) {
		return;
	}
	if (len > h_smem_sz - off) {
		len = h_smem_sz - off;
	}
	p = (volatile uint64_t *)(nc_smem + off);
	for (uint32_t i = 0; i < len / 8; i++) {
		p[i] = 0;
	}
}


/* ---- sub-stage 2a ---- */

#define CCIF_BUSY_BIT15         (1u << 15)
#define SMEM_RT_OFF             0xe800u         /* RAW_RUNTIME_DATA: the AP's feature list, 0x800 bytes; the MD part follows at +0xf000 */
#define SMEM_RT_SIZE            0x800u
#define SMEM_DBM_OFF            0xff50u
#define SRAM_UP_HDR             0xf0u
#define SRAM_FEAT_V21           0x100u
#define SMEM_RING_OFF           SMEM_CCISM_MCU_OFF
#define RING_USED_EXPECT        0xb0140u        /* feature 2's size, = Stage 1's "0xb0140" */
#define IMG_SIZE_STOCK          0x0163f500u     /* md1_chk img_size of the stock capture (mtkmodem.h does not pass the tag's value) */
#define RING_MAGIC_HEAD         0xaabbaabbu
#define RING_MAGIC_TAIL         0xccddeeffu
#define RX_QUEUES               7
#define MAX_PKT_LOG             40

static uint32_t rb_off[CCIF_QUEUE_NUM], rb_total;

static void
rb_layout(void)
{
	uint32_t o = 0;

	for (int q = 0; q < CCIF_QUEUE_NUM; q++) {
		rb_off[q] = o;
		o += RBF_CTL_LEN + rbf_rx_size[q] + rbf_tx_size[q];
	}
	rb_total = o;
}

/* CCB control rows (docs/cellular-stage2.md 1.9): dl_page, ul_page, dl_buff, ul_buff */
struct ccb_row { uint32_t dl_page, ul_page, dl_buff, ul_buff; };
#define CCB_ROWS 20
static const struct ccb_row ccb_rows[CCB_ROWS] = {
	{ 1024, 1024, 32768, 32768 },           /* 0 DHL ctrl */
	{ 20480, 128, 122880, 128 },            /* 1 DHL exception */
	{ 512, 1024, 36864, 32768 },            /* 2 DHL PS */
	{ 512, 128, 32768, 128 },               /* 3-5 DHL HWLOGGER1-3 */
	{ 512, 128, 32768, 128 },
	{ 512, 128, 131072, 128 },
	{ 128, 128, 128, 128 }, { 128, 128, 128, 128 }, { 128, 128, 128, 128 }, { 128, 128, 128, 128 },      /* 6-16 DHL dummy */
	{ 128, 128, 128, 128 }, { 128, 128, 128, 128 }, { 128, 128, 128, 128 }, { 128, 128, 128, 128 },
	{ 128, 128, 128, 128 }, { 128, 128, 128, 128 }, { 128, 128, 128, 128 },
	{ 128, 128, 640, 128 },                 /* 17 DHL padding */
	{ 512, 1024, 16384, 16384 },            /* 18 MD_MONITOR */
	/* 19 META: GUESS (haretic's table, 63K ul; MediaTek's public 4.14 table has 65K x 8 = 0x104000; the stock log says 0x100000) */
	{ 66560, 64512, 532480, 516096 },
};

static bool
ccb_ctrl_write(void)
{
	uint32_t dhl = 0, mdm = 0, meta = 0;

	zero_range(SMEM_CCB_CTRL_OFF, 0x1000);
	for (int r = 0; r < CCB_ROWS; r++) {
		vm_offset_t p = nc_smem + SMEM_CCB_CTRL_OFF + r * 64;
		uint32_t sum = ccb_rows[r].dl_buff + ccb_rows[r].ul_buff;

		*(volatile uint32_t *)(p + 0x14) = ccb_rows[r].dl_page;
		*(volatile uint32_t *)(p + 0x18) = ccb_rows[r].dl_buff;
		*(volatile uint32_t *)(p + 0x1c) = 0xeeff0011u;
		*(volatile uint32_t *)(p + 0x34) = ccb_rows[r].ul_page;
		*(volatile uint32_t *)(p + 0x38) = ccb_rows[r].ul_buff;
		*(volatile uint32_t *)(p + 0x3c) = 0xeeff0011u;
		if (r < 18) {
			dhl += sum;
		} else if (r == 18) {
			mdm += sum;
		} else {
			meta += sum;
		}
	}
	SAY("r1md: ccb ctrl: %d rows, dhl %#x mdm %#x meta %#x (the META row is a GUESS: haretic's 63K ul table)", CCB_ROWS, dhl, mdm, meta);
	if (dhl != 0x70000 || mdm != 0x8000 || meta != 0x100000 || dhl + mdm + meta != 0x178000) {
		SAY("r1md: CCB SUM WRONG (%#x, want 0x178000)", dhl + mdm + meta);
		return false;
	}
	return true;
}

static void
dbm_write(void)
{
	volatile uint32_t *p = (volatile uint32_t *)(nc_smem + SMEM_DBM_OFF);

	p[0] = p[1] = 0x44444444u;
	for (int i = 0; i < 40; i++) {
		p[2 + i] = 0;
	}
	p[42] = p[43] = 0x44444444u;
	SAY("r1md: dbm written at smem +%#x", SMEM_DBM_OFF);
}

/* runtime-data feature entries (1.4): word0 = id | support << 8, data_len, data; every id 0..63 once */
struct rt_feat { uint8_t sup, nw; uint32_t w[4]; };
#define RF(sup, nw, a, b, c, d) { sup, nw, { a, b, c, d } }
static struct rt_feat rt_feat[MD_FEATURE_COUNT] = {
	[0] = RF(2, 4, 0, 0, 0, 0),
	[1] = RF(2, 2, 0x40000000, 0x10000, 0, 0),
	[2] = RF(2, 2, 0x40028000, RING_USED_EXPECT, 0, 0),
	[3] = RF(1, 0, 0, 0, 0, 0), [4] = RF(1, 0, 0, 0, 0, 0),
	[5] = RF(2, 4, 0, 0, 0, 0), [6] = RF(2, 4, 0, 0, 0, 0),
	[7] = RF(2, 4, 0, 0, 0, 0),             /* seed filled in */
	[8] = RF(2, 4, 0, 0, 0, 0),
	[9] = RF(2, 4, 0, 0x3f, 0, 0),          /* sbp 0, rat 0x3f */
	[10] = RF(2, 4, 3, 0, 0, 0),            /* bit0 seq check, bit1 poll status */
	[11] = RF(2, 4, 0, 0, 0, 0),            /* UTC seconds lo/hi filled in, tz 0 dst 0 */
	[12] = RF(2, 4, 4, 0, 0, 0),
	[13] = RF(2, 2, 0x8e000000, IMG_SIZE_STOCK, 0, 0),      /* AP physical; filled from the handoff */
	[14] = RF(2, 2, 0x40010000, 0x8000, 0, 0),
	[15] = RF(2, 4, 0x40018000, 0x1000, 0x46000000, 0x178000),
	[16] = RF(2, 2, 0x46200000, 0x01400000, 0, 0),
	[17] = RF(2, 2, 0x40019000, 0x1000, 0, 0),
	[18] = RF(2, 2, 0x4001a000, 0x1000, 0, 0),
	[19] = RF(2, 4, 0, 0, 0, 0),
	[20] = RF(2, 1, 0xdf0, 0, 0, 0),
	[21] = RF(1, 0, 0, 0, 0, 0), [22] = RF(1, 0, 0, 0, 0, 0), [23] = RF(1, 0, 0, 0, 0, 0), [24] = RF(1, 0, 0, 0, 0, 0),
	[25] = RF(2, 2, 0x40170000, 0, 0, 0),
	[26] = RF(2, 2, 0x4001b000, 0xd000, 0, 0),
	[27] = RF(2, 0, 0, 0, 0, 0),
	[28] = RF(2, 2, 0x400dc400, 0x1e400, 0, 0),
	[29] = RF(1, 0, 0, 0, 0, 0),
	[30] = RF(2, 2, 0x46000000, 0x01400000, 0, 0),
	[31] = RF(1, 0, 0, 0, 0, 0),
	[32] = RF(2, 1, 0, 0, 0, 0),
	[33] = RF(1, 0, 0, 0, 0, 0), [34] = RF(1, 0, 0, 0, 0, 0),
};

static bool rt_sent;

/* md_cd_send_runtime_data_v2: feature list, SRAM header + v2_1 struct, BUSY, TCHNUM. Only the reply: nothing else is written to the MD. */
static bool
send_rt_data(const uint32_t *hs1_words)
{
	const uint8_t *feat = (const uint8_t *)hs1_words + CCCI_HEADER_LEN + 4;
	volatile uint32_t *rt = (volatile uint32_t *)(nc_smem + SMEM_RT_OFF);
	uint32_t n = 0, secs, usecs, seed = 0, busy;

	for (int i = 0; i < MD_FEATURE_COUNT; i++) {
		if ((feat[i] & 0xf) == 2 && rt_feat[i].sup != 2) {
			SAY("r1md: 2a: MD requires feature %d we lack", i);
			return false;
		}
	}
	read_random(&seed, sizeof(seed));
	clock_get_calendar_microtime(&secs, &usecs);
	rt_feat[7].w[0] = seed;
	rt_feat[11].w[0] = secs;
	rt_feat[11].w[1] = 0;
	rt_feat[13].w[0] = h_img;
	SAY("r1md: 2a: id 13 image size %#x (the stock capture's md1_chk value; the loader does not pass the tag's)", rt_feat[13].w[1]);
	if (rb_total != RING_USED_EXPECT) {
		SAY("r1md: 2a: ring buffers %#x bytes, feature 2 wants %#x", rb_total, RING_USED_EXPECT);
		return false;
	}
	rt_feat[2].w[1] = rb_total;
	for (uint32_t id = 0; id < MD_FEATURE_COUNT; id++) {
		const struct rt_feat *f = &rt_feat[id];

		if (n + 8 + 4u * f->nw > SMEM_RT_SIZE) {
			SAY("r1md: 2a: runtime data over %#x bytes at id %u", SMEM_RT_SIZE, id);
			return false;
		}
		rt[n / 4] = id | ((uint32_t)f->sup << 8);
		rt[n / 4 + 1] = 4u * f->nw;
		for (uint32_t k = 0; k < f->nw; k++) {
			rt[n / 4 + 2 + k] = f->w[k];
		}
		n += 8 + 4u * f->nw;
	}
	SAY("r1md: 2a: runtime data %#x bytes at smem +%#x", n, SMEM_RT_OFF);
	for (int l = 0; l < 2; l++) {
		SAY("r1md: 2a: rt[%d..%d] %08x %08x %08x %08x %08x %08x %08x %08x", l * 8, l * 8 + 7, rt[l * 8], rt[l * 8 + 1], rt[l * 8 + 2], rt[l * 8 + 3],
		    rt[l * 8 + 4], rt[l * 8 + 5], rt[l * 8 + 6], rt[l * 8 + 7]);
	}
	/* up_header, then ap_query_md_feature_v2_1 (156 bytes) */
	wr(ccif, APCCIF_CHDATA + SRAM_UP_HDR + 0, 0);
	wr(ccif, APCCIF_CHDATA + SRAM_UP_HDR + 4, 0xac);
	wr(ccif, APCCIF_CHDATA + SRAM_UP_HDR + 8, 1);           /* channel CCCI_CONTROL_TX 1, seq 0, assert 0 */
	wr(ccif, APCCIF_CHDATA + SRAM_UP_HDR + 12, MD_INIT_CHK_ID);
	for (uint32_t i = 0; i < 156; i += 4) {
		wr(ccif, APCCIF_CHDATA + SRAM_FEAT_V21 + i, 0);
	}
	wr(ccif, APCCIF_CHDATA + SRAM_FEAT_V21 + 0x00, 0x43434349);
	wr(ccif, APCCIF_CHDATA + SRAM_FEAT_V21 + 0x04 + 4 * 0, 0x00001000);     /* feature_set[1] = 0x10: byte 1 of the array */
	wr(ccif, APCCIF_CHDATA + SRAM_FEAT_V21 + 0x44, 2);      /* share_memory_support = MULTI_MD_MPU_SUPPORT */
	wr(ccif, APCCIF_CHDATA + SRAM_FEAT_V21 + 0x48, SMEM_MD_VIEW_BASE + SMEM_RT_OFF);
	wr(ccif, APCCIF_CHDATA + SRAM_FEAT_V21 + 0x4c, SMEM_RT_SIZE);
	wr(ccif, APCCIF_CHDATA + SRAM_FEAT_V21 + 0x50, SMEM_MD_VIEW_BASE + SMEM_RT_OFF + SMEM_RT_SIZE);
	wr(ccif, APCCIF_CHDATA + SRAM_FEAT_V21 + 0x54, SMEM_RT_SIZE);
	wr(ccif, APCCIF_CHDATA + SRAM_FEAT_V21 + 0x58, SMEM_MD_VIEW_BASE);
	wr(ccif, APCCIF_CHDATA + SRAM_FEAT_V21 + 0x5c, h_smem_sz);
	wr(ccif, APCCIF_CHDATA + SRAM_FEAT_V21 + 0x60, 0x46000000);
	wr(ccif, APCCIF_CHDATA + SRAM_FEAT_V21 + 0x64, h_ccb_sz);
	wr(ccif, APCCIF_CHDATA + SRAM_FEAT_V21 + 0x98, 0x43434349);
	__asm__ volatile ("dsb sy" ::: "memory");
	busy = rd(ccif, APCCIF_BUSY);
	if (busy & CCIF_BUSY_BIT15) {
		SAY("r1md: 2a: CCIF busy bit 15 set (%#x), retry in 10 ms", busy);
		delay_for_interval(10, NSEC_PER_MSEC);
		busy = rd(ccif, APCCIF_BUSY);
		if (busy & CCIF_BUSY_BIT15) {
			SAY("r1md: 2a: CCIF still busy (%#x): the reply is NOT sent", busy);
			return false;
		}
	}
	wr(ccif, APCCIF_BUSY, CCIF_BUSY_BIT15);
	wr(ccif, 0x0c, 15);     /* TCHNUM */
	rt_sent = true;
	SAY("r1md: [%u ms] 2a: runtime data sent (BUSY %#x)", md_ms(), rd(ccif, APCCIF_BUSY));
	return true;
}

/* ---- the RX rings, read only except for the read pointer ---- */

static struct {
	uint32_t pkts, fs, rpc, ctl, sys, other, logged, decoded, t_last, answered, unanswered, sys_late, ctl_odd, oth_bad;
	bool hs2, garbage[RX_QUEUES];
	uint32_t t_hs2;
} st2a;

static inline uint32_t
rb_word(vm_offset_t buf, uint32_t len, uint32_t pos)
{
	return *(volatile uint32_t *)(buf + (pos % len));
}


/* ---- sub-stage 2b: the TX path and the answers ---- */

static uint32_t tx_seq[128];
static uint32_t last_ans_op, last_ans_ms, ee_have;
static char ee_text[64];

static inline void
rb_put(vm_offset_t buf, uint32_t len, uint32_t pos, uint32_t v)
{
	*(volatile uint32_t *)(buf + (pos % len)) = v;
}

/* md_ccif_op_send_skb: packet = ccci_header {h0, h1, ch | seq << 16 | assert << 31, resv} + body bytes (multiple of 4) */
static bool
ring_tx(int q, uint32_t ch, uint32_t h0, uint32_t h1, uint32_t resv, const uint32_t *body, uint32_t bodybytes, uint32_t rel)
{
	vm_offset_t b = nc_smem + SMEM_RING_OFF + rb_off[q], buf = b + 0x20 + rbf_rx_size[q];
	uint32_t len = rbf_tx_size[q], r = *(volatile uint32_t *)(b + 0x14), w = *(volatile uint32_t *)(b + 0x18);
	uint32_t L = CCCI_HEADER_LEN + bodybytes, padded = (L + 16 + 7) & ~7u, avail, seq, asrt, pos;

	if (r >= len || w >= len || (r & 3) || (w & 3)) {
		SAY("r1md: TX q%d ring pointers odd (r %#x w %#x): not sent", q, r, w);
		return false;
	}
	avail = (r == w) ? len - 1 : (r < w ? len - w - 1 + r : r - w - 1);
	if (padded >= avail) {
		SAY("r1md: TX q%d full (need %#x, free %#x): not sent", q, padded, avail);
		return false;
	}
	seq = tx_seq[ch & 127]++ & 0x7fff;
	asrt = ((ch == 15 || ch == 33) && st2a.hs2) ? 0 : 1;    /* FS/RPC TX carry the assert bit only while waiting for HS2 */
	pos = w;
	rb_put(buf, len, pos, RING_MAGIC_HEAD);
	rb_put(buf, len, pos + 4, L);
	rb_put(buf, len, pos + 8, h0);
	rb_put(buf, len, pos + 12, h1);
	rb_put(buf, len, pos + 16, ch | (seq << 16) | (asrt << 31));
	rb_put(buf, len, pos + 20, resv);
	for (uint32_t i = 0; i < (bodybytes + 3) / 4; i++) {    /* the caller's last word is zero padded (AT text is not a multiple of 4) */
		rb_put(buf, len, pos + 24 + 4 * i, body[i]);
	}
	for (uint32_t o = (L + 3) & ~3u; o < ((L + 7) & ~7u); o += 4) {
		rb_put(buf, len, pos + 8 + o, 0);
	}
	rb_put(buf, len, pos + 8 + ((L + 7) & ~7u), RING_MAGIC_TAIL);
	rb_put(buf, len, pos + 12 + ((L + 7) & ~7u), RING_MAGIC_TAIL);
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(b + 0x18) = (w + padded) % len;
	__asm__ volatile ("dsb sy" ::: "memory");
	if (!(rd(ccif, APCCIF_BUSY) & (1u << q))) {
		wr(ccif, APCCIF_BUSY, 1u << q);
		wr(ccif, 0x0c, q);      /* TCHNUM */
	}
	if (md_level < 5 || ch != 15) {
		SAY("r1md: [%u ms] tx q%d ch %u seq %u len %u", rel, q, ch, seq, L);
	}
	return true;
}

/* an RPC answer under construction: word 0 = op | 0xffff0000, word 1 = para count, then {len, data padded to 4} each */
static uint32_t ob[256], on, onp;

static void
ob_begin(uint32_t op)
{
	ob[0] = op | 0xffff0000u;
	ob[1] = 0;
	on = 2;
	onp = 0;
}

static void
ob_word(uint32_t v)
{
	ob[on++] = 4;
	ob[on++] = v;
	onp++;
}

static void
ob_fill(uint32_t bytes, uint8_t b)
{
	ob[on++] = bytes;
	for (uint32_t i = 0; i < (bytes + 3) / 4; i++) {
		ob[on++] = b * 0x01010101u;
	}
	onp++;
}

static void
ob_copy(const uint8_t *src, uint32_t bytes)
{
	ob[on++] = bytes;
	for (uint32_t i = 0; i < (bytes + 3) / 4; i++) {
		uint32_t v = 0;

		memcpy(&v, src + 4 * i, bytes - 4 * i < 4 ? bytes - 4 * i : 4);
		ob[on++] = v;
	}
	onp++;
}

struct gpio_name { const char *name; uint32_t pin; };
static const struct gpio_name gpio_names[] = {
	{ "GPIO_SIM1_HOT_PLUG", 47 }, { "GPIO_SIM2_HOT_PLUG", 46 }, { "GPIO_SIM1_SIO", 40 }, { "GPIO_SIM1_SRST", 39 },
	{ "GPIO_SIM1_SCLK", 38 }, { "GPIO_SIM2_SCLK", 37 }, { "GPIO_SIM2_SRST", 36 }, { "GPIO_SIM2_SIO", 35 },
};

/* the kernel's RPC answers (1.8). Returns the answer's op for the log, or 0 when nothing is sent (userspace ops). */
static void
rpc_serve(const uint32_t *pkt, uint32_t L, uint32_t ncopy, uint32_t rel)
{
	const uint8_t *pb = (const uint8_t *)pkt;
	uint32_t op = pkt[4], nargs = pkt[5], aoff[6], alen[6], off = 0x18;
	char name[49];
	int32_t st = 0;
	bool send = true;

	if (L < 24 || ncopy < L || nargs > 6) {
		SAY("r1md: RPC REQUEST NOT PARSED: op %#x nargs %u len %u (copied %u): NOT ANSWERED", op, nargs, L, ncopy);
		st2a.unanswered++;
		return;
	}
	for (uint32_t i = 0; i < nargs; i++) {
		if (off + 4 > L || off + 4 + ((pkt[off / 4] + 3) & ~3u) > L) {
			SAY("r1md: RPC REQUEST BAD ARGS: op %#x arg %u: NOT ANSWERED", op, i);
			st2a.unanswered++;
			return;
		}
		alen[i] = pkt[off / 4];
		aoff[i] = off + 4;
		off += 4 + ((alen[i] + 3) & ~3u);
	}
	name[0] = 0;
	if (nargs) {
		uint32_t n = 0;

		for (uint32_t i = 0; i < alen[0] && n < 48; i++) {
			uint8_t c = pb[aoff[0] + i];

			if (!c) {
				break;
			}
			name[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
		}
		name[n] = 0;
	}
	ob_begin(op);
	switch (op) {
	case 0x4002: {  /* GET_GPIO_NUM */
		int found = -1;

		for (unsigned i = 0; i < sizeof(gpio_names) / sizeof(gpio_names[0]); i++) {
			if (!strcmp(name, gpio_names[i].name) || !strcmp(name, gpio_names[i].name + 5)) {
				found = (int)i;
			}
		}
		if (found < 0) {
			SAY("r1md: RPC 0x4002 UNKNOWN GPIO NAME \"%s\" -> -5", name);
			st = -5;
			ob_word((uint32_t)-5);
			ob_word((uint32_t)-5);
		} else {
			ob_word(gpio_names[found].pin);
			ob_word(gpio_names[found].pin);
		}
		SAY("r1md: rpc op 0x4002 name \"%s\" -> pin %d", name, found < 0 ? -5 : (int)gpio_names[found].pin);
		break;
	}
	case 0x4005: {  /* GET_EINT_ATTR (name, ..., u32 type) */
		uint32_t type = nargs ? *(const uint32_t *)(pb + aoff[nargs - 1]) : 0xffff, val = 0;
		int sim = strnstr(name, "SIM1", 48) ? 0 : (strnstr(name, "SIM2", 48) ? 1 : -1);

		if (sim < 0 || !strnstr(name, "HOT_PLUG_EINT", 48) || nargs < 2) {
			SAY("r1md: RPC 0x4005 UNKNOWN EINT NAME \"%s\" (nargs %u type %u) -> -11", name, nargs, type);
			ob_word((uint32_t)-11);
			ob_word((uint32_t)-11);
			st = -11;
		} else if (type == 2 || type == 3 || type > 6) {
			SAY("r1md: rpc op 0x4005 \"%s\" type %u (POLA/SENS/unknown: not in the DT) -> -12", name, type);
			ob_word((uint32_t)-12);
			ob_word((uint32_t)-12);
			st = -12;
		} else {
			val = type == 6 ? (uint32_t)(sim + 1) : (type == 0 ? (uint32_t)sim : (type == 1 ? 10000u : 0u));
			ob_word(0);
			ob_word(val);
			SAY("r1md: rpc op 0x4005 \"%s\" type %u -> 0, %u", name, type, val);
		}
		break;
	}
	case 0x400e:    /* DTSI_QUERY: both md_attr_node properties are 0x0f0f0f0f */
		ob_word(0);
		ob_fill(68, 0x0f);
		SAY("r1md: rpc op 0x400e DTSI_QUERY (arg0 len %u) -> 0, 68 x 0x0f", nargs ? alen[0] : 0);
		break;
	case 0x4006:
	case 0x4007:    /* GPIO_VAL / ADC_VAL: no level known (GUESS 0); ADC -1 */
		ob_word(op == 0x4007 ? (uint32_t)-1 : 0);
		ob_word(op == 0x4007 ? (uint32_t)-1 : 0);
		SAY("r1md: RPC op %#x \"%s\" answered with a GUESS (%d)", op, name, op == 0x4007 ? -1 : 0);
		break;
	case 0x4003:
	case 0x4001:
		ob_word((uint32_t)-5);
		ob_word((uint32_t)-5);
		SAY("r1md: rpc op %#x -> -5, -5", op);
		break;
	case 0x4004:
		ob_word((uint32_t)-5);
		ob_word((uint32_t)-5);
		ob_word((uint32_t)-5);
		SAY("r1md: rpc op 0x4004 -> -5 x3");
		break;
	case 0x4009:    /* GET_GPIO_ADC: {4, 0} + 32 (161-byte input) or 88 (425-byte input) bytes of 0x0f; no pins found (log) */
		ob_word(0);
		ob_fill(nargs && alen[0] >= 400 ? 88 : 32, 0x0f);
		SAY("r1md: rpc op 0x4009 GET_GPIO_ADC (arg0 len %u) -> 0, %u x 0x0f (no found pins)", nargs ? alen[0] : 0, nargs && alen[0] >= 400 ? 88 : 32);
		break;
	case 0x4012: {
		uint32_t rnd = 0;

		read_random(&rnd, sizeof(rnd));
		ob_word(0);
		ob_word(rnd);
		SAY("r1md: rpc op 0x4012 TRNG -> 0, <random>");
		break;
	}
	case 0x4013:    /* QUERY_CARD_TYPE: the inputs echoed */
		for (uint32_t i = 0; i < nargs; i++) {
			ob_copy(pb + aoff[i], alen[i]);
		}
		SAY("r1md: rpc op 0x4013 QUERY_CARD_TYPE: %u inputs echoed", nargs);
		break;
	case 0x4321:
		ob_word(1);
		ob_word(0xa5a5);
		SAY("r1md: rpc op 0x4321 IT_OP -> 1, 0xa5a5");
		break;
	case 0x400f: case 0x4010: case 0x4011: case 0x4014: case 0x4015:
		SAY("r1md: USER-SPACE RPC op %#x nargs %u arg0 len %u: logged, NOT ANSWERED", op, nargs, nargs ? alen[0] : 0);
		send = false;
		break;
	default:
		SAY("r1md: UNKNOWN RPC op %#x nargs %u arg0 len %u \"%s\": answered -1", op, nargs, nargs ? alen[0] : 0, name);
		ob_word((uint32_t)-1);
		st = -1;
		break;
	}
	(void)st;
	if (!send) {
		st2a.unanswered++;
		return;
	}
	ob[1] = onp;
	if (ring_tx(1, 33, pkt[0], 16 + on * 4, pkt[3], ob, on * 4, rel)) {
		st2a.answered++;
		last_ans_op = op;
		last_ans_ms = rel;
	}
}

static void
sys_serve(uint32_t id, uint32_t resv, uint32_t rel)
{
	if (id == 0x105) {      /* MD_GET_BATTERY_INFO: haretic answers 0 */
		SAY("r1md: sys msg 0x105 MD_GET_BATTERY_INFO: answered 0 mV (as haretic does)");
		if (ring_tx(0, 3, 0xffffffffu, 0x105, 0, NULL, 0, rel)) {
			st2a.answered++;
			last_ans_op = 0x105;
			last_ans_ms = rel;
		}
	} else if (id == 0x114 || id == 0x116) {
		SAY("r1md: sys msg %#x test message: echoed as %#x", id, id + 1);
		if (ring_tx(0, 3, 0xffffffffu, id + 1, resv, NULL, 0, rel)) {
			st2a.answered++;
			last_ans_op = id + 1;
			last_ans_ms = rel;
		}
	}
}

/* printable ASCII low bytes of a UTF-16LE name at pkt byte offset off, up to 64 characters */
static void
utf16_name(const uint8_t *b, uint32_t have, uint32_t off, uint32_t bytes, char *out, uint32_t outsz)
{
	uint32_t n = 0;

	for (uint32_t i = 0; off + 2 * i + 1 < have && 2 * i + 1 < bytes && n < 64 && n + 1 < outsz; i++) {
		uint8_t c = b[off + 2 * i];

		if (b[off + 2 * i + 1] != 0) {
			strlcpy(out, "<not utf16>", outsz);
			return;
		}
		out[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
	}
	out[n] = 0;
}

static bool
fs_op_has_name(uint32_t op)
{
	switch (op) {
	case 0x100a: case 0x100d: case 0x100e: case 0x1001: case 0x1007: case 0x1008: case 0x100b: case 0x100f: case 0x1010: case 0x1011: case 0x1012: case 0x1018:
	case 0x1021: case 0x1022: case 0x1024: case 0x1025:
		return true;
	}
	return false;
}

/* ---- sub-stage 2c: the FS channel. Requests (ch 14) are reassembled per slot, answered by redwood_mdfs.c, the reply split at 0xd80 ---- */

#define FS_SLOTS        5
#define FS_BUF          0x40dc
#define FS_FRAG         0xd80
static uint8_t *fs_req[FS_SLOTS];
static uint32_t fs_have[FS_SLOTS], fs_last_ms, fs_bad;
static uint32_t *fs_out, fs_frag[1 + FS_FRAG / 4];

static void
fs_rx(const uint32_t *pkt, uint32_t L, uint32_t data0, uint32_t slot, uint32_t rel)
{
	uint32_t n, body, sent = 0, h0 = data0 & ~0xc0000000u;

	if (slot >= FS_SLOTS || L < 0x14 || L > FS_BUF || fs_out == NULL) {
		fs_bad++;
		SAY("r1md: fs REQUEST DROPPED: slot %u len %u", slot, L);
		return;
	}
	if (fs_req[slot] == NULL && (fs_req[slot] = kalloc_data(FS_BUF, Z_WAITOK | Z_ZERO)) == NULL) {
		return;
	}
	if (fs_have[slot] == 0) {
		memcpy(fs_req[slot], pkt, L);
		fs_have[slot] = L;
	} else if (fs_have[slot] + (L - 0x14) <= FS_BUF) {
		memcpy(fs_req[slot] + fs_have[slot], (const uint8_t *)pkt + 0x14, L - 0x14);
		fs_have[slot] += L - 0x14;
	} else {
		fs_bad++;
		fs_have[slot] = 0;
		SAY("r1md: fs request too long: dropped");
		return;
	}
	if (data0 & 0x80000000u) {
		return;         /* more fragments follow */
	}
	n = mdfs_handle(fs_req[slot] + 0x10, fs_have[slot] - 0x10, fs_out);
	fs_have[slot] = 0;
	fs_last_ms = rel;
	body = n - 4;
	do {
		uint32_t chunk = body - sent > FS_FRAG ? FS_FRAG : body - sent;
		bool more = sent + chunk < body, ok = false;

		fs_frag[0] = fs_out[0];
		memcpy(&fs_frag[1], (const uint8_t *)(fs_out + 1) + sent, chunk);
		for (int tries = 0; tries < 2000 && !(ok = ring_tx(4, 15, h0 | (more ? 0x80000000u : 0), CCCI_HEADER_LEN + 4 + chunk, slot, fs_frag, 4 + chunk, rel)); tries++) {
			delay_for_interval(1, NSEC_PER_MSEC);
		}
		if (!ok) {
			SAY("r1md: fs REPLY NOT SENT (the TX ring stayed full)");
			return;
		}
		sent += chunk;
	} while (sent < body);
	st2a.answered++;
}

/* AT channel text the modem sends on its own (ch 10 / 12): the length; the text only for lines starting "+E" */
static uint32_t at_logged, oth_ch[256];
static void at_text(const uint8_t *t, uint32_t tl, uint32_t rel);

static void
at_rx(const uint8_t *pb, uint32_t L, uint32_t ch, uint32_t rel)
{
	const uint8_t *t = pb + CCCI_HEADER_LEN;
	uint32_t tl = L > CCCI_HEADER_LEN ? L - CCCI_HEADER_LEN : 0;
	char line[48];
	uint32_t n = 0;

	if (md_level >= 6) {
		if (ch == 10) {
			at_text(t, tl, rel);
		}
		return;
	}

	while (tl && (*t == '\r' || *t == '\n' || *t == ' ')) {
		t++;
		tl--;
	}
	if (at_logged++ >= 60) {
		return;
	}
	if (tl >= 2 && t[0] == '+' && t[1] == 'E') {
		while (n < tl && n < sizeof(line) - 1 && t[n] >= 0x20 && t[n] < 0x7f) {
			line[n] = (char)t[n];
			n++;
		}
		line[n] = 0;
		SAY("r1md: [%u ms] AT unsolicited ch %u len %u: \"%s\"", rel, ch, L - CCCI_HEADER_LEN, line);
	} else {
		SAY("r1md: [%u ms] AT unsolicited ch %u len %u (text not logged)", rel, ch, L - CCCI_HEADER_LEN);
	}
}

/* ---- sub-stage 2d / 2e-1: AT, one command at a time, only from the whitelist (docs/cellular-stage3.md D) ---- */

static const char *const at_white[] = {
	"AT", "ATE0", "AT+CMEE=1", "AT+ESIMS=1", "AT+ESIMS", "AT+ESIMS?", "AT+EFUN=0", "AT+EFUN=1" /* only with radio_go */, "AT+EFUN?", "AT+CFUN?",
	"AT+CPIN?", "AT+CGSN", "AT+CGMR", "AT+CREG=2", "AT+CGREG=2", "AT+CEREG=2", "AT+EREG=3", "AT+EGREG=3", "AT+ECSQ=1", "AT+CREG?", "AT+CGREG?",
	"AT+CEREG?", "AT+COPS?", "AT+CSQ", "AT+CGATT?", "AT+CEMODE?", "AT+ERAT?", "AT+EBOOT=0", "AT+EPOC",
};
/* the scripts: 2d (r1_md=6), 2e-1 (r1_md=7: SIM up, radio off), and the 2e-2 ones (radio_go only) */
static const char *const at_seq6[] = { "AT", "ATE0", "AT+CFUN?", "AT+EFUN?", "AT+CPIN?", "AT+ESIMS?", "AT+CGSN", "AT+CGMR" };
static const char *const at_seq7[] = {
	"AT", "ATE0", "AT+CMEE=1", "AT+ESIMS=1", "AT+ESIMS", "AT+EFUN=0", "AT+CPIN?", "AT+EFUN?", "AT+CFUN?", "AT+CGSN", "AT+CGMR", "AT+ERAT?", "AT+CEMODE?",
};
static const char *const at_seqA[] = { "AT+CREG=2", "AT+CGREG=2", "AT+CEREG=2", "AT+ECSQ=1", "AT+EFUN=1" };
static const char *const at_seqP[] = { "AT+COPS?", "AT+CSQ", "AT+CGATT?", "AT+CEREG?" };
static const char *const at_seqQ[] = { "AT+CEREG?" };
static const char *const at_seqS[] = { "AT+CSQ" };
#define AT_NCMD 8
static const char *const at_names[AT_NCMD] = { "AT", "ATE0", "CFUN", "EFUN", "CPIN", "ESIMS", "CGSN", "CGMR" };
static const char *const *at_seq = at_seq6;
static uint32_t at_n = AT_NCMD;
static char at_buf[1024], at_resp[700], at_res[AT_NCMD][24];
static uint32_t at_len, at_rlen, at_idx, at_sent_ms, at_ready_ms, at_last_ms, at_unsol, at_not_before, cpin_t0;
static bool at_ready, at_wait, at_done, at_radio_on, at_refused, at_stop, at_last_ok;
static int at_radio_val[2] = { -1, -1 };
/* 2e-1: what the SIM said, and the IMEI by its hash only */
static char sim_cpin[16];
static int sim_esims = -1;
static uint32_t sim_iccid_len, imei_hash, imei_digits;
static bool imei_luhn;
/* 2e-2: never set in a run that does not have r1_md=7 AND r1_md_radio=1 AND all ten gates passed */
static bool radio_req, radio_go, rd_active;
static int creg_stat = -1;
static uint32_t rd_t_on;
/* 2e-2 bookkeeping (RESULT 2e-2): EFUN=1 timing, operator/RAT/signal, detach, the post-radio-on channel counts */
static bool efun1_to, at_to, detach_seen, held_ok = true;
static uint32_t efun1_ok_ms, oth_new, oth_new_ch, n_quiet, n34, n38, late_logged;
static int rp_rat = -1, rp_csqv = -1;
static char rp_op[14] = "-", at_lastcmd[24];

/* runs of 8 or more hex digits never reach the log (identifiers): "<N digits>" instead */
static void
at_scrub(const char *in, char *out, uint32_t outsz)
{
	uint32_t o = 0;

	for (uint32_t i = 0; in[i] && o + 14 < outsz;) {
		uint32_t n = 0;
		bool digits = true;

		while ((in[i + n] >= '0' && in[i + n] <= '9') || (in[i + n] >= 'A' && in[i + n] <= 'F') || (in[i + n] >= 'a' && in[i + n] <= 'f')) {
			digits &= in[i + n] <= '9';
			n++;
		}
		if (n >= 8) {
			o += snprintf(out + o, outsz - o, digits ? "<%u digits>" : "<%u hex>", n);
			i += n;
		} else {
			out[o++] = (in[i] == '\n') ? '|' : ((uint8_t)in[i] >= 0x20 && (uint8_t)in[i] < 0x7f ? in[i] : '.');
			i++;
		}
	}
	out[o] = 0;
}

/* a line for the log: the value of an identifier URC only as its length, anything else with long digit runs masked */
static const char *const id_pfx[] = { "+ICCID", "+CIMI", "+CNUM", "+EIMSI", "+CGSN", "+EGMR", "+ESIMAPP", "+ESIMIND", "+ECIMI" };

static void
at_safe(const char *l, char *out, uint32_t outsz)
{
	const char *colon = strchr(l, ':');

	if (colon && colon - l < 12) {
		char pre[16];
		const char *v = colon + 1;

		memcpy(pre, l, (size_t)(colon - l));
		pre[colon - l] = 0;
		if (!strcmp(pre, "+CEREG") || !strcmp(pre, "+CREG") || !strcmp(pre, "+CGREG") || !strcmp(pre, "+EREG") || !strcmp(pre, "+EGREG") ||
			!strcmp(pre, "+ECSQ") || !strcmp(pre, "+COPS") || !strcmp(pre, "+CSQ") || !strcmp(pre, "+CGATT")) {      /* cell/signal/operator: not personal */
			uint32_t o = 0;

			for (; l[o] && o + 1 < outsz; o++) {
				out[o] = ((uint8_t)l[o] >= 0x20 && (uint8_t)l[o] < 0x7f) ? l[o] : '.';
			}
			out[o] = 0;
			return;
		}
		for (unsigned i = 0; i < sizeof(id_pfx) / sizeof(id_pfx[0]); i++) {
			if (!strcmp(pre, id_pfx[i])) {
				while (*v == ' ') {
					v++;
				}
				snprintf(out, outsz, "%s: <%u chars>", pre, (uint32_t)strlen(v));
				return;
			}
		}
	}
	at_scrub(l, out, outsz);
}

static uint32_t
fnv32(const char *s, uint32_t n)
{
	uint32_t h = 2166136261u;

	for (uint32_t i = 0; i < n; i++) {
		h = (h ^ (uint8_t)s[i]) * 16777619u;
	}
	return h;
}

static bool
at_send(const char *cmd, uint32_t rel)
{
	uint32_t body[17], n = (uint32_t)strlen(cmd);
	bool listed = false;

	for (unsigned i = 0; i < sizeof(at_white) / sizeof(at_white[0]); i++) {
		listed |= !strcmp(cmd, at_white[i]);
	}
	/* the one command that turns RF on: only 2e-2, which needs r1_md=7, r1_md_radio=1 and all ten gates (radio_go is set nowhere else) */
	if (!strcmp(cmd, "AT+EFUN=1") && !(md_level == 7 && radio_req && radio_go)) {
		listed = false;
	}
	if (!listed || n > 20) {
		SAY("r1md: at REFUSED \"%s\" (not on the whitelist)", cmd);
		at_refused = true;
		return false;
	}
	memset(body, 0, sizeof(body));
	memcpy(body, cmd, n);
	((char *)body)[n] = '\r';
	if (!ring_tx(5, 12, 0, CCCI_HEADER_LEN + n + 1, 0, body, n + 1, rel)) {
		return false;
	}
	SAY("r1md: [%u ms] AT> %s", rel, cmd);
	strlcpy(at_lastcmd, cmd, sizeof(at_lastcmd));
	at_to = false;
	return true;
}

static bool
at_is_final(const char *l)
{
	return !strcmp(l, "OK") || !strcmp(l, "ERROR") || !strncmp(l, "+CME ERROR", 10) || !strncmp(l, "+CMS ERROR", 10) || !strcmp(l, "NO CARRIER") ||
		   !strcmp(l, "BUSY") || !strcmp(l, "NO ANSWER");
}

static bool
luhn_ok(const char *d, uint32_t n)
{
	uint32_t sum = 0;

	for (uint32_t i = 0; i < n; i++) {
		uint32_t v = (uint32_t)(d[n - 1 - i] - '0');

		if (i & 1) {
			v *= 2;
			if (v > 9) {
				v -= 9;
			}
		}
		sum += v;
	}
	return sum % 10 == 0;
}

/* +CEREG: <n>,<stat>[,...] (a query) or +CEREG: <stat>[,"tac",...] (a URC): the stat */
static int
cereg_stat_of(const char *v)
{
	const char *c;

	while (*v == ' ') {
		v++;
	}
	c = strchr(v, ',');
	if (c && c[1] >= '0' && c[1] <= '9') {
		return (int)strtol(c + 1, NULL, 10);
	}
	return (int)strtol(v, NULL, 10);
}

/* 2e: every line from the modem, in or out of a response: what it says about the SIM and the registration */
static void
at_note(const char *l)
{
	if (!strncmp(l, "+ESIMS:", 7)) {
		sim_esims = (int)strtol(l + 7, NULL, 10);
	} else if (!strncmp(l, "+ICCID:", 7)) {
		const char *v = l + 7;

		while (*v == ' ') {
			v++;
		}
		sim_iccid_len = (uint32_t)strlen(v);
	} else if (!strncmp(l, "+CPIN:", 6)) {
		const char *v = l + 6;

		while (*v == ' ') {
			v++;
		}
		strlcpy(sim_cpin, v, sizeof(sim_cpin));
	} else if (!strncmp(l, "+CEREG:", 7)) {
		creg_stat = cereg_stat_of(l + 7);
	} else if (strnstr(l, "ME DETACH", strlen(l))) {
		detach_seen = true;
	}
}

/* the response to the current command is complete: log it (identifiers only as a digit count and a hash), keep what is needed */
static void
at_finish(const char *final, uint32_t rel)
{
	char show[160], one[120], *line, *next;
	const char *cmd = at_seq[at_idx];
	int val = -1;
	bool is_id = !strcmp(cmd, "AT+CGSN"), is_cpin = !strcmp(cmd, "AT+CPIN?");
	uint32_t digits = 0, sn = 0;
	bool luhn = false;
	char idbuf[20];

	show[0] = 0;
	idbuf[0] = 0;
	at_last_ok = !strcmp(final, "OK");
	at_resp[at_rlen] = 0;
	for (line = at_resp; line && *line; line = next) {
		next = strchr(line, '\n');
		if (next) {
			*next++ = 0;
		}
		if (is_id) {
			const char *v = line;
			uint32_t n = 0;

			if (*v == '+' && strchr(v, ':')) {
				v = strchr(v, ':') + 1;
				while (*v == ' ') {
					v++;
				}
			}
			while (v[n] >= '0' && v[n] <= '9') {
				n++;
			}
			if (n >= 14 && !v[n]) {
				digits = n;
				luhn = luhn_ok(v, n);
				if (n == 15) {
					memcpy(idbuf, v, 15);
				}
			}
		} else {
			if (!strncmp(line, "+CFUN:", 6) || !strncmp(line, "+EFUN:", 6)) {
				val = (int)strtol(line + 6, NULL, 10);
			}
			if (radio_go && !strncmp(line, "+COPS:", 6)) {
				const char *q1 = strchr(line, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;

				if (q2 && q2 - q1 - 1 < (long)sizeof(rp_op)) {
					memcpy(rp_op, q1 + 1, (size_t)(q2 - q1 - 1));
					rp_op[q2 - q1 - 1] = 0;
					rp_rat = q2[1] == ',' ? (int)strtol(q2 + 2, NULL, 10) : -1;
				}
			} else if (radio_go && !strncmp(line, "+CSQ:", 5)) {
				rp_csqv = (int)strtol(line + 5, NULL, 10);
			}
			at_safe(line, one, sizeof(one));
			if (md_level >= 7) {
				SAY("r1md: [%u ms] AT< %s: %s", rel, cmd, one);
			} else {
				sn += (uint32_t)snprintf(show + sn, sizeof(show) - sn, "%s%s", sn ? "|" : "", one);
			}
		}
	}
	if (is_id) {
		if (md_level >= 7) {
			imei_digits = digits;
			imei_luhn = luhn;
			imei_hash = digits == 15 ? fnv32(idbuf, 15) : 0;
			SAY("r1md: [%u ms] AT< %s: %s; returned %u digits, valid Luhn %s, FNV-32 hash %08x", rel, cmd, final, digits, digits ? (luhn ? "yes" : "no") : "n/a",
				imei_hash);
		} else {
			SAY("r1md: [%u ms] AT< %s: %s; returned %u digits, valid Luhn %s", rel, cmd, final, digits, digits ? (luhn ? "yes" : "no") : "n/a");
		}
		if (md_level == 6) {
			snprintf(at_res[at_idx], sizeof(at_res[at_idx]), "%ud/L%s", digits, luhn ? "y" : "n");
		}
	} else {
		if (md_level >= 7) {
			SAY("r1md: [%u ms] AT< %s: %s", rel, cmd, final);
		} else {
			SAY("r1md: [%u ms] AT< %s: %s; text \"%s\"", rel, cmd, final, show);
		}
		if (!strcmp(cmd, "AT+CFUN?") || !strcmp(cmd, "AT+EFUN?")) {
			bool cf = cmd[3] == 'C';

			at_radio_val[cf ? 0 : 1] = val;
			if (md_level == 6) {
				snprintf(at_res[at_idx], sizeof(at_res[at_idx]), "%d", val);
			}
			/* CFUN 1 = full functionality. EFUN: any nonzero value is taken as "on" (conservative: it ends the run) */
			/* in 2e-2 the radio is ours: +EFUN: 1 from the final query is NOT a reason to cut power before the off sequence ran */
			if (((cf && val == 1) || (!cf && val > 0)) && !rd_active) {
				at_radio_on = true;
			}
		} else if (md_level == 6) {
			if (is_cpin) {
				char *p = strnstr(show, "+CPIN: ", sizeof(show));

				snprintf(at_res[at_idx], sizeof(at_res[at_idx]), "%.12s", p ? p + 7 : final);
			} else {
				snprintf(at_res[at_idx], sizeof(at_res[at_idx]), "%.12s", final[0] == 'O' ? "ok" : final);
			}
		}
	}
	if (is_cpin && md_level >= 7) {
		if (!strncmp(final, "+CME ERROR", 10)) {
			const char *c = strchr(final, ':');
			int e = c ? (int)strtol(c + 1, NULL, 10) : -1;

			snprintf(sim_cpin, sizeof(sim_cpin), "CME %d", e);
			if (e == 14 && rel - cpin_t0 < 20000) {     /* busy: the SIM is still starting; ask again in 2 s, up to 20 s in all */
				SAY("r1md: [%u ms] CPIN busy: asking again in 2 s", rel);
				at_wait = false;
				at_last_ms = rel;
				at_not_before = rel + 2000;
				return;
			}
		}
		if (!strcmp(sim_cpin, "SIM PIN") || !strcmp(sim_cpin, "SIM PUK")) {
			SAY("r1md: [%u ms] the SIM wants a PIN/PUK: none is ever sent, stopping", rel);
			at_stop = true;
		}
	}
	if (md_level >= 7 && !strcmp(cmd, "AT+EFUN=1") && at_last_ok && !efun1_ok_ms) {
		efun1_ok_ms = rel - at_sent_ms + 1;     /* +1: 0 means "no OK seen" */
	}
	if (md_level >= 7 && !strcmp(cmd, "AT+ESIMS") && sim_esims == 0) {
		SAY("r1md: [%u ms] +ESIMS: 0, no SIM: stopping", rel);
		at_stop = true;
	}
	at_wait = false;
	at_last_ms = rel;
	at_idx++;
	if (at_radio_on && !rd_active) {
		SAY("r1md: *** RADIO ON BY DEFAULT (CFUN %d, EFUN %d): nothing more is sent, the run ends now ***", at_radio_val[0], at_radio_val[1]);
		at_done = true;
	} else if (at_stop || at_idx >= at_n) {
		at_done = true;
	}
}

/* one complete line from the modem on the AT channel */
static void
at_line(const char *l, uint32_t rel)
{
	if (!strcmp(l, "+EIND: 128") && !at_ready) {
		at_ready = true;
		at_ready_ms = rel;
	}
	if (md_level >= 7) {
		at_note(l);
	}
	if (at_wait) {
		if (!strcmp(l, at_seq[at_idx])) {
			return;         /* the echo */
		}
		if (at_is_final(l)) {
			at_finish(l, rel);
		} else if (at_rlen + strlen(l) + 1 < sizeof(at_resp)) {
			memcpy(at_resp + at_rlen, l, strlen(l));
			at_rlen += (uint32_t)strlen(l);
			at_resp[at_rlen++] = '\n';
		}
		return;
	}
	if (md_level >= 7 && at_is_final(l) && late_logged++ < 20) {
		SAY("r1md: [%u ms] late final for %s: %s (no command outstanding)", rel, at_lastcmd[0] ? at_lastcmd : "(none)", l);
		return;
	}
	if (at_unsol++ < (rd_t_on ? 800u : (md_level >= 7 ? 200u : 80u)) && (!st2a.hs2 || rd_t_on || rel - st2a.t_hs2 <= 60000)) {
		if (l[0] == '+') {
			char pre[20];
			uint32_t n = 0;

			if (md_level >= 7) {
				char one[120];

				at_safe(l, one, sizeof(one));
				SAY("r1md: [%u ms] AT unsolicited %s", rel, one);
				return;
			}
			while (l[n] && l[n] != ':' && n < sizeof(pre) - 1) {
				pre[n] = l[n];
				n++;
			}
			pre[n] = 0;
			if (!strcmp(pre, "+EIND")) {
				SAY("r1md: [%u ms] AT unsolicited +EIND: %d", rel, (int)strtol(l + 6, NULL, 10));
			} else {
				SAY("r1md: [%u ms] AT unsolicited %s (line length %u, text not logged)", rel, pre, (uint32_t)strlen(l));
			}
		} else {
			SAY("r1md: [%u ms] AT unsolicited line, not a + line (length %u, text not logged)", rel, (uint32_t)strlen(l));
		}
	}
}

static void
at_text(const uint8_t *t, uint32_t tl, uint32_t rel)
{
	for (uint32_t i = 0; i < tl; i++) {
		if (at_len + 1 < sizeof(at_buf)) {
			at_buf[at_len++] = (char)t[i];
		}
	}
	for (;;) {
		uint32_t e = 0;

		while (e < at_len && at_buf[e] != '\r' && at_buf[e] != '\n') {
			e++;
		}
		if (e == at_len) {
			if (at_len > sizeof(at_buf) - 8) {
				at_len = 0;     /* a line that never ends: dropped */
			}
			return;
		}
		at_buf[e] = 0;
		if (e) {
			at_line(at_buf, rel);
		}
		e++;
		memmove(at_buf, at_buf + e, at_len - e);
		at_len -= e;
	}
}

/* called every poll: start the next command when the modem said it is ready, time out a silent one */
static void
at_step(uint32_t rel)
{
	const char *cmd;

	if (at_done || !at_ready) {
		return;
	}
	cmd = at_idx < at_n ? at_seq[at_idx] : "";
	if (at_wait) {
		uint32_t to = !strcmp(cmd, "AT+EFUN=0") ? 15000u : (!strcmp(cmd, "AT+EFUN=1") ? 10000u : 5000u);

		if (rel - at_sent_ms >= to) {
			SAY("r1md: [%u ms] AT< %s: TIMEOUT (%u s without a final result)", rel, cmd, to / 1000);
			at_last_ok = false;
			at_to = true;
			efun1_to |= !strcmp(cmd, "AT+EFUN=1");
			if (md_level == 6) {
				snprintf(at_res[at_idx], sizeof(at_res[at_idx]), "timeout");
			}
			at_wait = false;
			at_last_ms = rel;
			at_idx++;
			/* no answer to the SIM power-up: the run ends (the modem is powered down anyway) */
			at_done = at_idx >= at_n || (md_level >= 7 && !strcmp(cmd, "AT+EFUN=0"));
		}
		return;
	}
	if (rel - at_ready_ms < 200 || rel - at_last_ms < 100 || rel < at_not_before || at_idx >= at_n) {
		return;
	}
	at_rlen = 0;
	if (!strcmp(at_seq[at_idx], "AT+CPIN?") && !cpin_t0) {
		cpin_t0 = rel;
	}
	if (at_send(at_seq[at_idx], rel)) {
		at_wait = true;
		at_sent_ms = rel;
		if (!strcmp(at_seq[at_idx], "AT+EFUN=1")) {
			rd_t_on = rel;
		}
	} else {
		if (md_level == 6) {
			snprintf(at_res[at_idx], sizeof(at_res[at_idx]), "notsent");
		}
		at_done = true;
	}
}

static uint32_t oth_logged[256];
static uint32_t rpc_ops[8], rpc_nops;
static bool rpc_bad;     /* an RPC op other than the stock set (0x4005, 0x400e) was asked, or too many different ones */
static uint32_t pktbuf[0x4100 / 4 + 4];

/* after AT+EFUN=1 (rd_t_on set): drain 4/57/59/169/171 silently; 34 and 38 drained with a few header logs; anything else logged and counted */
static uint32_t oth_new_seen[256];

static void
post_radio_rx(uint32_t ch, int q, uint32_t L, uint32_t seq, const uint32_t *pkt, uint32_t rel)
{
	const uint8_t *pb = (const uint8_t *)pkt;

	if (ch == 4 || ch == 57 || ch == 59 || ch == 169 || ch == 171) {
		n_quiet++;
	} else if (ch == 34) {
		if (n34++ < 8) {
			SAY("r1md: [%u ms] ch34 #%u q%d len %u: src %#x dest %#x resv %#x sap %#x msg %#x mlen %u", rel, n34, q, L, L >= 20 ? pkt[4] : 0,
			    L >= 24 ? pkt[5] : 0, pkt[3], L >= 28 ? pkt[6] : 0, L >= 32 ? pkt[7] : 0, L >= 44 ? (uint32_t)(pb[42] | pb[43] << 8) : 0);
		}
	} else if (ch == 38) {
		if (n38++ < 3) {
			SAY("r1md: [%u ms] ch38 #%u q%d len %u: payload %02x %02x %02x %02x %02x %02x %02x %02x", rel, n38, q, L, L >= 17 ? pb[16] : 0, L >= 18 ? pb[17] : 0,
			    L >= 19 ? pb[18] : 0, L >= 20 ? pb[19] : 0, L >= 21 ? pb[20] : 0, L >= 22 ? pb[21] : 0, L >= 23 ? pb[22] : 0, L >= 24 ? pb[23] : 0);
		}
	} else {
		oth_new++;
		if (ch >= 256 || oth_new_seen[ch]++ == 0) {
			oth_new_ch++;
		}
		if (ch >= 256 || oth_new_seen[ch] <= 3) {
			SAY("r1md: [%u ms] NEW ch %u q%d len %u seq %u: %08x %08x %08x %08x | %08x %08x", rel, ch, q, L, seq, pkt[0], pkt[1], pkt[2], pkt[3],
			    L >= 20 ? pkt[4] : 0, L >= 24 ? pkt[5] : 0);
		}
	}
}

/* returns false when the queue is empty or turned out garbage */
static bool
drain_one(int q, uint32_t rel)
{
	vm_offset_t b = nc_smem + SMEM_RING_OFF + rb_off[q], buf = b + 0x20;
	uint32_t len = rbf_rx_size[q], r = *(volatile uint32_t *)(b + RBF_RX_READ), w = *(volatile uint32_t *)(b + RBF_RX_WRITE);
	uint32_t avail, L, padded, *pkt = pktbuf, ncopy, ch, data0, data1, resv, seq;
	const uint8_t *pb = (const uint8_t *)pkt;

	if (r >= len || w >= len || (r & 3) || (w & 3)) {
		SAY("r1md: q%d rx ring garbage at %#x (w %#x)", q, r, w);
		st2a.garbage[q] = true;
		return false;
	}
	avail = (w + len - r) % len;
	if (avail < 32) {
		return false;
	}
	L = rb_word(buf, len, r + 4);
	padded = (L + 16 + 7) & ~7u;
	if (rb_word(buf, len, r) != RING_MAGIC_HEAD || L < CCCI_HEADER_LEN || L > 0x4100 || padded > avail ||
	    rb_word(buf, len, r + 8 + ((L + 7) & ~7u)) != RING_MAGIC_TAIL || rb_word(buf, len, r + 12 + ((L + 7) & ~7u)) != RING_MAGIC_TAIL) {
		SAY("r1md: q%d rx ring garbage at %#x (w %#x): head %#x len %#x foot %#x %#x", q, r, w, rb_word(buf, len, r), L,
		    rb_word(buf, len, r + 8 + ((L + 7) & ~7u)), rb_word(buf, len, r + 12 + ((L + 7) & ~7u)));
		st2a.garbage[q] = true;
		return false;
	}
	ncopy = L < sizeof(pktbuf) ? L : sizeof(pktbuf);
	for (uint32_t i = 0; i < (ncopy + 3) / 4; i++) {
		pkt[i] = rb_word(buf, len, r + 8 + 4 * i);
	}
	*(volatile uint32_t *)(b + RBF_RX_READ) = (r + padded) % len;
	__asm__ volatile ("dsb sy" ::: "memory");
	data0 = pkt[0];
	data1 = pkt[1];
	ch = pkt[2] & 0xffff;
	seq = (pkt[2] >> 16) & 0x7fff;
	resv = pkt[3];
	st2a.pkts++;
	st2a.t_last = rel;
	(void)data1;
	/* log: header always; payload words only for the small kernel channels, op/nargs for FS/RPC, nothing else (lengths only) */
	if (st2a.logged++ < MAX_PKT_LOG) {
		if (ch == 14 || ch == 32) {
			SAY("r1md: [%u ms] rx q%d ch %u seq %u len %u: %08x %08x %08x %08x | op %#x nargs %u", rel, q, ch, seq, L, pkt[0], pkt[1], pkt[2], pkt[3],
			    L >= 20 ? pkt[4] : 0, L >= 24 ? pkt[5] : 0);
		} else if (ch == 0 || ch == 2) {
			SAY("r1md: [%u ms] rx q%d ch %u seq %u len %u: %08x %08x %08x %08x | %08x %08x %08x %08x", rel, q, ch, seq, L, pkt[0], pkt[1], pkt[2],
			    pkt[3], L >= 20 ? pkt[4] : 0, L >= 24 ? pkt[5] : 0, L >= 28 ? pkt[6] : 0, L >= 32 ? pkt[7] : 0);
		} else {
			SAY("r1md: [%u ms] rx q%d ch %u seq %u len %u: %08x %08x %08x %08x (payload not logged)", rel, q, ch, seq, L, pkt[0], pkt[1], pkt[2], pkt[3]);
		}
	}
	if (ch == 0) {
		st2a.ctl++;
		if (data1 == 0 && resv != MD_INIT_CHK_ID) {
			SAY("r1md: [%u ms] HS2 (MD_NORMAL_BOOT) received", rel);
			st2a.hs2 = true;
			st2a.t_hs2 = rel;
			if (md_level >= 6) {
				mdfs_phase(1);
				mdfs_summary();         /* the FS activity up to HS2 (the same counters are said again at the end) */
			}
			} else {
				st2a.ctl_odd++;
				SAY("r1md: control msg %#x resv %#x", data1, resv);
			}
	} else if (ch == 14) {
		uint32_t op = L >= 20 ? pkt[4] : 0, nargs = L >= 24 ? pkt[5] : 0;
		char name[72];

		st2a.fs++;
		if (md_level >= 5) {
			fs_rx(pkt, L, data0, resv, rel);
			return true;
		}
		name[0] = 0;
		if (fs_op_has_name(op) && L >= 0x1c) {
			utf16_name(pb, ncopy, 0x1c, pkt[6], name, sizeof(name));
		}
		if (fs_op_has_name(op)) {
			uint32_t a1 = (L >= 0x1c && ((pkt[6] + 3) & ~3u) + 0x20 <= L) ? pkt[(0x1c + ((pkt[6] + 3) & ~3u)) / 4] : 0;

			SAY("r1md: fs request op %#x nargs %u name \"%s\" arg0 len %u arg1 len %u (data0 %#x slot %u)", op, nargs, name, pkt[6], a1, data0, resv);
		} else {
			SAY("r1md: fs request op %#x nargs %u arg0 len %u (data0 %#x slot %u)", op, nargs, L >= 28 ? pkt[6] : 0, data0, resv);
		}
	} else if (ch == 32) {
		uint32_t op = L >= 20 ? pkt[4] : 0, nargs = L >= 24 ? pkt[5] : 0, n = 0;
		char name[49];

		st2a.rpc++;
		{
			uint32_t k;

			for (k = 0; k < rpc_nops && rpc_ops[k] != op; k++) {
			}
			if (k == rpc_nops) {
				if (rpc_nops < 8) {
					rpc_ops[rpc_nops++] = op;
				} else {
					rpc_bad = true;
				}
			}
			if (op != 0x4005 && op != 0x400e) {
				rpc_bad = true;
			}
		}
		name[0] = 0;
		if ((op == 0x4002 || op == 0x4005) && L >= 0x1c) {
			for (uint32_t i = 0; i < pkt[6] && 0x1c + i < ncopy && n < 48; i++) {
				uint8_t c = pb[0x1c + i];

				if (!c) {
					break;
				}
				name[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
			}
			name[n] = 0;
		}
		SAY("r1md: rpc request op %#x nargs %u arg0 len %u \"%s\" (buffer %u)", op, nargs, L >= 28 ? pkt[6] : 0, name, resv);
		if (md_level >= 4) {
			rpc_serve(pkt, L, ncopy, rel);
		}
	} else if (ch == 2) {
		st2a.sys++;
		st2a.sys_late += st2a.hs2;
		SAY("r1md: sys msg %#x resv %#x", data1, resv);
		if (md_level >= 4) {
			sys_serve(data1, resv, rel);
		}
	} else if (md_level >= 5 && (ch == 10 || ch == 12)) {
		st2a.other++;
		at_rx(pb, L, ch, rel);
	} else {
		st2a.other++;
		if (ch < 256) {
			oth_ch[ch]++;
		}
		if (ch != 4 && ch != 34 && ch != 57 && ch != 59 && ch != 169 && ch != 171) {
			st2a.oth_bad++;     /* gate 7 and the pre-radio-on stop rule only; after EFUN=1 the rules in post_radio_rx apply */
		}
		if (rd_t_on) {
			post_radio_rx(ch, q, L, seq, pkt, rel);
		} else if (md_level >= 6 && ch < 256 && oth_logged[ch]++ < 3) {
			SAY("r1md: [%u ms] other ch %u q%d len %u seq %u: data0 %#x resv %#x op/word0 %#x (payload not logged, not handled by the kernel code)", rel, ch, q, L,
			    seq, data0, resv, L >= 20 ? pkt[4] : 0);
		}
	}
	return true;
}

static void
drain_rings(uint32_t rel)
{
	for (int q = 0; q < RX_QUEUES; q++) {
		for (int guard = 0; guard < 64 && !st2a.garbage[q] && drain_one(q, rel); guard++) {
		}
	}
}

/* a modem exception: the debug regions in smem (MDCCCI_DBG +0x0, MDSS_DBG +0x800), words, then the ASCII runs of the first 12 KB */
static void
ee_dump(bool strings)
{
	volatile uint32_t *p = (volatile uint32_t *)nc_smem;

	if (!strings) {
		for (uint32_t base = 0; base <= 0x800; base += 0x800) {
			for (uint32_t l = 0; l < 4; l++) {
				volatile uint32_t *x = p + (base + l * 32) / 4;

				SAY("r1md: ee dbg +%#x: %08x %08x %08x %08x %08x %08x %08x %08x", base + l * 32, x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]);
			}
		}
		return;
	}
	for (uint32_t i = 0, runs = 0; i < 0x3000 && runs < 24;) {
		char s[61];
		uint32_t n = 0, start = i;

		while (i < 0x3000) {
			uint32_t wv = p[i / 4];
			uint8_t c = (uint8_t)(wv >> (8 * (i & 3)));

			if (c < 0x20 || c >= 0x7f) {
				break;
			}
			if (n < 60) {
				s[n++] = (char)c;
			}
			i++;
		}
		if (n >= 6) {
			s[n] = 0;
			if (!ee_have || (ee_have == 1 && (strnstr(s, ".c", 61) || strnstr(s, ".h", 61)))) {
				strlcpy(ee_text, s, sizeof(ee_text));
				ee_have = (strnstr(s, ".c", 61) || strnstr(s, ".h", 61)) ? 2 : 1;
			}
			SAY("r1md: ee str +%#x: \"%s\"", start, s);
			runs++;
		}
		i++;
	}
}

/* ccci_create_ringbuf() for each queue, laid out back to back */
static bool
ringbufs_init(uint32_t off, uint32_t room, const uint32_t *rx, const uint32_t *tx, const char *what)
{
	uint32_t used = 0;
	vm_offset_t p = nc_smem + off;

	zero_range(off, room);
	for (int q = 0; q < CCIF_QUEUE_NUM; q++) {
		uint32_t len = RBF_CTL_LEN + rx[q] + tx[q];

		if (used + len > room) {
			SAY("r1md: %s ring buffers overflow their %#x bytes", what, room);
			return false;
		}
		*(volatile uint32_t *)(p + 0) = RBF_HEADER;
		*(volatile uint32_t *)(p + 4) = RBF_HEADER;
		*(volatile uint32_t *)(p + RBF_RX_LENGTH) = rx[q];
		*(volatile uint32_t *)(p + RBF_TX_LENGTH) = tx[q];
		*(volatile uint32_t *)(p + len - 8) = RBF_FOOTER;
		*(volatile uint32_t *)(p + len - 4) = RBF_FOOTER;
		p += len;
		used += len;
	}
	SAY("r1md: %s ring buffers: %d queues in %#x of %#x bytes at smem +%#x", what, CCIF_QUEUE_NUM, used, room, off);
	return true;
}

static bool
smem_prepare(void)
{
	if (h_smem_sz < SMEM_CCISM_MCU_EXP_OFF + SMEM_CCISM_MCU_EXP_SIZE) {
		SAY("r1md: nc smem %#x is smaller than the gen93 layout", h_smem_sz);
		return false;
	}
	nc_smem = ml_io_map(h_smem, h_smem_sz);
	SAY("r1md: [%u ms] nc smem %#x+%#x mapped", md_ms(), h_smem, h_smem_sz);
	/* first boot: everything without SMF_NCLR_FIRST */
	zero_range(0, SMEM_CCB_CTRL_OFF);
	zero_range(SMEM_NETD_OFF, SMEM_AUDIO_OFF - SMEM_NETD_OFF);
	if (h_dfd) {
		zero_range(SMEM_DFD_OFF, h_dfd);
	}
	if (h_ccb && h_ccb_sz) {
		/* ml_io_map's submap is 8 MB in all (md1b: kmem_alloc 0x1600000 failed; md1c: ml_io_unmap of an ml_io_map range is "permanent"):
		 * the unmappable kind (redwood_adsp.c's pattern), 1 MB at a time, each unmapped again */
		for (uint32_t off = 0; off < h_ccb_sz; off += 0x100000) {
			uint32_t len = h_ccb_sz - off < 0x100000 ? h_ccb_sz - off : 0x100000;
			volatile uint64_t *c = (volatile uint64_t *)ml_io_map_unmappable(h_ccb + off, (len + 0xfff) & ~0xfffu, VM_WIMG_WCOMB);

			for (uint32_t i = 0; i < len / 8; i++) {
				c[i] = 0;
			}
			ml_io_unmap((vm_offset_t)c, (len + 0xfff) & ~0xfffu);
		}
		SAY("r1md: [%u ms] c smem %#x+%#x cleared", md_ms(), h_ccb, h_ccb_sz);
	} else {
		SAY("r1md: c smem not cleared");
	}
	if (!ringbufs_init(SMEM_CCISM_MCU_OFF, SMEM_CCISM_MCU_SIZE, rbf_rx_size, rbf_tx_size, "normal") ||
	    !ringbufs_init(SMEM_CCISM_MCU_EXP_OFF, SMEM_CCISM_MCU_EXP_SIZE, rbf_exp_rx_size, rbf_exp_tx_size, "exception")) {
		return false;
	}
	if (md_level >= 3) {
		rb_layout();
		SAY("r1md: 2a: queue blocks at +%#x +%#x +%#x +%#x +%#x +%#x +%#x +%#x, end %#x", rb_off[0], rb_off[1], rb_off[2], rb_off[3], rb_off[4],
		    rb_off[5], rb_off[6], rb_off[7], rb_total);
		if (rb_total != RING_USED_EXPECT || !ccb_ctrl_write()) {
			SAY("r1md: 2a: the ring total %#x (want %#x) or the CCB page is wrong: not released", rb_total, RING_USED_EXPECT);
			return false;
		}
		dbm_write();
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	return true;
}

/* clears CCIF busy/channel/irq state, not the SRAM; then the SRAMs and the tail the modem reads */
static void
ccif_reset(void)
{
	wr(infra, INFRA_RST0, CCIF0_RST);
	wr(infra, INFRA_RST1, CCIF0_RST);
	for (uint32_t i = 0; i < CCIF_SRAM_SIZE; i += 4) {
		wr(ccif, APCCIF_CHDATA + i, 0);
		wr(mdccif, APCCIF_CHDATA + i, 0);
	}
	wr(ccif, APCCIF_CHDATA + CCIF_SRAM_SIZE - 12, 0x7274626e);
	wr(ccif, APCCIF_CHDATA + CCIF_SRAM_SIZE - 8, SMEM_MD_VIEW_BASE + SMEM_MDSS_DBG_OFF);
	wr(ccif, APCCIF_CHDATA + CCIF_SRAM_SIZE - 4, SMEM_MDSS_DBG_SIZE);
	SAY("r1md: CCIF reset: RST0 %#x RST1 %#x; AP RCHNUM %#x BUSY %#x, MD RCHNUM %#x; SRAM tail %#x %#x %#x", rd(infra, INFRA_RST0),
	    rd(infra, INFRA_RST1), rd(ccif, APCCIF_RCHNUM), rd(ccif, APCCIF_BUSY), rd(mdccif, APCCIF_RCHNUM),
	    rd(ccif, APCCIF_CHDATA + CCIF_SRAM_SIZE - 12), rd(ccif, APCCIF_CHDATA + CCIF_SRAM_SIZE - 8), rd(ccif, APCCIF_CHDATA + CCIF_SRAM_SIZE - 4));
}

static bool
let_md_go(void)
{
	vm_offset_t vec;

	if (!md_reachable()) {
		return false;
	}
	vec = ml_io_map(MD_BOOT_VECTOR_EN & ~0xfffUL, 0x1000) + (MD_BOOT_VECTOR_EN & 0xfff);
	wr(vec, 0, 1);
	SAY("r1md: [%u ms] MD boot vector enable %#x: modem released", md_ms(), rd(vec, 0));
	return post_access();
}

static uint32_t
boot_sta(uint32_t idx)
{
	/* the stock driver keeps the third of three reads */
	(void)rd(bsta, 4 * idx);
	(void)rd(bsta, 4 * idx);
	return rd(bsta, 4 * idx);
}

static const char *
boot_sta_desc(uint32_t s0)
{
	if (!s0) {
		return "not started";
	}
	if (s0 == 0x54430007 || (s0 >= 0x53310000 && s0 <= 0x533100ff)) {
		return "past the MD boot ROM";
	}
	return "MD boot ROM failure range";
}

/* HS1 as md_ccif_sram_rx_work reads it: the ccci_header and md_query_ap_feature out of the CCIF SRAM. Read only. */
static bool
read_hs1(uint32_t elapsed)
{
	uint32_t w[SRAM_HS1_LEN / 4], data0, data1, chseq, reserved, head, tail;
	uint64_t must = 0;
	const uint8_t *feat = (const uint8_t *)w + CCCI_HEADER_LEN + 4;

	for (uint32_t i = 0; i < SRAM_HS1_LEN / 4; i++) {
		w[i] = rd(ccif, APCCIF_CHDATA + 4 * i);
	}
	data0 = w[0];
	data1 = w[1];
	chseq = w[2];
	reserved = w[3];
	head = w[CCCI_HEADER_LEN / 4];
	tail = w[(CCCI_HEADER_LEN + 4 + MD_FEATURE_COUNT) / 4];
	SAY("r1md: [%u ms] CCIF SRAM first 64 bytes (le words): %08x %08x %08x %08x %08x %08x %08x %08x", elapsed, w[0], w[1], w[2], w[3], w[4],
	    w[5], w[6], w[7]);
	SAY("r1md: [%u ms] CCIF SRAM bytes 32..63: %08x %08x %08x %08x %08x %08x %08x %08x", elapsed, w[8], w[9], w[10], w[11], w[12], w[13],
	    w[14], w[15]);
	SAY("r1md: [%u ms] ccci_header data0 %#x data1 %#x channel %u seq %u reserved %#x", elapsed, data0, data1, chseq & 0xffff,
	    (chseq >> 16) & 0x7fff, reserved);
	if ((chseq & 0xffff) != 0 || data1 != MD_INIT_START_BOOT || reserved != MD_INIT_CHK_ID) {
		SAY("r1md: [%u ms] the CCIF SRAM message is NOT HS1", elapsed);
		return false;
	}
	SAY("r1md: [%u ms] HS1 (MD_INIT_START_BOOT, CHK_ID) received from the modem in the CCIF SRAM", elapsed);
	SAY("r1md: [%u ms] md_query_ap_feature head %#x tail %#x: %s", elapsed, head, tail,
	    (head == MD_FEATURE_QUERY_PATTERN && tail == MD_FEATURE_QUERY_PATTERN) ? "pattern 0x49434343 present (both)" : "PATTERN WRONG");
	for (int i = 0; i < MD_FEATURE_COUNT; i++) {
		if ((feat[i] & 0xf) == 2) {
			must |= 1ULL << i;
		}
	}
	SAY("r1md: [%u ms] feature bytes 0-31: %08x %08x %08x %08x %08x %08x %08x %08x", elapsed, w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12]);
	SAY("r1md: [%u ms] feature bytes 32-63: %08x %08x %08x %08x %08x %08x %08x %08x", elapsed, w[13], w[14], w[15], w[16], w[17], w[18], w[19],
	    w[20]);
	SAY("r1md: [%u ms] features the modem requires (mask 2): %#x%08x", elapsed, (uint32_t)(must >> 32), (uint32_t)must);
	return true;
}

/* ---- sub-stage 2e: the ten gates (docs/cellular-stage3.md A.10), evaluated where the radio-on command would be sent ---- */

static uint32_t gates_pass, g_sta0, g_sta1;
static char gates_bad[100];
static bool gates_done, g_exc, had_wifi, imei_h_set;
static uint32_t g_connsys, imei_h;

static void
gate_out(int n, const char *name, int res, const char *fmt, ...)
{
	char d[128];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(d, sizeof(d), fmt, ap);
	va_end(ap);
	SAY("r1md: gate %d %s: %s %s", n, name, res == 0 ? "pass" : (res == 1 ? "FAIL" : "pending reference"), d);
	if (res == 0) {
		gates_pass++;
	} else {
		size_t l = strlen(gates_bad);

		snprintf(gates_bad + l, sizeof(gates_bad) - l, "%s%d %s%s", l ? ", " : "", n, name, res == 2 ? " (pending)" : "");
	}
}

static void
gates_eval(uint32_t sta0, uint32_t sta1, bool exc)
{
	struct mdfs_gate_info g;
	int soc = -1, chg = 0, temp = 0;
	bool bat = redwood_battery_get(&soc, &chg), tmp = redwood_r1_temp(&temp);
	uint64_t refused = redwood_emmc_nv_refused();
	bool garbage = false;
	char ops[48];
	size_t on = 0;

	gates_done = true;
	gates_pass = 0;
	gates_bad[0] = 0;
	g_sta0 = sta0;
	g_sta1 = sta1;
	g_exc = exc;
	mdfs_gate(&g);
	for (int q = 0; q < RX_QUEUES; q++) {
		garbage |= st2a.garbage[q];
	}
	gate_out(1, "hs2_eind128", (at_ready && st2a.hs2 && !exc && !st2a.ctl_odd && sta0 == 0x54430007 && (sta1 & 0xffff0000u) == 0x53320000u) ? 0 : 1,
		"HS2 %s, +EIND 128 %s, exception %s, other ch0 messages %u, boot status %#x %#x", st2a.hs2 ? "yes" : "NO", at_ready ? "yes" : "NO", exc ? "YES" : "no",
		st2a.ctl_odd, sta0, sta1);
	gate_out(2, "fs_clean", (!g.unimpl && !g.bad_req && !g.cap_ops && !g.cap_hits && !g.errors_other) ? 0 : 1,
		"unimplemented %u, bad requests %u, reads cut %u (largest asked %u), overlay cap hits %u, errors %u (%u outside the known three)", g.unimpl, g.bad_req,
		g.cap_ops, g.max_req, g.cap_hits, g.errors, g.errors_other);
	gate_out(3, "protected_reads", (!g.prot_short && !g.prot_mut) ? 0 : 1,
		"CALIBRAT/IMEI %u reads %lluB, %u short, %u changed; other protected-drive writes %u (files: %s)", g.prot_reads,
		g.prot_bytes, g.prot_short, g.prot_mut, g.oth_mut, g.oth_files[0] ? g.oth_files : "none");
	gate_out(4, "overlay_checksum", (!g.crc_bad && !g.sweep_bad && !refused) ? 0 : 1,
		"%u overlay reads checked, %u nodes swept, bad %u+%u, eMMC NV write refusals %llu", g.crc_checked, g.sweep_nodes, g.crc_bad, g.sweep_bad, refused);
	for (uint32_t k = 0; k < rpc_nops && on + 8 < sizeof(ops); k++) {
		on += snprintf(ops + on, sizeof(ops) - on, "%x ", rpc_ops[k]);
	}
	ops[on] = 0;
	gate_out(5, "rpc", (!st2a.unanswered && !rpc_bad) ? 0 : 1, "unanswered %u, ops seen: %s(stock: 4005 400e)", st2a.unanswered, on ? ops : "none ");
	gate_out(6, "no_sysmsg_after_hs2", st2a.sys_late ? 1 : 0, "system messages after HS2 %u (in all %u)", st2a.sys_late, st2a.sys);
	gate_out(7, "channels", (!st2a.oth_bad && !garbage) ? 0 : 1, "other-channel packets outside {4,34,57,59,169,171}: %u, ring garbage %s", st2a.oth_bad,
		garbage ? "YES" : "no");
	gate_out(8, "sim_ready", (!strcmp(sim_cpin, "READY") && sim_esims >= 1) ? 0 : 1, "CPIN \"%s\", ESIMS %d", sim_cpin[0] ? sim_cpin : "none", sim_esims);
	if (imei_digits != 15 || !imei_luhn) {
		gate_out(9, "imei_hash", 1, "no valid IMEI read (%u digits)", imei_digits);
	} else if (!imei_h_set) {
		gate_out(9, "imei_hash", 2, "hash %08x, no reference given", imei_hash);
	} else {
		gate_out(9, "imei_hash", imei_hash == imei_h ? 0 : 1, "hash %08x %s the Android reference", imei_hash, imei_hash == imei_h ? "equals" : "DIFFERS from");
	}
	gate_out(10, "physical", (bat && (soc >= 30 || chg) && tmp && temp < 50 && !had_wifi && g_connsys < 12) ? 0 : 1,
		"battery %d%% charger %s%s, SoC %d C%s, Wi-Fi %s (wifi arg %u, connsys_step %u)", soc, chg ? "in" : "out", bat ? "" : " (not read)", temp,
		tmp ? "" : " (not read)", (had_wifi || g_connsys >= 12) ? "ON" : "off", had_wifi, g_connsys);
}

/* the one RESULT 2e-1 line: said as soon as the gates are in (the kept log fills up; the verbose summaries come after) */
static bool result_said;

static void
result_2e1(bool exc)
{
	const char *sim = exc ? "MD EXCEPTION" : (!at_ready ? "AT never ready" : (sim_cpin[0] ? sim_cpin : "unknown"));

	if (result_said) {
		return;
	}
	result_said = true;
	SAY("r1md: RESULT 2e-1: SIM %s (ESIMS %d), EFUN %d CFUN %d%s, gates passed %u of 10%s%s", sim, sim_esims, at_radio_val[1], at_radio_val[0],
	    at_radio_on ? " RADIO ON BY DEFAULT" : "", gates_pass, gates_bad[0] ? ", not passed: " : "", gates_bad);
}

/* ---- sub-stage 2e-2: registration only. Reached only with radio_go, which needs r1_md=7, r1_md_radio=1 and all ten gates. ---- */

enum { RP_PRE, RP_WAIT, RP_POST, RP_HOLD, RP_OFF, RP_END };
static int rp = RP_PRE;
static bool rp_new = true, rp_failed;
static uint32_t rp_t, rp_ereg, rp_csq, rp_reg_ms;
static char rp_why[40];

static void
radio_load(const char *const *seq, uint32_t n)
{
	at_seq = seq;
	at_n = n;
	at_idx = 0;
	at_done = false;
	at_stop = false;
}

static void
radio_stop(const char *why)
{
	if (rp < RP_OFF) {
		SAY("r1md: radio: stopping (%s): AT+EFUN=0", why);
		strlcpy(rp_why, why, sizeof(rp_why));
		rp_failed = true;
		rp = RP_OFF;
		rp_new = true;
	}
}

/* the two RESULT 2e-2 lines (each under the ~160-char log cap); said once, as soon as the off sequence ends (or at the end of the run) */
static bool result2_said;
static int off_try = 0, off_ph = 0, efun_final = -2;
static uint32_t off_tm;
static const char *off_res = "none";
static const char *const at_seqE0[] = { "AT+EFUN=0" };
static const char *const at_seqEQ[] = { "AT+EFUN?" };

static void
result_2e2(uint32_t rel)
{
	if (result2_said) {
		return;
	}
	result2_said = true;
	SAY("r1md: RESULT 2e-2: %s; EFUN=1 OK %d ms; reg stat %d at %d ms, held %s; radio on %u ms; hold %s", rp_failed ? rp_why : "completed",
	    efun1_ok_ms ? (int)efun1_ok_ms - 1 : -1, creg_stat, rp_reg_ms ? (int)rp_reg_ms : -1, held_ok ? "yes" : "NO", rd_t_on ? rel - rd_t_on : 0,
	    rp >= RP_HOLD ? "reached" : "not reached");
	SAY("r1md: RESULT 2e-2b: op %s RAT %d csq %d; EFUN=0 tries %d res %s detach %s final EFUN %d; ch34 %u ch38 %u new ch %u/%u pkts, quiet %u", rp_op, rp_rat,
	    rp_csqv, off_try, off_res, detach_seen ? "seen" : "no", efun_final, n34, n38, oth_new_ch, oth_new, n_quiet);
}

/* the off sequence: one command at a time (at_step is the only sender and sends nothing while at_wait), EFUN=0 up to 3 x 15 s 1 s apart,
 * then AT+EFUN? to confirm; the caller powers MD1 down in every case */
static void
off_drive(uint32_t rel)
{
	if (at_wait) {
		return;         /* the command in flight (EFUN=1 included) gets its result, or its timeout, first */
	}
	if (efun1_to) {         /* EFUN=1 never answered: the AT engine is not listening, EFUN=0 would not be heard either */
		SAY("r1md: radio: AT+EFUN=1 timed out: EFUN=0 skipped, MD1 powered down");
		off_res = "skipped";
		rp = RP_END;
		rd_active = false;
		result_2e2(rel);
		return;
	}
	switch (off_ph) {
	case 0:
		radio_load(at_seqE0, 1);        /* replaces any script not yet sent (at_wait is false here); EFUN=1 is never sent after a stop */
		off_try++;
		off_ph = 1;
		break;
	case 1:
		if (!at_done) {
			return;
		}
		off_tm = rel;
		if (at_last_ok) {
			off_res = "OK";
			off_ph = 2;
		} else if (at_to) {
			off_res = "timeout";
			off_ph = 4;
		} else {
			off_res = "error";
			off_ph = off_try < 3 ? 3 : 4;
		}
		break;
	case 2:         /* OK: 2 s of URCs (+CGEV: ME DETACH, +CEREG: 0) */
		if (rel - off_tm >= 2000) {
			off_ph = 4;
		}
		break;
	case 3:         /* error: 1 s, then the next try */
		if (rel - off_tm >= 1000) {
			off_ph = 0;
		}
		break;
	case 4:
		radio_load(at_seqEQ, 1);
		off_ph = 5;
		break;
	case 5:
		if (!at_done) {
			return;
		}
		efun_final = at_radio_val[1];
		SAY("r1md: radio: off sequence done: EFUN=0 %d tries (%s), AT+EFUN? -> %d%s", off_try, off_res, efun_final, efun_final == 0 ? " (confirmed off)" : " (NOT confirmed)");
		rp = RP_END;
		rd_active = false;
		result_2e2(rel);
		break;
	}
}

/* called every poll while rd_active */
static void
radio_drive(uint32_t rel)
{
	if (!radio_go) {
		rd_active = false;
		return;
	}
	if (rp < RP_OFF) {
		if (rd_t_on && rel - rd_t_on >= 240000) {
			radio_stop("hard cap of 4 minutes");
		} else if (efun1_to) {
			radio_stop("EFUN=1 timeout");
		} else if (creg_stat == 3) {
			radio_stop("registration denied");
		} else if (st2a.unanswered || rpc_bad || st2a.sys_late || st2a.ctl_odd || (rd_t_on ? (oth_new_ch > 3 || oth_new > 50) : st2a.oth_bad != 0)) {
			radio_stop("unexpected traffic");
		}
	}
	if (rp == RP_OFF) {
		off_drive(rel);
		return;
	}
	if (rp_new) {
		rp_new = false;
		if (rp == RP_PRE) {
			radio_load(at_seqA, 5);
		} else if (rp == RP_POST) {
			radio_load(at_seqP, 4);
		}
		return;
	}
	if (!at_done || at_wait) {
		return;
	}
	if (rp >= RP_POST && rp < RP_OFF && creg_stat != 1 && creg_stat != 5) {
		held_ok = false;
	}
	switch (rp) {
	case RP_PRE:
		if (!at_last_ok) {
			radio_stop("the radio-on step failed");
		} else {
			rp = RP_WAIT;
			rp_ereg = rp_csq = rel;
		}
		break;
	case RP_WAIT:
		if (creg_stat == 1 || creg_stat == 5) {
			rp_reg_ms = rel - rd_t_on;
			SAY("r1md: radio: registered (CEREG stat %d) %u ms after the radio-on command", creg_stat, rp_reg_ms);
			rp = RP_POST;
			rp_new = true;
		} else if (rel - rd_t_on >= 90000) {
			radio_stop("no registration in 90 s");
		} else if (rel - rp_ereg >= 5000) {
			rp_ereg = rel;
			radio_load(at_seqQ, 1);
		} else if (rel - rp_csq >= 10000) {
			rp_csq = rel;
			radio_load(at_seqS, 1);
		}
		break;
	case RP_POST:
		rp = RP_HOLD;
		rp_t = rel;
		break;
	case RP_HOLD:
		if (rel - rp_t >= 60000) {
			rp = RP_OFF;
			rp_new = true;
			SAY("r1md: radio: hold of 60 s done: off sequence");
		} else if (rel - rp_csq >= 10000) {
			rp_csq = rel;
			radio_load(at_seqS, 1);
		}
		break;
	}
}

/* up to 10 s of the boot status and the CCIF; read only except for the interrupt ack md_ccif_isr does */
static bool
wait_hs1(void)
{
	uint32_t sta0 = ~0u, sta1 = ~0u, trace[15], last_trace[15] = { 0 }, seen = 0, rch, s0, s1, elapsed, next_sec = 0;
	uint32_t ntrace = 0, rxrd = 0, rxwr = 0;
	int bsoc, bchg;
	bool hs1 = false, rx_said = false, sent = false, exc = false, failed = false;
	int after_hs1 = 0;
	uint32_t t_send = 0;
	uint64_t t_start = mach_absolute_time(), ns;

	SAY("r1md: [%u ms] waiting up to 10000 ms for HS1, polling every 10 ms", md_ms());
	for (;;) {
		ns = 0;
		absolutetime_to_nanoseconds(mach_absolute_time() - t_start, &ns);
		elapsed = (uint32_t)(ns / NSEC_PER_MSEC);
		s0 = boot_sta(0);
		s1 = boot_sta(1);
		if (s0 != sta0 || s1 != sta1) {
			SAY("r1md: [%u ms] MD boot status %#x %#x (%s)", elapsed, s0, s1, boot_sta_desc(s0));
			sta0 = s0;
			sta1 = s1;
		}
		for (int i = 0; i < 15; i++) {
			trace[i] = rd(ccif, APCCIF_CHDATA + 4 * i);
		}
		if (memcmp(trace, last_trace, sizeof(trace)) != 0) {
			if (ntrace++ < 24) {
				SAY("r1md: [%u ms] CCIF boot trace %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", elapsed,
				    trace[0], trace[1], trace[2], trace[3], trace[4], trace[5], trace[6], trace[7], trace[8], trace[9], trace[10], trace[11],
				    trace[12], trace[13], trace[14]);
			}
			memcpy(last_trace, trace, sizeof(trace));
		}
		if (elapsed >= next_sec && (md_level < 7 || next_sec < 3000)) {
			SAY("r1md: [%u ms] tick: boot status %#x %#x, AP CCIF RCHNUM %#x BUSY %#x, channels seen %#x", elapsed, s0, s1, rd(ccif, APCCIF_RCHNUM),
			    rd(ccif, APCCIF_BUSY), seen);
			next_sec += 1000;
		}
		rch = rd(ccif, APCCIF_RCHNUM);
		if (rch) {
			if (rch & ~seen) {
				SAY("r1md: [%u ms] CCIF RCHNUM %#x (new %#x)", elapsed, rch, rch & ~seen);
			}
			seen |= rch;
			wr(ccif, APCCIF_ACK, rch & 0xffff);     /* md_ccif_isr(): ack first, the low 16 channels; the SRAM content stays */
			if (rch & (1u << CCIF_RX_SRAM)) {
				bool h = read_hs1(elapsed);

				hs1 |= h;
				if (h && md_level >= 3 && !sent) {
					uint32_t w[SRAM_HS1_LEN / 4];

					for (uint32_t i = 0; i < SRAM_HS1_LEN / 4; i++) {
						w[i] = rd(ccif, APCCIF_CHDATA + 4 * i);
					}
					sent = true;
					t_send = elapsed;
					if (!send_rt_data(w)) {
						failed = true;
						break;
					}
				}
			}
			if (rch & (1u << CCIF_RX_EXCEPTION_INIT)) {
				if (md_level >= 3 && sent) {
					SAY("r1md: [%u ms] D2H_EXCEPTION_INIT after the runtime data", elapsed - t_send);
					ee_dump(false);
					delay_for_interval(300, NSEC_PER_MSEC);
					ee_dump(true);
					exc = true;
				} else {
					SAY("r1md: [%u ms] the modem raised D2H_EXCEPTION_INIT: it hit an exception", elapsed);
				}
				break;
			}
		}
		if (nc_smem && !rx_said) {      /* diagnostic, read only: is there anything in queue 0's AP RX ring (HS1 does not come this way) */
			rxrd = *(volatile uint32_t *)(nc_smem + SMEM_CCISM_MCU_OFF + RBF_RX_READ);
			rxwr = *(volatile uint32_t *)(nc_smem + SMEM_CCISM_MCU_OFF + RBF_RX_WRITE);
			if (rxrd != rxwr) {
				SAY("r1md: [%u ms] q0 rx ring: read %#x write %#x (left unread)", elapsed, rxrd, rxwr);
				rx_said = true;
			}
		}
		if (md_level >= 3) {
			if (sent) {
				drain_rings(elapsed - t_send);
				if (md_level >= 6) {
					at_step(elapsed - t_send);
					if (md_level >= 7) {
						if (at_done && !gates_done && (redwood_battery_get(&bsoc, &bchg) || elapsed - t_send - at_last_ms >= 60000)) {
							gates_eval(sta0, sta1, exc);
							result_2e1(exc);
							if (!radio_req) {
								SAY("r1md: radio-on NOT requested: stopping at 2e-1");
							} else if (gates_pass != 10) {
								SAY("r1md: radio-on REFUSED: gates passed %u of 10 (not passed: %s)", gates_pass, gates_bad);
							} else if (!at_radio_on) {
								radio_go = true;
								rd_active = true;
								SAY("r1md: radio-on requested and all ten gates passed: 2e-2 begins (hard cap 4 min)");
							}
						}
						if (rd_active) {
							radio_drive(elapsed - t_send);
						}
						/* 2e-1: the last response + 3 s, 120 s after HS2. 2e-2: until EFUN=0 is confirmed; the cap below is the kernel's own
						 * timer: 15 s past the 4 minutes, MD1 goes down whatever the state machine is doing */
						if (at_radio_on || (rd_active && rd_t_on && elapsed - t_send - rd_t_on >= 255000) || (rd_active && md_ms() >= 400000) || (radio_go && rp == RP_END) ||
							(!rd_active && ((st2a.hs2 && (elapsed - t_send - st2a.t_hs2 >= 120000 || (at_done && gates_done && elapsed - t_send - at_last_ms >= 3000))) ||
							(!st2a.hs2 && elapsed - t_send >= 30000) || md_ms() >= 150000))) {
							break;
						}
					} else if (at_radio_on || (st2a.hs2 && (elapsed - t_send - st2a.t_hs2 >= 90000 || (at_done && elapsed - t_send - at_last_ms >= 3000))) ||
						(!st2a.hs2 && elapsed - t_send >= 30000) || md_ms() >= 150000) {
						break;
					}
				} else if (st2a.hs2 && elapsed - t_send - st2a.t_hs2 >= (md_level >= 5 ? 3000u : 2000u)) {
					break;
				}
				if (md_level >= 6) {
					/* (above) */
				} else if (md_level >= 5) {
					/* 20 s after the last FS request (the reply, if there was none), 60 s in all */
					if (elapsed - t_send - fs_last_ms >= 20000 || md_ms() >= 60000) {
						break;
					}
				} else if (md_level >= 4) {
					if (elapsed - t_send - st2a.t_last >= 10000 || elapsed - t_send >= 30000) {
						break;
					}
				} else if (elapsed - t_send >= 10000) {
					break;
				}
			} else if (elapsed >= 10000) {
				break;
			}
		} else {
			if (hs1 && ++after_hs1 * 10 >= 500) {
				break;
			}
			if (elapsed >= 10000) {
				break;
			}
		}
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	if (md_level >= 5) {
		uint32_t rel = sent ? elapsed - t_send : elapsed, fops, fmiss, fovl;
		char atsum[150];
		size_t an = 0;
		const char *rt = md_level >= 6 ? "fs state (not the result)" : "RESULT 2c";

		mdfs_counts(&fops, &fmiss, &fovl);
		mdfs_summary();
		SAY("r1md: 2c: rx packets %u (fs %u rpc %u ctl %u sys %u other %u), answers %u, unanswered %u, fs dropped %u, boot status %#x %#x", st2a.pkts,
		    st2a.fs, st2a.rpc, st2a.ctl, st2a.sys, st2a.other, st2a.answered, st2a.unanswered, fs_bad, sta0, sta1);
		{
			char cl[120];
			size_t cn = 0;

for (int c = 0; c < 256 && cn < 100; c++) {
				if (oth_ch[c]) {
					cn += snprintf(cl + cn, sizeof(cl) - cn, "ch%d=%u ", c, oth_ch[c]);
				}
			}
			cl[cn] = 0;
			SAY("r1md: 2c: other channels (not fs/rpc/ctl/sys/AT): %s", cn ? cl : "none");
		}
		if (exc) {
			SAY("r1md: last fs exchange before the exception: %s", mdfs_last());
			SAY("r1md: %s: MD EXCEPTION %u ms after the reply; last answer op %#x; text \"%s\"; fs %u ops %u missing; jnl %s", rt, rel, last_ans_op,
			    ee_have ? ee_text : "none found", fops, fmiss, mdfs_jnl());
		} else if (st2a.hs2) {
			SAY("r1md: %s: HS2 at %u ms after the reply; fs %u ops %u missing, overlay %u B; jnl %s", rt, st2a.t_hs2, fops, fmiss, fovl, mdfs_jnl());
		} else {
			SAY("r1md: %s: NO HS2 in %u ms%s; fs %u ops %u missing, overlay %u B; jnl %s", rt, rel,
			    failed ? " (the reply was not sent)" : (hs1 ? "" : " (no HS1 either)"), fops, fmiss, fovl, mdfs_jnl());
			SAY("r1md: last fs exchange: %s", mdfs_last());
		}
		if (md_level >= 7) {
			if (!gates_done) {
				gates_eval(sta0, sta1, exc);
			}
			result_2e1(exc);
			if (radio_go) {
				result_2e2(elapsed - t_send);
			}

		} else if (md_level >= 6) {
			for (int c = 0; c < AT_NCMD && an < 120; c++) {
				if (at_res[c][0]) {
					an += snprintf(atsum + an, sizeof(atsum) - an, "%s=%s ", at_names[c], at_res[c]);
				}
			}
			atsum[an] = 0;
			if (at_ready) {
				SAY("r1md: RESULT 2d: AT ready at %u ms after HS2; %s%s", at_ready_ms - st2a.t_hs2, an ? atsum : "no command sent",
				    at_radio_on ? "RADIO ON, STOPPED" : (at_refused ? "REFUSED" : ""));
			} else if (exc) {
				SAY("r1md: RESULT 2d: MD EXCEPTION, AT never ready; text \"%s\"", ee_have ? ee_text : "none found");
			} else {
				SAY("r1md: RESULT 2d: AT never ready (%s); no AT command sent", st2a.hs2 ? "HS2 seen" : "NO HS2");
			}
		}
	} else if (md_level >= 3) {
		uint32_t rel = sent ? elapsed - t_send : elapsed;

		const char *tag = md_level >= 4 ? "2b" : "2a";

		if (exc) {
			SAY("r1md: RESULT %s: MD EXCEPTION %u ms after the reply; last answer op %#x at %u ms; text \"%s\"", tag, rel, last_ans_op, last_ans_ms,
			    ee_have ? ee_text : "none found");
		} else if (st2a.hs2) {
			SAY("r1md: RESULT %s: HS2 at %u ms after the reply; rx packets %u (fs %u rpc %u ctl %u sys %u other %u), answers %u, unanswered %u", tag,
			    st2a.t_hs2, st2a.pkts, st2a.fs, st2a.rpc, st2a.ctl, st2a.sys, st2a.other, st2a.answered, st2a.unanswered);
		} else {
			SAY("r1md: RESULT %s: NO HS2 in %u ms%s; rx packets %u (fs %u rpc %u ctl %u sys %u other %u), answers %u, unanswered %u, boot status %#x %#x", tag, rel,
			    failed ? " (the reply was not sent)" : (hs1 ? "" : " (no HS1 either)"), st2a.pkts, st2a.fs, st2a.rpc, st2a.ctl, st2a.sys,
			    st2a.other, st2a.answered, st2a.unanswered, sta0, sta1);
		}
	} else if (hs1) {
		SAY("r1md: RESULT: HS1 SEEN; boot status %#x %#x, CCIF channels seen %#x, %u ms", sta0, sta1, seen, elapsed);
	} else {
		SAY("r1md: RESULT: NO HS1 after %u ms; boot status %#x %#x (%s), CCIF channels seen %#x", elapsed, sta0, sta1, boot_sta_desc(sta0), seen);
	}
	return hs1;
}

/*
 * The reverse, as the reference does it (r1md_power_off, then scpsys_power_off): CCIF reset, the MD1 domain off with the bus
 * protection set in bp_cfg order (PERI2MD, masters, MD2PERI, each waiting for its ack), the legacy sequence reversed, SRCCLKENA
 * low byte 0, TOPCKGEN MD clock gate, VMODEM back as it was. The CCIF/CLDMA clocks are left ungated (as LK left them). A protection
 * ack that does not come stops it there (MD1 stays on): the reference does the same. Only MMIO and busy delays: usable on the way back
 * to the loader too.
 */
static void
md_power_off(const char *why)
{
	uint32_t v = 0;

	if (md_state != 1) {
		return;
	}
	md_state = 2;
	SAY("r1md: powering MD1 down (%s)", why);
	if (!md1_is_on()) {
		SAY("r1md: MD1 is already off");
	} else {
		ccif_reset();
		wr(infra, INFRA_PROTECTEN_SET, PERI2MD_PROT);
		if (!poll_infra(INFRA_PROTECTEN_STA1, PERI2MD_PROT, PERI2MD_PROT, "off: PERI2MD protect")) {
			goto stuck;
		}
		wr(infra, INFRA_PROTECTEN_SET, MDMASTER_PROT);
		if (!poll_infra(INFRA_PROTECTEN_STA1, MDMASTER_PROT, MDMASTER_PROT, "off: MD masters protect")) {
			goto stuck;
		}
		wr(infra, INFRA_PROTECTEN_1_SET, MD2PERI_PROT);
		if (!poll_infra(INFRA_PROTECTEN_STA1_1, MD2PERI_PROT, MD2PERI_PROT, "off: MD2PERI protect")) {
			goto stuck;
		}
		setb(spm, SPM_MD_EXTRA_PWR_CON, 1);
		setb(spm, SPM_MD1_PWR_CON, PWR_CLK_DIS);
		setb(spm, SPM_MD1_PWR_CON, PWR_ISO);
		clrb(spm, SPM_MD_SRAM_ISO_CON, 1);
		setb(spm, SPM_MD1_PWR_CON, SRAM_PDN);
		clrb(spm, SPM_MD1_PWR_CON, PWR_ON);
		clrb(spm, SPM_MD1_PWR_CON, PWR_ON_2ND);
		for (int i = 0; i < 100000 && md1_is_on(); i++) {
			delay(10);
		}
		if (md1_is_on()) {
			SAY("r1md: MD1 still powered after the power-off sequence (PWR_STATUS %#x/%#x)", rd(spm, SPM_PWR_STATUS), rd(spm, SPM_PWR_STATUS_2ND));
			goto stuck;
		}
		clrb(spm, SPM_MD1_PWR_CON, PWR_RST_B);
	}
	/* AP2MD_DUMMY as found (the reference leaves it set by its post-access) */
	wr(infra, INFRA_AP2MD_DUMMY, (rd(infra, INFRA_AP2MD_DUMMY) & ~1u) | (found_ap2md & 1u));
	wr(infra, INFRA_MD_SRCCLKENA, (rd(infra, INFRA_MD_SRCCLKENA) & ~0xffu) | 0);
	wr(top, 0, rd(top, 0) | 0x300);
	if (found_vm_valid) {
		if ((found_vm_elr0 & 0x7f) != VMODEM_SEL_800MV) {
			(void)pw_write(VMODEM_ELR0, found_vm_elr0);
		}
		if (!(found_vm_con0 & 1)) {
			(void)pw_write(VMODEM_CON0, found_vm_con0);
		}
		(void)pw_read(VMODEM_CON0, &v);
		SAY("r1md: VMODEM put back: CON0 %#x (found %#x) ELR0 found %#x", v, found_vm_con0, found_vm_elr0);
	}
	dump_status("powered down");
	SAY("r1md: found before: CLK_MODE %#x SRCCLKENA %#x AP2MD %#x", found_clk_mode, found_srcclkena, found_ap2md);
	return;
stuck:
	dump_status("power-off STOPPED");
}

static void
md_thread(__unused void *parameter, __unused wait_result_t result)
{
	md_t0 = mach_absolute_time();
	md_state = 1;
	SAY("r1md: stage 1 begins: handoff smem %#x+%#x ccb %#x+%#x image %#x+%#x dfd %#x", h_smem, h_smem_sz, h_ccb, h_ccb_sz, h_img, h_img_sz,
	    h_dfd);
	found_clk_mode = rd(top, 0);
	found_srcclkena = rd(infra, INFRA_MD_SRCCLKENA);
	found_ap2md = rd(infra, INFRA_AP2MD_DUMMY);
	dump_status("boot state");
	if (md_level >= 5) {
		fs_out = kalloc_data(0x4200, Z_WAITOK | Z_ZERO);
		if (fs_out == NULL || !mdfs_init()) {
			SAY("r1md: RESULT %s: NO MODEM START: the NV partitions could not be mounted read only (nothing was powered)", md_level >= 6 ? "2d" : "2c");
			md_state = 2;
			goto out;
		}
	}
	if (md1_is_on()) {
		SAY("r1md: MD1 is already powered; something else owns the modem: not touching it");
		md_state = 2;
		goto out;
	}
	/* step 1 of md_cd_power_on(): MD clock mode, before the power domain */
	clrb(top, 0, 0x300);
	SAY("r1md: [%u ms] TOPCKGEN CLK_MODE %#x", md_ms(), rd(top, 0));
	/* step 2: VMODEM */
	if (!vmodem_on()) {
		md_power_off("VMODEM");
		goto out;
	}
	/* step 3: the MD1 domain */
	if (!md1_domain_on()) {
		md_power_off("MD1 domain did not power");
		goto out;
	}
	dump_status("MD1 on");
	if (!md_reachable() || !pre_access()) {
		md_power_off("pre-access");
		goto out;
	}
	/* step 4: srcclkena */
	wr(infra, INFRA_MD_SRCCLKENA, (rd(infra, INFRA_MD_SRCCLKENA) & ~0xffu) | 0x21);
	SAY("r1md: srcclkena %#x", rd(infra, INFRA_MD_SRCCLKENA));
	/* step 5, 6: the PLLs, the watchdog */
	if (!pll_init()) {
		md_power_off("PLL init");
		goto out;
	}
	md_wdt_off();
	SAY("r1md: stage 1 done: MD1 powered, PLLs up, MD WDT off, modem parked");
	/* stage 2 of the reference: clocks, shared memory, CCIF, release, HS1 */
	clocks_on();
	if (!smem_prepare()) {
		md_power_off("shared memory");
		goto out;
	}
	ccif_reset();
	if (!let_md_go()) {
		md_power_off("release");
		goto out;
	}
	(void)wait_hs1();
	dump_status("after wait");
	md_power_off("end of the HS1 watch");
	mdfs_done();
out:
	SAY("r1md: stage 1 done");
	thread_terminate_self();
}

/* on the way back to the loader: if the thread did not finish, the modem is powered down here (MMIO only) */
void
redwood_md_quiesce(void)
{
	if (md_state == 1 && spm) {
		md_quiet = true;
		md_power_off("on the way back to the loader");
	}
}

void
redwood_md_init(void)
{
	uint32_t arg = 0, pwr, pwr2, cg1, cg2, cg3;
	vm_offset_t csta;
	bool on;
	thread_t thread;

	if (!PE_parse_boot_argn("r1_md", &arg, sizeof(arg)) || (arg < 1 || arg > 7)) {
		return;
	}
	md_level = arg;
	if (arg == 7) {
		uint32_t v = 0;

		at_seq = at_seq7;
		at_n = sizeof(at_seq7) / sizeof(at_seq7[0]);
		radio_req = PE_parse_boot_argn("r1_md_radio", &v, sizeof(v)) && v == 1;
		imei_h_set = PE_parse_boot_argn("r1_md_imei_h", &imei_h, sizeof(imei_h));
		had_wifi = PE_parse_boot_argn("wifi", &v, sizeof(v)) && v != 0;
		(void)PE_parse_boot_argn("connsys_step", &g_connsys, sizeof(g_connsys));
	}
	spm = ml_io_map(0x10006000UL, 0x1000);
	infra = ml_io_map(0x10001000UL, 0x1000);
	top = ml_io_map(0x10000000UL, 0x1000);
	pwr = rd(spm, SPM_PWR_STATUS);
	pwr2 = rd(spm, SPM_PWR_STATUS_2ND);
	on = (pwr & 1) && (pwr2 & 1);
	SAY("r1md: SPM PWR_STATUS %#x/%#x (MD1 %s) MD1_PWR_CON %#x ISO %#x EXTRA %#x", pwr, pwr2, on ? "on" : "off", rd(spm, SPM_MD1_PWR_CON),
	    rd(spm, SPM_MD_SRAM_ISO_CON), rd(spm, SPM_MD_EXTRA_PWR_CON));
	SAY("r1md: INFRA PROTECTEN %#x STA1 %#x PROTECTEN_1 %#x STA1_1 %#x AP2MD %#x SRCCLKENA %#x RST0 %#x RST1 %#x", rd(infra, INFRA_PROTECTEN),
	    rd(infra, INFRA_PROTECTEN_STA1), rd(infra, INFRA_PROTECTEN_1), rd(infra, INFRA_PROTECTEN_STA1_1), rd(infra, INFRA_AP2MD_DUMMY),
	    rd(infra, INFRA_MD_SRCCLKENA), rd(infra, INFRA_RST0), rd(infra, INFRA_RST1));
	cg1 = rd(infra, 0x94);
	cg2 = rd(infra, 0xac);
	cg3 = rd(infra, 0xc8);
	SAY("r1md: CG ccif_ap %s ccif_md %s cldma %s; TOPCKGEN CLK_MODE %#x (CG1 %#x CG2 %#x CG3 %#x)", md_cg(cg1, 23), md_cg(cg1, 26),
	    md_cg(cg2, 3), rd(top, 0), cg1, cg2, cg3);
	if (!(cg1 & (1u << 23))) {
		ccif = ml_io_map(0x10209000UL, 0x1000);
		SAY("r1md: CCIF CON %#x BUSY %#x START %#x RCHNUM %#x", rd(ccif, 0x00), rd(ccif, 0x04), rd(ccif, 0x08), rd(ccif, 0x10));
	} else {
		SAY("r1md: CCIF skipped (gated)");
	}
	if (on) {
		uint32_t w0 = 0, w1 = 0;

		csta = ml_io_map(0x1020e000UL, 0x1000);
		for (int i = 0; i < 3; i++) {   /* the third of three reads */
			w0 = rd(csta, 0x300);
			w1 = rd(csta, 0x304);
		}
		SAY("r1md: boot_sta %#x %#x", w0, w1);
	} else {
		SAY("r1md: boot_sta skipped (MD1 off)");
	}
	SAY("r1md: stage 0 done");
	if (arg < 2) {
		return;
	}
	/* stage 1: the loader's handoff must be there, and the CCIF clocks as found ungated */
	if (!PE_parse_boot_argn("r1_md_smem", &h_smem, sizeof(h_smem)) || !PE_parse_boot_argn("r1_md_smem_sz", &h_smem_sz, sizeof(h_smem_sz)) ||
	    !PE_parse_boot_argn("r1_md_ccb", &h_ccb, sizeof(h_ccb)) || !PE_parse_boot_argn("r1_md_ccb_sz", &h_ccb_sz, sizeof(h_ccb_sz)) ||
	    !PE_parse_boot_argn("r1_md_img", &h_img, sizeof(h_img)) || !PE_parse_boot_argn("r1_md_img_sz", &h_img_sz, sizeof(h_img_sz)) ||
	    !h_smem || !h_smem_sz || !h_img || !h_img_sz) {
		SAY("r1md: stage 1 REFUSED: the loader's handoff (r1_md_smem= ... r1_md_img_sz=) is not in the boot-args");
		return;
	}
	(void)PE_parse_boot_argn("r1_md_dfd", &h_dfd, sizeof(h_dfd));
	if ((cg1 & ((1u << 23) | (1u << 26))) || !ccif) {
		SAY("r1md: stage 1 REFUSED: the CCIF clocks are gated (CG1 %#x), the reference expects them ungated", cg1);
		return;
	}
	mdccif = ml_io_map(0x1020a000UL, 0x1000);
	bsta = ml_io_map(0x1020e000UL, 0x1000) + 0x300;
	apmix = ml_io_map(0x1000c000UL, 0x1000);
	pwrap = ml_io_map(PWRAP_BASE, 0x1000);
	if (kernel_thread_start(md_thread, NULL, &thread) != KERN_SUCCESS) {
		SAY("r1md: stage 1: the thread did not start");
		return;
	}
	thread_deallocate(thread);
}
