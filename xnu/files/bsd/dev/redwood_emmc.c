/*
 * The X6826B's eMMC as a block device: the "xnusys" partition, where the system image lives, read
 * when the kernel asks for it rather than held in memory. Boot-arg emmc_root=1 makes it the root
 * (iokit/bsddev/IOKitBSDInit.cpp asks redwood_emmc_root for it); /dev/emmc0 and /dev/remmc0 are its
 * nodes once devfs is up.
 *
 * Until now the loader read the whole image -- 2.4 GB of it -- into DRAM as the RAM disk md0,
 * in the 4 GB the kernel has: 2.5 GB wired before the first program ran, the rest too little for
 * iOS with its wallpaper (run 320: 10-26 MB free, the system "unhealthy", jetsam at work), and
 * three and a half minutes of every boot spent reading it (11 MB/s).
 *
 * Nothing is set up here: LK has powered the card, set its bus up and left it in the transfer
 * state, and mtkloader has read the GPT through it the same way (mtkloader/msdc.h) -- MSDC0 in PIO
 * mode, plain block reads (CMD17, or CMD18 then CMD12). Registers as Linux's
 * drivers/mmc/host/mtk-sd.c has them.
 *
 * Read only, by construction: the strategy routine refuses writes and opening for writing is
 * refused. One request at a time, under a mutex, in pieces of EMMC_MAX_BLOCKS.
 *
 * The one exception (r1): /dev/remmc_var, minor 1, a character device over the GPT partition xnudata,
 * written by userland/src/var_store.c. Off unless
 * emmc_write=1 (emmc_write=2: one test sector at boot, nothing else); see "the var window" below.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/crc.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/redwood_emmc.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void redwood_host_keep_add(const char *format, ...) __printflike(1, 2);

#define MSDC0_PHYS       0x11230000u
#define EMMC_PART        "xnusys"
#define EMMC_MAX_BLOCKS  256                /* 128 KB a command */

#define MSDC_CFG         0x00
#define MSDC_CFG_RST     (1u << 2)
#define MSDC_CFG_PIO     (1u << 3)
#define MSDC_INT         0x0c
#define MSDC_FIFOCS      0x14
#define MSDC_FIFOCS_CLR  (1u << 31)
#define MSDC_TXDATA      0x18
#define MSDC_RXDATA      0x1c
#define SDC_CMD          0x34
#define SDC_ARG          0x38
#define SDC_STS          0x3c
#define SDC_STS_BUSY     3u                 /* SDCBUSY, CMDBUSY */
#define SDC_RESP0        0x40
#define SDC_BLK_NUM      0x50

#define INT_CMDRDY       (1u << 8)
#define INT_CMDTMO       (1u << 9)
#define INT_RSPCRCERR    (1u << 10)
#define INT_XFER_COMPL   (1u << 12)
#define INT_DATTMO       (1u << 14)
#define INT_DATCRCERR    (1u << 15)

#define CMD_RSP_R1       (1u << 7)
#define CMD_RSP_R1B      (6u << 7)
#define CMD_DTYPE_SINGLE (1u << 11)
#define CMD_DTYPE_MULTI  (2u << 11)
#define CMD_WRITE        (1u << 13)         /* data from the host (mtk-sd.c: rawcmd |= 1 << 13) */
#define CMD_STOP         (1u << 14)
/* R1 card status: out of range, address, block length, WP violation, illegal, ECC, CC, error */
#define R1_ERRORS        0xe4780000u
#define CMD_BLKLEN(n)    (((n) & 0xfffu) << 16)

static vm_offset_t msdc;
static uint64_t emmc_lba, emmc_blocks;      /* the partition: its first block, and the image's length in blocks */
static uint64_t emmc_part_blocks;           /* the partition's whole length, before emmc_blocks is trimmed */
static int emmc_bmajor = -1, emmc_cmajor = -1;
static uint32_t emmc_int_seen;              /* MSDC_INT bits read (reading clears them) but not yet used */
static const char *emmc_failed;
static uint32_t emmc_failed_status;
static uint64_t emmc_reads, emmc_read_blocks, emmc_errors;
static LCK_GRP_DECLARE(emmc_lck_grp, "redwood-emmc");
static LCK_MTX_DECLARE(emmc_lock, &emmc_lck_grp);

/*
 * The NV partitions (the modem's nvdata, nvcfg, protect1, protect2 and the raw nvram, proinfo): never written, by any path.
 * Their ranges are taken from the GPT (nv_collect, whenever the GPT is read here) and write_blocks, the one place that writes the
 * card, refuses any block inside them and says so loudly. (The var window is also checked against every other GPT entry.)
 */
static const char *const nv_names[] = { "nvdata", "nvcfg", "protect1", "protect2", "nvram", "proinfo" };
#define NV_N (sizeof(nv_names) / sizeof(nv_names[0]))
static uint64_t nv_lo[NV_N], nv_hi[NV_N];   /* first block, one past the last; 0/0 = not found */
static uint64_t nv_refused;

static inline uint32_t
rd(uint32_t reg)
{
	return *(volatile uint32_t *)(msdc + reg);
}

static inline void
wr(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(msdc + reg) = value;
}

static bool
fail(const char *what, uint32_t status)
{
	emmc_failed = what;
	emmc_failed_status = status;
	return false;
}

