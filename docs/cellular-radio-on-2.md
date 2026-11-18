# Cellular radio-on, run 2: what md2e2 showed and the spec for the next run

Written 2026-11-17. Analysis only: no code edits, builds or device use. Guesses are marked **(guess)**.

Sources:

- **[K]** haretic/android-kernel-mt6765-rabbitr1 `drivers/staging/mtk-ccci/eccci/` (MediaTek eccci, gen 6293), fetched per file:
  `inc/ccci_core.h` (enum CCCI_CH), `port/port_cfg.c` (`md1_ccci_ports[]`), `port/port_char.c`, `port/port_ipc.c`,
  `ccci_ipc_task_ID.h`, `ccci_ipc_el1_msg_id.h`.
- **[RIL]** `xnu-deps/build/modem-blobs/evidence/stock-ril-boot-at.txt` (stock RIL AT transcript, masked).
- **[DUMP]** `evidence/ccci_dump.txt` (stock kernel CCCI log); **[KSPEC]** `evidence/kernel-spec.txt` §4.
- **[BLOB]** `strings` of `libccci_util.so` (device-node table).
- **[LOG]** `logs/md2e2.log` lines 3240-3286; **[CODE]** `xnu/files/bsd/dev/redwood_md.c`.

---

## 0. What md2e2 actually did (re-read of [LOG] with [CODE])

| t (ms) | Event |
|---|---|
| 20372 | `AT> AT+EFUN=1`; `rd_t_on` armed |
| 20384 | ch38 q1, L=24 (16 B header + 8 B payload), payload word0 0xd → `st2a.oth_bad++` |
| (same poll) | `radio_drive` → `radio_stop("unexpected traffic")` → RP_OFF; **`at_wait = false`** abandons the pending EFUN=1 |
| 20395 | ch34 q1 L=84 and L=1154, resv 0x80000003, word0 0x5 |
| 20395 | `AT> AT+EFUN=0` (EFUN=1 still executing) |
| 20408 | `+CME ERROR: 0`, attributed to EFUN=0 |
| 20440 | `+CREG: 2`, `+CGREG: 2` (searching), and "a non-+ line of length 2" |
| 20530 | `AT+EFUN?` → `+EFUN: 1` → `at_radio_on = true` → watch loop breaks at once → MD1 powered down |

Reading **(inference, high confidence)**:

- The length-2 non-+ line at 20440 is **EFUN=1's own `OK`**, 68 ms after the command. Stock takes 61 ms (§3).
- `+CME ERROR: 0` was the modem refusing EFUN=0, which arrived while the radio-on transition was still running. CME 0 is the generic "phone failure"; here it means "busy".
- So EFUN=1 succeeded and EFUN=0 was rejected. The one bug is in `radio_drive` RP_OFF: it sends a new command while one is outstanding.

---

## 1. Channels 38 and 34

### Channel 38

- **Name:** `CCCI_IPC_UART_RX = 38`, MD→AP ([K] ccci_core.h:347). TX is 40, the ACKs are 39/41.
- **Port:** `{CCCI_IPC_UART_TX, CCCI_IPC_UART_RX, 1, 1, …, PORT_F_WITH_CHAR_NODE, &char_port_ops, 5, "ttyC2"}` ([K] port_cfg.c:133). It is a plain char port on queue 1, with no ILM framing; [KSPEC] §4 agrees.
- **Userspace consumer:** unknown.
  - [BLOB] has `/dev/ttyC2` only as one row of libccci_util's shared node-name table (alias `eemcs_ipc_uart`). That table is copied into every CCCI daemon, so it names no reader.
  - [DUMP] has no per-port open or traffic record for ch38. Its `traffic(ch)` monitor lists only ch 4/5/10/12/14/15.
