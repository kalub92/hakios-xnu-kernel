# Cellular plan (MD1 modem on the XNU port)

Written 2026-11-17, analysis only (no code, no device). The user chose to KEEP cellular knowing it is
on the order of months (HANDOFF-2026-11-16.md, USER DECISIONS). Haretic reference copies:
`docs/ref/haretic-modem/` (see the end of this file). Guesses are marked **(guess)**.

## What haretic has demonstrated vs. drafted

- Kernel repo haretic/android-kernel-mt6765-rabbitr1 (main):
  - fb2f79f98c "Add r1-md-probe, a staged MT6765 modem power-on and HS1 bring-up module".
  - f9d6663740 "r1-md-probe: read HS1 from the CCIF SRAM and decode LK's 24-byte MPU entries" → HS1 reached (strong inference).
  - d3794ac668 (vendor CCCI imported verbatim) and 842499a703 (ported to Linux 7.1 as `drivers/staging/mtk-ccci`, ~46k lines, ~120 files).
  - 1d6ee47b41 "keep a booted modem's MD1 domain powered while the system sleeps" and 9c184289b8 "name the modem channel that woke the AP" → a running modem.
- Device repo haretic/android_device_rabbit_r1:
  - `modem/modem.mk` header still says "DRAFT for Stage 3", but `device.mk:297` inherits it, so it ships.
  - `modem/r1-modem-nvcheck.sh` records a failure they hit: an mdinit that starts before `/dev/ccci_ccb_ctrl` exists boots the modem without its CCB config and "the modem asserts in ccci_shm_bm.c and never finishes its handshake".
- haretic profile README: "RIL/Mobile data" works, "Calls and SMS untested".
- All of this uses the closed stock userland: `ccci_mdinit` (with built-in `ccci_fsd`), `ccci_rpcd`, `gsm0710muxd` (arm32), `mtkfusionrild` + `libmtk-ril.so`, and `nvram_daemon`.
- Conclusion: a working reference exists up to mobile data. Kernel side GPL; userland side has no open reference.

## Key facts

- **LK loads, verifies and EMI-MPU-protects md1img** (r1_md_probe.c header: "LK has loaded, verified and protected it before Linux starts").
  - LK describes it in its DTB at `/chosen/ccci,modem_info_v2`: struct `_ccci_lk_info_v2`, LE: u64 tag base, u32 size, s32 errno, u32 version, u32 count, u32 load flag, then s32 per-MD LK errno.
  - Tags (name 64 bytes when version ≥ 2, else 16; then u32 data_off, u32 data_size, u32 next):
    - `hdr_tbl_inf`: 24-byte entries u64 base, u32 size, s8 md_id, errno, type, ver.
    - `smem_layout`: u64 base, u32 ap_md1 off, size, ap_md3 off, size, md1_md3 off, size, total.
    - `ccb_info`: u64 base, u32 size.
    - `md_mpu_inf`: 24-byte entries u32 start, end; s32 region (-1 ends); u32 apc[2] (3 bits/domain, lock = apc0 bit 31); u32 unused.
    - `md1_chk`: magic +0, ver +12, product +16, type +20, platform +24, build +104 / +40, mem_size +172, img_size +176, dsp off +184, size +188.
    - `nc_smem_info_ext`: 16-byte ap_off, md_off, size, id.
    - `smem_dfd_size`, `retrieve*`.
  - mtkloader already receives this DTB (`mtkloader_main(dtb)`, `connsys_dt_report`). mtkloader.c:37: all modem/ccci reservations are below 0xc0000000, clear of XNU.
  - So we never parse md1img or program EMI MPU **(guess: LK does this regardless of kernel payload; Stage 0 verifies)**.
- **Interfaces** (eccci/inc/ccci_hif.h:38-39):
  - `MD1_NORMAL_HIF = CCIF`: control, AT `ttyC0`, `ccci_fs`, RPC, IPC, CCB.
  - `MD1_NET_HIF = CLDMA`: ccmni data.
- **NV storage:** ext4 partitions nvdata, nvcfg, protect1, protect2, plus raw `nvram` and `proinfo` (fstab.modem). The file server reads by path from the ext4 filesystems.

## Register / layout reference