/* (spins are counted in register reads, as the loader does: about a microsecond each, give or take) */
static bool
wait_idle(void)
{
	for (uint32_t i = 0; i < 2000000; i++) {
		if (!(rd(SDC_STS) & SDC_STS_BUSY)) {
			return true;
		}
	}
	return fail("controller busy", rd(SDC_STS));
}

static bool
wait_int(uint32_t want, uint32_t bad, const char *what)
{
	for (uint32_t i = 0; i < 20000000; i++) {
		uint32_t status = rd(MSDC_INT);

		if (status) {
			wr(MSDC_INT, status);
			emmc_int_seen |= status;
		}
		if (emmc_int_seen & bad) {
			status = emmc_int_seen;
			emmc_int_seen = 0;
			return fail(what, status);
		}
		if (emmc_int_seen & want) {
			emmc_int_seen &= ~want;
			return true;
		}
	}
	return fail(what, emmc_int_seen);
}

static void
pio_mode(void)
{
	wr(MSDC_CFG, rd(MSDC_CFG) | MSDC_CFG_PIO);
	wr(MSDC_INT, rd(MSDC_INT));
	emmc_int_seen = 0;
	wr(MSDC_FIFOCS, rd(MSDC_FIFOCS) | MSDC_FIFOCS_CLR);
	for (uint32_t i = 0; i < 100000 && (rd(MSDC_FIFOCS) & MSDC_FIFOCS_CLR); i++) {
	}
}

static bool
stop(void)
{
	wr(SDC_ARG, 0);
	wr(SDC_CMD, 12 | CMD_RSP_R1B | CMD_STOP);
	return wait_int(INT_CMDRDY, INT_CMDTMO | INT_RSPCRCERR, "stop") && wait_idle();
}

/* blocks of 512 bytes from the card's block lba into dst (4-aligned); op 8 (SEND_EXT_CSD) for the EXT_CSD */
static bool
read_op(uint32_t op, uint64_t lba, uint32_t blocks, uint32_t *dst)
{
	if (!wait_idle()) {
		return false;
	}
	pio_mode();
	wr(SDC_BLK_NUM, blocks);
	wr(SDC_ARG, (uint32_t)lba);                 /* high capacity: the unit is a block */
	wr(SDC_CMD, op | CMD_RSP_R1 | (blocks == 1 ? CMD_DTYPE_SINGLE : CMD_DTYPE_MULTI) | CMD_BLKLEN(512));
	if (!wait_int(INT_CMDRDY, INT_CMDTMO | INT_RSPCRCERR, "command")) {
		return false;
	}
	for (uint32_t left = blocks * 512 / 4; left;) {
		uint32_t fifo = rd(MSDC_FIFOCS) & 0xff, spin = 0, words;

		while (fifo < 4) {
			uint32_t status = rd(MSDC_INT);

			if (status & (INT_DATTMO | INT_DATCRCERR)) {
				wr(MSDC_INT, status);
				if (blocks > 1) {
					(void)stop();
				}
				return fail("data", status);
			}
			if (++spin > 20000000) {
				return fail("data stalled", rd(MSDC_FIFOCS));
			}
			fifo = rd(MSDC_FIFOCS) & 0xff;
		}
		words = fifo / 4 < left ? fifo / 4 : left;
		for (uint32_t i = 0; i < words; i++) {
			*dst++ = rd(MSDC_RXDATA);
		}
		left -= words;
	}
	if (!wait_int(INT_XFER_COMPL, INT_DATTMO | INT_DATCRCERR, "transfer end")) {
		return false;
	}
	return blocks == 1 || stop();
}

static bool
read_blocks(uint64_t lba, uint32_t blocks, uint32_t *dst)
{
	return read_op(blocks == 1 ? 17 : 18, lba, blocks, dst);
}

/* the same, tried again once after a failure (the controller cleared and the card told to stop) */
static bool
read_retrying(uint64_t lba, uint32_t blocks, uint32_t *dst)
{
	if (read_blocks(lba, blocks, dst)) {
		return true;
	}
	emmc_errors++;
	printf("emmc: reading %u blocks at %llu: %s (%#x); trying again\n", blocks, lba, emmc_failed, emmc_failed_status);
	pio_mode();
	(void)stop();
	return read_blocks(lba, blocks, dst);
}

/* ---- the partition ---- */

static uint32_t sector[128] __attribute__((aligned(16)));
static uint32_t entries[128 * 128 / 4] __attribute__((aligned(16)));

/* a GPT entry's name (UTF-16LE at 56, 36 characters) is this ASCII name */
static bool
gpt_named(const uint8_t *e, const char *name)
{
	size_t k;

	for (k = 0; k < 36 && name[k] && e[56 + 2 * k] == (uint8_t)name[k] && e[57 + 2 * k] == 0; k++) {
	}
	return name[k] == 0 && (k == 36 || (e[56 + 2 * k] == 0 && e[57 + 2 * k] == 0));
}

