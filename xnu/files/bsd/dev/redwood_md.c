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
/* Log budget (docs/cellular-attach-2.md 5c): SAY = bulk, dropped (and counted in md_cut) once ~20 KB of r1md output went out (26 KB after radio-on);
 * SAYM = must-lines (results, gate summary, radio on/off, registration URCs, CCIF bits >= 16, errors, exceptions): never dropped. Lines are <= 191 chars. */
static uint32_t md_bytes, md_cut, md_sup, md_budget = 20000;   /* md_sup: lines deduplicated on purpose */

void redwood_md_say(bool must, const char *format, ...) __printflike(2, 3);
void
redwood_md_say(bool must, const char *format, ...)
{
	char b[192];
	va_list ap;
	size_t l;

	if (md_quiet) {
		return;
	}
	va_start(ap, format);
	vsnprintf(b, sizeof(b), format, ap);
	va_end(ap);
	l = strlen(b) + 1;
	if (!must && md_bytes > md_budget) {
		md_cut++;
		return;
	}
	md_bytes += (uint32_t)l;
	redwood_host_keep_add("%s", b);
}
#define SAY(...)  redwood_md_say(false, __VA_ARGS__)
#define SAYM(...) redwood_md_say(true, __VA_ARGS__)
/* redwood_cldma.c */
extern void cldma_maps(vm_offset_t infra);
extern void cldma_snap(const char *why, uint32_t ms, bool md_on);
extern bool cldma_rx_start(uint32_t ms);
extern bool cldma_rx_poll(uint32_t ms);
extern void cldma_teardown(void);
extern void cldma_report(const char *tag, uint32_t ms);
extern uint32_t cldma_rx_count(void);
extern uint32_t cldma_rx_err(void);
/* Stage 3a (r1_md_data=1 only) */
extern void cldma_data_enable(void);
extern bool cldma_tx(const uint8_t *ip, uint32_t len, uint32_t ms);
extern uint32_t cldma_tx_pkts(void);
extern uint32_t cldma_tx_bytes(void);
extern uint32_t cldma_tx_completed(void);
/* Stage 3b (r1_md_if=1 only): redwood_cell.c, the pdp_ip0 interface */
extern void cldma_if_enable(uint32_t pkts_cap, uint32_t bytes_cap);
extern bool cell_if_attach(const uint8_t ip[4], const uint8_t peer[4], const uint8_t dns[4], uint32_t mtu, uint32_t pkts, uint32_t kb, char *why, size_t whylen);
extern void cell_if_input(const uint8_t *ip, uint32_t len);
extern bool cell_if_tick(uint32_t rel);
extern bool cell_if_active(void);
extern bool cell_if_done(void);
extern bool cell_if_exists(void);
extern void cell_if_stats(uint32_t *txp, uint32_t *txb, uint32_t *rxp, uint32_t *rxb, uint32_t *drops, bool *caphit);
extern void cell_if_stop(const char *why);

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
static uint32_t rd_t_on;        /* ms (run-relative) at which AT+EFUN=1 was sent; 0 = never */
static bool data_on, eif_on;    /* Stage 3a, see the comment at cldma_on */
/* Stage 3b: if_on = r1_md_if=1 (or the r1_md=9 shorthand) together with data_on. if_pkts / if_kb: per-direction traffic caps (boot-args r1_md_if_pkts, r1_md_if_kb) */
static bool if_on, if_ok;       /* if_ok: pdp_ip0 was attached and configured */
static uint32_t if_pkts = 300, if_kb = 150, if_hold_ms = 20000, if_t0;
static char if_why[48] = "";

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
	SAYM("r1md: [%u ms] %s: infra %#x = %#x, wanted %#x under %#x: TIMED OUT", md_ms(), what, reg, v, want, mask);
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
		SAYM("r1md: VMODEM: the PMIC wrapper did not answer");
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
	SAYM("r1md: [%u ms] MD1 power ack: %s, PWR_STATUS %#x/%#x MD1_PWR_CON %#x", md_ms(), on ? "on" : "NOT ON", rd(spm, SPM_PWR_STATUS),
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
		SAYM("r1md: MD1 is not powered, not touching MD registers");
		return false;
	}
	if (sta1 & PERI2MD_PROT) {
		SAYM("r1md: PERI2MD protection is active (STA1 %#x), not touching MD registers", sta1);
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
#define MAX_PKT_LOG             15

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
	SAYM("r1md: ccb ctrl: %d rows, dhl %#x mdm %#x meta %#x (the META row is a GUESS: haretic's 63K ul table)", CCB_ROWS, dhl, mdm, meta);
	if (dhl != 0x70000 || mdm != 0x8000 || meta != 0x100000 || dhl + mdm + meta != 0x178000) {
		SAYM("r1md: CCB SUM WRONG (%#x, want 0x178000)", dhl + mdm + meta);
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
			SAYM("r1md: 2a: MD requires feature %d we lack", i);
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
			SAYM("r1md: 2a: CCIF still busy (%#x): the reply is NOT sent", busy);
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

static uint32_t tx_seq[128], tx_logged;
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
		SAYM("r1md: TX q%d ring pointers odd (r %#x w %#x): not sent", q, r, w);
		return false;
	}
	avail = (r == w) ? len - 1 : (r < w ? len - w - 1 + r : r - w - 1);
	if (padded >= avail) {
		SAYM("r1md: TX q%d full (need %#x, free %#x): not sent", q, padded, avail);
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
	if (tx_logged++ < 5) {
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
static bool rpc_quiet;
#define RSAY(...) do { if (!rpc_quiet) SAY(__VA_ARGS__); } while (0)

static void
rpc_serve(const uint32_t *pkt, uint32_t L, uint32_t ncopy, uint32_t rel)
{
	const uint8_t *pb = (const uint8_t *)pkt;
	uint32_t op = pkt[4], nargs = pkt[5], aoff[6], alen[6], off = 0x18;
	char name[49];
	int32_t st = 0;
	bool send = true;

	if (L < 24 || ncopy < L || nargs > 6) {
		SAYM("r1md: RPC REQUEST NOT PARSED: op %#x nargs %u len %u (copied %u): NOT ANSWERED", op, nargs, L, ncopy);
		st2a.unanswered++;
		return;
	}
	for (uint32_t i = 0; i < nargs; i++) {
		if (off + 4 > L || off + 4 + ((pkt[off / 4] + 3) & ~3u) > L) {
			SAYM("r1md: RPC REQUEST BAD ARGS: op %#x arg %u: NOT ANSWERED", op, i);
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
			SAYM("r1md: RPC 0x4002 UNKNOWN GPIO NAME \"%s\" -> -5", name);
			st = -5;
			ob_word((uint32_t)-5);
			ob_word((uint32_t)-5);
		} else {
			ob_word(gpio_names[found].pin);
			ob_word(gpio_names[found].pin);
		}
		RSAY("r1md: rpc op 0x4002 name \"%s\" -> pin %d", name, found < 0 ? -5 : (int)gpio_names[found].pin);
		break;
	}
	case 0x4005: {  /* GET_EINT_ATTR (name, ..., u32 type) */
		uint32_t type = nargs ? *(const uint32_t *)(pb + aoff[nargs - 1]) : 0xffff, val = 0;
		int sim = strnstr(name, "SIM1", 48) ? 0 : (strnstr(name, "SIM2", 48) ? 1 : -1);

		if (sim < 0 || !strnstr(name, "HOT_PLUG_EINT", 48) || nargs < 2) {
			SAYM("r1md: RPC 0x4005 UNKNOWN EINT NAME \"%s\" (nargs %u type %u) -> -11", name, nargs, type);
			ob_word((uint32_t)-11);
			ob_word((uint32_t)-11);
			st = -11;
		} else if (type == 2 || type == 3 || type > 6) {
			RSAY("r1md: rpc op 0x4005 \"%s\" type %u (POLA/SENS/unknown: not in the DT) -> -12", name, type);
			ob_word((uint32_t)-12);
			ob_word((uint32_t)-12);
			st = -12;
		} else {
			val = type == 6 ? (uint32_t)(sim + 1) : (type == 0 ? (uint32_t)sim : (type == 1 ? 10000u : 0u));
			ob_word(0);
			ob_word(val);
			RSAY("r1md: rpc op 0x4005 \"%s\" type %u -> 0, %u", name, type, val);
		}
		break;
	}
	case 0x400e:    /* DTSI_QUERY: both md_attr_node properties are 0x0f0f0f0f */
		ob_word(0);
		ob_fill(68, 0x0f);
		RSAY("r1md: rpc op 0x400e DTSI_QUERY (arg0 len %u) -> 0, 68 x 0x0f", nargs ? alen[0] : 0);
		break;
	case 0x4006:
	case 0x4007:    /* GPIO_VAL / ADC_VAL: no level known (GUESS 0); ADC -1 */
		ob_word(op == 0x4007 ? (uint32_t)-1 : 0);
		ob_word(op == 0x4007 ? (uint32_t)-1 : 0);
		SAYM("r1md: RPC op %#x \"%s\" answered with a GUESS (%d)", op, name, op == 0x4007 ? -1 : 0);
		break;
	case 0x4003:
	case 0x4001:
		ob_word((uint32_t)-5);
		ob_word((uint32_t)-5);
		RSAY("r1md: rpc op %#x -> -5, -5", op);
		break;
	case 0x4004:
		ob_word((uint32_t)-5);
		ob_word((uint32_t)-5);
		ob_word((uint32_t)-5);
		RSAY("r1md: rpc op 0x4004 -> -5 x3");
		break;
	case 0x4009:    /* GET_GPIO_ADC: {4, 0} + 32 (161-byte input) or 88 (425-byte input) bytes of 0x0f; no pins found (log) */
		ob_word(0);
		ob_fill(nargs && alen[0] >= 400 ? 88 : 32, 0x0f);
		RSAY("r1md: rpc op 0x4009 GET_GPIO_ADC (arg0 len %u) -> 0, %u x 0x0f (no found pins)", nargs ? alen[0] : 0, nargs && alen[0] >= 400 ? 88 : 32);
		break;
	case 0x4012: {
		uint32_t rnd = 0;

		read_random(&rnd, sizeof(rnd));
		ob_word(0);
		ob_word(rnd);
		RSAY("r1md: rpc op 0x4012 TRNG -> 0, <random>");
		break;
	}
	case 0x4013:    /* QUERY_CARD_TYPE: the inputs echoed */
		for (uint32_t i = 0; i < nargs; i++) {
			ob_copy(pb + aoff[i], alen[i]);
		}
		RSAY("r1md: rpc op 0x4013 QUERY_CARD_TYPE: %u inputs echoed", nargs);
		break;
	case 0x4321:
		ob_word(1);
		ob_word(0xa5a5);
		RSAY("r1md: rpc op 0x4321 IT_OP -> 1, 0xa5a5");
		break;
	case 0x400f: case 0x4010: case 0x4011: case 0x4014: case 0x4015:
		SAYM("r1md: USER-SPACE RPC op %#x nargs %u arg0 len %u: logged, NOT ANSWERED", op, nargs, nargs ? alen[0] : 0);
		send = false;
		break;
	case 0x400d:    /* IPC_RPC_CCCI_LHIF_MAPPING (port_rpc.c ccci_rpc_remap_queue): arg0 = {u32 net_if, u32 lhif_q}; reply 2 x {4, 0}. Only with r1_md_data=1 */
		if (data_on) {
			uint32_t nif = nargs && alen[0] >= 8 ? *(const uint32_t *)(pb + aoff[0]) : 0xffff, lq = nargs && alen[0] >= 8 ? *(const uint32_t *)(pb + aoff[0] + 4) : 0xffff;

			ob_word(0);
			ob_word(0);
			SAYM("r1md: rpc op 0x400d LHIF_MAPPING net_if %u lhif_q %u (2 = UL Q0 -> CLDMA txq 0) -> 0, 0", nif, lq);
			break;
		}
		__attribute__((fallthrough));
	default:
		SAYM("r1md: UNKNOWN RPC op %#x nargs %u arg0 len %u \"%s\": answered -1", op, nargs, nargs ? alen[0] : 0, name);
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

/* attempt 3: every FS request after EFUN=1 (the attach window), logged compactly (cap 25) + counters for RESULT 2f-A */
static uint32_t fsw_ops, fsw_err, fsw_wr, fsw_logged;

static void
fs_window(uint32_t op, uint32_t rel)
{
	const char *l = mdfs_last(), *a = strnstr(l, " -> ", strlen(l));

	fsw_ops++;
	if (a && strtol(a + 4, NULL, 10) < 0) {
		fsw_err++;
	}
	if (op == 0x1004 || op == 0x1025) {
		fsw_wr++;
	}
	if (fsw_logged++ < 25) {
		SAYM("r1md: [%u ms] fs* op %#x %s", rel, op, l);
	}
}

static void
fs_rx(const uint32_t *pkt, uint32_t L, uint32_t data0, uint32_t slot, uint32_t rel)
{
	uint32_t n, body, sent = 0, h0 = data0 & ~0xc0000000u;

	if (slot >= FS_SLOTS || L < 0x14 || L > FS_BUF || fs_out == NULL) {
		fs_bad++;
		SAYM("r1md: fs REQUEST DROPPED: slot %u len %u", slot, L);
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
		SAYM("r1md: fs request too long: dropped");
		return;
	}
	if (data0 & 0x80000000u) {
		return;         /* more fragments follow */
	}
	n = mdfs_handle(fs_req[slot] + 0x10, fs_have[slot] - 0x10, fs_out);
	if (rd_t_on) {
		fs_window(*(const uint32_t *)(fs_req[slot] + 0x10), rel);
	}
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
			SAYM("r1md: fs REPLY NOT SENT (the TX ring stayed full)");
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
	/* attempt 2 (docs/cellular-attach.md 4.1): stock volatile pre-radio settings, URC enables, read-only diagnostics */
	"AT+EDALLOW=1", "AT+EGRAT=14", "AT+CEREG=3", "AT+CSCON=3", "AT+EIAREG=1", "AT+PSBEARER=1", "AT+ECCAUSE=1", "AT+CEER", "AT+EREG?", "AT+EGREG?", "AT+CSCON?", "AT+EPSB?",
};
/* the scripts: 2d (r1_md=6), 2e-1 (r1_md=7: SIM up, radio off), and the 2e-2 ones (radio_go only) */
static const char *const at_seq6[] = { "AT", "ATE0", "AT+CFUN?", "AT+EFUN?", "AT+CPIN?", "AT+ESIMS?", "AT+CGSN", "AT+CGMR" };
static const char *const at_seq7[] = {
	"AT", "ATE0", "AT+CMEE=1", "AT+ESIMS=1", "AT+ESIMS", "AT+EDALLOW=1", "AT+EBOOT=0", "AT+EPOC", "AT+EFUN=0", "AT+CPIN?", "AT+EFUN?", "AT+CFUN?",
	"AT+CGSN", "AT+CGMR", "AT+ERAT?", "AT+CEMODE?", "AT+EPSB?",
};
static const char *const at_seqA[] = {
	"AT+CREG=2", "AT+CGREG=2", "AT+CEREG=3", "AT+EREG=3", "AT+EGREG=3", "AT+CSCON=3", "AT+EIAREG=1", "AT+PSBEARER=1", "AT+ECCAUSE=1", "AT+ECSQ=1",
	"AT+EGRAT=14", "AT+EFUN=1",
};
static const char *const at_seqP[] = { "AT+COPS?", "AT+CSQ", "AT+CGATT?", "AT+CEREG?", "AT+EREG?", "AT+CSCON?" };
static const char *const at_seqC[] = { "AT+CSCON?" };
static const char *const at_seqD[] = { "AT+CEER", "AT+CEREG?", "AT+EREG?", "AT+EGREG?", "AT+CSCON?" };
static const char *const at_seqQ[] = { "AT+CEREG?" };
static const char *const at_seqS[] = { "AT+CSQ" };
#define AT_NCMD 8
static const char *const at_names[AT_NCMD] = { "AT", "ATE0", "CFUN", "EFUN", "CPIN", "ESIMS", "CGSN", "CGMR" };
static const char *const *at_seq = at_seq6;
static uint32_t at_n = AT_NCMD;
static char at_buf[1024], at_resp[1500], at_res[AT_NCMD][24];
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
/* attempt 2: EPS registration evidence, reject cause, RRC state, kept for RESULT 2e-2 */
static bool eps_reg, stat3_pend, rej_perm, held_started, final_diag;
static int cs_stat = -1, ereg_stat = -1, rej_type = -1, rej_cause = -1, cscon_n;
static uint32_t n_stat3, diag_rounds, cscon1_ms_v, eps_reg_at, stat3_ms_v;
static bool cldma_on, cl_fatal;     /* cldma_on: boot-arg r1_md_cldma=1 (redwood_cldma.c) */
/* Stage 3a: data_on = r1_md_data=1 together with radio_req and cldma_on (set in redwood_md_init); nothing below runs without it. eif_on: optional
 * r1_md_eif=1 (GUESS): also send AT+EPDN=0,"addr" if no IPv4 address was found by then. (EPDN ifst 20 / EIF ifst up are stock-literal, ccci_dump.txt, and always sent.) */
static bool d_cgev, d_have_ip, d_fin;
enum { DP_START, DP_ACT, DP_EPDN, DP_EIF, DP_PA, DP_RD, DP_EPA, DP_WAIT, DP_IF };
static int dp = DP_START;
static const char *d_src = "-";                 /* where d_ip came from (RESULT 3a) */
static bool d_v6seen, d_hexok;                  /* an IPv6-looking token was seen; d_hex = 8-hex-digit token (fallback, GUESS: only used if no dotted IPv4 turns up) */
static uint8_t d_hex[4];
static uint32_t d_scan_logged;
static void data_scan(const char *src, const char *l, bool accept);
static char d_apn[40], d_res[64] = "";
static uint8_t d_ip[4], d_dns[2][4];
static uint32_t d_ndns, d_cur, d_tries, d_rxp, d_rxb, d_rx_logged, d_sendms[2], d_ncontrdp;
static int d_mtu = -1;
static bool d_reply;
static void contrdp_parse(const char *line);
static char eps_seq[28], eps_src[12] = "-", eiareg_s[40], ceer_s[64];
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
static const char *const id_pfx[] = { "+ICCID", "+CIMI", "+CNUM", "+EIMSI", "+CGSN", "+EGMR", "+ESIMAPP", "+ESIMIND", "+ECIMI",
	"+CGCONTRDP", "+EPDN", "+EIF" /* Stage 3a: addresses; contrdp_parse logs the masked values */ };

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
			!strcmp(pre, "+ECSQ") || !strcmp(pre, "+COPS") || !strcmp(pre, "+CSQ") || !strcmp(pre, "+CGATT") || !strcmp(pre, "+CSCON") ||
			!strcmp(pre, "+EIAREG") || !strcmp(pre, "+CGEV") || !strcmp(pre, "+PSBEARER") || !strcmp(pre, "+ECCAUSE") || !strcmp(pre, "+CEER") ||
			!strcmp(pre, "+EPSB")) {      /* cell/signal/operator: not personal */
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

/* the periodic queries: their echo is not logged and their response only when it differs from the last one */
static bool
at_periodic(const char *c)
{
	return !strcmp(c, "AT+CEREG?") || !strcmp(c, "AT+CSQ") || !strcmp(c, "AT+CSCON?") || !strcmp(c, "AT+EREG?") || !strcmp(c, "AT+COPS?") ||
	       !strcmp(c, "AT+CGATT?");
}

static bool at_is_urc(const char *cmd, const char *l);

/* hash of the response's non-URC lines and the final result; true = the same as the last response to this command */
static bool
at_same_as_last(const char *cmd, const char *final)
{
	static uint32_t keys[8], sigs[8], nk;
	uint32_t h = fnv32(final, (uint32_t)strlen(final)), ck = fnv32(cmd, (uint32_t)strlen(cmd)), k;
	char tmp[120];

	for (const char *p = at_resp; p < at_resp + at_rlen;) {
		const char *e = memchr(p, '\n', (size_t)(at_resp + at_rlen - p));
		uint32_t n = e ? (uint32_t)(e - p) : (uint32_t)(at_resp + at_rlen - p);

		n = n < sizeof(tmp) - 1 ? n : (uint32_t)sizeof(tmp) - 1;
		memcpy(tmp, p, n);
		tmp[n] = 0;
		if (!at_is_urc(cmd, tmp)) {
			h = (h ^ fnv32(tmp, n)) * 16777619u;
		}
		p = e ? e + 1 : at_resp + at_rlen;
	}
	for (k = 0; k < nk && keys[k] != ck; k++) {
	}
	if (k < nk && sigs[k] == h) {
		return true;
	}
	if (k == nk && nk < 8) {
		keys[nk++] = ck;
	}
	if (k < 8) {
		sigs[k] = h;
	}
	return false;
}

static uint32_t csv_split(const char *v, char f[][20], uint32_t n);

/* +ECSQ: log the first 3, then only a rsrp (x4, field 7) move of 3 dB or more, or the cell lost/found (field 1 = 255) */
static bool
ecsq_log(const char *l)
{
	static uint32_t n;
	static int last, lost;
	char f[10][20];
	uint32_t nf = csv_split(l + 6, f, 10);
	int rsrp, lnow;
	bool chg;

	if (nf < 7) {
		return true;
	}
	rsrp = (int)strtol(f[6], NULL, 10);
	lnow = strtol(f[0], NULL, 10) == 255;
	chg = lnow != lost || (!lnow && (rsrp > last ? rsrp - last : last - rsrp) >= 12);
	if (n++ < 3 || chg) {
		last = rsrp;
		lost = lnow;
		return true;
	}
	return false;
}

static bool
urc_must(const char *l)
{
	static const char *const p[] = { "+CEREG", "+CREG", "+CGREG", "+EREG", "+EGREG", "+CSCON", "+EIAREG", "+CGEV", "+ECCAUSE", "+PSBEARER", "+EPSB", "+EIND" };

	for (unsigned i = 0; i < sizeof(p) / sizeof(p[0]); i++) {
		size_t n = strlen(p[i]);

		if (!strncmp(l, p[i], n) && l[n] == ':') {
			return true;
		}
	}
	return false;
}

/* Stage 3a, only with data_on (the stock RIL's bearer binding, docs/cellular-stage3.md C.1; EAPNSET/EIAAPN, which write the APN table, are NOT sent) */
static const char at_apn_cmd[] = "AT+EAPNACT=1,\"RESELLER\",\"default\",0,,\"dnn=RESELLER\",,0";
/* stock literal (ccci_dump.txt 80.7017/80.7028 s, libmtk-ril 0x11d9e2 / 0x12851d): sent right after the +EIF URCs; the +EPDN reply carries addresses */
static const char at_epdn_cmd[] = "AT+EPDN=0,\"ifst\",20";
static const char at_eif_cmd[] = "AT+EIF=100, \"ifst\", \"up\", 1";
static const char at_pa_cmd[] = "AT+CGPADDR=0";
static const char at_epa_cmd[] = "AT+EPDN=0,\"addr\"";        /* libmtk-ril 0x103f57 only, never seen on the wire: GUESS, r1_md_eif=1 only */
static const char at_rd_cmd[] = "AT+CGCONTRDP=0";
static const char *const at_seqAPN[] = { at_apn_cmd };
static const char *const at_seqEPDN[] = { at_epdn_cmd };
static const char *const at_seqEIF[] = { at_eif_cmd };
static const char *const at_seqPA[] = { at_pa_cmd };
static const char *const at_seqEPA[] = { at_epa_cmd };
static const char *const at_seqRD[] = { at_rd_cmd };

static bool
at_send(const char *cmd, uint32_t rel)
{
	uint32_t body[17], n = (uint32_t)strlen(cmd), maxlen = 20;
	bool listed = false;

	for (unsigned i = 0; i < sizeof(at_white) / sizeof(at_white[0]); i++) {
		listed |= !strcmp(cmd, at_white[i]);
	}
	/* the one command that turns RF on: only 2e-2, which needs r1_md=7, r1_md_radio=1 and all ten gates (radio_go is set nowhere else) */
	if (!strcmp(cmd, "AT+EFUN=1") && !(md_level == 7 && radio_req && radio_go)) {
		listed = false;
	}
	if (data_on && md_level == 7 && radio_go) {
		if (!strcmp(cmd, at_apn_cmd) || !strcmp(cmd, at_epdn_cmd) || !strcmp(cmd, at_eif_cmd) || (eif_on && !strcmp(cmd, at_epa_cmd))) {
			listed = true;
			maxlen = 60;            /* the long ones: EAPNACT, EPDN ifst, EIF ifst (+ EPDN addr with r1_md_eif=1) only; the body holds 67 characters */
		} else if (!strcmp(cmd, at_rd_cmd) || !strcmp(cmd, at_pa_cmd)) {
			listed = true;
		}
	}
	if (!listed || n > maxlen) {
		SAYM("r1md: at REFUSED \"%s\" (not on the whitelist)", cmd);
		at_refused = true;
		return false;
	}
	memset(body, 0, sizeof(body));
	memcpy(body, cmd, n);
	((char *)body)[n] = '\r';
	if (!ring_tx(5, 12, 0, CCCI_HEADER_LEN + n + 1, 0, body, n + 1, rel)) {
		return false;
	}
	if (!(rd_t_on && at_periodic(cmd))) {
		SAY("r1md: [%u ms] AT> %s", rel, cmd);
	}
	strlcpy(at_lastcmd, cmd, sizeof(at_lastcmd));
	at_to = false;
	return true;
}

static bool
at_is_final(const char *l)
{
	/* only these end a command: every other line (+CREG: 2 ...) is a URC or an information line */
	return !strcmp(l, "OK") || !strcmp(l, "ERROR") || !strncmp(l, "+CME ERROR", 10) || !strncmp(l, "+CMS ERROR", 10);
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

/* split `a,"b",,c` into up to n fields (quotes dropped, empty fields stay empty) */
static uint32_t
csv_split(const char *v, char f[][20], uint32_t n)
{
	uint32_t k = 0, o = 0;
	bool q = false;

	while (*v == ' ') {
		v++;
	}
	f[0][0] = 0;
	for (; *v && k < n; v++) {
		if (*v == '"') {
			q = !q;
		} else if (*v == ',' && !q) {
			f[k][o] = 0;
			o = 0;
			if (++k < n) {
				f[k][0] = 0;
			}
		} else if (o < 19) {
			f[k][o++] = *v;
		}
	}
	if (k < n) {
		f[k][o] = 0;
		k++;
	}
	return k;
}

static int
fnum(char f[][20], uint32_t nf, uint32_t i)
{
	return (i < nf && f[i][0] >= '0' && f[i][0] <= '9') ? (int)strtol(f[i], NULL, 10) : -1;
}

static uint32_t chg_n;          /* radio: +CEREG / +CSCON change lines (cap 24 together) */
static int cscon_last = -2;

static void
eps_mark(const char *src, uint32_t rel)
{
	if (!eps_reg_at) {
		eps_reg_at = rd_t_on ? rel - rd_t_on + 1 : 1;     /* +1: 0 means never */
		strlcpy(eps_src, src, sizeof(eps_src));
	}
	eps_reg = true;
	rej_perm = false;
}

/* +CEREG / +EREG. A query response is `+CEREG: <n>,<stat>,...`, a URC is `+CEREG: <stat>[,"tac",...]`: the caller says which (the line is a
 * query response only while that very query is outstanding and its 2nd field is an unquoted digit). EREG: stat,lac,ci,eAct,nwk,roam,cause_type,cause;
 * CEREG: stat,tac,ci,AcT,cause_type,cause. */
static void
reg_note(bool is_cereg, const char *v, bool query_ctx, uint32_t rel)
{
	char f[10][20];
	uint32_t nf = csv_split(v, f, 10);
	const char *c = strchr(v, ',');
	uint32_t o = (query_ctx && c && c[1] >= '0' && c[1] <= '9') ? 1 : 0;
	int stat = fnum(f, nf, o), act = fnum(f, nf, o + 3), ct = fnum(f, nf, o + (is_cereg ? 4 : 6)), ca = fnum(f, nf, o + (is_cereg ? 5 : 7));
	size_t sl = strlen(eps_seq);

	if (stat < 0) {
		return;
	}
	if (is_cereg) {
		if (stat != creg_stat && sl + 3 < sizeof(eps_seq)) {
			if (sl) {
				eps_seq[sl++] = ',';
			}
			eps_seq[sl++] = (char)('0' + stat % 10);
			eps_seq[sl] = 0;
		}
		if (stat != creg_stat && rd_t_on && chg_n++ < 24) {
			SAYM("r1md: [%u ms] radio: +CEREG stat %d (was %d), EFUN=1 +%u ms", rel, stat, creg_stat, rel - rd_t_on);
		}
		if (stat == 3 && creg_stat != 3) {
			n_stat3++;
			stat3_pend = true;
			if (!stat3_ms_v && rd_t_on) {
				stat3_ms_v = rel - rd_t_on + 1;
			}
		}
		creg_stat = stat;
		if (stat == 1 || stat == 5) {
			eps_mark("CEREG", rel);
		} else {
			eps_reg = false;
		}
	} else {
		ereg_stat = stat;
		if ((stat == 1 || stat == 5) && act == 7) {
			eps_mark("EREG", rel);
		}
	}
	if (ca > 0 && (ct <= 0)) {
		rej_type = ct;
		rej_cause = ca;
		rej_perm = (ca == 3 || ca == 6 || ca == 7 || ca == 8) && !eps_reg;
	}
}

/* 2e: every line from the modem, in or out of a response: what it says about the SIM and the registration */
static void
at_note(const char *l, uint32_t rel)
{
	bool wq = at_wait && at_idx < at_n;

	if (data_on && dp >= DP_ACT && !d_fin) {      /* Stage 3a: every line of the data phase, in or out of a response, URCs included */
		if (!strncmp(l, "+EIF:", 5)) {
			data_scan("EIF", l + 5, true);
		} else if (!strncmp(l, "+EPDN:", 6)) {
			data_scan("EPDN", l + 6, true);
		} else if (!strncmp(l, "+CGPADDR:", 9)) {
			data_scan("CGPADDR", l + 9, true);
		} else if (!strncmp(l, "+EAPNINFO:", 10)) {
			data_scan("EAPNINFO", l + 10, false);
		}
	}
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
		reg_note(true, l + 7, wq && !strcmp(at_seq[at_idx], "AT+CEREG?"), rel);
	} else if (!strncmp(l, "+EREG:", 6)) {
		reg_note(false, l + 6, wq && !strcmp(at_seq[at_idx], "AT+EREG?"), rel);
	} else if (!strncmp(l, "+CREG:", 6)) {
		char f[2][20];

		cs_stat = fnum(f, csv_split(l + 6, f, 2), 0);      /* CS domain: recorded, never acted on */
	} else if (!strncmp(l, "+CSCON:", 7)) {
		char f[3][20];
		uint32_t nf = csv_split(l + 7, f, 3);
		const char *c = strchr(l, ',');
		bool q = wq && !strcmp(at_seq[at_idx], "AT+CSCON?") && c && nf > 1;      /* query: +CSCON: <n>,<mode>[,...] */

		if (rd_t_on && fnum(f, nf, q ? 1 : 0) != cscon_last && chg_n++ < 24) {
			SAYM("r1md: [%u ms] radio: +CSCON %d (was %d), EFUN=1 +%u ms", rel, fnum(f, nf, q ? 1 : 0), cscon_last, rel - rd_t_on);
		}
		cscon_last = fnum(f, nf, q ? 1 : 0);
		if (fnum(f, nf, q ? 1 : 0) == 1 && rd_t_on && !cscon1_ms_v) {
			cscon1_ms_v = rel - rd_t_on + 1;
		}
	} else if (!strncmp(l, "+EIAREG:", 8) && strnstr(l, "ME ATTACH", strlen(l))) {
		const char *v = l + 8;

		while (*v == ' ') {
			v++;
		}
		at_safe(v, eiareg_s, sizeof(eiareg_s));
		eps_mark("EIAREG", rel);
		if (data_on) {      /* the APN name the modem attached with: "reseller.mnc410.mcc310.gprs",IP,0 */
			const char *q1 = strchr(v, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;

			if (q2 && q2 - q1 - 1 < (long)sizeof(d_apn)) {
				memcpy(d_apn, q1 + 1, (size_t)(q2 - q1 - 1));
				d_apn[q2 - q1 - 1] = 0;
			}
		}
	} else if (!strncmp(l, "+CEER:", 6)) {
		const char *v = l + 6;

		while (*v == ' ') {
			v++;
		}
		at_safe(v, ceer_s, sizeof(ceer_s));
	} else if (data_on && dp == DP_ACT && strnstr(l, "ME PDN ACT", strlen(l))) {
		d_cgev = true;
	} else if (strnstr(l, "ME DETACH", strlen(l))) {
		detach_seen = true;
		eps_reg = false;
	}
}

/* a "+XXX: ..." line that is not the information response of this command (its name is the command's) is an unsolicited result code */
static bool
at_is_urc(const char *cmd, const char *l)
{
	size_t n = 0;

	if (l[0] != '+') {
		return false;
	}
	if (cmd[2] != '+') {
		return true;
	}
	cmd += 3;
	while (cmd[n] && cmd[n] != '?' && cmd[n] != '=') {
		n++;
	}
	return strncmp(l + 1, cmd, n) || l[1 + n] != ':';
}

/* a command timed out: the lines buffered during its wait (URCs included) are logged here, not dropped */
static void
at_dump(const char *cmd, uint32_t rel)
{
	char one[112], *line, *next;
	uint32_t n = 0;

	at_resp[at_rlen] = 0;
	for (line = at_resp; line && *line && n < 40; line = next, n++) {
		next = strchr(line, '\n');
		if (next) {
			*next++ = 0;
		}
		at_safe(line, one, sizeof(one));
		SAY("r1md: [%u ms] AT< %s (no final result) buffered: %s", rel, cmd, one);
	}
	at_rlen = 0;
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
	bool luhn = false, suppress = false;
	char idbuf[20];

	show[0] = 0;
	idbuf[0] = 0;
	at_last_ok = !strcmp(final, "OK");
	at_resp[at_rlen] = 0;
	if (md_level >= 7 && rd_t_on && at_periodic(cmd) && at_same_as_last(cmd, final)) {
		suppress = true;
		md_sup++;
	}
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
			if (data_on && !strncmp(line, "+CGCONTRDP:", 11)) {
				contrdp_parse(line);
			}
			at_safe(line, one, sizeof(one));
			if (md_level >= 7) {
				if (!suppress || at_is_urc(cmd, line)) {
					redwood_md_say(at_is_urc(cmd, line) && urc_must(line), "r1md: [%u ms] AT< %s: %s%s", rel, cmd, at_is_urc(cmd, line) ? "[urc] " : "", one);
				}
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
			if (!suppress) {
				SAY("r1md: [%u ms] AT< %s: %s", rel, cmd, final);
			}
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
				SAYM("r1md: [%u ms] CPIN busy: asking again in 2 s", rel);
				at_wait = false;
				at_last_ms = rel;
				at_not_before = rel + 2000;
				return;
			}
		}
		if (!strcmp(sim_cpin, "SIM PIN") || !strcmp(sim_cpin, "SIM PUK")) {
			SAYM("r1md: [%u ms] the SIM wants a PIN/PUK: none is ever sent, stopping", rel);
			at_stop = true;
		}
	}
	if (md_level >= 7 && !strcmp(cmd, "AT+EFUN=1") && at_last_ok && !efun1_ok_ms) {
		efun1_ok_ms = rel - at_sent_ms + 1;     /* +1: 0 means "no OK seen" */
	}
	if (md_level >= 7 && !strcmp(cmd, "AT+ESIMS") && sim_esims == 0) {
		SAYM("r1md: [%u ms] +ESIMS: 0, no SIM: stopping", rel);
		at_stop = true;
	}
	at_wait = false;
	at_last_ms = rel;
	at_idx++;
	if (at_radio_on && !rd_active) {
		SAYM("r1md: *** RADIO ON BY DEFAULT (CFUN %d, EFUN %d): nothing more is sent, the run ends now ***", at_radio_val[0], at_radio_val[1]);
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
		at_note(l, rel);
	}
	if (at_wait) {
		if (!strcmp(l, at_seq[at_idx])) {
			return;         /* the echo */
		}
		if (data_on && !strncmp(l, "+EPDN:", 6) && !strcmp(at_seq[at_idx], at_epdn_cmd) && at_rlen + strlen(l) + 1 < sizeof(at_resp)) {
			/* md3c2: AT+EPDN=0,"ifst",20 answered with this line and never a final result (5 s lost): the line completes the command (a late OK is logged as such) */
			memcpy(at_resp + at_rlen, l, strlen(l));
			at_rlen += (uint32_t)strlen(l);
			at_resp[at_rlen++] = '\n';
			at_finish("OK", rel);
			return;
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
		SAYM("r1md: [%u ms] late final for %s: %s (no command outstanding)", rel, at_lastcmd[0] ? at_lastcmd : "(none)", l);
		return;
	}
	if (at_unsol++ < (rd_t_on ? 800u : (md_level >= 7 ? 200u : 80u)) && (!st2a.hs2 || rd_t_on || rel - st2a.t_hs2 <= 60000)) {
		if (l[0] == '+') {
			char pre[20];
			uint32_t n = 0;

			if (md_level >= 7) {
				char one[120];

				at_safe(l, one, sizeof(one));
				if (!strncmp(l, "+ECSQ:", 6) && !ecsq_log(l)) {
					md_sup++;
					return;
				}
				redwood_md_say(urc_must(l), "r1md: [%u ms] AT unsolicited %s", rel, one);
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
		uint32_t to = !strcmp(cmd, "AT+EFUN=0") ? 30000u : (!strcmp(cmd, "AT+EFUN=1") ? 10000u : (!strcmp(cmd, at_epdn_cmd) ? 1000u : 5000u));
		/* md3d2: the +EPDN: line of AT+EPDN=0,"ifst",20 is NOT what the modem answers with within 5 s: it came 5.1 s later, right after the next command (AT+EIF ifst up) was sent, so the completion above never fires. The modem never gives this command a final result: wait 1 s, not 5 */

		if (rel - at_sent_ms >= to) {
			SAYM("r1md: [%u ms] AT< %s: TIMEOUT (%u s without a final result)", rel, cmd, to / 1000);
			at_dump(cmd, rel);
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
	if (!strcmp(at_seq[at_idx], "AT+EFUN=1")) {
		cldma_snap("pre-radio", rel, md1_is_on());
	}
	if (at_send(at_seq[at_idx], rel)) {
		at_wait = true;
		at_sent_ms = rel;
		if (!strcmp(at_seq[at_idx], "AT+EFUN=1")) {
			rd_t_on = rel;
			md_budget = 26000;      /* the radio phase gets its own headroom */
		}
	} else {
		if (md_level == 6) {
			snprintf(at_res[at_idx], sizeof(at_res[at_idx]), "notsent");
		}
		at_done = true;
	}
}

static uint32_t oth_logged[256], rpc_keys[32], rpc_nkeys, rpc_dups, rpc_win;
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
		SAYM("r1md: q%d rx ring garbage at %#x (w %#x)", q, r, w);
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
		SAYM("r1md: q%d rx ring garbage at %#x (w %#x): head %#x len %#x foot %#x %#x", q, r, w, rb_word(buf, len, r), L,
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
			SAYM("r1md: [%u ms] HS2 (MD_NORMAL_BOOT) received", rel);
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
			if (op != 0x4005 && op != 0x400e && !(data_on && op == 0x400d)) {
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
		{
			uint32_t key = fnv32(name, (uint32_t)strlen(name)) ^ (op * 2654435761u), k;

			for (k = 0; k < rpc_nkeys && rpc_keys[k] != key; k++) {
			}
			rpc_quiet = k < rpc_nkeys;
			if (rpc_quiet) {
				rpc_dups++;
			} else {
				if (rpc_nkeys < 32) {
					rpc_keys[rpc_nkeys++] = key;
				}
				SAY("r1md: rpc request op %#x nargs %u arg0 len %u \"%s\" (buffer %u)", op, nargs, L >= 28 ? pkt[6] : 0, name, resv);
			}
		}
		if (rd_t_on) {
			rpc_win++;
		}
		if (md_level >= 4) {
			rpc_serve(pkt, L, ncopy, rel);
			rpc_quiet = false;
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

				SAYM("r1md: ee dbg +%#x: %08x %08x %08x %08x %08x %08x %08x %08x", base + l * 32, x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]);
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
			SAYM("r1md: ee str +%#x: \"%s\"", start, s);
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
		SAYM("r1md: [%u ms] the CCIF SRAM message is NOT HS1", elapsed);
		return false;
	}
	SAYM("r1md: [%u ms] HS1 (MD_INIT_START_BOOT, CHK_ID) received from the modem in the CCIF SRAM", elapsed);
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
	if (res != 0) {         /* passing gates are in the summary line "gates N/10" */
		SAYM("r1md: gate %d %s: %s %s", n, name, res == 1 ? "FAIL" : "pending reference", d);
	}
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
	SAYM("r1md: gates %u/10%s%s", gates_pass, gates_bad[0] ? "; not passed: " : "", gates_bad);
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
	SAYM("r1md: RESULT 2e-1: SIM %s (ESIMS %d), EFUN %d CFUN %d%s, gates passed %u of 10%s%s", sim, sim_esims, at_radio_val[1], at_radio_val[0],
	    at_radio_on ? " RADIO ON BY DEFAULT" : "", gates_pass, gates_bad[0] ? ", not passed: " : "", gates_bad);
}

/* ---- sub-stage 2e-2: registration only. Reached only with radio_go, which needs r1_md=7, r1_md_radio=1 and all ten gates. ---- */

enum { RP_PRE, RP_WAIT, RP_POST, RP_DATA, RP_HOLD, RP_OFF, RP_END };
static int rp = RP_PRE;
static bool rp_new = true, rp_failed;
static uint32_t rp_t, rp_ereg, rp_csq, rp_reg_ms;
static char rp_why[40];

static void result_2fa(const char *tag, uint32_t rel);
static void result_3a(uint32_t rel);

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
		SAYM("r1md: radio: stopping (%s): AT+EFUN=0", why);
		strlcpy(rp_why, why, sizeof(rp_why));
		rp_failed = true;
		rp = RP_OFF;
		rp_new = true;
	}
}

/* the two RESULT 2e-2 lines (each under the ~160-char log cap); said once, as soon as the off sequence ends (or at the end of the run) */
static bool result2_said;
static const char *res_tag = "end";
static uint32_t res_rel;
static int off_try = 0, off_ph = 0, efun_final = -2;
static uint32_t off_tm;
static const char *off_res = "none";
static const char *const at_seqE0[] = { "AT+EFUN=0" };
static const char *const at_seqEQ[] = { "AT+EFUN?" };

static void
result_2e2(uint32_t rel)
{
	char cs[16], rg[24];

	if (result2_said) {
		return;
	}
	result2_said = true;
	res_rel = rel;
	if (cscon1_ms_v) {
		snprintf(cs, sizeof(cs), "%d ms", (int)cscon1_ms_v - 1);
	} else {
		strlcpy(cs, "never", sizeof(cs));
	}
	if (eps_reg_at) {
		snprintf(rg, sizeof(rg), "%d ms (%s)", (int)eps_reg_at - 1, eps_src);
	} else {
		strlcpy(rg, "never", sizeof(rg));
	}
	SAYM("r1md: RESULT 2e-2: %s; EFUN=1 OK %d ms; CSCON1 %s; eps %s; reg %s; held %s", rp_failed ? rp_why : "completed", efun1_ok_ms ? (int)efun1_ok_ms - 1 : -1, cs,
	    eps_seq[0] ? eps_seq : "-", rg, held_started ? (held_ok ? "yes" : "NO") : "n/a");
	SAYM("r1md: RESULT 2e-2b: reject type %d cause %d n3 %u cs %d; op %s RAT %d csq %d; EFUN=0 x%d %s, EFUN? %d, detach %s; on %u ms", rej_type, rej_cause, n_stat3,
	    cs_stat, rp_op, rp_rat, rp_csqv, off_try, off_res, efun_final, detach_seen ? "seen" : "no", rd_t_on ? rel - rd_t_on : 0);
	SAYM("r1md: RESULT 2e-2c: ch34 %u ch38 %u new ch %u/%u pkts quiet %u; eiareg \"%s\"; diag rounds %u; ereg stat %d; hold %s", n34, n38, oth_new_ch, oth_new, n_quiet,
	    eiareg_s, diag_rounds, ereg_stat, held_started ? "reached" : "not reached");
	SAYM("r1md: RESULT 2e-2d: ceer \"%s\"", ceer_s);
	result_2fa(res_tag, rel);
	if (data_on) {
		result_3a(rel);
	}
}

/* the off sequence: one command at a time (at_step is the only sender and sends nothing while at_wait). Plain AT+EFUN=0 (30 s), a quiet wait,
 * AT+EFUN?; if that is not 0 one more EFUN=0 + EFUN?; the caller powers MD1 down in every case */
static uint32_t off_wait;

static void
off_drive(uint32_t rel)
{
	if (at_wait) {
		return;         /* the command in flight (EFUN=1 included) gets its result, or its timeout, first */
	}
	if (efun1_to) {         /* EFUN=1 never answered: the AT engine is not listening, EFUN=0 would not be heard either */
		SAYM("r1md: radio: AT+EFUN=1 timed out: EFUN=0 skipped, MD1 powered down");
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
		off_res = at_last_ok ? "OK" : (at_to ? "timeout" : "error");
		off_wait = at_last_ok ? 2000 : (at_to ? 3000 : 1000);      /* quiet time for the late final / the URCs (+CGEV: ME DETACH) */
		off_ph = 2;
		break;
	case 2:
		if (rel - off_tm >= off_wait) {
			off_ph = 3;
		}
		break;
	case 3:
		at_radio_val[1] = -1;           /* a timed-out EFUN? must not look like the earlier +EFUN: 0 */
		radio_load(at_seqEQ, 1);
		off_ph = 4;
		break;
	case 4:
		if (!at_done) {
			return;
		}
		efun_final = at_radio_val[1];
		if (efun_final != 0 && off_try < 2) {
			off_ph = 0;     /* not confirmed off: one more EFUN=0 + EFUN? */
			break;
		}
		SAYM("r1md: radio: off sequence done: EFUN=0 %d tries (%s), AT+EFUN? -> %d%s", off_try, off_res, efun_final, efun_final == 0 ? " (confirmed off)" : " (NOT confirmed)");
		rp = RP_END;
		rd_active = false;
		result_2e2(rel);
		break;
	}
}

/* ---- Stage 3a: bearer binding, one kernel-built DNS query over CLDMA (r1_md_data=1; docs/cellular-stage3.md C) ---- */

static uint32_t dp_t0, dp_t;
static uint16_t d_id, d_sport;

static bool
v4_parse(const char *s, uint8_t o[4])
{
	const char *p = s;

	for (int n = 0; n < 4; n++) {
		uint32_t v = 0, digs = 0;

		while (*p >= '0' && *p <= '9' && digs < 4) {
			v = v * 10 + (uint32_t)(*p - '0');
			p++;
			digs++;
		}
		if (!digs || v > 255) {
			return false;
		}
		o[n] = (uint8_t)v;
		if (n < 3) {
			if (*p != '.') {
				return false;
			}
			p++;
		}
	}
	return *p == 0 || *p == '.';    /* "a.b.c.d" or "a.b.c.d.m.m.m.m" (address and mask): the first four numbers */
}

/* Stage 3a: a "+EIF: ..." / "+EPDN: ..." / "+CGPADDR: ..." / "+EAPNINFO: ..." line. Format NOT known (the stock transcript masks them; ccci_dump shows only "+EIF: 100, "),
 * so it is split generically at , space " / ( ) and every field is logged as its shape: keywords and short numbers in full, a dotted IPv4 as a.b.x.x, 8 hex digits as <hex8>,
 * IPv6 as <v6>, anything else as <N chars>. accept: the first dotted IPv4 that is not 0.x / 127.x / 255.x becomes the bearer address (never from an "ipdel" line). */
static void
data_scan(const char *src, const char *l, bool accept)
{
	char t[200], out[150], *p = t;
	uint32_t o = 0, nt = 0;
	bool del;

	strlcpy(t, l, sizeof(t));
	del = strnstr(t, "ipdel", strlen(t)) != NULL;
	out[0] = 0;
	while (*p) {
		char *tok;
		uint32_t n = 0, colons = 0, hex = 0, dig = 0, alnum = 0;
		uint8_t a[4];

		while (*p == ',' || *p == ' ' || *p == '"' || *p == '/' || *p == '(' || *p == ')') {
			p++;
		}
		if (!*p) {
			break;
		}
		tok = p;
		while (*p && *p != ',' && *p != ' ' && *p != '"' && *p != '/' && *p != '(' && *p != ')') {
			char c = *p++;

			n++;
			colons += c == ':';
			dig += c >= '0' && c <= '9';
			hex += (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
			alnum += (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
		}
		if (*p) {
			*p++ = 0;
		}
		nt++;
		if (o + 24 >= sizeof(out)) {
			continue;
		}
		if (strchr(tok, '.') && v4_parse(tok, a)) {
			o += (uint32_t)snprintf(out + o, sizeof(out) - o, "%s%u.%u.x.x", o ? "|" : "", a[0], a[1]);
			if (accept && !del && !d_have_ip && a[0] && a[0] != 127 && a[0] < 224) {
				memcpy(d_ip, a, 4);
				d_have_ip = true;
				d_src = src;
				SAYM("r1cell: bearer address %u.%u.x.x taken from +%s", a[0], a[1], src);
			}
		} else if (colons >= 2) {
			d_v6seen = true;
			o += (uint32_t)snprintf(out + o, sizeof(out) - o, "%s<v6>", o ? "|" : "");
		} else if (n == 8 && hex == 8) {
			if (accept && !del && !d_hexok && (tok[0] != '0' || tok[1] != '0')) {     /* GUESS: an IPv4 as 8 hex digits; used only if nothing dotted turns up */
				for (int i = 0; i < 4; i++) {
					char b[3] = { tok[2 * i], tok[2 * i + 1], 0 };

					d_hex[i] = (uint8_t)strtoul(b, NULL, 16);
				}
				d_hexok = true;
			}
			o += (uint32_t)snprintf(out + o, sizeof(out) - o, "%s<hex8>", o ? "|" : "");
		} else if (n <= 10 && alnum == n && (dig == n ? n <= 5 : ((tok[0] | 0x20) >= 'a' && (tok[0] | 0x20) <= 'z'))) {
			o += (uint32_t)snprintf(out + o, sizeof(out) - o, "%s%s", o ? "|" : "", tok);       /* keyword / short number: in full */
		} else {
			o += (uint32_t)snprintf(out + o, sizeof(out) - o, "%s<%u chars>", o ? "|" : "", n);
		}
	}
	if (d_scan_logged++ < 30) {
		SAYM("r1cell: data line +%s: %u fields [%s]", src, nt, out);
	}
}

/* "+CGCONTRDP: <cid>,<bearer>,<apn>,<addr[.mask]>,<gw>,<dns1>,<dns2>,<pcscf1>,<pcscf2>,<imcn>,<lipa>,<mtu>,..." (27.007 layout: GUESS for this modem;
 * the field shapes are logged so a different layout shows). The IPv4 address is logged as its first two octets only; the DNS servers in full. */
static void
contrdp_parse(const char *line)
{
	char t[220], *f[16], *p = t, shape[40];
	uint32_t nf = 0, sn = 0;
	uint8_t gw[4];
	bool first = !d_ncontrdp++;

	strlcpy(t, line + 11, sizeof(t));
	while (nf < 16) {
		char *fs;

		while (*p == ' ') {
			p++;
		}
		if (*p == '"') {
			fs = ++p;
			while (*p && *p != '"') {
				p++;
			}
			if (*p) {
				*p++ = 0;
			}
			while (*p && *p != ',') {
				p++;
			}
		} else {
			fs = p;
			while (*p && *p != ',') {
				p++;
			}
		}
		f[nf++] = fs;
		if (*p != ',') {
			break;
		}
		*p++ = 0;
	}
	for (uint32_t i = 0; i < nf && sn + 3 < sizeof(shape); i++) {
		uint8_t tmp[4];
		char c = !f[i][0] ? 'e' : (strchr(f[i], ':') ? '6' : (v4_parse(f[i], tmp) ? '4' : ((f[i][0] >= '0' && f[i][0] <= '9') ? 'n' : 's')));

		shape[sn++] = c;
	}
	shape[sn] = 0;
	if (!first) {
		SAY("r1cell: CGCONTRDP line #%u: %u fields [%s] (ignored, the first one was taken)", d_ncontrdp, nf, shape);
		return;
	}
	memset(d_dns, 0, sizeof(d_dns));
	if (nf > 3 && !d_have_ip && v4_parse(f[3], d_ip)) {     /* 27.007: local address and subnet mask (empty on this modem; GUESS for others) */
		d_have_ip = true;
		d_src = "CGCONTRDP field 3";
	}
	for (uint32_t i = 0; i < nf; i++) {
		d_v6seen |= strchr(f[i], ':') != NULL;
	}
	d_ndns = 0;
	for (uint32_t i = 5; i < 7 && i < nf; i++) {
		if (v4_parse(f[i], d_dns[d_ndns])) {
			d_ndns++;
		} else {
			memset(d_dns[d_ndns], 0, 4);
		}
	}
	d_mtu = (nf > 11 && f[11][0] >= '0' && f[11][0] <= '9') ? (int)strtol(f[11], NULL, 10) : -1;
	if (!d_apn[0] && nf > 2 && strlen(f[2]) < sizeof(d_apn)) {
		strlcpy(d_apn, f[2], sizeof(d_apn));
	}
	SAYM("r1cell: CGCONTRDP: %u fields [%s] cid %s apn \"%.30s\" ip(now) %u.%u.x.x gw %s dns %u.%u.%u.%u %u.%u.%u.%u (%u found) mtu %d", nf, shape, nf ? f[0] : "-", nf > 2 ? f[2] : "-",
	    d_ip[0], d_ip[1], (nf > 4 && v4_parse(f[4], gw)) ? "yes" : "no", d_dns[0][0], d_dns[0][1], d_dns[0][2], d_dns[0][3], d_dns[1][0], d_dns[1][1], d_dns[1][2], d_dns[1][3],
	    d_ndns, d_mtu);
}

static uint16_t
inet_csum(const uint8_t *b, uint32_t n, uint32_t sum)
{
	for (uint32_t i = 0; i + 1 < n; i += 2) {
		sum += (uint32_t)(b[i] << 8 | b[i + 1]);
	}
	if (n & 1) {
		sum += (uint32_t)b[n - 1] << 8;
	}
	while (sum >> 16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}
	return (uint16_t)(~sum & 0xffff);
}

/* IPv4/UDP, bearer address -> d_dns[which]:53, standard query example.com A (57 bytes); IP and UDP checksums set; sent raw on CLDMA q0 netif 0 */
static bool
dns_send(uint32_t which, uint32_t rel)
{
	static const uint8_t qname[13] = { 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0 };
	uint8_t p[57];
	const uint8_t *dst = d_dns[which];
	uint32_t ps, c;

	if (!d_id) {
		uint32_t r = 0;

		read_random(&r, sizeof(r));
		d_id = (uint16_t)(r & 0xffff) | 1;
		d_sport = (uint16_t)(40000 + ((r >> 16) % 20000));
	}
	memset(p, 0, sizeof(p));
	p[0] = 0x45;
	p[2] = 0;
	p[3] = 57;
	p[4] = 0x52;
	p[5] = 0x31;
	p[6] = 0x40;            /* DF */
	p[8] = 64;
	p[9] = 17;
	memcpy(p + 12, d_ip, 4);
	memcpy(p + 16, dst, 4);
	c = inet_csum(p, 20, 0);
	p[10] = (uint8_t)(c >> 8);
	p[11] = (uint8_t)c;
	p[20] = (uint8_t)(d_sport >> 8);
	p[21] = (uint8_t)d_sport;
	p[23] = 53;
	p[25] = 37;             /* UDP length 8 + 29 */
	p[28] = (uint8_t)(d_id >> 8);
	p[29] = (uint8_t)d_id;
	p[30] = 0x01;           /* RD */
	p[33] = 1;              /* QDCOUNT */
	memcpy(p + 40, qname, sizeof(qname));
	p[54] = 1;              /* A */
	p[56] = 1;              /* IN */
	ps = (uint32_t)(d_ip[0] << 8 | d_ip[1]) + (uint32_t)(d_ip[2] << 8 | d_ip[3]) + (uint32_t)(dst[0] << 8 | dst[1]) + (uint32_t)(dst[2] << 8 | dst[3]) + 17 + 37;
	c = inet_csum(p + 20, 37, ps);
	if (!c) {
		c = 0xffff;
	}
	p[26] = (uint8_t)(c >> 8);
	p[27] = (uint8_t)c;
	d_sendms[which] = rel;
	d_tries++;
	SAYM("r1cell: [%u ms] dns query #%u: example.com A to %u.%u.%u.%u:53 from %u.%u.x.x port %u id %#x, 57 bytes", rel, d_tries, dst[0], dst[1], dst[2], dst[3], d_ip[0], d_ip[1],
	    d_sport, d_id);
	return cldma_tx(p, sizeof(p), rel);
}

static bool
dns_skip(const uint8_t *m, uint32_t ml, uint32_t *pos)
{
	uint32_t q = *pos;

	for (int guard = 0; guard < 64; guard++) {
		uint8_t c;

		if (q >= ml) {
			return false;
		}
		c = m[q];
		if (c == 0) {
			*pos = q + 1;
			return true;
		}
		if ((c & 0xc0) == 0xc0) {
			*pos = q + 2;
			return *pos <= ml;
		}
		if (c & 0xc0) {
			return false;
		}
		q += 1 + c;
	}
	return false;
}

/* every downlink packet from redwood_cldma.c, still in its ring buffer: 4-byte lhif_header {u16 pdcp_count; u8 flow:4,f:3,rsv:1; u8 netif:5,nw_type:3}, then the raw IP packet */
void
cell_rx(const uint8_t *b, uint32_t len, uint32_t ms)
{
	const uint8_t *ip = b + 4, *u, *m;
	uint32_t il, ihl, tl, ml, pos, qd, an, k;
	uint32_t from = 9;
	uint16_t fl;
	bool have_a = false;
	uint8_t a[4] = { 0, 0, 0, 0 };

	d_rxp++;
	if (len < 5) {
		SAY("r1cell: rx #%u len %u (too short for the lhif header)", d_rxp, len);
		return;
	}
	il = len - 4;
	d_rxb += il;
	for (k = 0; k < d_ndns; k++) {
		if (il >= 20 && (ip[0] >> 4) == 4 && !memcmp(ip + 12, d_dns[k], 4)) {
			from = k;
		}
	}
	if (d_rx_logged++ < 20) {
		if (il >= 20 && (ip[0] >> 4) == 4) {
			if (from < 2) {
				SAY("r1cell: rx #%u len %u netif %u IPv4 proto %u from %u.%u.%u.%u (a DNS server)", d_rxp, il, b[3] & 0x1f, ip[9], ip[12], ip[13], ip[14], ip[15]);
			} else {
				SAY("r1cell: rx #%u len %u netif %u IPv4 proto %u from %u.%u.x.x", d_rxp, il, b[3] & 0x1f, ip[9], ip[12], ip[13]);
			}
		} else {
			SAY("r1cell: rx #%u len %u netif %u version %u first byte %02x", d_rxp, il, b[3] & 0x1f, ip[0] >> 4, ip[0]);
		}
	}
	if (if_ok) {
		cell_if_input(ip, il);          /* Stage 3b: the stack gets it (or the cap / v6 rules drop it) */
		return;
	}
	if (d_tries == 0 || d_reply || from > 1 || il < 20 + 8 + 12) {
		return;
	}
	ihl = (uint32_t)(ip[0] & 15) * 4;
	tl = (uint32_t)(ip[2] << 8 | ip[3]);
	if (ihl < 20 || ip[9] != 17 || tl > il || tl < ihl + 8 + 12) {
		return;
	}
	u = ip + ihl;
	m = u + 8;
	ml = tl - ihl - 8;
	if ((u[0] << 8 | u[1]) != 53 || (u[2] << 8 | u[3]) != d_sport || (m[0] << 8 | m[1]) != d_id) {
		return;
	}
	fl = (uint16_t)(m[2] << 8 | m[3]);
	qd = (uint32_t)(m[4] << 8 | m[5]);
	an = (uint32_t)(m[6] << 8 | m[7]);
	pos = 12;
	for (k = 0; k < qd && k < 4; k++) {
		if (!dns_skip(m, ml, &pos)) {
			break;
		}
		pos += 4;
	}
	for (k = 0; k < an && k < 16 && pos < ml && !have_a; k++) {
		uint32_t type, cls, rdl;

		if (!dns_skip(m, ml, &pos) || pos + 10 > ml) {
			break;
		}
		type = (uint32_t)(m[pos] << 8 | m[pos + 1]);
		cls = (uint32_t)(m[pos + 2] << 8 | m[pos + 3]);
		rdl = (uint32_t)(m[pos + 8] << 8 | m[pos + 9]);
		if (pos + 10 + rdl > ml) {
			break;
		}
		if (type == 1 && cls == 1 && rdl == 4) {
			memcpy(a, m + pos + 10, 4);
			have_a = true;
		}
		pos += 10 + rdl;
	}
	d_reply = true;
	snprintf(d_res, sizeof(d_res), "ok rcode %u/answers %u/rtt %u ms", fl & 15, an, ms - d_sendms[from]);
	SAYM("r1cell: dns reply %u bytes from %u.%u.%u.%u, rcode %u, answers %u, first A %u.%u.%u.%u, rtt %u ms", tl, ip[12], ip[13], ip[14], ip[15], fl & 15, an, a[0], a[1], a[2], a[3],
	    ms - d_sendms[from]);
}

/* Stage 3b: the kernel's result line */
static void
result_3b(void)
{
	uint32_t txp = 0, txb = 0, rxp = 0, rxb = 0, dr = 0;
	bool hit = false;

	if (cell_if_exists()) {
		cell_if_stats(&txp, &txb, &rxp, &rxb, &dr, &hit);
	}
	SAYM("r1md: RESULT 3b: if pdp_ip0 %s%s; addr-src %s; tx %u/%u rx %u/%u drops %u; caps %s", if_ok ? "attached" : "failed: ", if_ok ? "" : (if_why[0] ? if_why : (d_fin ? "not reached" : "data phase not reached")),
	    d_have_ip ? d_src : "none", txp, txb, rxp, rxb, dr, hit ? "hit" : "ok");
}

/* the must-line, at the time and again at the end (from result_2e2) */
static void
result_3a(uint32_t rel)
{
	char res[64], apn[24], ip[16];
	uint32_t n = 0;

	(void)rel;
	while (d_apn[n] && d_apn[n] != '.' && n < sizeof(apn) - 1) {
		apn[n] = d_apn[n];
		n++;
	}
	apn[n] = 0;
	snprintf(ip, sizeof(ip), "%u.%u.x.x", d_ip[0], d_ip[1]);
	if (d_res[0]) {
		strlcpy(res, d_res, sizeof(res));
	} else if (d_tries) {
		snprintf(res, sizeof(res), "no reply%s", d_fin ? "" : " (stopped)");
	} else {
		snprintf(res, sizeof(res), "not sent: %s", rp_failed ? rp_why : "data phase not reached");
	}
	if (if_on) {
		result_3b();
	}
	SAYM("r1md: RESULT 3a: addr-src %s; bearer %s ip %s dns %u.%u.%u.%u %u.%u.%u.%u; tx %u pkts %u bytes, rx %u pkts %u bytes; dns %s", d_have_ip ? d_src : "none", apn[0] ? apn : "-", d_have_ip ? ip : "-",
	    d_dns[0][0], d_dns[0][1], d_dns[0][2], d_dns[0][3], d_dns[1][0], d_dns[1][1], d_dns[1][2], d_dns[1][3], cldma_tx_pkts(), cldma_tx_bytes(), d_rxp, d_rxb, res);
}

/* data finished (reply, no reply, deadline): hold 20 s, then the normal off sequence */
static void
data_finish(uint32_t rel, const char *why)
{
	d_fin = true;
	SAYM("r1cell: [%u ms] data phase done (%s): hold %u s", rel, why, if_hold_ms / 1000);
	result_3a(rel);
	held_started = true;
	rp_t = rel;
	rp = RP_HOLD;
}

/* data could not start / was refused: no hold, straight to the off sequence */
static void
data_fail(uint32_t rel, const char *why)
{
	d_fin = true;
	if (!d_tries && !d_res[0]) {
		snprintf(d_res, sizeof(d_res), "not sent: %.40s", why);
	}
	SAYM("r1cell: [%u ms] data stopped (%s): off sequence", rel, why);
	result_3a(rel);
	rp = RP_OFF;
	rp_new = true;
}

/* Stage 3b: the bearer has an address and a DNS server: pdp_ip0 up (address /32, peer = the first DNS server, default route, kern.redwood.cell_dns) */
static bool
if_start(uint32_t rel)
{
	uint8_t peer[4];

	memcpy(peer, d_dns[0], 4);
	if (!memcmp(peer, d_ip, 4)) {
		memcpy(peer, d_ip, 4);
		peer[3] ^= 1;
	}
	if_t0 = rel;
	if_ok = cell_if_attach(d_ip, peer, d_dns[0], d_mtu > 0 ? (uint32_t)d_mtu : 1430, if_pkts, if_kb, if_why, sizeof(if_why));
	if (!if_ok && !if_why[0]) {
		strlcpy(if_why, "unknown", sizeof(if_why));
	}
	SAYM("r1cell: [%u ms] pdp_ip0: %s%s%s", rel, if_ok ? "attached and up" : "FAILED: ", if_ok ? "" : if_why, if_ok ? "" : " (off sequence)");
	return if_ok;
}

/* called from radio_drive in state RP_DATA when no AT command is in flight */
static void
data_drive(uint32_t rel)
{
	if (rel - dp_t0 >= (if_on ? 75000u : 30000u) || (if_on && rd_t_on && rel - rd_t_on >= 150000)) {
		data_finish(rel, if_on ? "75 s deadline" : "30 s deadline");
		return;
	}
	switch (dp) {
	case DP_START: {
		static const char want[] = "reseller";
		bool ok = d_apn[0] != 0;

		for (uint32_t i = 0; ok && i < 8; i++) {
			ok = (d_apn[i] | 0x20) == want[i];
		}
		ok = ok && (d_apn[8] == 0 || d_apn[8] == '.');
		if (!ok) {
			data_fail(rel, d_apn[0] ? "attach APN is not RESELLER" : "no APN from +EIAREG");
			return;
		}
		SAYM("r1cell: [%u ms] bearer binding: AT+EAPNACT for APN RESELLER (EAPNSET/EIAAPN not sent)", rel);
		radio_load(at_seqAPN, 1);
		dp = DP_ACT;
		break;
	}
	case DP_ACT:
		if (!at_last_ok) {
			data_fail(rel, at_to ? "EAPNACT timeout" : "EAPNACT failed");
			return;
		}
		SAYM("r1cell: [%u ms] EAPNACT OK, +CGEV: ME PDN ACT %s; address so far: %s", rel, d_cgev ? "seen" : "NOT seen", d_have_ip ? d_src : "none");
		radio_load(at_seqEPDN, 1);      /* stock order: EPDN ifst, EIF ifst up, CGCONTRDP; then CGPADDR (extra) */
		dp = DP_EPDN;
		break;
	case DP_EPDN:
		SAYM("r1cell: [%u ms] AT+EPDN ifst: %s; address so far: %s", rel, at_last_ok ? "OK" : (at_to ? "timeout" : "error (continuing)"), d_have_ip ? d_src : "none");
		radio_load(at_seqEIF, 1);
		dp = DP_EIF;
		break;
	case DP_EIF:
		SAYM("r1cell: [%u ms] AT+EIF ifst up: %s", rel, at_last_ok ? "OK" : (at_to ? "timeout" : "error (continuing)"));
		radio_load(at_seqRD, 1);
		dp = DP_RD;
		break;
	case DP_RD:
		SAYM("r1cell: [%u ms] AT+CGCONTRDP: %s; address so far: %s, %u DNS", rel, at_last_ok ? "OK" : (at_to ? "timeout" : "error (continuing)"), d_have_ip ? d_src : "none", d_ndns);
		radio_load(at_seqPA, 1);
		dp = DP_PA;
		break;
	case DP_PA:
		SAYM("r1cell: [%u ms] AT+CGPADDR: %s; address so far: %s", rel, at_last_ok ? "OK" : (at_to ? "timeout" : "error (continuing)"), d_have_ip ? d_src : "none");
		if (!d_have_ip && eif_on) {
			radio_load(at_seqEPA, 1);       /* GUESS (r1_md_eif=1): AT+EPDN=0,"addr" */
			dp = DP_EPA;
			break;
		}
		__attribute__((fallthrough));
	case DP_EPA:
		if (dp == DP_EPA) {
			SAYM("r1cell: [%u ms] AT+EPDN addr (GUESS): %s; address so far: %s", rel, at_last_ok ? "OK" : (at_to ? "timeout" : "error"), d_have_ip ? d_src : "none");
		}
		if (!d_have_ip && d_hexok) {
			memcpy(d_ip, d_hex, 4);
			d_have_ip = true;
			d_src = "8-hex-digit token (GUESS)";
			SAYM("r1cell: no dotted IPv4 anywhere: using the 8-hex-digit token %u.%u.x.x (GUESS)", d_ip[0], d_ip[1]);
		}
		if (!d_have_ip) {
			data_fail(rel, d_v6seen ? "bearer is IPv6-only (no IPv4 address in any response; IPv6 not attempted)" : "no IPv4 address from any source");
		} else if (!d_ndns) {
			data_fail(rel, "no DNS server");
		} else if (if_on) {
			if (!if_start(rel)) {
				data_fail(rel, if_why);
			} else {
				dp = DP_IF;
				dp_t = rel;
			}
		} else if (!dns_send(0, rel)) {
			data_fail(rel, "tx refused");
		} else {
			d_cur = 0;
			dp_t = rel;
			dp = DP_WAIT;
		}
		break;
	case DP_IF:
		/* pdp_ip0 is up: only the test tool talks (cell_test); the tick in radio_drive watches the caps and the 10 s counters */
		if (cell_if_done()) {
			data_finish(rel, "cell_test finished");
		}
		break;
	case DP_WAIT:
		if (d_reply) {
			data_finish(rel, "dns answered");
		} else if (rel - dp_t >= 5000) {
			if (d_cur == 0 && d_ndns > 1) {
				if (!dns_send(1, rel)) {
					data_fail(rel, "tx refused (retry)");
					return;
				}
				d_cur = 1;
				dp_t = rel;
			} else {
				data_finish(rel, "no reply in 5 s");
			}
		}
		break;
	}
}

/* called every poll while rd_active */
static void
radio_drive(uint32_t rel)
{
	uint32_t t = rd_t_on ? rel - rd_t_on : 0;

	if (!radio_go) {
		rd_active = false;
		return;
	}
	if (if_on) {
		if (rp >= RP_OFF) {
			cell_if_stop("off sequence");   /* before the off sequence: down, routes and address removed (idempotent) */
		} else if (cell_if_tick(rel) && rp < RP_OFF) {
			cell_if_stop("traffic cap hit");
			radio_stop("traffic cap hit");
		}
	}
	if (rd_t_on) {
		static bool a_reg, a_s3, a_60, snp[3];

		if (eps_reg && !a_reg) {
			a_reg = true;
			result_2fa("reg", rel);
		}
		if (n_stat3 && !a_s3) {
			a_s3 = true;
			cldma_snap("first stat 3", rel, md1_is_on());
			result_2fa("stat3", rel);
		}
		if (t >= 60000 && !a_60) {
			a_60 = true;
			result_2fa("60s", rel);
		}
		for (int i = 0; i < 3; i++) {
			static const uint32_t at_ms[3] = { 1000, 5000, 15000 };

			if (t >= at_ms[i] && !snp[i]) {
				snp[i] = true;
				cldma_snap(i == 0 ? "+1s" : (i == 1 ? "+5s" : "+15s"), rel, md1_is_on());
			}
		}
	}
	if (rp < RP_OFF) {
		if (rd_t_on && t >= 240000) {
			radio_stop("hard cap of 4 minutes");
		} else if (efun1_to) {
			radio_stop("EFUN=1 timeout");
		} else if (st2a.unanswered || rpc_bad || st2a.sys_late || st2a.ctl_odd || (rd_t_on ? (oth_new_ch > (data_on ? 6 : 3) || oth_new > (data_on ? 120 : 50)) : st2a.oth_bad != 0)) {
			radio_stop("unexpected traffic");
		}
	}
	if (rp == RP_OFF) {
		off_drive(rel);
		return;
	}
	if (rp_new) {
		if (at_wait) {
			return;         /* never replace the script under a command in flight */
		}
		rp_new = false;
		if (rp == RP_PRE) {
			radio_load(at_seqA, sizeof(at_seqA) / sizeof(at_seqA[0]));
		} else if (rp == RP_POST) {
			radio_load(at_seqP, sizeof(at_seqP) / sizeof(at_seqP[0]));
		}
		return;
	}
	if (!at_done || at_wait) {
		return;
	}
	if (rp >= RP_POST && rp < RP_OFF && !eps_reg) {
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
		if (eps_reg) {
			rp_reg_ms = eps_reg_at - 1;
			SAYM("r1md: radio: registered (%s) %u ms after the radio-on command", eps_src, rp_reg_ms);
			rp = RP_POST;
			rp_new = true;
		} else if (stat3_pend && diag_rounds < 3) {
			/* a single stat 3 is not final (T3411 retries): read the cause and keep listening, unless the cause is a permanent one */
			stat3_pend = false;
			diag_rounds++;
			SAYM("r1md: radio: EPS stat 3 (#%u): diagnostics round %u (CEER, CEREG?, EREG?, EGREG?, CSCON?)", n_stat3, diag_rounds);
			radio_load(at_seqD, sizeof(at_seqD) / sizeof(at_seqD[0]));
		} else if (rej_perm && diag_rounds) {
			char why[40];

			snprintf(why, sizeof(why), "reject cause #%d", rej_cause);
			radio_stop(why);
		} else if (t >= 90000) {
			if (!final_diag) {
				final_diag = true;
				radio_load(at_seqD, sizeof(at_seqD) / sizeof(at_seqD[0]));
			} else {
				radio_stop("no registration in 90 s");
			}
		} else if (rel - rp_ereg >= 5000) {
			rp_ereg = rel;
			radio_load(at_seqQ, 1);
		} else if ((cscon_n == 0 && t >= 5000) || (cscon_n == 1 && t >= 15000)) {
			cscon_n++;
			radio_load(at_seqC, 1);
		} else if (rel - rp_csq >= 10000) {
			rp_csq = rel;
			radio_load(at_seqS, 1);
		}
		break;
	case RP_POST:
		if (data_on) {
			rp = RP_DATA;
			dp = DP_START;
			dp_t0 = rel;
			md_budget += 6000;
			if (if_on) {
				SAYM("r1cell: [%u ms] registered: Stage 3b data phase begins (pdp_ip0 after the bearer address; deadline 75 s or the tool's end, then hold 5 s)", rel);
			} else {
				SAYM("r1cell: [%u ms] registered: Stage 3a data phase begins (deadline 30 s, then hold 20 s)", rel);
			}
			break;
		}
		rp = RP_HOLD;
		held_started = true;
		rp_t = rel;
		break;
	case RP_DATA:
		data_drive(rel);
		break;
	case RP_HOLD:
		if (rel - rp_t >= (data_on ? if_hold_ms : 60000u)) {
			rp = RP_OFF;
			rp_new = true;
			SAYM("r1md: radio: hold of %u s done: off sequence", data_on ? if_hold_ms / 1000 : 60u);
		} else if (rel - rp_csq >= 10000) {
			rp_csq = rel;
			radio_load(at_seqS, 1);
		}
		break;
	}
}

/* ---- attempt 3: per-bit RCHNUM accounting (docs/cellular-attach-2.md 5a, 5d) ---- */

static uint32_t bit_cnt[32], bit_first[32], bit_last[32], bit_edges;
static struct { bool pend; uint32_t t, bit, n; } busy_chk;

/* rch as read; apbusy was read before the ack was written. Every edge of a bit >= 16 is logged (cap 6) with both CCIFs' RCHNUM/BUSY and 4 SRAM words. */
static void
ccif_bits_note(uint32_t rch, uint32_t apbusy, uint32_t elapsed)
{
	for (int b = 0; b < 32; b++) {
		if (rch & (1u << b)) {
			if (!bit_cnt[b]++) {
				bit_first[b] = elapsed;
			}
			bit_last[b] = elapsed;
		}
	}
	if (!(rch >> 16)) {
		return;
	}
	for (int b = 16; b < 32; b++) {
		if (!(rch & (1u << b)) || bit_edges++ >= 6) {
			continue;
		}
		SAYM("r1md: [%u ms] CCIF bit %d #%u: AP RCHNUM %#x BUSY %#x, MD RCHNUM %#x BUSY %#x; SRAM %08x %08x %08x %08x", elapsed, b, bit_cnt[b], rch, apbusy,
		    rd(mdccif, APCCIF_RCHNUM), rd(mdccif, APCCIF_BUSY), rd(ccif, APCCIF_CHDATA + 0x100), rd(ccif, APCCIF_CHDATA + 0x104), rd(ccif, APCCIF_CHDATA + 0x108),
		    rd(ccif, APCCIF_CHDATA + 0x10c));
		if (!busy_chk.pend) {
			busy_chk.pend = true;
			busy_chk.t = elapsed;
			busy_chk.bit = (uint32_t)b;
			busy_chk.n = bit_cnt[b];
		}
	}
	cldma_snap("ccif bit>=16", elapsed, md1_is_on());
}

/* 10 ms after the ack: did the MD-side BUSY bit clear (proves the ack reaches the modem side) */
static void
ccif_busy_check(uint32_t elapsed)
{
	if (busy_chk.pend && elapsed - busy_chk.t >= 10) {
		uint32_t mb = rd(mdccif, APCCIF_BUSY);

		busy_chk.pend = false;
		SAYM("r1md: [%u ms] CCIF bit %u #%u: MD BUSY %u ms after the ack %#x: bit %s", elapsed, busy_chk.bit, busy_chk.n, elapsed - busy_chk.t, mb,
		    (mb >> busy_chk.bit) & 1 ? "STILL SET" : "cleared");
	}
}

/* RESULT 2f-A: the one line that answers "attached? T3410? bit 20? FS in the window?" (said at registration, first stat 3, +60 s, the end, after power-down) */
static void
result_2fa(const char *tag, uint32_t rel)
{
	char bits[56];
	size_t n = 0;
	int cs = cscon1_ms_v ? (int)cscon1_ms_v - 1 : -1, s3 = stat3_ms_v ? (int)stat3_ms_v - 1 : -1;

	bits[0] = 0;
	for (int b = 16; b < 32 && n + 16 < sizeof(bits); b++) {
		if (bit_cnt[b]) {
			n += (size_t)snprintf(bits + n, sizeof(bits) - n, "%s%d:%u@%u", n ? "," : "", b, bit_cnt[b], bit_first[b]);
		}
	}
	cldma_report(tag, rel);
	SAYM("r1md: RESULT 2f-A [%u ms] %s: cscon1 %d ms, stat3 %d ms (t3410 %d), bits>=16 {%s}, fs* %u (err %u wr %u), rpc* %u, cldma rx %u err %#x, cut %u+%u", rel, tag, cs, s3,
	    (cs >= 0 && s3 >= 0) ? s3 - cs : -1, bits, fsw_ops, fsw_err, fsw_wr, rpc_win, cldma_rx_count(), cldma_rx_err(), md_cut, md_sup + rpc_dups);
}

/* up to 10 s of the boot status and the CCIF; read only except for the interrupt ack md_ccif_isr does */
static bool
wait_hs1(void)
{
	uint32_t sta0 = ~0u, sta1 = ~0u, trace[15], last_trace[15] = { 0 }, seen = 0, rch, s0, s1, elapsed, next_sec = 0;
	uint32_t ntrace = 0, rxrd = 0, rxwr = 0;
	int bsoc, bchg;
	bool hs1 = false, rx_said = false, sent = false, exc = false, failed = false, exc_radio = false;
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
			if (ntrace++ < 6) {
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
		ccif_busy_check(elapsed);
		if (rch) {
			uint32_t apbusy = rd(ccif, APCCIF_BUSY);

			if (rch & ~seen & 0xffff) {
				SAY("r1md: [%u ms] CCIF RCHNUM %#x (new %#x)", elapsed, rch, rch & ~seen & 0xffff);
			}
			seen |= rch;
			/* stock acks every bit: md_ccif_isr (irq0) the low 16, md_cd_ccif_isr (irq1) bits 15..30. The ring bits lose nothing: the rings are
			 * drained every pass whatever RCHNUM says, and a bit raised again after this ack is just the next RCHNUM. Nothing is sent back on bit 20. */
			wr(ccif, APCCIF_ACK, rch);
			ccif_bits_note(rch, apbusy, elapsed);
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
			if ((rch & (1u << CCIF_RX_EXCEPTION_INIT)) && rd_t_on) {
				/* EFUN=1 was sent: an MD exception does NOT end the watch. Dump once, then the off sequence runs (EFUN=0 will probably time out
				 * on a dead AT engine, but the radio-off attempt and the EFUN? confirm are tried); power-down only after it, or at the 255/400 s caps. */
				if (!exc_radio) {
					exc_radio = true;
					SAYM("r1md: [%u ms] D2H_EXCEPTION_INIT with the radio on (%u ms after EFUN=1): off sequence follows", elapsed - t_send, elapsed - t_send - rd_t_on);
					ee_dump(false);
					delay_for_interval(300, NSEC_PER_MSEC);
					ee_dump(true);
					radio_stop("MD exception");
				}
			} else if (rch & (1u << CCIF_RX_EXCEPTION_INIT)) {
				if (md_level >= 3 && sent) {
					SAYM("r1md: [%u ms] D2H_EXCEPTION_INIT after the runtime data", elapsed - t_send);
					ee_dump(false);
					delay_for_interval(300, NSEC_PER_MSEC);
					ee_dump(true);
					exc = true;
				} else {
					SAYM("r1md: [%u ms] the modem raised D2H_EXCEPTION_INIT: it hit an exception", elapsed);
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
				if (cldma_on && cldma_rx_poll(elapsed - t_send)) {
					if (rd_active) {
						radio_stop("cldma stop rule");
					} else {
						cl_fatal = true;
					}
				}
				if (md_level >= 6) {
					at_step(elapsed - t_send);
					if (md_level >= 7) {
						if (at_done && !gates_done && (redwood_battery_get(&bsoc, &bchg) || elapsed - t_send - at_last_ms >= 60000)) {
							gates_eval(sta0, sta1, exc);
							result_2e1(exc);
							if (!radio_req) {
								SAYM("r1md: radio-on NOT requested: stopping at 2e-1");
							} else if (gates_pass != 10) {
								SAYM("r1md: radio-on REFUSED: gates passed %u of 10 (not passed: %s)", gates_pass, gates_bad);
							} else if (!at_radio_on) {
								radio_go = true;
								rd_active = true;
								SAYM("r1md: radio-on requested and all ten gates passed: 2e-2 begins (hard cap 4 min)");
							}
						}
						if (rd_active) {
							radio_drive(elapsed - t_send);
						}
						/* 2e-1: the last response + 3 s, 120 s after HS2. 2e-2: until EFUN=0 is confirmed; the cap below is the kernel's own
						 * timer: 15 s past the 4 minutes, MD1 goes down whatever the state machine is doing */
						if (at_radio_on || cl_fatal || (rd_active && rd_t_on && elapsed - t_send - rd_t_on >= 255000) || (rd_active && md_ms() >= 400000) || (radio_go && rp == RP_END) ||
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
		delay_for_interval(cell_if_active() ? 3 : 10, NSEC_PER_MSEC);   /* pdp_ip0 up: 3 ms, short enough for interactive latency */
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
			SAYM("r1md: %s: MD EXCEPTION %u ms after the reply; last answer op %#x; text \"%s\"; fs %u ops %u missing; jnl %s", rt, rel, last_ans_op,
			    ee_have ? ee_text : "none found", fops, fmiss, mdfs_jnl());
		} else if (st2a.hs2) {
			SAY("r1md: %s: HS2 at %u ms after the reply; fs %u ops %u missing, overlay %u B; jnl %s", rt, st2a.t_hs2, fops, fmiss, fovl, mdfs_jnl());
		} else {
			SAYM("r1md: %s: NO HS2 in %u ms%s; fs %u ops %u missing, overlay %u B; jnl %s", rt, rel,
			    failed ? " (the reply was not sent)" : (hs1 ? "" : " (no HS1 either)"), fops, fmiss, fovl, mdfs_jnl());
			SAY("r1md: last fs exchange: %s", mdfs_last());
		}
		if (md_level >= 7) {
			if (!gates_done) {
				gates_eval(sta0, sta1, exc);
			}
			result_2e1(exc);
			if (radio_go) {
				if (rp != RP_END && !rp_failed) {      /* the loop ended while the radio state machine had not: say why, never "completed" */
					strlcpy(rp_why, at_radio_on ? "radio on by query" : (cl_fatal ? "cldma stop rule" : "watch ended early"), sizeof(rp_why));
					rp_failed = true;
				}
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
				SAYM("r1md: RESULT 2d: AT ready at %u ms after HS2; %s%s", at_ready_ms - st2a.t_hs2, an ? atsum : "no command sent",
				    at_radio_on ? "RADIO ON, STOPPED" : (at_refused ? "REFUSED" : ""));
			} else if (exc) {
				SAYM("r1md: RESULT 2d: MD EXCEPTION, AT never ready; text \"%s\"", ee_have ? ee_text : "none found");
			} else {
				SAYM("r1md: RESULT 2d: AT never ready (%s); no AT command sent", st2a.hs2 ? "HS2 seen" : "NO HS2");
			}
		}
	} else if (md_level >= 3) {
		uint32_t rel = sent ? elapsed - t_send : elapsed;

		const char *tag = md_level >= 4 ? "2b" : "2a";

		if (exc) {
			SAYM("r1md: RESULT %s: MD EXCEPTION %u ms after the reply; last answer op %#x at %u ms; text \"%s\"", tag, rel, last_ans_op, last_ans_ms,
			    ee_have ? ee_text : "none found");
		} else if (st2a.hs2) {
			SAYM("r1md: RESULT %s: HS2 at %u ms after the reply; rx packets %u (fs %u rpc %u ctl %u sys %u other %u), answers %u, unanswered %u", tag,
			    st2a.t_hs2, st2a.pkts, st2a.fs, st2a.rpc, st2a.ctl, st2a.sys, st2a.other, st2a.answered, st2a.unanswered);
		} else {
			SAYM("r1md: RESULT %s: NO HS2 in %u ms%s; rx packets %u (fs %u rpc %u ctl %u sys %u other %u), answers %u, unanswered %u, boot status %#x %#x", tag, rel,
			    failed ? " (the reply was not sent)" : (hs1 ? "" : " (no HS1 either)"), st2a.pkts, st2a.fs, st2a.rpc, st2a.ctl, st2a.sys,
			    st2a.other, st2a.answered, st2a.unanswered, sta0, sta1);
		}
	} else if (hs1) {
		SAYM("r1md: RESULT: HS1 SEEN; boot status %#x %#x, CCIF channels seen %#x, %u ms", sta0, sta1, seen, elapsed);
	} else {
		SAYM("r1md: RESULT: NO HS1 after %u ms; boot status %#x %#x (%s), CCIF channels seen %#x", elapsed, sta0, sta1, boot_sta_desc(sta0), seen);
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
	SAYM("r1md: powering MD1 down (%s)", why);
	if (if_on && !md_quiet) {
		cell_if_stop("MD1 power-down");         /* no-op when the off sequence already did it; never from the loader hand-back path */
	}
	cldma_teardown();       /* stop and reset the CLDMA receive ring first, if one was started (no-op otherwise) */
	if (!md1_is_on()) {
		SAYM("r1md: MD1 is already off");
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
			SAYM("r1md: MD1 still powered after the power-off sequence (PWR_STATUS %#x/%#x)", rd(spm, SPM_PWR_STATUS), rd(spm, SPM_PWR_STATUS_2ND));
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
			SAYM("r1md: RESULT %s: NO MODEM START: the NV partitions could not be mounted read only (nothing was powered)", md_level >= 6 ? "2d" : "2c");
			md_state = 2;
			goto out;
		}
	}
	if (md1_is_on()) {
		SAYM("r1md: MD1 is already powered; something else owns the modem: not touching it");
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
	cldma_maps(infra);
	if (!smem_prepare()) {
		md_power_off("shared memory");
		goto out;
	}
	ccif_reset();
	if (data_on) {
		cldma_data_enable();
		if (if_on) {
			cldma_if_enable(if_pkts, if_kb * 1000);
		}
		SAYM("r1md: Stage 3a armed (r1_md_data=1%s): TX ring, 0x400d answer, bearer binding after registration", eif_on ? ", r1_md_eif=1" : "");
		if (if_on) {
			SAYM("r1md: Stage 3b armed (r1_md_if=1%s): pdp_ip0 after the bearer address, caps %u pkts %u KB per direction, data phase <= 75 s then hold 5 s", md_level == 7 && eif_on ? " / r1_md=9 shorthand" : "", if_pkts, if_kb);
		}
	}
	if (cldma_on && !cldma_rx_start(md_ms())) {
		md_power_off("cldma rx not started");
		goto out;
	}
	if (!let_md_go()) {
		md_power_off("release");
		goto out;
	}
	(void)wait_hs1();
	dump_status("after wait");
	md_power_off("end of the HS1 watch");
	if (radio_go) {         /* the last thing the driver says: the results again, after the modem is down */
		result2_said = false;
		res_tag = "off";
		result_2e2(res_rel);
	}
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

	bool sh9 = false;

	if (!PE_parse_boot_argn("r1_md", &arg, sizeof(arg)) || (arg < 1 || (arg > 7 && arg != 9))) {
		return;
	}
	if (arg == 9) {         /* shorthand (the kernel command line is length-limited): 7 + r1_md_cldma=1 + r1_md_data=1 + r1_md_if=1 + r1_md_eif=1 */
		sh9 = true;
		arg = 7;
	}
	md_level = arg;
	if (arg == 7) {
		uint32_t v = 0;

		at_seq = at_seq7;
		at_n = sizeof(at_seq7) / sizeof(at_seq7[0]);
		radio_req = PE_parse_boot_argn("r1_md_radio", &v, sizeof(v)) && v == 1;
		/* the CLDMA receive ring is started ONLY with r1_md_cldma=1; without it not one CLDMA register is written (snapshots are reads) */
		cldma_on = sh9 || (PE_parse_boot_argn("r1_md_cldma", &v, sizeof(v)) && v == 1);
		/* Stage 3a: r1_md_data=1 only counts together with r1_md_radio=1 and r1_md_cldma=1 (the ten gates and registration are checked at run time) */
		data_on = radio_req && cldma_on && (sh9 || (PE_parse_boot_argn("r1_md_data", &v, sizeof(v)) && v == 1));
		eif_on = data_on && (sh9 || (PE_parse_boot_argn("r1_md_eif", &v, sizeof(v)) && v == 1));
		if_on = data_on && (sh9 || (PE_parse_boot_argn("r1_md_if", &v, sizeof(v)) && v == 1));
		if (if_on) {
			if (PE_parse_boot_argn("r1_md_if_pkts", &v, sizeof(v)) && v >= 10 && v <= 2000) {
				if_pkts = v;
			}
			if (PE_parse_boot_argn("r1_md_if_kb", &v, sizeof(v)) && v >= 4 && v <= 2000) {
				if_kb = v;
			}
			if_hold_ms = 5000;
		}
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
		SAYM("r1md: stage 1 REFUSED: the loader's handoff (r1_md_smem= ... r1_md_img_sz=) is not in the boot-args");
		return;
	}
	(void)PE_parse_boot_argn("r1_md_dfd", &h_dfd, sizeof(h_dfd));
	if ((cg1 & ((1u << 23) | (1u << 26))) || !ccif) {
		SAYM("r1md: stage 1 REFUSED: the CCIF clocks are gated (CG1 %#x), the reference expects them ungated", cg1);
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