| Block | Base | Registers |
|---|---|---|
| SPM | 0x10006000 | +0x180 PWR_STATUS, +0x184 PWR_STATUS_2ND (MD1 = bit 0); +0x320 MD1_PWR_CON; +0x394 MD_SRAM_ISO_CON (bit 0); +0x398 MD_EXTRA_PWR_CON (bit 0, ext buck iso) |
| INFRACFG_AO | 0x10001000 | +0x150 RST0, +0x154 RST1 (CCIF0 rst bit 8); +0x220 PROTECTEN, +0x228 STA1, +0x2a0 SET, +0x2a4 CLR (PERI2MD bit 7, MD masters bits 3/4); +0x250 PROTECTEN_1, +0x258 STA1_1, +0x2a8 SET, +0x2ac CLR (MD2PERI bit 6); +0x370 AP2MD_DUMMY bit 0; +0xf0c MD_SRCCLKENA (set low byte to 0x21) |
| INFRACFG clock gates (set=gated) | 0x10001000 | CG1: set +0x88, sta +0x94: CCIF1_AP 12, CCIF1_MD 13, CCIF_AP 23, CCIF_MD 26. CG2: set +0xa4, sta +0xac: CLDMA_BCLK 3. CG3: set +0xc0, sta +0xc8: CCIF2_AP 18, CCIF2_MD 19 (clear regs = set+4, from clk-mt6765.c) |
| TOPCKGEN | 0x10000000 | +0x0 CLK_MODE, bits 8-9 MD clock gate, cleared before MD power on |
| CLDMA AO / PD | 0x10014000 / 0x1021b000 | |
| AP CCIF | 0x10209000 | +0x00 CON, +0x04 BUSY, +0x08 START, +0x0c TCHNUM, +0x10 RCHNUM, +0x14 ACK, +0x100 CHDATA (512-byte SRAM) |
| MD CCIF | 0x1020a000 | |
| MD boot status | 0x1020e300 | 2 words; read 3×, keep the third |
| MD side (AP view, only with PERI2MD open) | | 0x20000024 boot vector enable (write 1 = release); 0x200f0100 MD RGU WDT_MODE (write 0x55000030 = off); 0x20140000 MDTOP PLLMIXED; 0x20150000 MDTOP CLKSW |

- IRQs: CLDMA SPI 165 (level high), CCIF0 SPI 148 (low), CCIF1 SPI 149 (low), MD WDT SPI 261 (falling edge).
- VMODEM: MT6357 VMODEM at 0.8 V, always on. Vcore is read only.
- CCIF channels: 15 = D2H_SRAM (HS1), 16 = D2H_EXCEPTION_INIT.
- Shared memory, gen93 layout; the MD sees the non-cacheable region at 0x40000000. Offsets: MDSS_DBG +2K (10K), CCB_CTRL +96K, NETD +100K, AUDIO +108K, CCISM_MCU +160K (721K), CCISM_MCU_EXP +881K (121K), DFD +1M.
- CCIF ring buffers, 8 queues:
  - Control header 0xee0000ee … 0xff0000ff, {read, write, length} for RX then TX; packets framed 0xaabbaabb / 0xccddeeff.
  - RX sizes K: 80, 80, 40, 80, 20, 20, 64, 0. TX sizes K: 128, 40, 8, 40, 20, 20, 64, 0. Exception RX/TX K: 12, 32, 8, 0, 0, 0, 8, 0.
- HS1 = ccci_header (16 bytes, MD_INIT_START_BOOT 0x0 / CHK_ID 0x5555ffff) + md_query_ap_feature (head 0x49434343 "CCCI", 64 feature bytes, tail, 152 reserved) in CCIF SRAM at +0x100. The AP's HS2 reply area follows it.
- Boot status alive: 0x54430007 or 0x5331xxxx = past the boot ROM; 0 = not started; anything else = boot ROM failure range.
- MD1 power sequence (mtk-pm-domains.c `scpsys_modem_legacy_pwrseq_on`, data in mt6765-pm-domains.h):
  1. MD1_PWR_CON: clear RST_B, set PWR_ON, set PWR_ON_2ND, wait for both status bits.
  2. SRAM enable (pdn bit 8, no ack), clear SRAM iso 0x394 bit 0.
  3. Clear ISO, clear CLK_DIS, set RST_B, clear ext buck iso 0x398 bit 0.
  4. Release bus protection in reverse order of the bp_cfg list: PERI2MD 7, MD masters 3|4, MD2PERI 6.
  - Power-off is the reverse.
- Stock clear of shared memory before the first boot: `clear_smem`. Port lists: eccci/port/port_cfg.c (ttyC0 = CCCI_UART2, AT queue 1 or 5; ccci_fs = DATA_FSD_Q 1 or 4).

## Stages

Every stage is a one-shot harness image, judged on kernel log lines, then the device returns to Android.

### Stage 0 – read only (~150 lines, low risk)