static bool
find_partition(void)
{
	const uint8_t *s = (const uint8_t *)sector;
	uint64_t entries_lba, at;
	uint32_t count, size;

	/* r1: the system image may sit in a logical partition of "super", which the GPT does not name;
	 * then the loader passes its one extent as emmc_lba= / emmc_blocks= (blocks of 512 bytes). */
	uint64_t arg_lba = 0, arg_blocks = 0;
	if (PE_parse_boot_argn("emmc_lba", &arg_lba, sizeof(arg_lba)) &&
	    PE_parse_boot_argn("emmc_blocks", &arg_blocks, sizeof(arg_blocks)) && arg_lba && arg_blocks) {
		emmc_lba = arg_lba;
		emmc_blocks = arg_blocks;
		goto measure;
	}
	if (!read_retrying(1, 1, sector) || memcmp(s, "EFI PART", 8) != 0) {
		printf("emmc: no GPT (%s)\n", emmc_failed ? emmc_failed : "no signature");
		return false;
	}
	memcpy(&entries_lba, s + 72, 8);
	memcpy(&count, s + 80, 4);
	memcpy(&size, s + 84, 4);
	if (count > 128 || size != 128 || !read_retrying(entries_lba, count * size / 512, entries)) {
		printf("emmc: the GPT's entries: %u of %u bytes\n", count, size);
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = (const uint8_t *)entries + i * size;
		uint64_t first, last;

		if (!gpt_named(e, EMMC_PART)) {
			continue;
		}
		memcpy(&first, e + 32, 8);
		memcpy(&last, e + 40, 8);
		if (first == 0 || last < first) {
			return false;
		}
		emmc_lba = first;
		emmc_blocks = last - first + 1;
		break;
	}
	if (emmc_lba == 0) {
		printf("emmc: no " EMMC_PART " partition\n");
		return false;
	}
measure:
	emmc_part_blocks = emmc_blocks;
	/* as long as its APFS containers, back to back, as the loader measured it for md0 */
	for (at = 0; at < emmc_blocks * 512;) {
		uint32_t bs;
		uint64_t n;

		if (!read_retrying(emmc_lba + at / 512, 1, sector) || sector[8] != 0x4253584eu) {   /* "NXSB" */
			break;
		}
		bs = sector[9];
		memcpy(&n, s + 40, 8);
		if (bs < 512 || bs > 65536 || n == 0 || n * bs > emmc_blocks * 512 - at) {
			break;
		}
		at += n * bs;
	}
	if (at == 0) {
		printf("emmc: no APFS container in " EMMC_PART " (tools/system-flash.sh writes it)\n");
		return false;
	}
	printf("emmc: " EMMC_PART " at block %llu, %llu MB of %llu MB in its APFS containers\n", emmc_lba, at >> 20,
	    (emmc_blocks * 512) >> 20);
	emmc_blocks = at / 512;
	return true;
}

/*
 * ---- the var window (r1): the one place this driver writes ----
 *
 * The window is the GPT partition xnudata, found in the primary GPT, and emmc_write=1 turns it on.
 * Read and checked once, at boot, and frozen; nothing changes it after. Refused (never trimmed) unless:
 *   - the GPT's header and entries CRCs are right, exactly one entry is named xnudata, it lies inside
 *     the usable blocks, no other entry overlaps it (super among them), nor the xnusys extent
 *     (emmc_lba=, emmc_blocks=); var_lba= / var_blocks=, if given, must be the entry exactly (var_find);
 *   - its block 0 is a header naming this very window (struct var_header, tools/r1-varhdr.py);
 *   - the card answers CMD13 in the transfer state, and its EXT_CSD says user area, no cache,
 *     no power-on or permanent write protection of the user area, and a card at least this long.
 * Block 0 itself is never written: writes go to [var_w0, var_w1) = [var_lba + 1, var_lba + var_blocks),
 * checked in write_blocks, the only place a write command is issued. Any failure: var_w1 = 0, for good.
 */
#define VAR_PART         "xnudata"
#define VAR_MAGIC        "XNUVAR01"
#define VAR_MINOR        1

struct var_header {                         /* little-endian, at byte 0 of the window's block 0; the rest of it zero */
	char     magic[8];
	uint64_t lba;                       /* the window's first sector: this block */
	uint64_t blocks;                    /* the window's length in sectors, this block included */
	uint32_t crc;                       /* CRC-32 (zlib's) of the 24 bytes above */
};

static uint64_t var_lba, var_blocks, var_w0, var_w1;
static bool var_on;                         /* /dev/remmc_var takes writes */
static uint32_t var_verify = 1, emmc_rca = 1;
static uint32_t *var_check;                 /* EMMC_MAX_BLOCKS of read-back */
static uint64_t var_writes, var_sectors, var_verify_failed, var_refused;
static const char *var_why = "not looked for";
extern int ufs_var_busy_pid;                /* kern.redwood.var_busy (redwood_ufs.c): the one claim, for both phones */

/* CMD13: the card's R1 status */
static bool
card_status(uint32_t *r1)
{
	if (!wait_idle()) {
		return false;
	}
	wr(MSDC_INT, rd(MSDC_INT));
	emmc_int_seen = 0;
	wr(SDC_ARG, emmc_rca << 16);
	wr(SDC_CMD, 13 | CMD_RSP_R1);
	if (!wait_int(INT_CMDRDY, INT_CMDTMO | INT_RSPCRCERR, "status (CMD13; emmc_rca=?)")) {
		return false;
	}
	*r1 = rd(SDC_RESP0);
	return true;
}

