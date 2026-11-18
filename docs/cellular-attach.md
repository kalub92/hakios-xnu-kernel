# Cellular attach: why md2e3 was "denied", and the spec for the next attempt

Written 2026-11-17 (night). Analysis only: no code, builds or device use.
Inferences are marked **(inference)** and guesses **(guess)**.

Sources:
- **[LOG]** logs/md2e3.log (`r1md:` lines).
- **[CODE]** xnu/files/bsd/dev/redwood_md.c (at_white, at_line, at_note, cereg_stat_of, at_step, radio_drive, off_drive, result_2e2).
- **[RIL]** xnu-deps/build/modem-blobs/evidence/stock-ril-boot-at.txt: the stock transcript from a modem boot (`+EIND: 128`) to the first data call.
- **[LIB]** `strings -t x` of xnu-deps/build/modem-blobs/libmtk-ril.so. The REA/Hopper provider failed to open the binary, so this is strings-only.
- docs/cellular-stage3.md §A and docs/cellular-radio-on-2.md.

---

## 0. Summary

1. **It was a real EPS (LTE) failure, not a CS-only rejection.**
   - The modem sent `+CREG: 3,"9005","05095510",7` and `+CEREG: 3,"9005","05095510",7` in the same millisecond (35915 ms).
   - `AT+CEREG?` had returned `+CEREG: 2,2` (searching) 0.4 s earlier.
   - No EPS registration was ever reported, so the state machine was right to treat this as an EPS denial. It did not cut the radio just as attach completed.
2. **The timing points to a timer, not to a network reject (inference, strong).**
   - The first real `+ECSQ` (cell measured, rsrp −467/4 ≈ −117 dBm) came at 20932 ms. The denial came at 35915 ms: **14.98 s later**.
   - That equals **T3410 = 15 s**, the NAS attach guard timer. T3410 starts when the ATTACH REQUEST is handed down, just after cell selection.
   - A network ATTACH REJECT, at any cause, arrives within about 1 s of RRC set-up. Stock got its ATTACH ACCEPT 0.93 s after its first real ECSQ.
   - So most likely the modem sent an attach and received **no answer at all**: no ACCEPT, no REJECT, possibly not even an RRC connection. It then reported the expiry as stat 3.