- Decode LK's handoff in mtkloader and read MD1 power, protection and clock-gate state in XNU. Spec at the end.
- Android cross-check: `su -c 'cat /proc/pl_lk' | grep -i -E 'ccci|md1|modem|mpu|emi'`.
- **Kill:** `modem_info_v2` missing, LK errno ≠ 0, or no md_id 0 in `hdr_tbl_inf`. That means loading/verifying md1img and EMI MPU via secure-world calls ourselves, which is not realistic.

### Stage 1 – power on and release to HS1 (~600 lines, medium risk)

- Port and condense r1_md_probe.c stages 1–2 (lines 588–1165) plus the power sequence above. Order:
  1. VMODEM 0.8 V via our pwrap code.
  2. MD1 MTCMOS with bus protection.
  3. Open the CCIF and CLDMA clock gates.
  4. Pre-access: AP2MD_DUMMY clear, PROTECTEN_1_SET bit 6, poll STA1_1.
  5. SRCCLKENA 0x21.
  6. `r1md_pll_init`.
  7. MD WDT off.
  8. CCIF reset; clear shared memory and lay out ring buffers (`r1md_smem_prepare`).
  9. Write 0x20000024 = 1; post-access (PROTECTEN_SET bit 7, open MD2PERI).
  10. Poll boot status and CCIF channel 15 for up to 10 s.
- Needs the handoff (tag base) passed from mtkloader to XNU, e.g. a loader-added boot-arg.
- **Kill:** no HS1 after two faithful attempts (boot status 0 or boot-ROM range), or a bus hang (the watchdog returns to Android).

### Stage 2 – minimal CCCI runtime (~2,500–4,000 lines (guess), high risk, the bulk of the months)

- Kernel side, GPL:
  - CCIF HIF + ring buffers: ccci_hif_ccif.c 2409 lines, ccci_ringbuf.c 423.
  - FSM HS1 → runtime-data reply → HS2: ccci_fsm.c, ccci_modem.c, port_ctlmsg.c, port_sysmsg.c.
  - Kernel RPC subset (the modem asks for GPIOs by name, from `gpio-usage-mapping`): port_rpc.c.
  - CCB: port_smem.c.
  - cdevs `ccci_fs`, `ttyC0`, `ccci_ipc_*`, `ccci_ccb_ctrl`, `ccci_monitor`.
- Closed, must be reverse-engineered:
  1. ccci_mdinit's setup ioctls (`CCCI_IOC_SET_BOOT_DATA`, `SET_MD_BOOT_MODE`, CCB config).
  2. The ccci_fsd protocol (file ops framed in ccci_header packets on ccci_fs). No open reimplementation known (unverified).
  - Method: static RE of the vendor blobs (REA). Once the device is free, `strace -f -e ioctl,read,write` of ccci_mdinit on stock Android during a modem restart, to get a transcript; implement only the operations it shows.
- FSD as a userland daemon, as on Android:
  - Serve NV files read-only with an in-RAM write overlay, discarded at reboot.
  - Needs a read-only ext4 reader (~500 lines) over redwood_emmc.c reads.
  - Lazier first cut: a one-time tar of `/mnt/vendor/{nvdata,nvcfg,protect_f,protect_s}` from this same device into the payload.
- First proof in the kernel log: raw AT on ttyC0, no 07.10 mux yet (guess: gsm0710muxd sends AT+CMUX itself).
  - `AT+CGSN` and `AT+CPIN?` with the radio off (CFUN=4/0).
  - `AT+CFUN=1`, `AT+CSQ`, `AT+CREG?` only behind `r1_md_radio=1`.
- **Kill:** the modem keeps asserting or never reaches HS2 although every requested file was served; or mdinit/fsd behaviour is not recovered within ~3 weeks.

### Stage 3 – data (~1,700 lines (guess), medium risk)

- CLDMA HIF: ccci_hif_cldma.c 3268 lines condensed to ~1,200; ccci_cldma_plat.c 265.
- ccmni (ccmni.c 1847, port_net.c 634) → an XNU ifnet modeled on redwood_mtkwifi.c, ~500 lines. Point-to-point, raw IP, no ARP **(guess)**.
- PDN over AT: AT+CGDCONT / AT+CGACT / AT+CGPADDR. MTK-specific EGACT/EPDN-style commands **(guess)** from the mtkfusionrild capture. ~300 lines in the daemon.
- Throughput bound by CLDMA queue handling; polling first, IRQs later.
- **Kill:** the PDN comes up but no packets cross CLDMA.

### Stage 4 – iOS integration

- CommCenter will never accept a non-Apple baseband, so no telephony UI.
- Least bad: the daemon sets the ifnet address and default route and publishes IPv4/DNS in SCDynamicStore (`State:/Network/Service/...`), as VPN apps do. iOS routes through it when Wi-Fi is down **(guess about service order)**.
- Bars and carrier name via our SpringBoard hook (inject_sb.c) from the daemon's +CSQ/+COPS.
- SMS and calls out of scope (at most a minimal app later).