/* CMD13 until the card is back in the transfer state, ready for data: at most 10 s */
static bool
wait_tran(void)
{
	uint64_t end;
	uint32_t r1 = 0;

	nanoseconds_to_absolutetime(10ull * NSEC_PER_SEC, &end);
	end += mach_absolute_time();
	do {
		if (!card_status(&r1)) {
			return false;
		}
		if (r1 & R1_ERRORS) {
			return fail("card status", r1);
		}
		if (((r1 >> 9) & 15) == 4 && (r1 & (1u << 8))) {
			return true;
		}
		delay(10);
	} while (mach_absolute_time() < end);
	return fail("card stays busy", r1);
}

/*
 * After a failed write: the controller reset as mtk-sd.c's msdc_reset_hw does, FIFO and interrupts
 * cleared, CMD12 (the card may be in rcv or prg; in tran it is an illegal command, whose flag the
 * one CMD13 after it reads away), then the transfer state -- so the root's reads go on working.
 */
static void
recover(void)
{
	uint32_t r1;

	wr(MSDC_CFG, rd(MSDC_CFG) | MSDC_CFG_RST);
	for (uint32_t i = 0; i < 100000 && (rd(MSDC_CFG) & MSDC_CFG_RST); i++) {
	}
	pio_mode();
	(void)stop();
	pio_mode();
	(void)card_status(&r1);
	if (!wait_tran()) {
		printf("emmc: var: the card did not come back after a failed write: %s (%#x)\n", emmc_failed,
		    emmc_failed_status);
	}
}

/* n blocks from src (4-aligned) to the card's block lba: 0, EROFS outside the window, EIO */
static int
write_blocks(uint64_t lba, uint32_t n, const uint32_t *src)
{
	for (unsigned k = 0; k < NV_N; k++) {
		if (nv_hi[k] && lba < nv_hi[k] && lba + n > nv_lo[k]) {
			nv_refused++;
			printf("emmc: REFUSED write to NV partition %s (blocks %llu+%u)\n", nv_names[k], lba, n);
			redwood_host_keep_add("emmc: REFUSED write to NV partition %s (blocks %llu+%u)", nv_names[k], lba, n);
			return EROFS;
		}
	}
	if (!(lba >= var_w0 && lba < var_w1 && n >= 1 && n <= var_w1 - lba) || n > EMMC_MAX_BLOCKS) {
		var_refused++;
		return EROFS;                       /* (var_w1 == 0 until the window is frozen: nothing passes) */
	}
	if (!wait_idle()) {
		goto abort;
	}
	pio_mode();
	wr(SDC_BLK_NUM, n);
	wr(SDC_ARG, (uint32_t)lba);
	wr(SDC_CMD, (n == 1 ? 24 : 25) | CMD_RSP_R1 | (n == 1 ? CMD_DTYPE_SINGLE : CMD_DTYPE_MULTI) | CMD_WRITE |
	    CMD_BLKLEN(512));
	if (!wait_int(INT_CMDRDY, INT_CMDTMO | INT_RSPCRCERR, "write command")) {
		goto abort;
	}
	if (rd(SDC_RESP0) & R1_ERRORS) {
		fail("write refused by the card", rd(SDC_RESP0));
		goto abort;
	}
	/* PIO out, as MediaTek's own msdc_pio_write: a FIFO's worth (128 bytes) each time it is empty */
	for (uint32_t left = n * 512 / 4; left;) {
		uint32_t spin = 0, words = left < 32 ? left : 32;

		while ((rd(MSDC_FIFOCS) >> 16) & 0xff) {
			uint32_t status = rd(MSDC_INT);

			if (status & (INT_DATTMO | INT_DATCRCERR)) {
				wr(MSDC_INT, status);
				fail("write data", status);
				goto abort;
			}
			if (++spin > 20000000) {
				fail("write data stalled", rd(MSDC_FIFOCS));
				goto abort;
			}
		}
		for (uint32_t i = 0; i < words; i++) {
			wr(MSDC_TXDATA, *src++);
		}
		left -= words;
	}
	if (!wait_int(INT_XFER_COMPL, INT_DATTMO | INT_DATCRCERR, "write end")) {
		goto abort;
	}
	if (n > 1 && (!stop() || (rd(SDC_RESP0) & R1_ERRORS))) {
		fail("stop after the write", rd(SDC_RESP0));     /* errors of the write itself, read back or not */
		goto abort;
	}
	if (!wait_tran()) {
		goto abort;
	}
	return 0;
abort:
	{
		const char *what = emmc_failed;
		uint32_t status = emmc_failed_status;

		recover();
		emmc_failed = what;                 /* the write's own failure, not the recovery's */
		emmc_failed_status = status;
	}
	return EIO;
}

/* a write of the window, read back; the first failure turns writing off for the rest of the boot */
static int
var_write(uint64_t lba, uint32_t n, const uint32_t *src)
{
	int error;

	lck_mtx_lock(&emmc_lock);
	if (!var_on) {
		var_refused++;
		lck_mtx_unlock(&emmc_lock);
		return EROFS;
	}
	error = write_blocks(lba, n, src);
	if (error == 0 && var_verify && (!read_retrying(lba, n, var_check) || memcmp(var_check, src, n * 512) != 0)) {
		var_verify_failed++;
		fail("read back differs", 0);
		error = EIO;
	}
	if (error == 0) {
		var_writes++;
		var_sectors += n;
	} else {
		var_on = false;
		var_w1 = 0;
	}
	lck_mtx_unlock(&emmc_lock);
	if (error != 0) {
		printf("emmc: var: writing %u blocks at %llu: %d, %s (%#x); writing is off from now on\n", n, lba, error,
		    emmc_failed, emmc_failed_status);
	}
	return error;
}

