# Cellular Stage 2e and Stage 3: radio on, the other channels, the data path (spec)

Written 2026-11-17. Analysis only: no code, builds or flashing. The device was used read-only in Android.
Guesses are marked **(guess)**. Sources:

- **[K]** haretic/android-kernel-mt6765-rabbitr1 `drivers/staging/mtk-ccci/` (MediaTek eccci, GPL), fetched one file at a time.
- **[RIL]** a fresh stock boot transcript, read from `logcat -b radio` (MTK RIL `AT>`/`AT<` lines).
  - The device had been up 5 min, so the transcript runs from `+EIND: 128` to the first data call.
  - A masked copy is in `xnu-deps/build/modem-blobs/evidence/stock-ril-boot-at.txt` (git-ignored): every run of 5 or more digits, the IPs, EGMR and EOSID are masked.
- **[DUMP]** `/proc/ccci_dump` of the same boot (read in place, not kept) and the older `evidence/ccci_dump.txt`.
- **[LIB]** `xnu-deps/build/modem-blobs/libmtk-ril.so`, pulled from `/vendor/lib64` (md5 d9bd3392…). String offsets are from `strings -t x`.
  - mtkfusionrild loads this file. librilfusion.so is the framework and was not needed.
- **[OURS]** logs/md2c3.log, logs/md2d1.log and xnu/files/bsd/dev/redwood_md.c.

---

## 0. Two findings that change the picture

1. **Channels 41 and 43 never had any traffic. They are counter aliases.**
   - redwood_md.c (`drain_one`, the "other" branch) counts `oth_ch[ch & 127]`. So 169 & 127 = 41 and 171 & 127 = 43.
   - "ch41=4 ch43=1" really means ch169 ×4 (3 logged, cap 3) and ch171 ×1.
   - Fix: index a 256-entry array by `ch`.
   - So the real list of unanswered channels is **4, 34, 57, 59, 169, 171**.
2. **`+CME ERROR: 14` is not a fault. The SIM is simply off. MediaTek boots the modem at CFUN=0 with the SIM unpowered, and the RIL powers it with `AT+EFUN=0`.**
   - [RIL] 19:54:12.061: `AT> AT+EFUN=0`.
   - Before the OK for that command (+300 ms), the modem sent `+ESIMS: 1,29`, `+ECT3G`, `+EUSIM: 1, 2, 1`, `+ICCID…` and `+ESIMECC: 0`.
   - The string `+ESIMS: 1,29` is a constant in [LIB] at 0x11a70d.
   - No other step was missing. No ch34/169/171 answer is involved: stock leaves those to daemons, and they are not on the SIM path.
   - Our 2d sent only queries, so the SIM never powered.

---

## A. Sub-stage 2e: "radio on, registration only"

### A.1 What the stock RIL sends, +EIND: 128 → registered ([RIL], RIL_CMD_RADIO_9 thread, verbatim order)

```
+EIND: 128 (t=0)          ES3G? -> +ES3G: 1      ATE0Q0V1     AT+CMEE=1    AT+CMER=1,0,0,2,0   AT+ICCID=1
AT+ESIMS=1 (URC on)       AT+ECFGSET="disable_sms","0"         AT+ESBP=5,"SBP_ETWS_FOR_AOSP_DESIGN",1
AT+ESIMS -> +ESIMS: 1 (SIM present)     [t=+1.1 s]  AT+CSCS="UCS2"  AT+EGTP=1  AT+EMPPCH=1  AT+EDSDA=1 (CME 100)
AT+ESADM=1  AT+EBOOT=0  AT+EDSBP=0  AT+EPOC
AT+EFUN=0  -> +ESIMS: 1,29, +EUSIM: 1, 2, 1, +ICCID, +ESIMECC: 0, OK   (SIM powered, radio still off)
AT+ESLOTSINFO=1 (CME 100)  AT+ES3G?  AT+ESBP=7,"SBP_T_PLUS_W" -> 1  AT+ESBP=7,"SBP_GEMINI_LG_WG_MODE" -> 0  AT+EPSB? -> 1023
AT+EGRAT=14 -> OK
AT+EFUN=1 -> OK            (t=+1.5 s; RADIO ON)
AT+ERAT=6,4 -> OK          (GSM/UMTS/LTE, LTE preferred; sent AFTER EFUN=1)
+ECEMODE: 1, +ECEVDP: 3, +EREG…, +EGREG…, +ECPI…, +ETESTSIM: 0, +CSCON: 0,,3, +ECSQ: 255,…  (searching)
+ESIMAPP… +CSCON: 1,7,3 (RRC connected) +ECSQ: 12,27,1,1,1,-55,-455,4096,2,12
+PSBEARER: 4096,3,0,0  +CIEV: 3, 1  +EREG/+EGREG (registered)  +CIEV: 9 (NITZ)  +CIEV: 10,"<plmn>","AT&T…"
+EIAREG: ME ATTACH "reseller.mnc410.mcc310.gprs",IP,0    (t≈+3.5 s: LTE attach WITH default bearer, cid 0)
+EIND: 2, +EIND: 1  -> RIL starts SMS init (CMGF/CSMS/CNMI/CPMS) -- not for us
```