- **Stock kernel behaviour when nobody has ttyC2 open:** `port_char_recv_skb` ([K] port_char.c:175) returns `-CCCI_ERR_DROP_PACKET` for any char port with `usage_cnt == 0`. The exception list is UART2 (AT), C2K_AT, PCM, FS, RPC, UDC and the LWA IPC minor; ttyC2 is not on it. **So when ttyC2 is unopened, stock drops ch38 in the kernel, exactly what draining does.**
- **The 8-byte payload, first word 0xd:** it cannot be an ILM (an ILM header is 24 B). 0xd is `MD_MOD_GMMGR` (global mode manager), the same source as the 61-byte ch34 note seen at boot.
  - **(guess)** a GMMGR mode/RAT status word emitted on RF power-on. The rest of the payload was not logged; the next run logs it (8 bytes, no identifiers).

### Channel 34 (`CCCI_IPC_RX`)

- Logged "op/word0" is `pkt[4]`, the first payload word after the 16-B CCCI header. On IPC that word is `ccci_ipc_ilm.src_mod_id`.
- **"op 5" = `MD_MOD_EL1`** (LTE L1). resv 0x80000003 = `AP_UNIFY_ID_FLAG | AP_IPC_WMT` ([K] ccci_ipc_task_ID.h).
- So both packets are **EL1 → connsys WMT** messages. Kernel port `ccci_ipc_3` hands them to `mtk_conn_md_bridge_send_msg()` ([K] port_ipc.c:378-381, registered for MD_MOD_EL1 and MD_MOD_GMMGR at :462-463).
- **Sizes:** local_para = L − 16 − 24.
  - L=84 → 44 B (40 B data).
  - L=1154 → 1114 B.
- **The EL1 → AP message set** ([K] ccci_ipc_el1_msg_id.h): LTE_DEFAULT_PARAM_IND, LTE_OPER_FREQ_PARAM_IND, WIFI_MAX_PWR_IND, LTE_TX_IND, LTE_CONNECTION_STATUS_IND, PIN_TYPE_IND, LTE_HW_INTERFACE_IND. All are *_IND, i.e. indications.
- **(guess)** The 1154-B packet is LTE_DEFAULT_PARAM_IND, the LTE/Wi-Fi-BT coexistence ("IDC") parameter table, sent once at RF start. The 84-B packet is LTE_OPER_FREQ_PARAM_IND or LTE_CONNECTION_STATUS_IND.
  - Either way it is the coexistence feed to the Wi-Fi/BT chip that the spec predicted, not a request.
  - The `msg_id` word (`pkt[7]`) settles it; log it next run.