/* emmc_write=2: one pattern sector into the window's first writable block (next to the header that
 * proves the window ours), read back; nothing else */
static void
var_test(void)
{
	uint64_t lba = var_w0;
	int error;

	for (uint32_t i = 0; i < 128; i++) {
		sector[i] = 0x52315654u ^ (uint32_t)(lba * 2654435761u) ^ (i * 0x01010101u);
	}
	lck_mtx_lock(&emmc_lock);
	error = write_blocks(lba, 1, sector);
	if (error == 0 && (!read_retrying(lba, 1, var_check) || memcmp(var_check, sector, 512) != 0)) {
		var_verify_failed++;
		error = -1;
	}
	if (error == 0) {
		var_writes++;
		var_sectors++;
	}
	lck_mtx_unlock(&emmc_lock);
	printf("emmc: var test: block %llu: %s (%d; %s %#x); writes %llu, verify failures %llu, refused %llu\n", lba,
	    error == 0 ? "PASS" : "FAIL", error, emmc_failed ? emmc_failed : "-", emmc_failed_status, var_writes,
	    var_verify_failed, var_refused);
}

/* emmc_write=1 or =2 written out; a bare emmc_write (which PE_parse_boot_argn takes for 1) is not enough */
static uint32_t
write_mode(void)
{
	for (const char *p = PE_boot_args(); p != NULL && *p;) {
		while (*p == ' ') {
			p++;
		}
		if (strncmp(p, "emmc_write", 10) == 0) {
			return p[10] == '=' && (p[11] == '1' || p[11] == '2') && (p[12] == 0 || p[12] == ' ') ? (uint32_t)(p[11] - '0') : 0;
		}
		while (*p && *p != ' ') {
			p++;
		}
	}
	return 0;
}

/* the primary GPT into sector and entries, its CRCs checked: NULL, or what is wrong */
static const char *
gpt_read(uint32_t *count, uint64_t *usable_first, uint64_t *usable_last)
{
	uint8_t *s = (uint8_t *)sector;
	uint64_t elba;
	uint32_t hsize, hcrc, size, ecrc;

	if (!read_retrying(1, 1, sector) || memcmp(s, "EFI PART", 8) != 0) {
		return "no GPT at block 1";
	}
	memcpy(&hsize, s + 12, 4);
	memcpy(&hcrc, s + 16, 4);
	if (hsize < 92 || hsize > 512) {
		return "the GPT header's size is wrong";
	}
	memset(s + 16, 0, 4);
	if (crc32(0, s, hsize) != hcrc) {
		return "the GPT header's CRC is wrong";
	}
	memcpy(usable_first, s + 40, 8);
	memcpy(usable_last, s + 48, 8);
	memcpy(&elba, s + 72, 8);
	memcpy(count, s + 80, 4);
	memcpy(&size, s + 84, 4);
	memcpy(&ecrc, s + 88, 4);
	if (*count == 0 || *count > 128 || size != 128 || !read_retrying(elba, *count * size / 512, entries)) {
		return "the GPT's entries cannot be read (at most 128 of 128 bytes)";
	}
	if (crc32(0, entries, *count * size) != ecrc) {
		return "the GPT entries' CRC is wrong";
	}
	return NULL;
}

/* the six NV partitions' ranges out of the GPT entries just read (count of them in entries) */
static void
nv_collect(uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = (const uint8_t *)entries + i * 128;

		for (unsigned k = 0; k < NV_N; k++) {
			if (gpt_named(e, nv_names[k])) {
				uint64_t first, last;

				memcpy(&first, e + 32, 8);
				memcpy(&last, e + 40, 8);
				if (first && last >= first) {
					nv_lo[k] = first;
					nv_hi[k] = last + 1;
				}
			}
		}
	}
}

/* r1_md=5: an NV partition's first block and length (512-byte blocks) from the GPT, read only; also (re)learns every NV range */
bool
redwood_emmc_nv_find(const char *name, uint64_t *lba, uint64_t *blocks)
{
	uint64_t uf, ul;
	uint32_t count;
	const char *why;
	bool ok = false;

	lck_mtx_lock(&emmc_lock);
	if (msdc == 0) {
		msdc = ml_io_map(MSDC0_PHYS, 0x1000);
	}
	why = gpt_read(&count, &uf, &ul);
	if (why != NULL) {
		printf("emmc: nv: %s\n", why);
	} else {
		nv_collect(count);
		for (unsigned k = 0; k < NV_N; k++) {
			if (!strcmp(nv_names[k], name) && nv_hi[k]) {
				*lba = nv_lo[k];
				*blocks = nv_hi[k] - nv_lo[k];
				ok = true;
			}
		}
	}
	lck_mtx_unlock(&emmc_lock);
	return ok;
}

/* r1_md=5: blocks of an NV partition IN, into dst (4-aligned), at most EMMC_MAX_BLOCKS; refused outside the NV partitions */
bool
redwood_emmc_nv_read(uint64_t lba, uint32_t blocks, void *dst)
{
	bool in = false, ok;

	for (unsigned k = 0; k < NV_N; k++) {
		in |= nv_hi[k] && lba >= nv_lo[k] && lba + blocks <= nv_hi[k];
	}
	if (!in || blocks == 0 || blocks > EMMC_MAX_BLOCKS || ((uintptr_t)dst & 3)) {
		return false;
	}
	lck_mtx_lock(&emmc_lock);
	ok = read_retrying(lba, blocks, (uint32_t *)dst);
	lck_mtx_unlock(&emmc_lock);
	return ok;
}