3. **The stock transcript largely rules out the APN / PDN-type / IMS theory.**
   - The stock RIL sends **no** `AT+EIAAPN`, `AT+EAPNSET` or `AT+CGDCONT=` before its first `AT+EFUN=1`. The modem attaches at +1.98 s on its NV defaults (`+EIAREG: ME ATTACH "reseller.mnc410.mcc310.gprs",IP,0`).
   - EAPNSET/EIAAPN come only at 19:54:33, 21 s later, as part of the data set-up.
   - Our modem reads the same NV (the overlay serves Android's last copy). It reported the same `+ECEMODE: 1` and `+ECEVDP: 3`, and the same `+ERAT: 0,0,6,4,0`.
   - A wrong attach APN would also give a fast ESM reject (EMM #19), not a 15 s silence.
4. **Leading hypotheses, untested.** Two classes are left:
   - **H-A, the uplink never reaches the eNB:**
     - no PRACH or RRC set-up because some TX-side dependency is missing under XNU (PA supply, front-end control, a pin mux);
     - or AP-supplied state the modem waits on before transmitting.
   - **H-B, a stock pre-radio command that the modem's NAS needs:**
     - the best candidate is **`AT+EDALLOW=1`**. [LIB] 0x12fd1b says "Single SIM project, send EDALLOW=1 at boot time", and stock sends it twice before EFUN=1;
     - next is `AT+EGRAT=14` (capability switch, sent every boot just before EFUN=1).
   - The EFUN=0 behaviour (§3) fits H-A slightly better (inference): a switch-off DETACH that cannot get an RRC connection stalls.
5. **md2e3 could not tell H-A from H-B.** We enabled no RRC-state URC (`+CSCON`), no cause-bearing registration format (`CEREG=3` / `EREG=3`), and we did not query `AT+CEER`.
   - The next run must add those read-only diagnostics.
   - It should also add the stock-every-boot volatile commands, `EDALLOW=1` first.

---

## 1. Stock commands we do not send, by purpose ([RIL] lines 1-340, `+EIND: 128` → first `+EIAREG`)

Verdicts on "needed for LTE attach on AT&T":
- **R**: required or very likely needed.
- **L**: likely or possibly needed (cheap, stock-identical).
- **D**: diagnostic only (add for visibility).
- **I**: irrelevant to attach.
- **X**: do not send (NV/config write, IMS, calls, SMS, SS).

### 1.1 Data / PS permission and capability (before EFUN=1)

| Stock command | Verdict | Note |
|---|---|---|
| `AT+EDALLOW=1` (10.907 and 11.017) | **R/L** | Data allowed on this SIM. [LIB] "Single SIM project, send EDALLOW=1 at boot time" (RmcDcMiscHandler). The modem default without it is unknown; on MTK multi-SIM stacks PS may stay barred **(guess)**. Runtime state: the RIL resends it every boot. |
| `AT+EGRAT=14` (12.391, just before EFUN=1) | **L** | `RmcCapabilitySwitchRequestHandler::sendEGRAT` ("raf=%d, rat=%d"). Sent every boot with the same value, so it is stock-equivalent even if persisted. |
| `AT+EBOOT=0`, `AT+EPOC` (12.027 / 12.054) | **L** | Already whitelisted, never sent. Sent every boot just before EFUN=0. Meaning unknown. |
| `AT+ESADM=1`, `AT+EDSBP=0`, `AT+EMPPCH=1`, `AT+EGTP=1`, `AT+EDSDA=1` (CME 100) | I (2nd tier) | DSDA/DSBP/multi-PS settings for dual SIM. Keep for a later attempt only if the first stays denied. |
| `AT+ES3G?`, `AT+EPSB?` (→1023), `AT+ESBP=7,"SBP_T_PLUS_W"` (→1), `AT+ESBP=7,"SBP_GEMINI_LG_WG_MODE"` (→0) | D | Reads. `EPSB?` is worth one read. The ESBP=7 strings are >20 chars, over the at_send limit; skip them. |
| `AT+EFUN=0` → `AT+EFUN=1` | (sent) | Same as ours. Stock uses plain `AT+EFUN=1` (single SIM, bitmask 1). There is no dual-SIM variant to worry about. |

### 1.2 Attach APN / PDN (initial attach)

| Stock command | Verdict | Note |
|---|---|---|
| `AT+EIAAPN`, `AT+EAPNSET`, `AT+CGDCONT=` | **not sent before attach by stock** | Stock attaches with the modem's own default (empty APN → network assigns `reseller.mnc410.mcc310.gprs`, PDN type IP). EIAAPN/EAPNSET come at 33.0 s, after attach, for data. They write the modem APN table (NV), so they are X for registration and Stage 3 only. |
| `AT+CGDCONT=?` | I | Test command. |
| `AT+EIAREG=1` (10.927) | **D** | Turns on the `+EIAREG: ME ATTACH "<apn>",<type>,<cid>` URC: the definitive "EPS attached with default bearer" signal. |
| `AT+PSBEARER=1` (11.004) | D | `+PSBEARER: 4096,3,0,0` on attach. |
| `AT+ECCAUSE=1` (10.935) | **D** | Cause URC (`+ECCAUSE: 0,1,0` seen at stock detach). [LIB] "fail to get ECCAUSE=1 response" in the data code. Volatile **(guess)**. |

### 1.3 UE mode / voice domain / IMS

| Stock command | Verdict | Note |
|---|---|---|
| (none) | — | Stock sends **no** `AT+CEMODE=` or `AT+CEVDP=`. The modem reports `+ECEMODE: 1`, `+ECEVDP: 3` exactly as in ours, and attaches. Voice-centric with no AP IMS stack does not stop the first attach in stock either: IMS registers much later. A voice-centric UE disabling E-UTRA would show `+CEREG: 1` first, then a drop; we never saw stat 1. **Do not change CEMODE.** |
| `AT+EIMSPDN= "onoff", 0`, `AT+EIMSESS=1`, `AT+CIREG=2`, `AT+CIREP=1`, `AT+EEPDG=0`, `AT+EIWLPLEN =1`, `AT+ECFGSET="report_eimsreginfo_urc","1"`, `AT+ECFGSET="mtk_ct_volte_support","0"`, `AT+EMULTIIMS?`, `AT+EVOCD=1`, `AT+EWMPOLICY=3` | X | IMS / ePDG / Wi-Fi calling. `EIMSPDN onoff 0` is "AP does not want the modem's IMS PDN", the safe default. |
| `AT+ECSRA=1`, `AT+CSRA?` | I | CS/PS RAT availability report. |

### 1.4 Carrier configuration (SBP/ECFG)

| Stock command | Verdict | Note |
|---|---|---|
| `AT+ECFGSET="disable_cs_call","0"`, `"disable_cs_ss","0"`, `"disable_sms","0"` | X (I) | Writes "not disabled", which are the defaults **(guess)**. No bearing on EPS attach. |
| `AT+ESBP=5,"SBP_ETWS_FOR_AOSP_DESIGN",1`, `AT+EGMC=1,…` | X | Feature bits (ETWS, STK, ECC). |
| `AT+EOSID=1,"…"`, `AT+EGMR=…` | X | OS id / IMEI-class reads/writes. Never. |

### 1.5 Registration / signal reporting (all volatile URC configuration)

| Stock command | Verdict | Note |
|---|---|---|
| `AT+EREG=3`, `AT+EGREG=3` (already whitelisted) | **D** | MTK extended registration. [LIB] 0x11315b: fields `stat, lac, ci, eAct, nwk_existence, roam_indicator, cause_type, reject_cause`. **This gives the reject cause.** |
| `AT+CSCON=3` | **D (decisive)** | `+CSCON: 1,7,3` = RRC connected on E-UTRAN. If it never appears, nothing was ever heard on the uplink (H-A). |
| `AT+CEREG=3` (in [LIB] 0x12247a) | **D** | 27.007 format with `<cause_type>,<reject_cause>`. Use it instead of CEREG=2. |
| `AT+ECSQ=1` | (sent) | |
| `AT+CTZR=3`, `AT+EOPS=3,2`, `AT+EOPS=6,1`, `AT+EMCCMNC=0`, `AT+ECELL=4/7,1`, `AT+EONS=1`, `AT+ECAMPUI=1`, `AT+ECAPABILITY=1`, `AT+EPHYCONFIG=2`, `AT+E5GUW/E5GUC=1`, `AT+ECPI=…`, `AT+CMER=1,0,0,2,0` | I | Display/reporting. |
| `AT+COPS=3,3` | I (X by our rule) | Format-only set, harmless, but our stop list bans every `COPS=`. |

### 1.6 Calls, SS, SMS, STK, misc. (all X)

`ATE0Q0V1`, `AT+CSSN`, `AT+CMOD`, `AT+COLP`, `ATS0=0`, `AT+CCWA`, `AT+CUSD`, `AT+CMUT`, `AT+CLIP`, `AT+CNAP`, `AT+EAIC=3` (RmcImsCCReqHandler, call control), `AT+ESSP`, `AT+CSCS`, `AT+ECCP`, `AT+EECCFC`, `AT+EAPC?`, `AT+ICCID=1` (identifier URC), `AT+EINFO=…`, `AT+EIUSD`, `AT+EDALLOW` (see 1.1), `AT+EFD=…` (fast dormancy, after EFUN=1), `AT+ESLOTSINFO`, `AT+EPINC`, `AT+EXCGLA?`, `AT+ECGLA=?`, `AT+EMDVER?`, `AT+EXCAPCFGGET`, `AT+ECAL?`, SMS init after `+EIND: 1`.

`AT+ERAT=6,4` (sent after EFUN=1) matches the NV value we already read (`+ERAT: 0,0,6,4,0`). Not needed.

---

## 2. What `+CREG: 3` meant and whether we stopped wrongly

### Timeline (md2e3, ms since power-on; stock offsets from its EFUN=1)

| ours | event | stock |
|---|---|---|
| 20373 | `AT> AT+EFUN=1` | 0 |
| 20384 | ch38 ×1, ch34 ×5 (EL1→WMT coex, msg 0x80000052/46/50) | — |
| 20440 | `+CREG: 2`, `+CGREG: 2`, OK (67 ms) | OK 61 ms |
| 20605 | `+ECSQ: 255,…` (no cell) | +0.73 s |
| 20932 | first real `+ECSQ` rsrp −467/4 | +1.07 s (−455/4) |
| 21050 | CCIF RCHNUM bit 20 (0x100000), once; not handled or understood | ? |
| 25470/30585/35491 | `+CEREG: 2,2` | — |
| — | (no `+CSCON` reporting enabled: RRC state unknown) | `+CSCON: 1,7,3` +1.05 s |
| — | — | `+EIAREG: ME ATTACH` +1.98 s |
| **35915** | `+CREG: 3,"9005","05095510",7` **and** `+CEREG: 3,"9005","05095510",7` | — |
| 35927 | `AT> AT+EFUN=0` | |
| 35927-50931 | lines buffered, not logged (see §3); they included `ME DETACH` and a `+CEREG` with stat 4 | |
| 50931 | EFUN=0 TIMEOUT; EFUN? → 0 | |

### Conclusions

- **CS-only denial on a combined attach would look different.** The UE would be EPS-attached (`+CEREG: 1`, `+EIAREG: ME ATTACH`) with `+CREG` 3 or 0. Here CEREG itself went 2 → 3 in the same ms as CREG, with the same TAC/CI.
- [CODE] `at_note` sets `creg_stat` only from `+CEREG`. So the stop was triggered by the EPS line, correctly.
- **EPS was not attached.** With CEREG=2 a stat-1 URC would have been printed. URCs outside a command wait are all logged, and none came. URCs inside the 12 ms query waits are logged by at_finish as `AT< …` lines, and none appeared either.
- **T3410 timing (inference).** cell measured 20932 → denial 35915 = 14.98 s.
  - Under 24.301, T3410 expiry gives attach attempt counter +1 and T3411 (10 s), then a retry, with up to 5 attempts and then T3402.
  - So "stat 3" after a single expiry is MTK's choice of report. The modem would probably have retried 10 s later.
  - **Stopping on the first stat 3 cut that retry.** The next run must keep listening (§4.4).
- **Was the stop drawn too early?** Yes, for this one reason: a single stat 3 is not final under 24.301 unless the reject cause is a permanent one (#3, #6, #7, #8). We did not know the cause, because no cause-bearing format was enabled.
- **CS +CREG.** Record it; never act on it (§4.5).

### Small code defects seen while reading [CODE] (for the implementer)

1. **`at_step` timeout drops the buffered response.** On a timeout `at_resp` is never logged. Every URC that arrived during the 15 s EFUN=0 wait is lost (ME DETACH, the stat-4 CEREG, anything else). **Log `at_resp` on timeout.**
2. **`result_2e2` "hold reached"** uses `rp >= RP_HOLD`, which is true for RP_OFF/RP_END. md2e3 printed "hold reached" and "held yes" although registration never happened. Use a flag set on entering RP_HOLD.
3. **`cereg_stat_of`** picks field 2 if it starts with a digit. That is right for a query response (`+CEREG: <n>,<stat>`). It is wrong for a URC whose TAC is unquoted, which some firmware sends.
   - Decide by context: field 2 only when the line is part of the response to `AT+CEREG?`, field 1 otherwise.
   - With `CEREG=3` the query response is `+CEREG: 3,<stat>,…`, which is still field 2.

---

## 3. EFUN=0: no final result in 15 s

**Facts**
- `AT+EFUN=0` was sent at 35927 ms, 12 ms after the stat-3 URC.
- No final result arrived in 15 s.
- No "late final" line came afterwards. The `AT+EFUN?` response held `+EFUN: 0` and its own OK, with no stray OK in front.
- **So the modem never sent a final result for that EFUN=0.**
- During the wait it did emit a line containing `ME DETACH` and a `+CEREG` line with stat 4 (from `detach_seen` and RESULT's "reg stat 4"; [CODE] at_note runs on buffered lines too). The radio did go off (`+EFUN: 0`).

**Stock**
- `AT+EFUN=0,2` while attached: `+CGEV: ME DETACH` +12 ms, `+CSCON: 1,7,3` +64 ms (RRC up to send the DETACH REQUEST), `OK` +206 ms.

**Reading (inference)**
- The switch-off was issued while NAS was mid-attach-attempt (T3410 just expired, T3411 running). An MTK modem completes EFUN only after its switch-off detach handling ends.
- If it cannot get an RRC connection (H-A), that handling waits for RRC establishment failures before a local detach. This can exceed 15 s, and the final was then dropped or folded into the radio-off.
- Under H-B the final should have come quickly, as in stock. Weak evidence, but it leans toward H-A.

**Off command and timeouts for the next run**
1. Keep plain `AT+EFUN=0` (it worked: `+EFUN: 0`). Do not use `AT+EFUN=0,2`: the second parameter is an undocumented reason code, and stock used it only for an internal radio cycle.
2. Never send EFUN=0 while another command is outstanding (already the case).
3. EFUN=0 timeout **30 s** (was 15). Log every buffered line on timeout.
4. After a timeout, stay silent for **3 s**, logging any late final (it names the previous command). Then `AT+EFUN?`.
5. `+EFUN: 0` counts as confirmed off whether or not EFUN=0 returned OK.
   - If EFUN? is not 0: one more EFUN=0 (30 s), then EFUN?, then MD1 power-down regardless.
   - Keep the existing 240/255 s caps. The worst case is 36 + 30 + 3 + 30 + … s, which is still inside 240 s.
6. **Better still, avoid sending EFUN=0 mid-attempt.** After a denial, wait for the modem to settle (§4.4) and send EFUN=0 from a quiet state.

---

## 4. Spec for the next attempt (Sonnet implements in redwood_md.c only; the main session runs it)

### 4.1 Whitelist additions (`at_white`; every entry is ≤ 20 chars)

```
"AT+EDALLOW=1", "AT+EGRAT=14", "AT+CEREG=3", "AT+CSCON=3", "AT+EIAREG=1",
"AT+PSBEARER=1", "AT+ECCAUSE=1", "AT+CEER", "AT+EREG?", "AT+EGREG?",
"AT+CSCON?", "AT+EPSB?", "AT+CGREG?"(already), "AT+CREG?"(already)
```

`AT+EBOOT=0`, `AT+EPOC`, `AT+EREG=3` and `AT+EGREG=3` are already whitelisted.

### 4.2 Sequence

Phase 1 (unchanged up to EFUN=0, then insert):

```
AT, ATE0, AT+CMEE=1, AT+ESIMS=1, AT+ESIMS,
AT+EDALLOW=1                     (stock: before EFUN=0; "single SIM: send at boot")
AT+EBOOT=0, AT+EPOC              (stock: right before EFUN=0)
AT+EFUN=0                        (SIM power; 15 s as now)
AT+CPIN?, AT+EFUN?, AT+CFUN?, AT+CGSN, AT+CGMR, AT+ERAT?, AT+CEMODE?, AT+EPSB?
[gates]
```

Phase 2 (`at_seqA`):

```
AT+CREG=2, AT+CGREG=2, AT+CEREG=3, AT+EREG=3, AT+EGREG=3,
AT+CSCON=3, AT+EIAREG=1, AT+PSBEARER=1, AT+ECCAUSE=1, AT+ECSQ=1,
AT+EGRAT=14                      (stock: the command right before EFUN=1)
AT+EFUN=1                        (10 s timeout as now)
```

Error handling:
- Any of the URC enables returning ERROR or CME: log it and continue (not a stop).
- `AT+EDALLOW=1` or `AT+EGRAT=14` returning an error: log it and continue.
- Only `AT+EFUN=1` failing stops the run (as now).

### 4.3 What counts as registered

- **EPS registered** = `+CEREG` stat 1 or 5 (URC or query), **or** `+EREG` stat 1/5 with eAct LTE, **or** `+EIAREG: ME ATTACH`. Record which came first and its ms.
- Log TAC/CI, cause_type and reject_cause in full. They are not personal.
- `+CREG` / `+CGREG` (CS / GPRS domain): record the stat and never act on it. A `+CREG: 3` while EPS is registered is logged as "CS denied, EPS ok", not a stop.

### 4.4 Registration wait (90 s from EFUN=1, as now) and denial handling

- Poll `AT+CEREG?` every 5 s and `AT+CSQ` every 10 s (as now). Also `AT+CSCON?` at +5 s and +15 s.
- **On EPS stat 3, do not stop at once.**
  1. Send `AT+CEER`, `AT+CEREG?`, `AT+EREG?`, `AT+EGREG?`, `AT+CSCON?` (read-only), and log all of it.
  2. If the reject cause (from CEREG=3 / EREG=3 / CEER) is one of #3, #6, #7, #8 (illegal UE / illegal ME / EPS not allowed / EPS and non-EPS not allowed), the UE itself will stop: go to the off path.
  3. Otherwise keep listening until 90 s from EFUN=1. This covers T3411 retries, which are exactly what a phone does. Each later stat 3 gets the same diagnostics, at most 3 rounds.
- No registration by 90 s → the same diagnostics, then the off path.
- **The result line must state whether `+CSCON: 1` was ever seen**, with the time of the first one. That is the H-A/H-B discriminator.

### 4.5 When registered

1. `AT+COPS?`, `AT+CSQ`, `AT+CGATT?`, `AT+CEREG?`, `AT+EREG?`, `AT+CSCON?`.
2. Hold 60 s, `AT+CSQ` every 10 s (as now).
3. Off path (§3).

With `r1_md_data=1` (Stage 3, user-approved): after 30 s of stable registration, hand over to the data spec instead of the off path (§4.8).

### 4.6 Off path (§3)

1. EFUN=0, 30 s.
2. Log buffered lines on timeout.
3. 3 s of silence (late finals logged).
4. `AT+EFUN?`.
5. If not 0: one more EFUN=0 and EFUN?.
6. MD1 power-down always.

Caps unchanged: 240 s → off path; 255 s → power-down; `md_ms` 400 s; quiesce.

### 4.7 Logging and result

- Log `at_resp` on any timeout (§2 defect 1).
- Fix "hold reached" / "held" (§2 defect 2) and the CEREG field choice (§2 defect 3).
- Let these prefixes through `at_safe` unmasked: `+CSCON`, `+EIAREG`, `+PSBEARER`, `+ECCAUSE`, `+CEER`, `+CGEV`, `+EPSB`. None carry identifiers.
- Keep `+ICCID`, `+CIMI`, `+ESIMAPP`, `+ESIMIND`, `+EGMR`, `+CGSN` masked (as now).
- RESULT 2e-2 additions:
  - `cscon1_ms` (−1 if never);
  - `eps_stat_first1_ms`;
  - `eiareg apn/type/cid`;
  - `reject cause_type/cause` (last);
  - `ceer "…"` (first 60 chars);
  - number of stat-3 events;
  - `efun0_ms_to_final` (or timeout).
- Log CCIF RCHNUM bits ≥ 16 whenever they are new (md2e3 had bit 20 once at 21050 ms; meaning unknown **(guess: CCB/DHL log-buffer wake)**). Read-only, no ack change.

### 4.8 Data (only with `r1_md_data=1`, user-approved 2026-11-17 23:10)

- **None of the pre-radio commands above sets up a bearer.**
- **The data bearer is the attach default bearer.** On LTE the default EPS bearer (cid 0) comes up as part of the attach (`+EIAREG: ME ATTACH "reseller.mnc410.mcc310.gprs",IP,0`, `+PSBEARER: 4096,3,0,0`). So `EDALLOW=1` (PS allowed) and `EIAREG=1` / `PSBEARER=1` (reporting) are the only pre-radio pieces the data path depends on.
- Stock then binds that bearer for the AP with no new PDN request (19:54:33.082, after `EAPNLOCK/EAPNSET×n/EIAAPN`): `AT+EAPNACT=1,"RESELLER","default",0,,"dnn=RESELLER",,0` → `+CGEV: ME PDN ACT 0`.
- It then queries `AT+CGCONTRDP=0` (IP/DNS) and `AT+EPDN=…` (interface binding).
- **Minimal path for the first data run:**
  1. `AT+CGCONTRDP=0`, `AT+CGACT?` (both read-only).
  2. `AT+EAPNACT=1,"RESELLER","default",0,,"dnn=RESELLER",,0`. It is >20 chars, so the length limit needs a per-entry exception.
  3. Skip `EAPNSET` / `EIAAPN`: they rewrite the modem APN table (NV), and the attach already used the right APN.
  4. Then the CLDMA/ccmni work of docs/cellular-stage3.md §C.

### 4.9 Stop list (unchanged plus clarifications)

- No `ATD*` / `ATA` / `CHLD` / `CHUP` / `ATH`.
- No SMS commands.
- No SS/USSD.
- No `CPIN=` / `CSIM` / `CRSM` / `CGLA` / STK responses.
- No `COPS=` (any), `CFUN=` (any), `CGATT=`, `CEMODE=`, `CEVDP=`, `ERAT=`, `EPBSE*`, `ESBP=` (set), `ECFGSET=`, `EGMC=1`, `EGMR=1`, `EIMS*`, `EVOLTE*`, `EEPDG`, `EWFC*`, `EBOOT=1`, `EFD=`, `ERFTX`/`ESAR*`/`ETXPWR*`, `EMETACFG`.
- Data commands (`EAPNACT`, `CGCONTRDP`, `EPDN`, `CGACT?`) only under `r1_md_data=1`.
- `CGACT=` / `CGDATA` / `CGDCONT=` / `EAPNSET` / `EIAAPN`: still not.

---

## 5. Regulatory / safety

| Added command | Persistent? | RF / band effect | Stock does it every boot? |
|---|---|---|---|
| `AT+EDALLOW=1` | runtime (RIL resends each boot) **(guess)** | none (PS permission) | yes, twice |
| `AT+EGRAT=14` | possibly NV **(guess)** | RAT capability set; same value Android writes every boot, so no change to bands or power | yes, right before EFUN=1 |
| `AT+EBOOT=0`, `AT+EPOC` | unknown | none known | yes |
| `CEREG=3`, `EREG=3`, `EGREG=3`, `CSCON=3`, `EIAREG=1`, `PSBEARER=1`, `ECCAUSE=1` | URC configuration, session only (27.007 says `<n>` is not stored) | none | yes (CEREG=3 is in [LIB]; stock uses EREG=3) |
| `AT+CEER`, `EREG?`, `EGREG?`, `CSCON?`, `EPSB?` | read | none | (queries) |

- Any NV write by the modem goes to the RAM overlay. Gate 3 still stops on any CALIBRAT/NVD_IMEI write.
- Nothing added changes TX power, SAR, band masks or RAT preference beyond the stock-identical `EGRAT=14`.
- Continuing to listen after a stat 3 (§4.4) means the modem may retry the attach (PRACH + ATTACH REQUEST) 2-4 more times within 90 s. That is the standard UE behaviour on every phone, at the calibrated power.
- **If H-A is true**, the modem's TX path is not behaving as in stock, possibly transmitting PRACH that the eNB never decodes. That would be a reason to look at the TX-side hardware state before more radio runs:
  - read-only, under Android vs. XNU: MT6357 VPA buck enable/voltage registers, PMIC-wrapper arbitration/MD-master enables, and the GPIO mode of the BPI/MIPI-RFFE pins;
  - and confirm no XNU driver (camera, audio, connsys) re-muxes them.
  - This does not make the modem exceed stock power: power control is inside the modem with its calibration. It is a correctness concern, not an over-power one.

---

## 6. What confirms or refutes the diagnosis

| Next-run observation | Meaning |
|---|---|
| `+CEREG: 1/5` and `+EIAREG: ME ATTACH` within a few s, with `EDALLOW=1` / `EGRAT=14` added | **H-B confirmed**: a missing stock pre-radio command (most likely EDALLOW). A later run can drop `EGRAT=14` to isolate it, if wanted. |
| `+CSCON` never 1, stat 3 again at ≈15 s after the first real ECSQ, reject_cause 0/absent, CEER "no cause" | **H-A** (no uplink, T3410). Stop radio runs and compare the TX-side hardware state under Android vs XNU (§5). |
| `+CSCON: 1,7,3` seen, then stat 3 within ~1-2 s with reject_cause N | Network reject. Decode N: #19 ESM failure → APN/PDN (then the EIAAPN question becomes real, Stage-3 approval); #7/#8 → subscription/EPS not allowed; #15 → TA; #111 → protocol. |
| `+CSCON: 1` seen, then stat 3 at ≈15 s with no cause | Uplink OK, network silent after RRC: authentication/security stall **(guess)**. Look at the SIM AUTHENTICATE path (modem-internal) and the ch34/ch38/CCIF bit-20 events. |
| `+CEREG: 1`, then drop to 2/0 within seconds | Voice-centric E-UTRA disable (A.4 of cellular-stage3.md). Then consider `AT+CEMODE=2` behind a separate approval. |