## Safety and legal rules (technical)

- Never write nvram, nvdata, nvcfg, protect1, protect2 or proinfo. The eMMC write path must refuse them. Early stages are read-only with a RAM overlay.
- The IMEI must never change. The daemon blocks `AT+EGMR=1,...` and any other NV-write AT commands.
- No transmitting (CFUN=1) until the log shows the RF calibration files were served from this device's own partitions.
- MD boot mode must be normal, never META/factory.

## Stage 0 spec (next action; inert unless `r1_md=1`)

**A. `mtkloader/mtkmodem.h` (new)**
- Include right after `mtkconnsys.h` (mtkloader.c:443); reuse its `dt_be32`, `dt_streq`, `dt_len`.
- Call `modem_dt_report(dtb)` after `connsys_dt_report(dtb)` (mtkloader.c:3767), only if `boot_arg(" r1_md=",0)`.
- Read `/chosen/ccci,modem_info_v2` LE, decoding as r1_md_probe.c `r1md_read_handoff` / `r1md_parse_tags` / `r1md_decode_tag` (lines 349–554). Tag name length 64 when version ≥ 2. The tag area is a physical address the loader reads directly.
- Print:
  - `md0: modem_info_v2 tags BASE size S ver V count N lk_errno E flag F`
  - `md0: hdr_tbl_inf md_id I base B size Z errno E`
  - `md0: smem_layout base B ap_md1 +O size Z`
  - `md0: ccb_info B size Z`
  - `md0: md1_chk build ...` (strings at +104 and +40)
  - one `md0: mpu region R START-END apc A0 A1` per 24-byte entry
  - `md0: reserved-memory NAME reg ...` for reserved-memory nodes whose name contains `ccci` or `md`
  - finally `md0: handoff usable|INCOMPLETE|NONE`

**B. `xnu/files/bsd/dev/redwood_md.c` (new, ~80 lines)**
- Add to bsd/conf/files like the other redwood drivers.
- `redwood_md_init()` is called next to `redwood_connsys_init()` (redwood_input.c:232). It returns unless `PE_parse_boot_argn("r1_md")` gives 1.
- Map with `ml_io_map`; read only. Read every register in the table above:
  - SPM +0x180/+0x184/+0x320/+0x394/+0x398.
  - INFRA +0x150/+0x154/+0x220/+0x228/+0x250/+0x258/+0x370/+0xf0c/+0x94/+0xac/+0xc8.
  - TOPCKGEN +0x0.
- Guards:
  - AP CCIF +0x00..+0x14 only if CCIF_AP (INFRA +0x94 bit 23) is ungated.
  - Boot status 0x1020e300 (2 words, third of three reads) only if MD1 is on in both status registers.
- Log lines via `redwood_host_keep_add`:
  - `r1md: SPM PWR_STATUS %#x/%#x (MD1 on|off) MD1_PWR_CON %#x ISO %#x EXTRA %#x`
  - `r1md: INFRA PROTECTEN %#x STA1 %#x PROTECTEN_1 %#x STA1_1 %#x AP2MD %#x SRCCLKENA %#x RST0 %#x RST1 %#x`
  - `r1md: CG ccif_ap %s ccif_md %s cldma %s; TOPCKGEN CLK_MODE %#x`
  - `r1md: CCIF CON %#x BUSY %#x START %#x RCHNUM %#x` (or `skipped (gated)`)
  - `r1md: boot_sta %#x %#x` (or `skipped (MD1 off)`)
  - `r1md: stage 0 done`

**Expected (guess):** MD1 off, PERI2MD/MD2PERI protection set, handoff usable → go to Stage 1.

## Reference copies (docs/ref/haretic-modem/, haretic GPL-2.0 / Apache-2.0)

- `r1_md_probe.c`: drivers/staging/mtk-ccci, stages 0–2.
- `mt6765-pm-domains.h`, `mtk-pm-domains.c`: MD1 domain data, bus protection, `scpsys_modem_legacy_pwrseq_on` at ~line 720.
- `clk-mt6765.c`: infracfg clock-gate offsets and bits.
- `mt6765-dtsi-modem-node.txt`: modem node from mt6765.dtsi 705–740 and the r1 dts `&modem` 1217–1240.
- `init.r1.modem.rc`, `r1-modem-nvcheck.sh`, `fstab.modem`: haretic's userland ordering and NV mounts.
- Full ccci tree: github.com/haretic/android-kernel-mt6765-rabbitr1 `drivers/staging/mtk-ccci` (Stage 2/3 sources, not copied).