/* r1_md=7 gate 4: how many writes to an NV partition write_blocks has refused (must stay 0) */
uint64_t
redwood_emmc_nv_refused(void)
{
	return nv_refused;
}

/* xnudata out of the GPT into lba, blocks (which, if given by boot-args, must be it exactly) */
static const char *
var_find(uint64_t *lba, uint64_t *blocks)
{
	uint64_t ufirst, ulast, first = 0, last = 0;
	uint32_t count, at = 0, found = 0;
	const char *why = gpt_read(&count, &ufirst, &ulast);
	static const uint8_t none[16];

	if (why != NULL) {
		return why;
	}
	nv_collect(count);
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = (const uint8_t *)entries + i * 128;

		if (memcmp(e, none, 16) != 0 && gpt_named(e, VAR_PART)) {
			memcpy(&first, e + 32, 8);
			memcpy(&last, e + 40, 8);
			at = i;
			found++;
		}
	}
	if (found != 1) {
		return "not exactly one " VAR_PART " in the GPT";
	}
	if (first < ufirst || last > ulast || last < first + 4095 || last >= (1ull << 32)) {
		return VAR_PART " is not inside the GPT's usable blocks, or under 2 MB, or past 2^32 blocks";
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = (const uint8_t *)entries + i * 128;
		uint64_t f, l;

		memcpy(&f, e + 32, 8);
		memcpy(&l, e + 40, 8);
		if (i != at && memcmp(e, none, 16) != 0 && f <= last && first <= l) {
			return "another GPT entry overlaps " VAR_PART;
		}
	}
	if ((*lba != 0 || *blocks != 0) && (*lba != first || *blocks != last - first + 1)) {
		return "var_lba= / var_blocks= are not the GPT's " VAR_PART;
	}
	*lba = first;
	*blocks = last - first + 1;
	return NULL;
}

static void
var_setup(void)
{
	const struct var_header *h = (const struct var_header *)(void *)sector;
	const uint8_t *ext = (const uint8_t *)(void *)sector;
	uint64_t lba = 0, blocks = 0;
	uint32_t mode = write_mode(), sectors;
	const char *gpt;
	PE_parse_boot_argn("var_lba", &lba, sizeof(lba));
	PE_parse_boot_argn("var_blocks", &blocks, sizeof(blocks));
	PE_parse_boot_argn("var_verify", &var_verify, sizeof(var_verify));
	PE_parse_boot_argn("emmc_rca", &emmc_rca, sizeof(emmc_rca));
	if (mode != 1 && mode != 2) {
		var_why = "emmc_write= is not 1 or 2";
	} else if ((gpt = var_find(&lba, &blocks)) != NULL) {
		var_why = gpt;
	} else if (lba < emmc_lba + emmc_part_blocks && emmc_lba < lba + blocks) {
		var_why = "the window overlaps xnusys";
	} else if ((var_check = kalloc_data(EMMC_MAX_BLOCKS * 512, Z_WAITOK)) == NULL) {
		var_why = "no memory";
	} else if (!read_retrying(lba, 1, sector)) {
		var_why = "its header block cannot be read";
	} else if (memcmp(h->magic, VAR_MAGIC, 8) != 0 || h->lba != lba || h->blocks != blocks ||
	    h->crc != crc32(0, h, 24)) {
		var_why = "its header block does not name this window (tools/r1-varhdr.py writes it)";
	} else if (!wait_tran()) {
		var_why = "the card does not answer CMD13 in the transfer state (emmc_rca=)";
	} else if (!read_op(8, 0, 1, sector)) {
		var_why = "no EXT_CSD";
	} else if ((ext[179] & 7) != 0 || (ext[33] & 1) != 0 || (ext[171] & 5) != 0) {
		var_why = "EXT_CSD: not the user area, or cache on, or the user area write-protected";
	} else if ((memcpy(&sectors, ext + 212, 4), sectors) < lba + blocks) {
		var_why = "EXT_CSD: the card is shorter than the window's end";
	} else {
		var_why = NULL;
	}
	if (var_why == NULL) {
		printf("emmc: var: the GPT's " VAR_PART " is this window; EXT_CSD partition_config %#x cache_ctrl %#x "
		    "user_wp %#x wr_rel_param %#x wr_rel_set %#x sec_count %u\n", ext[179], ext[33], ext[171], ext[166],
		    ext[167], sectors);
		var_lba = lba;
		var_blocks = blocks;
		var_w0 = lba + 1;
		var_w1 = lba + blocks;
		if (mode == 2) {
			var_test();
			var_w1 = 0;                 /* the test was all: nothing else writes */
			var_why = "test mode (emmc_write=2): one sector, nothing else";
		} else {
			var_on = true;
		}
	}
	if (var_on) {
		printf("emmc: var: window frozen: blocks %llu..%llu (%llu MB), header at %llu, writes %llu..%llu%s\n",
		    var_lba, var_lba + var_blocks - 1, (var_blocks * 512) >> 20, var_lba, var_w0, var_w1 - 1,
		    var_verify ? ", each read back" : ", NOT read back (var_verify=0)");
	} else {
		printf("emmc: var: writing is off: %s (last error: %s %#x)\n", var_why, emmc_failed ? emmc_failed : "none",
		    emmc_failed_status);
	}
}