In parallel, on other threads (none of it is on the radio path): `AT+EREG=3`, `AT+EGREG=3`, `AT+ECSQ=1`, `AT+PSBEARER=1`, `AT+CSCON=3`, `AT+CTZR=3`, `AT+EOPS=3,2`, `AT+EIAREG=1`, `AT+EDALLOW=1`.

There were also IMS/VoLTE configs, SS/USSD/CLIP/CCWA, `AT+EGMR=0,x` reads and SBP/ECFG writes.

### A.2 Which radio command, which value

- **EFUN, not CFUN.** The stock RIL never sends `AT+CFUN=`.
  - [LIB] only has `AT+CFUN?` as a query.
  - The formats are `AT+EFUN=%d,%d` (0x124773) and `AT+EFUN=0,%d` (0x1157d7).
- The value is a bitmask of the SIM slots whose radio is on:
  - `AT+EFUN=0`: all radios off, SIM on.
  - `AT+EFUN=1`: SIM1 radio on (single SIM).
  - The second parameter is a reason/mode **(guess)**. Stock sent `AT+EFUN=0,2` once at +14 s for an internal radio cycle. We do not need it.
- `+EFUN: 0` / `+CFUN: 0` at boot (2d) means radio off.

### A.3 RAT/band: leave the modem's NV defaults

- **ERAT** = `AT+ERAT=6,4` (GSM/UMTS/LTE, LTE preferred). Stock sends it every boot after EFUN=1.
  - It is persisted by the modem **(guess)**, so the value already in nvdata (written by Android) is the same 6,4.
  - Skip it. Allow `AT+ERAT?` as a check only.
- **EGRAT=14**: the capability switch ([LIB] `RmcCapabilitySwitchRequestHandler::sendEGRAT`, "raf=%d, rat=%d").
  - Single SIM with `+ES3G: 1` already reported, so not needed **(guess)**.
  - If registration never starts (no `+CEREG: 2,2` searching within 10 s after EFUN=1), add `AT+EGRAT=14` before EFUN=1 on the next run.
- **EPBSE / EPBSEH** (band masks): stock does not send them at boot; the modem uses NV. Never send them.
- The Android reference is **LTE band 12**, EARFCN 5110, 10 MHz, AT&T 310410 (MVNO), PCI 88, rsrp ≈ −113 dBm (`+ECSQ … -455` = −455/4).
  - `dumpsys telephony.registry` showed this when this spec was written.

### A.4 Data-centric vs voice-centric (the one real risk to registration)