- **Messages in the other direction (AP→MD: LTE_TX_ALLOW_IND, WIFIBT_*):** these come from WMT, and only when connsys is running. With Wi-Fi off, stock sends none **(guess, consistent with MediaTek's design)**. The modem's default without them is its own unconstrained-by-Wi-Fi behaviour.

### Does stock answer? Is anything required for registration?

- [DUMP] cannot show ch34/38 per channel. Its rxq1 `isr_cnt` rises 79→81 during the dump, so q1 traffic keeps flowing on stock and is consumed/dropped there.
- No AP reply is defined for any EL1/GMMGR IND ([K] port_ipc.c has no reply path for them), and ch38 is dropped when unopened.
- **Nothing on ch34/38 must be answered for registration.**
- Registration needs only what 2b-2d already answer: control, sys 0x105/0x114/0x116, RPC 0x4005/0x400e, FS, AT.

### TX power, thermal, SAR, antenna?

- **WIFI_MAX_PWR_IND** limits the *Wi-Fi* transmitter. **LTE_TX_IND** tells Wi-Fi when LTE transmits.
- Dropping them can only matter to a running Wi-Fi/BT radio, and gate 10 keeps that off.
- None of these messages changes the modem's own TX power. The LTE TX-power inputs are on other paths, all still stop rules:
  - sys msgs MD_TX_POWER / RF_TEMPERATURE / SW_MD1_TX_POWER_REQ on ch2;
  - RPC 0x4010 SAR_TABLE_IDX;
  - ADC RPCs 0x4003/0x4007 and RF_CLK_BUF 0x4008.

---

## 2. Is draining ch38 safe and stock-equivalent?

**Yes**, for the reasons in §1: the stock kernel itself drops ch38 while ttyC2 is unopened, and ch38 carries nothing the modem waits on. The one residual unknown is whether some stock daemon opens ttyC2 and acts on it.

**(guess)** If one does, it would be a logging or diagnostic consumer. md2e2 shows the modem did not wait: EFUN=1 returned OK and `+CREG: 2` followed, all while ch38 sat unanswered.

### Per-channel rules after `AT+EFUN=1`

| Channel | Rule |
|---|---|
| 4, 57, 59, 169, 171 | drain silently (count only) |
| 34 | drain. Log `src, dest(resv), msg_id, msg_len` for the first 8 per run (no payload). src ∈ {5 EL1, 0xd GMMGR} with dest 0x80000003 is expected. Any other src/dest is logged (first 8), not a stop. |
| 38 | drain. Log the first 3 with the 8 payload bytes in hex. Not a stop. |
| any other channel ∉ {0,2,10,12,14,32} | drain and log the first 3 per channel (header + first 2 words). **Stop** if more than 3 distinct such channels appear, or more than 50 such packets in total (a flood or wrong-state indicator). |
| 0 (control) | **stop** on anything after HS2: MD_EX, REC_OK, PASS, `ctl_odd`. Also stop on CCIF bit 16 (exception). |
| 2 (system) | **stop** on any sys msg after HS2 (`sys_late`). Stock gets none in 11 min, so any arrival is non-stock and may be TX-power/thermal. |
| 32 (RPC) | answer 0x4005/0x400e as now. **Stop** on any other op (`rpc_bad`): 0x4003/0x4007 (ADC), 0x4008 (RF_CLK_BUF), 0x4009-0x400b, 0x4010 (SAR_TABLE_IDX), 0x4011, EFUSE ops. Log the op first. Also stop on `unanswered` > 0. |
| 14 (FS) | the existing gate rules continue: any CALIBRAT/NVD_IMEI write, delete, rename or create; short protected read; overlay CRC mismatch; eMMC refusal ≠ 0; unimplemented op; cap hit. All stop. |
| ring | `garbage[q]` → stop |

Implementation: `oth_bad` keeps the pre-radio set {4,34,57,59,169,171} for gate 7 (unchanged). After `rd_t_on` is set, use a separate counter (`oth_new`) with the rules above; ch38 is added to the allowed set only after EFUN=1.

---

## 3. EFUN handling

### Stock timings ([RIL])

- **First `AT+EFUN=1`:** sent 12.396, `OK` at 12.457 (61 ms).
  - URCs before the OK: `+CSCON: 0,,0` (12.408), `+ECEMODE: 1`, `+ECEVDP: 3`, `+EREG…` (12.456-12.457).
  - Later: `+ECPI`/`+EGREG` at +0.53 s; first `+ECSQ: 255,…` (no cell yet) at +0.73 s.
  - `+CSCON: 1,7,3` (RRC connected, **first uplink**) at +1.05 s; first real `+ECSQ` (rsrp ≈ −113 dBm) at +1.07 s.
  - `+PSBEARER: 4096,3`, `+CIEV: 3, 1`, NITZ, operator name at +1.97 s.
  - `+EIAREG: ME ATTACH …` at **+1.98 s**.
- **Second `AT+EFUN=1`** (26.271): `OK` at 26.336 (65 ms). RRC connected at +0.48 s.
- **`AT+EFUN=0,2`** while attached (25.493):
  - `+CGEV: ME DETACH` +12 ms; `+CSCON: 1,7,3` +64 ms (RRC set up to send the DETACH REQUEST);
  - `+CIEV: 3, 0`, `+PSBEARER: 4096,0…`, `+ECSQ: 255…` (+95-146 ms);
  - **`OK` at +206 ms**.
  - So a radio-off from the attached state detaches cleanly and quickly.
  - Stock uses `,2`. **(guess)** The second parameter is a reason/mode code. Plain `AT+EFUN=0` should also run the switch-off detach (TS 24.301 §5.5.2.2: a UE that switches off sends DETACH REQUEST "switch off" when it can). Keep `AT+EFUN=0` (already whitelisted) and check for `+CGEV: ME DETACH`.

### What `+CME ERROR: 0` meant

- A rejection of a second command on the same AT channel while EFUN=1 was mid-transition **(inference)**.
- The stock RIL never does this: RIL_CMD_RADIO_9 serialises radio commands and only sends the next one after the previous `OK`.

### Correct abort path

1. Never send while `at_wait`. `radio_stop()` only sets RP_OFF + a flag. RP_OFF loads `at_seqO` only when `!at_wait`, i.e. after the outstanding command's final result or its timeout.
2. **EFUN=1 timeout: 10 s** (was 30 s; stock 61-65 ms).
   - If it times out, the AT engine is not responding, so EFUN=0 would not be heard either. Skip EFUN=0, log, and power down MD1.
3. `AT+EFUN=0`, wait up to **15 s** for its final result (stock 206 ms). Then:
   - `OK`: wait 2 s, logging `+CGEV: ME DETACH`, `+CEREG: 0`, `+CSCON`. Then `AT+EFUN?`; expect `+EFUN: 0`.
   - `ERROR` / `+CME ERROR: n`: log n, wait 1 s, resend `AT+EFUN=0`. **At most 3 attempts.**
   - timeout: no retry, go to step 4.
4. `AT+EFUN?`.
   - `+EFUN: 0`: confirmed.
   - Anything else: "EFUN=0 not confirmed".
   - Both cases end with MD1 power-down, as now.
   - During the 2e-2 OFF sequence, `at_radio_on` must not cut the run short before the retries in step 3 are used. Make it set only when `rp != RP_OFF`.
5. The existing kernel caps stay as the backstop: 240 s → OFF path; 255 s → break and power down; `md_ms() >= 400000`; `redwood_md_quiesce()`.

### Is an abrupt MD1 power cut while attached harmful?

**No, beyond an implicit detach.** It is what a battery pull does:

- **Network:** without the DETACH REQUEST, the MME keeps the UE EMM-REGISTERED until the mobile-reachable timer runs out (T3412 + 4 min by default; T3412 is often 54 min), then detaches it implicitly. Until then, incoming calls/SMS fail over to voicemail/store-and-forward.
- **Next boot:** the next attach (Android) uses the stored GUTI. If it is stale, the network asks for identity or re-authenticates.
- **SIM:** EF_EPSLOCI/EPSNSC are updated at attach/TAU/detach. A cut during an UPDATE BINARY is covered by the USIM's anti-tearing **(guess: standard practice for UICC OSes)**. No PIN or counter is touched.
- **Modem NV:** anything it would persist at EFUN=0 goes to the RAM overlay and is lost anyway.
- **Empirical:** after md2e2's cut (pre-attach), Android registered at once (IN_SERVICE LTE AT&T).

---

## 4. The 160 ms window

The modem could not register in 160 ms: stock needs 1.05 s just to reach RRC connected. Within 160 ms of EFUN=1, an LTE modem does:

- RF transceiver power-up: LDOs, PLL/VCO lock, AGC/RX-gain set-up;
- a stored-frequency search (EARFCN 5110 from NV) for PSS/SSS;
- possibly MIB decoding.

None of this transmits. An LTE UE's first uplink is PRACH. PRACH needs:

- downlink sync;
- MIB;
- SIB1 (80 ms period);
- SIB2 (its scheduling period is ≥ 80-160 ms after SIB1);
- in NAS, a cell selected and an attach queued.

The first transmission is therefore physically after ≥ ~200-300 ms even on a known cell. Stock's first evidence of uplink (`+CSCON: 1`) is +1.05 s, and md2e2 only ever reported `+CREG: 2` (searching).

**(guess)** Some MediaTek RF front ends run a brief TX-path self-calibration (DPD/power-detector loopback) at RF power-on. If that exists here, it is the same firmware and calibration data as stock, does not go through PRACH, and would be identical on every stock airplane-mode toggle.

**Verdict:** receive-only, and indistinguishable from a stock airplane-mode-off immediately followed by a battery pull.

---

## 5. Spec for the next radio run (for the implementing agent)

Scope: `redwood_md.c` only, no other files. Whitelist unchanged (`AT+EFUN=0`, `AT+EFUN=1`, `AT+EFUN?` and the queries already in `at_white`).

1. **Channel rules after `rd_t_on` != 0:** implement §2 exactly.
   - Add `oth_new` (packets on channels outside {4,34,38,57,59,169,171}) and `oth_new_ch` (distinct count).
   - `radio_drive` stops on `oth_new_ch > 3 || oth_new > 50`, not on `oth_bad`.
   - Keep `unanswered`, `rpc_bad`, `sys_late`, `ctl_odd`, FS/overlay/protected-path and exception stops unchanged.
   - Before RF-on, gate 7 still uses `oth_bad` (unchanged).
2. **Logging:**
   - ch34: `src dest sap msg_id msg_len` (`pkt[4..7]`, `pb[42..43]`), first 8.
   - ch38: the 8 payload bytes in hex, first 3.
   - Other new channels: header + 2 words, first 3.
   - The line for a final result that arrives with no command outstanding must name the previous command (`late final for AT+EFUN=1: OK`).
   - Log every URC after EFUN=1 as now. Mask `+ICCID`, `+ESIMAPP`, `+ESIMIND`, `+CIMI`. Log TAC/CI in full.
3. **EFUN sequencing:** implement §3 "Correct abort path" steps 1-4:
   - no send while `at_wait`;
   - EFUN=1 timeout 10 s → power down without EFUN=0;
   - EFUN=0 up to 3 attempts, 1 s apart, 15 s timeout each;
   - then `AT+EFUN?`;
   - `at_radio_on` must not break the loop while `rp == RP_OFF` has attempts left.
4. **Registration wait:** 90 s from EFUN=1 (code value; stock needs ~1-2 s).
   - `AT+CEREG?` every 5 s, `AT+CSQ` every 10 s.
   - Success: CEREG stat 1 or 5. Stop: stat 3 (denied), or 90 s.
   - Then `AT+COPS?`, `AT+CSQ`, `AT+CGATT?`, `AT+CEREG?`.
5. **Hold:** 60 s, logging URCs, `+ECSQ`, and every FS/RPC/new-channel event. `AT+CSQ` every 10 s. Then the OFF path.
6. **Result line:** add `efun1_ok_ms`, `efun0_attempts`, `efun0_result`, `detach_seen`, `efun_final`, `ch34 n`, `ch38 n`, `new ch n`.
7. **Unchanged hard caps:**
   - all 10 gates before EFUN=1;
   - 240 s from EFUN=1 → OFF path;
   - 255 s → power MD1 down regardless;
   - `md_ms() >= 400000` backstop;
   - quiesce in loader_return;
   - NOWIFI=1; battery ≥ 30 % or charger; SoC < 50 °C;
   - no data, calls, SMS, SS, COPS=, CFUN=, CGATT=.
8. **One run.** Do not loop on failure.

---

## 6. For the user (to decide on a second radio-on run)

- md2e2 was cut short by our own over-strict rule, not by anything the modem did wrong. Channel 38 is a modem→phone notice port, and stock Android's own kernel throws it away when nobody listens. Channel 34 carried LTE→Wi-Fi coexistence notices. Neither needs an answer or touches the modem's transmit power.
- The radio was on for about 160 ms. In that time the modem only listens: it powers its receiver and starts scanning. Stock needs about 1 s before its first transmission and about 2 s to attach.
- The `+CME ERROR: 0` was our mistake: we sent "radio off" while "radio on" was still finishing. The fix is to wait for the "radio on" reply (stock: about 60 ms), then send "radio off" and retry it up to 3 times.
- Next run, as approved before: registration only, at most 90 s to register, a 60 s hold, then radio off with a clean detach (stock: about 0.2 s), then the modem is powered off.
  - Hard caps: 4 min and 4 min 15 s.
  - No calls, SMS or data.
  - Wi-Fi off.
- The run still stops at once on: any modem exception, any system/thermal/TX-power message, any SAR/ADC/RF-clock/EFUSE request, any write to calibration/IMEI files, or a flood of unknown channels.
- Worst case if radio-off is not confirmed: the modem is powered off while attached, as with a battery pull. The network marks the phone unreachable until its timer expires (typically up to about an hour). In that window, calls to this number go to voicemail.
- Guesses remaining:
  - what exactly the ch38/ch34 messages contain (the next run logs their headers);
  - whether the modem does a brief internal TX self-calibration at radio-on (it would be identical to stock).