/* /dev/remmc_var: the window, block 0 = its header; reads and writes in whole sectors through a bounce
 * buffer (no physio: user memory goes through uiomove, outside the lock) */
static int
var_rw(struct uio *uio, bool write)
{
	uint8_t *bounce;
	int error = 0;

	if (uio_offset(uio) < 0 || uio_offset(uio) % 512 != 0 || uio_resid(uio) % 512 != 0) {
		return EINVAL;
	}
	if (write && (!var_on || ufs_var_busy_pid != proc_selfpid())) {
		var_refused++;
		return write && !var_on ? EROFS : EPERM;    /* the claim first: kern.redwood.var_busy */
	}
	if (write && ((uint64_t)uio_offset(uio) < 512 || (uint64_t)uio_offset(uio) / 512 > var_blocks ||
	    (uint64_t)uio_resid(uio) / 512 > var_blocks - (uint64_t)uio_offset(uio) / 512)) {
		var_refused++;
		return EROFS;                       /* the header, or past the end: the whole request, before any of it */
	}
	bounce = kalloc_data(EMMC_MAX_BLOCKS * 512, Z_WAITOK);
	if (bounce == NULL) {
		return ENOMEM;
	}
	while (error == 0 && uio_resid(uio) > 0) {
		uint64_t at = (uint64_t)uio_offset(uio) / 512, n = (uint64_t)uio_resid(uio) / 512;

		if (n > EMMC_MAX_BLOCKS) {
			n = EMMC_MAX_BLOCKS;
		}
		if (write) {
			/* not trimmed: a write past the window's end is refused whole by write_blocks */
			error = uiomove((caddr_t)bounce, (int)(n * 512), uio);
			if (error == 0 && (error = var_write(var_lba + at, (uint32_t)n, (uint32_t *)(void *)bounce)) != 0) {
				uio_setoffset(uio, (off_t)(at * 512));
				uio_setresid(uio, uio_resid(uio) + (user_ssize_t)(n * 512));
			}
			continue;
		}
		if (at >= var_blocks) {
			break;                      /* the end of the window */
		}
		if (n > var_blocks - at) {
			n = var_blocks - at;
		}
		lck_mtx_lock(&emmc_lock);
		error = read_retrying(var_lba + at, (uint32_t)n, (uint32_t *)(void *)bounce) ? 0 : EIO;
		lck_mtx_unlock(&emmc_lock);
		if (error == 0) {
			error = uiomove((caddr_t)bounce, (int)(n * 512), uio);
		}
	}
	kfree_data(bounce, EMMC_MAX_BLOCKS * 512);
	return error;
}

bool
redwood_emmc_var_on(void)
{
	return var_on;
}

/* the run is ending: no write after this one returns (the lock waits out a chunk in flight) */
void
redwood_emmc_var_stop(void)
{
	lck_mtx_lock(&emmc_lock);
	var_on = false;
	var_w1 = 0;
	lck_mtx_unlock(&emmc_lock);
}

/* ---- the devices ---- */

static int
emmc_open(dev_t dev, int flags, int devtype, __unused proc_t p)
{
	if (minor(dev) == VAR_MINOR) {
		return var_on && devtype == S_IFCHR ? 0 : ENXIO;
	}
	return (flags & FWRITE) ? EROFS : 0;
}

static int
emmc_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static void
emmc_strategy(buf_t bp)
{
	uint64_t block = (uint64_t)buf_blkno(bp);
	uint32_t count = buf_count(bp), done = 0;
	uint8_t *data = (uint8_t *)buf_dataptr(bp);
	int error = 0;

	if (!(buf_flags(bp) & B_READ)) {
		error = EROFS;                      /* this device never writes */
		goto out;
	}
	if (block >= emmc_blocks) {
		buf_setresid(bp, count);
		error = block == emmc_blocks ? 0 : EINVAL;
		goto out;
	}
	if (count > (emmc_blocks - block) * 512) {
		count = (uint32_t)((emmc_blocks - block) * 512);
	}
	while (done < count && error == 0) {
		uint32_t blocks = (count - done) / 512;

		/* the lock a piece (at most 128 KB) at a time: a page-in waits for one piece of a var write, not all of it */
		lck_mtx_lock(&emmc_lock);
		if (blocks == 0 || ((uintptr_t)(data + done) & 3)) {
			/* a part block, or a buffer not 4-aligned: one block at a time through the sector buffer */
			uint32_t n = count - done < 512 ? count - done : 512;

			if (read_retrying(emmc_lba + block + done / 512, 1, sector)) {
				memcpy(data + done, sector, n);
				done += n;
			} else {
				error = EIO;
			}
		} else {
			if (blocks > EMMC_MAX_BLOCKS) {
				blocks = EMMC_MAX_BLOCKS;
			}
			if (read_retrying(emmc_lba + block + done / 512, blocks, (uint32_t *)(void *)(data + done))) {
				emmc_reads++;
				emmc_read_blocks += blocks;
				done += blocks * 512;
			} else {
				error = EIO;
			}
		}
		lck_mtx_unlock(&emmc_lock);
	}
	buf_setresid(bp, buf_count(bp) - done);
	if (error) {
		printf("emmc: block %llu, %u bytes: %s (%#x)\n", block, count, emmc_failed, emmc_failed_status);
	}
out:
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
emmc_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	if (minor(dev) == VAR_MINOR && (cmd == DKIOCGETBLOCKCOUNT || cmd == DKIOCISWRITABLE)) {
		if (cmd == DKIOCGETBLOCKCOUNT) {
			*(uint64_t *)data = var_blocks;
		} else {
			*(uint32_t *)data = var_on;
		}
		return 0;
	}
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = 512;
		return 0;
	case DKIOCSETBLOCKSIZE:
		/* (spec_open sets 512 and back again, and takes a refusal for a broken device) */
		return *(uint32_t *)data == 512 ? 0 : EINVAL;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = emmc_blocks;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 0;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*(uint64_t *)data = EMMC_MAX_BLOCKS;
		return 0;
	default:
		return ENOTTY;
	}
}