- Stock reports `+ECEMODE: 1` (CS/PS mode 1, voice-centric) and `+ECEVDP: 3` (IMS voice preferred, CS second).
- We do not run the IMS stack, so IMS voice never comes up.
- Under 3GPP TS 24.301, a voice-centric UE without IMS voice and without CSFB may disable E-UTRA. AT&T has no 2G/3G left, so the result would be no service. **(guess about the MD's actual behaviour)**
- 2e does NOT change it (it is not optional-free and it persists).
- **If 2e registers and then drops** (`+CEREG: 2,0` or `2,3` after `2,1`), the next run adds `AT+CEMODE=2` (data-centric, 27.007 standard) before `AT+EFUN=1`, behind a separate approval. It may be NV-persisted; that only reaches the RAM overlay.

### A.5 IMS/VoLTE and other things we must NOT enable

- **IMS / VoLTE / Wi-Fi calling:**
  - `AT+EIMS*` (EIMSVOICE, EIMSVOLTE, EIMSCFG, EIMSESS, EIMSPDN "onoff",1)
  - `AT+EVOLTE*`, `AT+EWFC*`, `AT+EEPDG=1`, `AT+EIWLPLEN`
  - `AT+EAPNACT` with an "ims"/"emergency" type, `AT+CAVIMS`
- **Config / SBP writes:** `AT+ECFGSET=`, `AT+EGMC=1,…`, `AT+ESBP=`
- **Boot mode:** `AT+EBOOT=1`
- **Fast dormancy timers:** `AT+EFD=` (optional, persistent **(guess)**)

### A.6 AT commands that write NVRAM (avoid; there is a whitelist anyway)

| Command | What it writes |
|---|---|
| `AT+EGMR=1,…` | IMEI / barcode. **Never.** |
| `AT+ERAT=`, `AT+EPBSE=`, `AT+EPBSEH=`, `AT+EGRAT=` | RAT/band (guess: persisted) |
| `AT+ESBP=`, `AT+ECFGSET=`, `AT+EGMC=1` | carrier feature bits |
| `AT+CGDCONT=`, `AT+EAPNSET=`, `AT+EIAAPN=` | stored contexts / attach APN |
| `AT+CEMODE=` | UE mode |
| `AT+COPS=1/4` | manual PLMN |
| `AT+ERFTX`, `AT+ERFIDX`, `AT+ESAR*`, `AT+ETXPWR*` | **TX power / SAR (MediaTek RF test commands)** |
| `AT+EMETACFG`, any `AT+ECAL`/`AT+EMMCH` set | META / calibration |

**What happens to a write the modem expects to persist.** It goes to the RAM overlay, the FS reply says success, and it reads back from the overlay in this session.

At the next boot the modem reads the partition's untouched copy, written last by Android.

| Kind of file | Effect of losing the write |
|---|---|
| Learned items (last PLMN / frequency list, EPS NAS context if not on the USIM, AFC tracking value, counters) | The modem starts from Android's last saved state: a slower first cell search, perhaps a fresh attach/authentication. Harmless. |
| Calibration (CALIBRAT) and IMEI (NVD_IMEI) | Never written in 2c/2d ([OURS] write list: SP82_000, INFO_FILE, IM83_026, IM03_001, IM90_010, MT6K_004, CK10_001, CK14_000, LD6S_014, all NVD_DATA/INFO_FILE). 2e treats a write there as a stop. |

### A.7 Unsolicited indications

| URC | Meaning | Source |
|---|---|---|
| `+EIND: 128` | modem ready for AT | muxd waits for it, stage2 §2.3 |
| `+EIND: 64` | earlier init stage | (guess) |
| `+EIND: 32` | after SIM/ECC init | (guess) |
| `+EIND: 1` | SMS ready | [RIL]: SMS init follows at once |
| `+EIND: 2` | phonebook ready | (guess) |
| `+ESIMS: <inserted>[,<cause>]` | SIM presence; `1,29` = present after power-on | [LIB] 0x11a70d; cause meaning (guess) |
| `+ESIMS: 0,x` | absent/removed: **stop** | |
| `+EUSIM: 1, 2, 1` | app type, USIM **(guess)** | |
| `+ECT3G`, `+ESIMECC` | SIM capability / ECC list | |
| `+ICCID: …` | ICCID: **mask in the log** | |
| `+CPIN: READY` | SIM ready | `SIM PIN` / `SIM PUK`: **stop, never send a PIN** |
| `+ECPIN`, `+EPINC: 3, 3, 10, 10` | PIN state / retries | |
| `+ETESTSIM: 0` | not a test SIM | |
| `+ESIMAPP`, `+ESIMIND` | SIM app / IMSI state: **mask** | |
| `+CREG/+CGREG/+CEREG: <n>,<stat>[,tac,ci,AcT]` | stat 0 not searching, 1 home, 2 searching, 3 denied, 4 unknown, 5 roaming; AcT 7 = E-UTRAN | 27.007 |
| `+EREG` / `+EGREG` | MTK extended versions of the same (stock uses these) | |
| `+ECSQ: <rssi>,<ber>,<rssi_dBm?>,…,<rsrq·4>,<rsrp·4>,<act>,…` | signal; act 4096 = LTE **(guess from values)** | |
| `+CSCON: 1,7,3` / `0,,3` | RRC connected / idle | |
| `+PSBEARER: 4096,…` | PS bearer on LTE | |
| `+EIAREG: ME ATTACH "<apn>",IP,0` | LTE attach done with default bearer cid 0 | |
| `+CIEV: 9` / `+CIEV: 10` | NITZ / operator name | |
| `+CGEV: ME DETACH` | detach (on EFUN=0) | |
| `+ERSA…` | remote SIM / VSIM, unused: ignore | |
| `+STKPCI` | SIM toolkit proactive command: ignore, never answer | |
| `+ECEMODE`, `+ECEVDP` | UE mode / voice domain | A.4 |

### A.8 The 2e sequence (r1_md=7), radio-on part behind `r1_md_radio=1`

The current AT engine (one command outstanding; final result = `OK` / `ERROR` / `+CME ERROR: n` / `+CMS ERROR: n`; everything else is logged as a URC) is kept. A command not on the whitelist is refused in `at_send` (as in 2d).

**Phase 1, SIM only (no RF; can run without radio approval)**

| # | Command | Expect | Timeout |
|---|---|---|---|
| 1 | wait for `+EIND: 128` | | 20 s after HS2 |
| 2 | `AT` / `ATE0` / `AT+CMEE=1` | OK | 5 s each |
| 3 | `AT+ESIMS=1` | OK (turns the ESIMS URC on) | 5 s |
| 4 | `AT+ESIMS` | `+ESIMS: 1` OK. `+ESIMS: 0` → stop, no SIM | 5 s |
| 5 | `AT+EFUN=0` | URCs `+ESIMS: 1,29`, `+EUSIM…`, then OK | 15 s |
| 6 | `AT+CPIN?`, every 2 s up to 10× | `+CPIN: READY` | 20 s total |
| 7 | `AT+CGSN` | 15 digits, Luhn OK, FNV-32 hash equal to the reference hash (A.10) | 5 s |
| 8 | `AT+CFUN?` / `AT+EFUN?` | `+CFUN: 4` (guess) / `+EFUN: 0` | 5 s |
| 9 | `AT+CEMODE?`, `AT+ERAT?` | logged only | 5 s |

- If step 5 gives OK but no SIM URC in 15 s, rerun Phase 1 with `AT+EBOOT=0` and `AT+EPOC` sent before step 5, as stock does. Their meaning is unknown and they look volatile **(guess)**.
- **Run 2e-1 first, alone.** It proves the SIM path with zero RF.

**Phase 2, radio on (needs `r1_md_radio=1`, set only after the user's approval)**

| # | Command | Expect | Timeout |
|---|---|---|---|
| 10 | Pre-condition gate | all of A.10, checked in code, else stop | |
| 11 | `AT+CREG=2`, `AT+CGREG=2`, `AT+CEREG=2`, `AT+ECSQ=1` | OK. On ERROR use `AT+EREG=3` / `AT+EGREG=3` (stock-proven) | 5 s each |
| 12 | `AT+EFUN=1` | OK in about 60 ms (stock) | 30 s |
| 13 | Wait for registration (A.9). Poll `AT+CEREG?` every 5 s, `AT+CSQ` every 10 s | | 120 s |
| 14 | Once registered: `AT+COPS?`, `AT+CSQ`, `AT+CGATT?`, `AT+CEREG?` | logged | |
| 15 | Hold 60 s, logging URCs and every FS/RPC/other-channel packet | | 60 s |
| 16 | `AT+EFUN=0` | OK, `+CGEV: ME DETACH`, `+CEREG: 2,0` | 15 s |

- After step 16, power the modem down as now.
- **Hard cap: 4 min from step 12 to step 16.** On any stop condition, jump straight to step 16, then power down.
- If `AT+EFUN=0` gives no answer in 15 s, power MD1 down anyway.

### A.9 What "registered" looks like

1. `+CEREG: 2,2` (searching), then `+CEREG: 2,1,"<tac>","<ci>",7`. Stat 5 (roaming) is also success.
   - Stock: about 2 s after EFUN=1.
   - Log TAC/CI in full: they are cell, not personal, identifiers.
2. `+EIAREG: ME ATTACH "<apn>",IP,0`: the default bearer is up (stock +2 s).
3. `AT+COPS?` → `+COPS: 0,0,"AT&T…",7`.
4. `AT+CSQ` → `+CSQ: 10..20,99`. `+ECSQ` rsrp/4 around −110 to −115 dBm (matches Android, A.3).
5. `+CSCON: 1,7,3` then `0,,3`.

Result line: `RESULT 2e: SIM ready X ms, EFUN=1 at Y, CEREG stat S at Z ms, COPS "…", CSQ n, attach apn "…", radio on T s, detach OK`.

- **Denied:** `+CEREG: 2,3`, or still 2 after 120 s. Radio off at once; no retry this run.

### A.10 Pre-conditions (checked in code before `AT+EFUN=1`; each failure logs and stops)

1. HS2 and `+EIND: 128` received.
   - No CCIF bit 16 and no ch0 `MD_EX`/`REC_OK`/`PASS`.
   - Boot status still 0x54430007/0x5332xxxx.
2. FS (counters from mdfs_summary):
   - unimplemented 0, bad requests 0, cap hits 0, reads cut 0;
   - errors returned = exactly the known three: GETATTRIBUTES `Z:\FAT6E63FA35.log` −9, CMPT_READ `SP81_049` −9, DELETE `SP81_049` −9.
     - SP81_049 is absent on Android too (182 files in NVD_DATA, SP80_723 and SP82_000 only), so this matches stock.
3. Every CMPT_READ/READ under `CALIBRAT/` and `NVD_IMEI/` and on X:/Y: returned the full requested length.
   - Zero WRITE/DELETE/RENAME/CREATE on those paths since power-on.
4. Overlay integrity (new, about 20 lines):
   - CRC32 of each WRITE payload is stored with the node.
   - Each later READ/CMPT_READ served from the overlay re-checks the CRC; a mismatch is a stop.
   - The eMMC write-refusal counter in redwood_emmc.c is 0.
5. RPC:
   - unanswered 0;
   - only ops seen are 0x4005 and 0x400e (stock's set: [DUMP] shows only md_eint and dtsi lines from ccci_rpc_k);
   - any 0x4003/0x4007/0x4008/0x4009/0x400a/0x400b/0x4010/0x4011 (EFUSE_BLOWING, [K] port_rpc.h) → stop.
6. No ch2 system message after HS2.
   - Stock: rxq0 isr_cnt = 3 over 11 min ([DUMP] traffic monitor) = HS2 + 2 PCM. So stock gets no battery/TX-power/RF-temp sysmsg at all.
7. "Other" channel traffic only on {4, 34, 57, 59, 169, 171}. Rings drained (no `garbage[q]`).
8. `+CPIN: READY`; `+ESIMS: 1,…`.
9. IMEI hash (FNV-32 of the 15 digits) equals the reference hash taken under Android:
   - `service call iphonesubinfo …` piped through a hash on the device; the digits are never printed **(needs a one-time approval; read-only)**.
10. Physical:
    - Battery ≥ 30 %, or on USB power; the PA draws ~1–2 A peaks **(guess)**.
    - SoC thermal < 50 °C (our thermal thread).
    - Wi-Fi off (no LTE/Wi-Fi coexistence exchange, see B).

### A.11 Under Android beforehand (read-only reference, all masked)

- `dumpsys telephony.registry | grep -m1 mServiceState`: RAT LTE, band 12, EARFCN 5110, operator, rsrp. Done today: IN_SERVICE LTE/LTE, VoPS supported.
- `getprop gsm.version.baseband`: MOLY.LR12A.R3.MP.V230.
- `cat /proc/ccci_dump | grep ccci_rpc_k`: the stock RPC op set (A.10 item 5).
- `logcat -b radio -d | grep 'AT[<>]'` → diff against evidence/stock-ril-boot-at.txt. Mask before saving.
- SIM PIN disabled: `+EPINC: 3, 3, 10, 10` in the transcript, and no `+CPIN: SIM PIN`.

### A.12 Stop list (never sent; not on the whitelist)

- **Dialling:** `ATD*`, `ATA`, `AT+CHLD`, `AT+CHUP`, `ATH`.
- **SMS:** `AT+CMGS/CMGW/CMSS/CNMI/CSMS/CMGF/CPMS/CSCA`.
- **Data:** `AT+CGACT`, `AT+CGDATA`, `AT+EAPNACT`, `AT+CGDCONT=`, `AT+EGACT`, `AT+EPDN`, `AT+EIF` (Stage 3 only, separate approval).
- **Supplementary services / USSD:** `AT+CUSD`, `AT+CCFC`, `AT+CCWA`, `AT+CLCK`, `AT+CPWD`, `AT+EIUSD`.
- **SIM:** any `AT+CPIN=`, `AT+CSIM`, `AT+CRSM`, `AT+CGLA`, `AT+ESIMAPP=`, STK responses.
- **Everything in A.5 and A.6. Also:** `AT+CMUX`, `AT+COPS=` (any set), `AT+CFUN=` (any set), `AT+EFUN=` other than 0/1, `AT+CGATT=`.

### A.13 Regulatory/safety review (technical)

The modem transmits exactly as stock if it runs the same firmware with the same RF data and the same index into that data:

| Item | Status |
|---|---|
| Firmware | Same md1img: LK loads and verifies it, MOLY.LR12A.R3.MP.V230 |
| Calibration | Byte-identical CALIBRAT/protect files, read-only from this device's partitions (reader check: 168 files / 856262 B = Android) |
| DRDI / RF set index | Same as stock: `md_drdi_rf_set_idx` and `md_product_name_model_id` are 0x0F0F0F0F in [DUMP] INIT LOG, and we answer DTSI_QUERY with 0x0F0F0F0F |
| Rewrites at boot | Calibration-adjacent files the MD writes and reads back are served by the overlay in the same session: fine |

What differs from stock, and why it does not change TX:

1. **SP81_049 missing.** Missing on Android too: same.
2. **SIM3/SIM4 EINT "unknown" (−11).** Stock: `MD1_SIM3_HOT_PLUG_EINT: node 2 no found` ([DUMP] INIT LOG): same. SIM hot-plug only.
3. **Battery / thermal / TX-power inputs.**
   - The kernel-path ones are MD_GET_BATTERY_INFO 0x105, MD_TX_POWER, MD_RF_TEMPERATURE(_3G) and MD_SW_MD1_TX_POWER_REQ on ch2 ([K] port_sysmsg.c `sys_msg_handler`). Of these only 0x105 has a reply; the rest are MD→AP notices.
   - The MD sent none in 2c/2d, and stock sent none in 11 min (rxq0 = 3).
   - SWTP is inactive on r1 even in stock (`swtp_init_delayed_work end: ret = -4`, old dump).
   - ADC RPCs (0x4003/0x4007) and RF_CLK_BUF (0x4008) never appear in stock.
   - SAR_TABLE_IDX (0x4010, ccci_rpcd `mtk_sar_table_id_get`) was never requested in stock this boot.
   - **None of the unanswered channels (B) is thermal or TX-power.**
   - The AP→MD throttling messages (thermal "mdtx", low-battery, PBM) are AP-initiated. Stock sends them only when hot or low **(guess from MediaTek's design)**, hence the A.10 item 10 limits.
   - The MD's own PA/RF temperature sensing is internal and unaffected **(guess)**.
4. **LTE/Wi-Fi coexistence (ch34 GMMGR→WMT) not delivered.**
   - Affects only ISM-adjacent bands (7/38/40/41). Band 12 is unaffected.
   - Keep Wi-Fi off during cellular runs.
5. **Lost NV writes.** See A.6. No calibration or IMEI writes occur.

**Verdict: nothing in this set-up can make the MD transmit with different power or parameters from stock. That holds while the A.10 gates hold.** The residual unknowns are A.4 (registration may drop; that is a reliability issue, not a TX one) and the guesses marked above.

---

## B. The unanswered channels

Channel numbers are from [K] `inc/ccci_core.h` `enum CCCI_CH`; ports from `port/port_cfg.c` `md1_ccci_ports[]` (MD_GENERATION 6293).

| ch (MD→AP) | Name | Queue | Port / consumer in Android | Seen ([OURS] md2d1) | Needed? |
|---|---|---|---|---|---|
| 4 | CCCI_PCM_RX | q0 | `ccci_aud` char port (PORT_F_USER_HEADER), Audio HAL speech driver | 2 × 16 B, data0 0xffffffff (header only) at +1.5 s / +3.3 s | **Ignore.** Voice speech path only. Stock also gets 2 on ch4 and the HAL sends 1 on ch5 ([DUMP] `traffic(ch): tx:[5]1 … rx:[4]2`) |
| 34 | CCCI_IPC_RX | q1 | IPC ports `ccci_ipc_*`; routed by header.reserved = destination "unify id" | 1 × 61 B, reserved 0x80000003, word0 0xd | **Ignore.** See below |
| 57 | CCCI_IMSA_DL | q6 | `ccci_imsa` (volte stack: IMS audio) | 1 × 20 B word0 7 | **Ignore** (IMS) |
| 59 | CCCI_IMSDC_DL | q6 | `ccci_imsdc` (IMS data channel) | 1 × 28 B word0 3 | **Ignore** (IMS) |
| 169 | CCCI_IMSM_RX | q1 | `ccci_imsm` → volte_imsm_93 daemon (IMS manager handshake) | 4 (3 logged: 40/24/40 B, word0 0x10002454 / 0x80002 / 0x10012454) | **Ignore while IMS is off.** The MD reached +EIND: 128 regardless |
| 171 | CCCI_WOA_RX | q1 | `ccci_woa` → Wi-Fi-offload agent (epdg_wod, Wi-Fi calling) | 1 × 30 B, ASCII "wost…" | **Ignore** (Wi-Fi calling) |
| 41, 43 | (IPC_UART_TX_ACK / MD_LOG_TX, AP→MD direction) | | | 0: counter alias of 169/171 (§0) | n/a |

Channel 34 decoded ([K] `port/port_ipc.c` `port_ipc_kernel_thread`, `ccci_ipc_task_ID.h`, `port_ipc_internal.h` `struct ccci_ipc_ilm`):

- reserved 0x80000003 = AP_UNIFY_ID_FLAG | AP_IPC_WMT → kernel port `ccci_ipc_3` → `mtk_conn_md_bridge_send_msg()` to connsys WMT.
- Payload = `ccci_ipc_ilm {u32 src_mod_id = 0xd (MD_MOD_GMMGR), dest_mod_id, sap_id, msg_id, local_para_ptr, peer_buff_ptr}` (24 B), then `local_para {u8 ref_count, u8 reserved, u16 msg_len (includes these 4 B), data}`.
- Content: an LTE/Wi-Fi coexistence notice. One-way; no AP reply is defined.

**What the kernel must answer for normal operation:**

1. **Registration:** nothing beyond what 2b-2d already answer (control, sys 0x105/0x114/0x116, kernel RPC, FS). All six channels above must be **drained and dropped**, as now.
   - An undrained q1 would block RPC, which shares q1.
2. **Stage 3:** kernel RPC **0x400D IPC_RPC_CCCI_LHIF_MAPPING** ([K] port_rpc.c `ccci_rpc_work_helper` / `ccci_rpc_remap_queue`; port_rpc.h numbers it 0x400D, and 0x400E is DTSI_QUERY, consistent with our table).
   - Request: 1 param = `{u32 net_if (ccmni index), u32 lhif_q}`, where lhif_q 2 = AP_UL_Q0 → CLDMA txq 0, and 3 = AP_UL_Q1 (IMS) → txq 3.
   - Reply: 2 × `{4, 0}`.
   - Our RPC table must add this row. The current "anything else → {4, −1}" would refuse it.
   - Stock very likely sends it right after EAPNACT ([DUMP] a ccci_rpc_k send at +79 s, next to the data call) **(guess)**.

Optional later (not needed): IPC time push AP→MD (`IPC_MSG_ID_CCCIIPC_CLIB_TIME_REQ`, port_ipc.c `send_new_time_to_md`: ilm src AP_MOD_CCCIIPC → MD_MOD_CCCIIPC, local_para msg_len 20, data {u32 sec_lo, sec_hi, tz, dst}). NITZ already gives the MD the time.

---

## C. Stage 3: the data path

### C.1 PDN as the MediaTek RIL does it ([RIL] RIL_CMD_READER_4, stock)

1. On LTE the default bearer comes **automatically with attach**: `+EIAREG: ME ATTACH "reseller.mnc410.mcc310.gprs",IP,0` (cid 0, IPv4).
2. The RIL then binds it to an AP interface:
   - `AT+EAPNACT=1,"RESELLER","default",0,,"dnn=RESELLER",,0` ([LIB] 0x12292d)
   - → `+CGEV: ME PDN ACT 0`, OK
   - MD → AP on the NetAgent channel: `+EIF: 100, …` (2 lines, [DUMP] old history 80.69 s)
   - AP: `AT+EPDN=0,"ifst",20` ([LIB] 0x11d9e2) and `AT+EIF=100, "ifst", "up", 1` ([LIB] 0x12851d)
   - → `+EPDN: 0, "…"` (addresses)
3. `AT+CGCONTRDP=0` returns IP, DNS and MTU. `AT+CGEQOSRDP=0` and `AT+CGACT?` (→ `+CGACT: 0,1`) follow.
4. Android result: ifname `ccmni0`, interface id 100, IPv4 /32, one DNS, **MTU 1430**, link type 519 = ARPHRD_RAWIP, flags NOARP|UP (`/sys/class/net/ccmni0`).

Stage 3 sequence, each step separately loggable:

| Step | Commands | Notes |
|---|---|---|
| 3a, read-only | `AT+CGCONTRDP=0`, `AT+CGPADDR=0` | log IP/DNS masked to /24 |
| 3b | `AT+EAPNACT=1,"<apn from +EIAREG>","default",0` | short form [LIB] `AT+EAPNACT=1,"%s","%s",%d` |
| 3c | `AT+EPDN=0,"ifst",20`, `AT+EIF=100, "ifst", "up", 1` | copy stock literally; meaning of 20 unknown **(guess)** |
| 3d | answer RPC 0x400D | B |
| 3e | `AT+CGCONTRDP=0` again | |

**Kill if:** EAPNACT gives `+CME ERROR`, or no `+CGEV: ME PDN ACT`.

### C.2 Which HIF carries ccmni: CLDMA, not CCIF

- [K] `port_cfg.c`: every `ccmniN` port has hif `MD1_NET_HIF`, and `inc/ccci_hif.h` sets MD1_NET_HIF = CLDMA (plan, Key facts).
- ccmni0 = tx ch 22 (CCMNI1_TX) on **txq DATA_TX_Q = 0**, ack queue 0xF0|1, **rxq 0**.
- CCIF carries no ccmni on 6765. Stock confirms it: `cldma_plat_hw_reset` during MD start, and `net Txq0-3: 15-15, 21-21` counters ([DUMP]).

Gen93 CLDMA ([K] `hif/ccci_hif_cldma.h`, `ccci_hif_cldma.c`, `mt6765/cldma_reg.h`, `hif/ccci_cldma_plat.c`):

- **Queues:** CLDMA_TXQ_NUM 4 (net txq 0..3; 0 = data, 1 = data ack, 3 = IMS after remap), CLDMA_RXQ_NUM 1 (rxq 0, all ccmni).
- **Stock ring sizes:** RX 512 × NET_RX_BUF; TX 256 per queue.
  - NET_RX_BUF = SKB_4K = CCCI_MTU + 128 = 3456 + 128 = **3584 B** (`ccci_bm.h`, `ccci_common_config.h`).
  - This matches runtime-data AP_CCMNI_MTU 0xDF0 = 3568 (stage2 §1.4).
  - **We use 64 RX × 3584 B and 32 TX** (≈ 230 KB + GPDs).
- **Descriptors** (16 B, packed, LE):
  - `cldma_tgpd {u8 flags (bit0 HWO, bit7 IOC); u8 non_used(=1); u8 msb (addr bits 35:32); u8 netif; u32 next_gpd_ptr; u32 data_buff_bd_ptr; u16 data_buff_len; u16 psn}`.
  - `cldma_rgpd {u8 flags; u8 non_used; u16 data_allow_len; u32 next_gpd_ptr; u32 data_buff_bd_ptr; u16 data_buff_len; u8 msb; u8 non_used2}`.
  - Rings are circular via next_gpd_ptr. Physical addresses must be < 36 bit, and memory must be uncached or cache-maintained.
- **Bases** (DT `cldma-ao`, `cldma-pd`): AO 0x10014000, PD 0x1021b000. IRQ SPI 165, level high. Clock CG2 bit 3 is already ungated in Stage 1.

**Start sequence** (`cldma_start`, `cldma_reset`, `cldma_plat_hw_reset`):

1. Reset. INFRA RST0 +0x140 bit 6 (AO) set, then RST1 +0x144 bit 6. Then RST0 +0x150 bit 2 (PD), then RST1 +0x154 bit 2.
2. AO +0x418 DL_MTU_SIZE = 3584. AO +0x400 SO_CFG |= 1 (DMA enable).
3. TX start addresses: PD +0x00 + 4·q (TQSAR, low 32), +0x10 the 4 MSB. Also the backup copy at AO +0x04 + 4·q.
4. RX start address: AO +0x404 (SO_START_ADDR_0), +0x40C MSB.
5. RX start: PD +0x404 SO_START_CMD = 0x1 (then a dummy read). TX is started on the first send: PD +0x30 UL_START_CMD = 1<<q, later PD +0x34 UL_RESUME_CMD.
6. Interrupts (later):
   - PD +0x808 L2TIMCR0 = 0xFFF; AO +0x804 L2RIMCR0 = 0x7 (via `cldma_write32_ao_misc`); PD +0x820/+0x824/+0x848/+0x84C = all.
   - Status: PD +0x800 L2TISAR0, PD +0x830 L2RISAR0 (write 1 to clear).
   - Bits: TX done 0..3, TX empty 4..7, TX error 8..11; RX done bit 0, empty bit 1, error bit 2.

**Polling first:**

- **RX** (`cldma_gpd_rx_collect`): while the current rgpd has HWO == 0:
  1. Take data_buff_len bytes.
  2. Re-arm with the same buffer: `data_allow_len = 3584`, `flags = 0x81`.
  3. Advance.
  4. If SO_STATUS (AO +0x414) shows the queue stopped, write PD +0x408 SO_RESUME_CMD = 1.
  5. Clear L2RISAR0.
- **TX** (`cldma_gpd_handle_tx_request` + `md_cd_send_skb`):
  1. Fill the tgpd: data ptr, len, **netif = ccmni index (0)**, non_used = 1, `flags |= 1` (HWO; IOC as wanted).
  2. Barrier.
  3. First send: UL_START_CMD = 1<<0; afterwards UL_RESUME_CMD = 1<<0.
  4. Reclaim when HWO clears.
- **Timing:** do the CLDMA start (rings + SO_START) **before the MD release in the Stage-1 sequence**, as stock does (`cldma_plat_hw_reset` precedes "modem is starting" in [DUMP]). The MD assumes a ready DL ring **(guess)**.

### C.3 Packet format and MTU

| Direction | Format | Source |
|---|---|---|
| **TX (UL)** | The buffer is the **raw IP packet**. The ccci_header that port_net pushes is stripped by the HIF on gen93, and tgpd.netif = ccci_h.data[0] = ccmni index. | [K] `ccmni_send_pkt`, `cldma_gpd_handle_tx_request` (MD_GENERATION ≥ 6293) |
| **RX (DL)** | **4-byte `lhif_header`** {u16 pdcp_count; u8 flow:4, f:3, rsv:1; u8 netif:5, nw_type:3}, then the raw IP packet. netif selects the ccmni (0); skip the 4 bytes. | [K] `port_proxy.c proxy_dispatch_recv_skb` (CLDMA_NET_DATA), `port_net_recv_skb` `skb_pull(sizeof(lhif_header))`, `mtk_ccci_common.h` |
| **MTU** | Network-given 1430 (Android ccmni0), from `+CGCONTRDP`/`AT+EPDN=…,"mtu"`. Default 1500 (CCMNI_MTU, ccmni.h). | |
| Link | No L2, no ARP. IPv4/IPv6 is decided by the version nibble. | |

### C.4 Smallest XNU interface

**One new file, `bsd/dev/redwood_cell.c`, ~700 lines (guess):**

| Part | Lines (guess) | Content |
|---|---|---|
| CLDMA | ~300 | ring alloc (wired, physically contiguous, uncached; addresses < 2^36), reset/start (C.2), `cell_tx(buf,len)`, `cell_rx_poll()` |
| ifnet | ~200 | below |
| AT PDN steps (C.1) | ~120 | in redwood_md.c's AT engine |
| RPC 0x400D row | ~20 | |
| Unattended test | ~80 | C.5 |

The ifnet:

- Modelled on redwood_net.c `redwood_net_attach`, but not Ethernet. That code is single-instance (`net_link`), so do not reuse it; copy its pattern.
- `ifnet_allocate_extended`:
  - name "pdp_ip", unit 0 (iOS's own cellular interface name);
  - family IFNET_FAMILY_CELLULAR, type IFT_CELLULAR;
  - flags IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST; MTU from PDN; hdrlen 0, addrlen 0.
- **demux:** the first nibble 4 → PF_INET, 6 → PF_INET6.
- **framer:** none (no header).
- **output:** mbuf → copy to the TX buffer → `cell_tx`.
- **input:** `mbuf_allocpacket` + `ifnet_input` with the protocol family set from the nibble.
- `bpfattach(DLT_RAW)`.
- Address: the kernel sets the /32 from CGCONTRDP via `ifnet_set_addr`-style SIOCAIFADDR inside the kernel, as a test hook. Routes and DNS are userland (Stage 4).
- **Polling thread:** every 2 ms while there is traffic, otherwise every 20 ms. IRQ SPI 165 later.

### C.5 Unattended test plan (r1_md=8, Wi-Fi left unconfigured)

1. Gate on A.10 plus 2e success.
2. 3a–3e (C.1). Log CGCONTRDP fields, IP/DNS masked to /24.
3. **Data proof without the IP stack** (3f):
   - The kernel builds one UDP DNS query (A record for a fixed public name) from the PDN address to the PDN DNS, port 53.
   - It sends it on txq 0 (netif 0) and polls RX for 5 s.
   - Repeat 3×. Then one ICMP echo to the DNS server, which may be filtered, so it is not a kill.
   - Log: TX GPD done (HWO cleared), RX packets, first 28 bytes of the IP/UDP header (no payload), DNS rcode, answer count.
4. Attach the ifnet and set the address. Send the same query through a kernel socket (`sock_socket` + `sock_send` bound to pdp_ip0) to prove the ifnet path.
5. Counters once per 10 s: tx/rx packets and bytes, GPD errors, L2/L3 status, the AO +0x870 DMA_ERR word, lhif netif histogram.
6. Teardown: `AT+EAPNACT=0,<aid>,…`, which may be skipped since EFUN=0 detaches anyway; then `AT+EFUN=0`, then stop CLDMA (UL/SO STOP_CMD PD +0x38 / +0x40C), then power the MD down.

### C.6 Kill criteria (Stage 3)

- The PDN does not come up: no `+EIAREG` after registration, EAPNACT error, or CGCONTRDP without an address, in 2 runs.
- TX GPD HWO never clears (the MD side does not consume) in 2 runs with the CLDMA start before the MD release.
- RX gets zero packets after 3 DNS queries whose TX completed, in 2 runs **after** trying `AT+EIF … "ifst" "up"` and the 0x400D answer.
- Any CLDMA error bit (L2 bits 8–11 / RX bit 2, DMA_ERR ≠ 0), MD exception, or bus hang. Stop and power down.
- Total effort beyond ~3 weeks without the first DNS answer (plan's Stage 3 kill: "PDN up but no packets cross CLDMA").

---

## D. The 2e whitelist (exact strings, `\r` appended by at_send)

```
AT
ATE0
AT+CMEE=1
AT+ESIMS=1
AT+ESIMS
AT+ESIMS?
AT+EFUN=0
AT+EFUN=1            (only if r1_md_radio=1 and all A.10 gates passed)
AT+EFUN?
AT+CFUN?
AT+CPIN?
AT+CGSN             (digits never logged; hash only)
AT+CGMR
AT+CREG=2
AT+CGREG=2
AT+CEREG=2
AT+EREG=3           (fallback)
AT+EGREG=3          (fallback)
AT+ECSQ=1
AT+CREG?
AT+CGREG?
AT+CEREG?
AT+COPS?
AT+CSQ
AT+CGATT?
AT+CEMODE?
AT+ERAT?
AT+EBOOT=0          (fallback only, A.8)
AT+EPOC             (fallback only, A.8)
```

`AT+EGRAT=14` and `AT+CEMODE=2` each need their own approval (A.3 / A.4).