static int
emmc_size(__unused dev_t dev)
{
	return 512;
}

static int
emmc_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	if (minor(dev) == VAR_MINOR) {
		return var_rw(uio, false);
	}
	return physio(emmc_strategy, NULL, dev, B_READ, (u_int (*)(buf_t))emmc_size, uio, 512);
}

/* only /dev/remmc_var; /dev/remmc0 (xnusys) never writes */
static int
emmc_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return minor(dev) == VAR_MINOR ? var_rw(uio, true) : EROFS;
}

static const struct bdevsw emmc_bdevsw = {
	.d_open     = emmc_open,
	.d_close    = emmc_close,
	.d_strategy = emmc_strategy,
	.d_ioctl    = emmc_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = emmc_size,
	.d_type     = D_DISK,
};

static const struct cdevsw emmc_cdevsw = {
	.d_open       = emmc_open,
	.d_close      = emmc_close,
	.d_read       = emmc_read,
	.d_write      = emmc_write,
	.d_ioctl      = emmc_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

int
redwood_emmc_root(dev_t *root)
{
	uint64_t t0 = mach_absolute_time(), ns;
	uint32_t *probe;

	if (emmc_bmajor >= 0) {
		*root = makedev(emmc_bmajor, 0);
		return 0;
	}
	msdc = ml_io_map(MSDC0_PHYS, 0x1000);
	if (!find_partition()) {
		return ENXIO;
	}
	/* how fast: the first 4 MB, which the root's mount reads anyway */
	probe = kalloc_data(EMMC_MAX_BLOCKS * 512, Z_WAITOK);
	if (probe != NULL) {
		for (uint32_t at = 0; at < 4 * 2048 && at + EMMC_MAX_BLOCKS <= emmc_blocks; at += EMMC_MAX_BLOCKS) {
			if (!read_retrying(emmc_lba + at, EMMC_MAX_BLOCKS, probe)) {
				break;
			}
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		printf("emmc: 4 MB in %llu ms\n", ns / 1000000);
		kfree_data(probe, EMMC_MAX_BLOCKS * 512);
	}
	emmc_bmajor = bdevsw_add(-1, &emmc_bdevsw);
	if (emmc_bmajor < 0 || (emmc_cmajor = cdevsw_add_with_bdev(-1, &emmc_cdevsw, emmc_bmajor)) < 0) {
		printf("emmc: no room among the devices (%d, %d)\n", emmc_bmajor, emmc_cmajor);
		return ENXIO;
	}
	var_setup();
	*root = makedev(emmc_bmajor, 0);
	return 0;
}

void
redwood_emmc_devfs(void)
{
	if (emmc_bmajor < 0 || emmc_cmajor < 0) {
		return;
	}
	if (devfs_make_node(makedev(emmc_bmajor, 0), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "emmc0") == NULL ||
	    devfs_make_node(makedev(emmc_cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "remmc0") == NULL) {
		printf("emmc: no /dev nodes\n");
	}
	if (var_on && devfs_make_node(makedev(emmc_cmajor, VAR_MINOR), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600,
	    "remmc_var") == NULL) {
		printf("emmc: var: no /dev/remmc_var\n");
	}
}

/* sysctl kern.redwood.emmc: how much it has read, and its errors */
static int
emmc_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char line[160];

	snprintf(line, sizeof(line), "emmc: %llu reads, %llu MB, %llu errors%s%s", emmc_reads, emmc_read_blocks >> 11,
	    emmc_errors, emmc_failed ? "; last: " : "", emmc_failed ? emmc_failed : "");
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}

SYSCTL_DECL(_kern_redwood);
SYSCTL_PROC(_kern_redwood, OID_AUTO, emmc, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, emmc_sysctl, "A",
    "the eMMC's reads so far");

/* sysctl kern.redwood.emmc_var: the window and its writes */
static int
emmc_var_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char line[256];

	snprintf(line, sizeof(line), "var: blocks %llu+%llu, %s; %llu writes, %llu sectors, %llu verify failures, "
	    "%llu refused (%llu NV)", var_lba, var_blocks, var_on ? "writable" : var_why ? var_why : "off", var_writes,
	    var_sectors, var_verify_failed, var_refused, nv_refused);
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}

SYSCTL_PROC(_kern_redwood, OID_AUTO, emmc_var, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, emmc_var_sysctl,
    "A", "the eMMC's var window: writes, sectors, verify failures, refusals");
