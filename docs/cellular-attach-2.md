# Cellular attach 2: CCIF bit 20, the attach stall, and the spec for radio attempt 3

Written 2026-11-18. Analysis only: no code edits, no builds, no flashing. The Android checks were read-only adb reads.
Inferences are marked **(inference)**. Guesses are marked **(guess)**.

Sources:
- **[LOG]** logs/md2f4.log (attempt 2), md2e3.log (attempt 1), md2f3.log (2e-1 only, no radio).
- **[CODE]** xnu/files/bsd/dev/redwood_md.c: the poll loop at ~2593-2657, drain_one/drain_rings at 1879-2037, ring_tx at 792.
- **[K]** haretic eccci (`drivers/staging/mtk-ccci/eccci/`):
  - hif/ccci_hif_ccif.c (md_ccif_isr 1312, md_ccif_launch_work 1271, md_ccif_handle_exception 1237, md_cd_ccif_isr 1863);
  - mt6765/ccif_hif_platform.h;
  - modem_sys1.c (md_fsm_exp_info 246);
  - hif/ccci_hif.c (ccci_hif_state_notification 316);
  - fsm/ccci_fsm.c 385-387;
  - hif/ccci_hif_cldma.c (cldma_start 2149);
  - port/port_cfg.c, inc/ccci_hif.h.
- **[OLD]** MediaTek's MT6735 eccci (github profglavcho/mt6735-kernel-3.10.61, `eccci/modem_cldma.c`). It is the CLDMA-era origin of the PEER_WAKEUP channel.
- **[DUMP]** xnu-deps/build/modem-blobs/evidence/ccci_dump.txt (stock). Its HISTORY region covers the stock attach at 58.48-60.44 s.
- **[RIL]** stock-ril-boot-at.txt.
- **[DEV]** live read-only reads on the r1 under Android (2026-11-18 00:40, uptime 6-7 min, `gsm.network.type` LTE, in service).

---

## 0. Summary

1. **Bit 20 is `AP_MD_PEER_WAKEUP`.**
   - [K] ccif_hif_platform.h: `RINGQ_EXP_BASE 15`, `AP_MD_PEER_WAKEUP (RINGQ_EXP_BASE+5)` = bit 20. It is also in `CCIF_HW_CH_RX_RESERVED` (bits 15 and 20). `AP_MD_SEQ_ERROR` is bit 21, not 20.
   - It is the modem's "wake the AP" signal. Its wakelock is named **`md1_cldma_peer`**; in the CLDMA era it was the companion of CLDMA data traffic ([OLD]).
   - Stock does no protocol work on it. The second CCIF IRQ handler `md_cd_ccif_isr` **acks bits 15..30** (`ACK = RCHNUM & (0xFFFF << 15)`), then `md_fsm_exp_info` takes a 1 s wakelock (`__pm_wakeup_event(peer_wake_lock, HZ)`). Nothing is sent back.
   - **Our code never acks it.** We ack only `rch & 0xFFFF`. So bit 20 stays latched in RCHNUM, and the modem-side BUSY bit for 20 stays set for the rest of the run.
2. **Stock never raises bit 20.** [DEV], fresh boot that attached on LTE:
   - wakeup source `md1_cldma_peer` event_count **0**;
   - CCIF irq1 (GIC 149) **0** interrupts;
   - `/proc/ccci_dump` isr_cnt for bit 20 = **0**. [DUMP] (the earlier boot) also has rxq20 = 0.
   - Under XNU it came **once in each radio-on run**, ~120-160 ms after the first real cell measurement (md2e3 21050 ms; md2f4 21878 ms = CSCON:1 + 161 ms). It never came in md2f3 (no radio).
   - So it is a modem reaction to something different about our AP side, at the start of the attach.
3. **The attach failed by T3410, measured to the millisecond.** `+CSCON: 1,7,3` at 21717 ms and `+CEREG: 3` / `+CSCON: 0` at 36725 ms: **15.008 s**.
   - T3410 starts with the ATTACH REQUEST, which goes out in RRCConnectionSetupComplete. It stops only on ATTACH ACCEPT or REJECT.
   - So NAS never received an ATTACH ACCEPT/REJECT, although the RRC connection was up the whole time.
   - `+ECCAUSE: 0,1,0` + `+PSBEARER: 4096,0,0,0` is **exactly the pair stock prints on an ordinary detach** ([RIL] 19:54:25.588). It is not a failure cause. EREG cause fields are 0,0 and CEER is `0,NONE`.
4. **Most likely reason (inference, not proven):** the modem asked the AP for something at the start of the attach (bit 20) and never got the stock answer. Two answers are possible, and only one of them is stock-cheap:
   - **H1, the ack.** In a wake handshake, the AP's ack (MD-side BUSY clearing) *is* the "AP is awake" reply. Stock acks within microseconds. We never ack. If the modem's user-plane/HIF task waits on that before it finishes bearer/DRB setup, the ATTACH ACCEPT is never delivered to NAS. Fix = 1 line.
   - **H2, CLDMA down.** Stock starts CLDMA (rings, RGPDs, SO_START, IRQs) at `BOOT_WAITING_FOR_HS1`, *before* MD power-on ([K] ccci_fsm.c 385-387 → ccci_hif.c 316-326 → cldma_start). We never start it. The modem's data path (LHIF → CLDMA) may need the AP DL queue to be live when the default bearer/DRB is set up. Bit 20 would then be the modem trying to wake an AP whose CLDMA queue looks asleep. Stock never sees bit 20 because its queue is always running.
   - Both fit "stock never raises it". H1 is testable for free; H2 needs ~200 lines of DMA code.
5. **Attempt 3:**
   - ack every RCHNUM bit, as stock's two ISRs do together;
   - per-bit accounting;
   - read-only CLDMA and CCIF snapshots around bit 20;
   - FS requests in the attach window made visible (today they are invisible: the 400-request log cap was hit before radio-on);
   - a log budget that cannot lose the RESULT lines.
   - CLDMA RX bring-up is specified (§5b) behind `r1_md_cldma`, default 0, for attempt 4 if attempt 3 does not attach.

---

## 1. Question 1: CCIF D2H bit 20, and our handling of every RCHNUM bit

### 1.1 The channel map on MT6765 ([K] mt6765/ccif_hif_platform.h)

| bit | D2H name | stock handling |
|---|---|---|
| 0-7 | D2H_RINGQ0..7 (bit 7 also AP_MD_CCB_WAKEUP) | irq0 `md_ccif_isr`: ack low 16, queue rx work per bit, or `md_ccif_check_ringbuf` for queues without a bit |
| 15 | D2H_SRAM (HS1/runtime data) | irq0: ack, `ccif_sram_work` |
| 16 | D2H_EXCEPTION_INIT | irq1: ack, FSM exception |
| 17-19 | EXCEPTION_INIT_DONE / CLEARQ_DONE / ALLQ_RESET | polled during the exception handshake |
| **20** | **AP_MD_PEER_WAKEUP** | **irq1 `md_cd_ccif_isr`: `ACK = RCHNUM & (0xFFFF<<15)`, then `md_fsm_exp_info`: `__pm_wakeup_event(md1_cldma_peer, 1 s)`, bit dropped** |
| 21 | AP_MD_SEQ_ERROR | irq1: ack, "MD check seq fail", CCIF register dump |

- **irq0 vs irq1.** `md_ccif_isr` (irq0, "ccif0") acks only `ch_id & 0xFFFF`; for bit 20 it only bumps `isr_cnt[20]`.
- The ack of bits ≥15 comes from `md_cd_ccif_isr` (irq1, "ccif1"). Its comment reads "don't ack data queue to avoid missing rx intr".
- **Stock acks bit 20; we do not.**
- **[OLD] origin.**
  - In MT6735 `modem_cldma.c` the AP raised `AP_MD_PEER_WAKEUP` to the MD after kicking a CLDMA UL transfer (`md_cd_ccif_send(md, AP_MD_PEER_WAKEUP)` after UL_RESUME, line 1815).
  - The MD-to-AP direction was a separate `MD2AP_PEER_WAKEUP` register, cleared by the AP on CLDMA RX_DONE (line 730).
  - The AP-side handling was the same 1 s `peer_wake_lock` ("md%d_cldma_peer").
  - Gen93 moved the MD→AP wake onto CCIF bit 20. So PEER_WAKEUP is the wake signal of the CLDMA data path **(inference from the names and the old code)**.

### 1.2 Can leaving it unserviced stall the modem?

- **The AP-side protocol needs nothing:** no reply message, no state change, only the ack.
- **The modem's side is not visible.** If the MD treats "my CCIF BUSY bit 20 cleared" as "AP is awake" and waits for it before proceeding, an un-acked bit 20 blocks that task forever **(guess)**.
- It also stops the modem from raising bit 20 again. The T3411 retry (10 s later) could not wake the AP either. It is also invisible to us, because the bit is already latched and is in `seen`.
- Probability that the ack alone fixes the attach: **moderate** **(guess)**. It is the only stock behaviour on this event that we omit, and it costs one line.

### 1.3 Every RCHNUM bit we have seen ([LOG] md2e3, md2f3, md2f4)

| bit | first seen | acked by us | handled |
|---|---|---|---|
| 15 | 64 ms (HS1) | yes | read_hs1, runtime data |
| 1 | 199 ms | yes | ring drain (poll) |
| 4 | 377 ms | yes | ring drain (FS) |
| 0 | ~1550 ms | yes | ring drain (HS2/ctrl) |
| 6 | ~1600 ms | yes | ring drain (stock also has rxq6 isr_cnt = 1) |
| 5 | ~2550 ms | yes | ring drain (AT) |
| **20** | 21050 / 21878 ms, radio runs only | **no, latched for the rest of the run** | logged once |

- Bits 7, 16-19, 21-23 never came. **No SEQ_ERROR (bit 21) ever**, so the modem accepts our TX sequence numbers.
- We never fail to clear a low bit: `wr(ACK, rch & 0xffff)` runs each 10 ms poll before draining.
- Bits ≥16 are never cleared. For bit 16 that does not matter, because we stop on it.
- The `seen` mask hides every later edge of any bit. This makes the log useless for repeated events.

---

## 2. Question 2: what the AP must provide for LTE attach on MT6765

### 2.1 Facts

- **CLDMA carries only ccmni on gen93.**
  - [K] inc/ccci_hif.h: `MD_GENERATION 6293 → MD1_NET_HIF = CLDMA_HIF_ID`.
  - port_cfg.c: only `ccmniN` and `ccmni-lan` use it.
  - The normal CLDMA queues have 0 buffers (`normal_rx_queue_buffer_number = 0`).
  - No control, FS, RPC or AT port is on CLDMA. Nothing the attach needs *as data* travels over CLDMA.
- **Stock starts CLDMA unconditionally before MD power-on.**
  - `fsm_broadcast_state(BOOT_WAITING_FOR_HS1)` → `ccci_hif_state_notification` → `ccci_hif_start(CCIF)` + `ccci_hif_start(CLDMA)` → `ccci_md_start`.
  - `cldma_start` does the following:
    1. late init: rings of 512 RGPDs × 3584 B, 4 × 256 TGPDs;
    2. clock on, hardware reset;
    3. `cldma_reset`: DL_MTU = 3584, SO_CFG |= 1;
    4. TX start addresses (PD + AO backup) and the RX start address (AO);
    5. **`SO_START_CMD = all RX queues`** (TX started on the first send);
    6. L2/L3 interrupt unmask; state PWRON.
  - [DUMP] confirms the order: `cldma_plat_hw_reset` at 55.310 s, then "modem is starting" at 55.322 s.
- **There is no CLDMA handshake message.** No "HIF ready", port-enum or CCCI_STATUS exchange exists for CLDMA in [K]. The MD learns that the AP DL ring is live only from the hardware (SO_START, RGPDs with HWO).
- The kernel RPC **0x400D** (LHIF queue mapping) exists ([K] port_rpc.c). In our run no RPC of any kind came during the attach window (RPC lines are logged without a cap, and the window 21.2-36.7 s is inside the kept log). So the modem did not ask for 0x400D before failing.
- **Stock CLDMA during attach ([DEV], [DUMP]).**
  - CLDMA_AP irq 165 = 65 interrupts in 6 min. ccmni_md1 wakeup events 43.
  - [DUMP] `traffic(net)` rx counters stay 0 at 65/75 s, i.e. no DL packet during or right after the stock attach.
  - So stock's attach does **not** move data over CLDMA. If CLDMA matters, it matters as *state* (queue started, RGPDs owned by HW), not as traffic **(inference)**.

### 2.2 What the modem's view tells us

| Item | Reading |
|---|---|
| T3410 = 15.008 s from CSCON:1 | ATTACH ACCEPT never reached NAS. The stall is between ATTACH REQUEST and ATTACH ACCEPT: authentication, NAS SMC, AS SMC/UE capability, and the RRCConnectionReconfiguration that sets up the DRB and carries the ATTACH ACCEPT. |
| RRC up 15 s, released locally at T3410 | The eNB kept the connection: consistent with the MME retransmitting (T3460/T3450) to a UE that does not answer, or with an eNB waiting for RRCReconfigurationComplete **(inference)**. |
| `+ECCAUSE: 0,1,0` | Identical to stock's normal detach ([RIL] 19:54:25.588, after `AT+EFUN=0,2` while attached). Generic. Fields: probably type, group, cause **(guess)**. Not a reject. |
| `+PSBEARER: 4096,0,0,0` | 4096 = 0x1000 = the LTE AcT bit (the same value as EREG's eAct). 0 = no bearer. Stock reports `4096,3,0,0` at attach and `4096,0,0,0` at detach. |
| `+EREG: 3,"9005","05095510",4096,0,0,0,0` | eAct LTE, nwk_existence 0, roam 0, **cause_type 0, reject_cause 0**: no NAS cause. |
| `AT+CEER` → `0,NONE` | No cause recorded. |
| FS/RPC/IPC during attach | **RPC: none. sys msgs: none. New channels: none. ch34/38: only the 5+1 at EFUN=1. FS: unknown.** The FS per-request log hit its 400 cap before radio-on, and the FS summary was lost with the end of the log. |

**Stock attach window (only the channels its HISTORY region records; RPC/ch32-33 are not recorded):**

| time | event |
|---|---|
| 58.485 | EFUN=1 |
| 58.49-59.34 | ~50 FS ops (ch14: 0x1001 open, 0x1002, 0x1009, 0x1004 read, 0x1005 close, 0x1022 CMPT_READ, one 0x1003) |
| 59.564 | CSCON:1 |
| 59.86 | `+STKPCI` (SIM toolkit) |
| **60.05-60.43** | **FS: 0x1022, open/read/close, open/0x1003 (write)/close, open/read×n/close (~17 ops)** |
| 60.430 | PSBEARER 4096,3 |
| 60.438 | EIAREG ME ATTACH |

- **The modem does NVRAM file I/O inside the attach window** (open/read/write/close between RRC connected and ATTACH ACCEPT). This is presumably EPS NAS security context / EPSLOCI / LTE NV **(guess)**.
- Our FS server should answer it. **We cannot see whether it was asked, or what we answered.**
- This is the second blind spot (with bit 20), and attempt 3 must make it visible (§5d).

### 2.3 Verdict on "CLDMA must be up for attach"

- **Unproven, and plausible.**
- **For it:**
  - stock never runs without it;
  - the only abnormal signal we got (bit 20) is the CLDMA wake channel;
  - it came at attach start, not before;
  - the stall point (before ATTACH ACCEPT, i.e. DRB set-up) is where the user plane is first bound to a host interface **(inference)**.
- **Against it:**
  - nothing in the AP sources makes attach wait on CLDMA;
  - stock moves no CLDMA data during attach;
  - bit 20 at CSCON + 161 ms looks early for DRB set-up (stock needed 870 ms from CSCON:1 to PSBEARER). It is not impossible on a quiet cell.
- It remains the leading **structural** difference after the ack.

---

## 3. Question 3: other candidates, judged against the evidence

| Candidate | Verdict | Why |
|---|---|---|
| SIM authentication failure | **Unlikely** | USIM AUTHENTICATE is MD↔SIM only. A MAC/SQN failure gives AUTHENTICATION FAILURE (#20/#21), then either a network AUTH REJECT or a resync: both fast, and AUTH REJECT would show a cause in EREG/CEER. A hung SIM would show `+ESIMS`/`+EUSIM` URCs; none came. |
| NV/config making ATTACH REQUEST unacceptable | **Ruled out** | It would be a fast ATTACH REJECT with cause (#7/#15/#19/#111), not 15 s of silence with RRC up. |
| EDALLOW / data-allowed | **Ruled out** | Sent, OK. A barred PS would not even reach RRC/ATTACH. |
| IMS / voice-centric | **Ruled out** | It acts after attach (E-UTRA disable → CEREG 1 then a drop). We never reached stat 1. |
| Time / RTC | **Ruled out** | No attach step uses AP time. NITZ comes after. |
| MD waiting on AP power/thermal/battery RPC | **Ruled out for this window** | No RPC requests were logged between EFUN=1 and the stat-3 (uncapped logging, kept-log window intact). |
| Sequence-number error on our TX | **Ruled out** | That is CCIF bit 21 (AP_MD_SEQ_ERROR), never seen. Bit 20 is not it. |
| FS request during attach answered wrongly or late | **Open (blind spot)** | Stock does ~17 FS ops in its attach window, including a write. Ours were not logged (cap). A failing write would more likely give an MD assert than a silent stall **(guess)**, but it must be seen. |
| STK proactive command unanswered | **Low** | Stock gets `+STKPCI` 300 ms after RRC. We get none (STK URC not enabled). A pending proactive command does not block AUTHENTICATE (ETSI 102 221). |
| Bit 20 un-acked (H1) | **Open, test first** | §1.2. |
| CLDMA not started (H2) | **Open, test second** | §2.3. |

---

## 4. Question 4: Android (stock kernel), read-only, 2026-11-18 00:40, uptime ~7 min, LTE in service

| Read | Value |
|---|---|
| `/proc/interrupts` CCIF_AP_DATA GIC 148 (ccif0, md_ccif_isr) | 3001 in total |
| `/proc/interrupts` CCIF_AP_DATA GIC 149 (ccif1, md_cd_ccif_isr: bits ≥15 ack path) | **0** |
| `/proc/interrupts` CLDMA_AP GIC 165 | 65 |
| `/proc/ccci_dump` isr_cnt (non-zero) | q0 3, q1 92, q4 1445, q5 1479, q6 1, **bit15 1**, **bit20 0** |
| wakeup source `md1_cldma_peer` (bit-20 wakelock) | active 0, **event 0** |
| `ccci_fs` events | 1465 |
| `ccmni_md1` events | 43 |
| `md1_wakelock` | 2 |
| `/proc/ccci_dump` CLDMA lines | only `cldma_plat_hw_reset` ×3 at 55.47 s (the CLDMA start before MD power-on). No queue/peer lines. |

Notes:
- On this boot, the MD → AP bit-20 path never fired in stock, while a full LTE attach and data happened.
- Our q0/q1/q4/q5/q6/15 bit pattern matches stock's.
- An attempt to grep the md1img partition for MD-side strings was stopped. It was too slow, and the image is likely compressed. No data, nothing written. The stray grep process was killed.

---

## 5. Question 5: spec for the next step (Sonnet implements in redwood_md.c only; the main session runs attempt 3)

### 5a. Bit 20 (and all bits ≥15): do what stock's two ISRs do together

In the poll loop ([CODE] ~2620-2626) replace the ack with:

```c
rch = rd(ccif, APCCIF_RCHNUM);
if (rch) {
	wr(ccif, APCCIF_ACK, rch);     /* stock: irq0 acks rch & 0xFFFF, irq1 acks rch & (0xFFFF << 15): together, every bit */
	ccif_bits_note(rch, elapsed);  /* 5d: per-bit count / first / last; log every edge of bits >= 16 */
	...existing bit 15 / bit 16 handling unchanged...
}
```

- No other state change. Stock's only other action is a 1 s AP wakelock; our AP never sleeps during the run.
- Do **not** send anything to the MD on bit 20.
- Bit 21 (SEQ_ERROR): ack, log the CCIF register line (5d), and continue. Stock only dumps; it does not assert.
- Remove the `seen` gating of the "new bit" log for bits ≥16. Use the per-bit counters instead (5d).

### 5b. CLDMA RX bring-up (implement now behind `r1_md_cldma`, default **0**; turn on for attempt 4 only if attempt 3 does not attach)

- Minimal = what stock has live during attach: **the DL (SO) queue 0 started with RGPDs owned by hardware**. TX/UL is not needed for attach; leave it stopped.
- No interrupts: poll from the existing 10 ms loop.
- Register facts are from docs/cellular-stage3.md §C.2 (bases AO 0x10014000, PD 0x1021b000, RGPD layout) and [K] cldma_start / cldma_reset / cldma_plat_hw_reset / cldma_rx_ring_init.

**Memory**
- 1 page holds 32 RGPDs (16 B each, 16-B aligned).
- 32 buffers of 4096 B: one page each, data_allow_len 3584.
- Wired, uncached (or cache-cleaned/invalidated around the HWO handoff), physical address < 2^36. The low 32 bits go in the pointer fields; bits 35:32 go in `msb` (bits 3:0).
- Never free it while the MD is powered.

**RGPD i**
```
gpd_flags        = 0x81   (HWO | IOC)
non_used         = 0
data_allow_len   = 3584
next_gpd_ptr     = pa(rgpd[(i+1) % 32]) low 32
data_buff_bd_ptr = pa(buf[i]) low 32
data_buff_len    = 0
msb              = (pa(buf[i]) >> 32) & 0xF   (msb holds bits 35:32 of the data pointer; the next pointer shares
                                               the same high bits; keep all of it below 4 GB and msb = 0)
```

**Sequence: in stage 1, after `clocks_on()` and before the MD boot-vector release (stock order: CLDMA start precedes "modem is starting")**
1. Hardware reset, as in stage3 §C.2 step 1: INFRA RST0 +0x140 bit 6 then RST1 +0x144 bit 6 (AO); RST0 +0x150 bit 2 then RST1 +0x154 bit 2 (PD).
2. AO +0x418 `DL_MTU_SIZE` = 3584. AO +0x400 `SO_CFG` |= 1.
3. AO +0x404 `SO_START_ADDR_0` = pa(rgpd[0]) low 32. AO +0x40C = high 4 bits (0).
4. `dsb sy`. Then PD +0x404 `SO_START_CMD` = 0x1, and a dummy read.
5. Leave every interrupt masked (do not write the L2/L3 mask-clear registers).
6. Log one line: `r1md: cldma: rx q0 started: rgpd %#llx x32, SO_STATUS %#x SO_CFG %#x`.

**Poll (each loop pass, after drain_rings)**
1. While `rgpd[cur].gpd_flags & 1` == 0:
   - count it: `cldma_rx++`, bytes += data_buff_len;
   - log the first 8 packets: `r1md: [%u ms] cldma rx #%u len %u: %02x x8` (the first 8 bytes = lhif header + IP version);
   - re-arm: data_buff_len = 0, data_allow_len = 3584, then a barrier, then flags = 0x81;
   - cur = (cur + 1) % 32.
2. If AO +0x414 `SO_STATUS` bit 0 is clear (queue stopped) while armed GPDs exist: PD +0x408 `SO_RESUME_CMD` = 1. Count the resumes.
3. Read PD +0x830 `L2RISAR0`. If non-zero: log it once per change and write it back (W1C).
   - Any bit 2 (RX error) or L3RISAR0/1 non-zero → log, and **stop the radio run** (off path). Do not try to recover.

**Teardown (before MD power-down, in the existing off/power-down path)**
- PD +0x40C `SO_STOP_CMD` = 1.
- Poll AO +0x414 until bit 0 clears (≤ 10 ms).
- Then the AO/PD hardware reset (step 1).
- Free nothing until after the reset.

**Kill rules:** any CLDMA error bit, any write outside the ring (check that the two pages before and after keep a canary pattern), MD exception → off path, power down.

### 5c. Logging budget (the 64 KB kept log, shared with other drivers)

**Measured.** The r1md lines in md2f4 up to the overflow (37.2 s) total 43.6 KB in 649 lines. Other kept-log users added ~21 KB.

| Category | Lines | Bytes |
|---|---|---|
| FS per-request (`fs #N …`) | 218 | 16.0 KB |
| misc stage 0/1 | 132 | 10.2 KB |
| AT `>`/`<` | 116 | 4.7 KB |
| `rx q` | 40 | 3.9 KB |
| rpc | 33 | 2.5 KB |
| `tx q5 ch 12` | 40 | 1.7 KB |
| URC | 27 | 1.5 KB |
| other tx | 19 | 0.8 KB |

**Target:** r1md ≤ 24 KB total, with 4 KB reserved for must-lines.

**Cuts**
1. `tx q%d ch %u seq …` (ring_tx 832): log only the first 5 TX overall and every TX failure. Count TX per channel in a summary.
2. FS per-request lines: cap 400 → **30** (boot), then counters only.
   - **New:** after `rd_t_on` (EFUN=1), log FS requests again, compactly (`r1md: [%u ms] fs* op %#x "name" -> rc len`), cap 25, plus per-op counts and error count in the window (5d).
3. rpc request lines: log each distinct (op, name) once; count repeats (the HOT_PLUG_EINT lines repeat ×6/×3).
4. `rx q …` lines: MAX_PKT_LOG 40 → 15.
5. CCIF boot trace: 24 → 6. Stage 0/1 register lines: keep (they are the power-sequence record), but drop the duplicated before/after lines that repeat unchanged ("found before" vs "boot state").
6. Gate lines: print only failed gates. Passing gates are in `RESULT 2e-1` ("10 of 10").
7. Periodic queries (CEREG?, CSQ, CSCON?): log the response only when it differs from the previous one. Log the `AT>` echo only for non-periodic commands.
8. `+ECSQ` URCs: the first 3, then only when rsrp moves ≥ 3 dB (or the cell is lost/found).

**Budget mechanism (small)**
- `SAY` measures each line: vsnprintf into a 192-B stack buffer, then `redwood_host_keep_add("%s", buf)`. Add the length to `md_bytes`.
- Bulk lines use `SAYB`: dropped (counted in `md_cut`) once `md_bytes > 20 KB`.
- RESULT / off-path / CLDMA-error / bit ≥16 lines use `SAYM` (never dropped).

**Must-survive lines: emit early, re-emit at the end**
- `RESULT 2e-1` (as now, at the gates).
- **`RESULT 2f-A` (interim)** at the first of: EPS registered, first EPS stat 3, EFUN=1 + 60 s:
  ```
  r1md: RESULT 2f-A [%u ms]: cscon1 %d ms, stat3 %d ms (t3410 %d ms), bits>=16 {20:%u@%u,%u ...}, fs* %u (err %u, wr %u), rpc* %u, cldma rx %u err %#x
  ```
- `RESULT 2e-2` (existing two lines) plus `RESULT 2f-A` again at the end of the off sequence.
- Also re-emit both after MD power-down (the last thing the driver says), with `md_cut` appended.
- If the interim line is the only one that survives, it still answers "attached? T3410? bit 20? FS in window?".

### 5d. Read-only diagnostics for attempt 3

1. **RCHNUM per bit:** `cnt[24]`, `first_ms[24]`, `last_ms[24]`.
   - Every edge of bits ≥ 16 is logged with SAYM, cap 20:
     ```
     r1md: [%u ms] CCIF bit %d #%u: AP RCHNUM %#x BUSY %#x, MD RCHNUM %#x BUSY %#x; SRAM %08x %08x %08x %08x
     ```
     - AP CCIF is 0x10209000 (+0x10 RCHNUM, +0x04 BUSY); MD CCIF is 0x1020a000 (the same offsets; already mapped as `mdccif`).
     - The 4 SRAM words are at AP CHDATA +0x100.
   - 10 ms after the ack, read MD BUSY again and log whether bit 20 cleared. This proves the ack reaches the MD side.
   - Ring bits (0-7, 15): counters only.
2. **CLDMA snapshot (read-only, valid with r1_md_cldma = 0 too).** First check that the CG2 bit-3 status (INFRA +0xac) shows ungated; else skip.
   - One line, logged only when it changed. Taken at: pre-EFUN=1, every bit ≥16 edge, EFUN=1 + 1/5/15 s, first stat 3.
   - AO 0x10014000: +0x400 SO_CFG, +0x404 SO_START_ADDR_0, +0x408 SO_CURRENT_ADDR_0, +0x414 SO_STATUS, +0x418 DL_MTU, +0x800 L2RIMR0.
   - PD 0x1021b000: +0x28 UL_STATUS, +0x3C UL_ERROR, +0x400 SO_ERROR, +0x410/414/418/41C DL_DEBUG0-3, +0x800 L2TISAR0, +0x830 L2RISAR0, +0x834 L2RISAR1, +0x838 L3RISAR0, +0x83C L3RISAR1, +0x878 DEBUG0.
   - Purpose: whether the MD's side touched the AP CLDMA (DL debug / SO error / RX status bits change at the bit-20 moment).
3. **FS in the attach window:** every request after EFUN=1 (the compact line of 5c item 2), plus `fs*` counters by op and the errors/writes. Writes are served to the overlay as now. Gate 3's CALIBRAT/NVD_IMEI rule still applies.
4. **Timers:** log `t3410 = stat3_ms − cscon1_ms`. Keep listening after stat 3, as now. Log each `+CSCON` edge with ms, so that the T3411 retry (~+10 s) and its second RRC set-up are visible. With the ack in place, a second bit-20 edge would now be visible too.
5. **Cause decoding:**
   - log CEREG=3 fields `cause_type/reject_cause` and EREG's last two fields as integers;
   - log `+ECCAUSE` verbatim. It is not a reject cause: stock prints `0,1,0` on a normal detach.
   - A reject is real only if reject_cause ≠ 0.

### 5e. The change list for attempt 3, and what each outcome means

```
A3 changes (redwood_md.c only; r1_md_cldma stays 0):
 1. ACK = RCHNUM (all bits), as stock irq0+irq1 together; nothing sent back on bit 20.
 2. Per-bit RCHNUM counters; every edge of bits >=16 logged with AP/MD RCHNUM+BUSY, 4 SRAM words,
    and MD BUSY re-read 10 ms after the ack.
 3. Read-only CLDMA AO/PD snapshot (on change) at pre-radio, each bit-20 edge, +1/+5/+15 s, first stat 3.
 4. FS requests after EFUN=1 logged (cap 25) + window counters (ops, errors, writes).
 5. Log budget: tx-line cap 5, FS cap 30, rpc dedupe, rx cap 15, gates on failure only,
    periodic query/ECSQ dedupe, SAYB/SAYM with a 20 KB bulk ceiling.
 6. RESULT 2f-A interim at registration / first stat 3 / +60 s; RESULT 2e-2 + 2f-A at the end and
    again after MD power-down.
 7. CLDMA RX start (5b) implemented but OFF (r1_md_cldma=0) for attempt 4.
```

| Attempt-3 observation | Meaning | Next |
|---|---|---|
| `+CEREG: 1/5` / `+EIAREG: ME ATTACH` within ~2 s; bit 20 once, MD BUSY clears after the ack | **H1 confirmed:** the modem waited for the AP's ack of its wake signal | Stage 3 data. CLDMA is still needed for data, not for attach. |
| No attach; bit 20 re-raised (edges at RRC set-up, again at the T3411 retry), acked each time; CLDMA snapshot changes (SO_ERROR / DL_DEBUG / L2RISAR) at the edges | The MD wants the AP's CLDMA DL queue (**H2**) | Attempt 4 with `r1_md_cldma=1`. |
| No attach; bit 20 once, acked, no CLDMA change; FS window shows a request we failed (error / missing file / write refused) | The attach waits on NVRAM I/O | Fix that FS path (file, overlay write), re-run. |
| No attach; FS window clean, no bit-20 edge at all | The bit-20 trigger moved. H2 is still the only structural difference | Attempt 4 with CLDMA. |
| Bit 21 (SEQ_ERROR) appears | Our TX seq/assert handling is wrong | Fix seq before any more radio. |
| Fast stat 3 with reject_cause ≠ 0 | A network reject: decode #N (cellular-attach.md §6) | Per cause. |
| Any CLDMA error bits with `r1_md_cldma=0` | The MD drove the AP CLDMA with no rings: strong H2 | Attempt 4 with CLDMA. |

Safety: attempt 3 adds one register write per event (the CCIF ack, which stock performs) plus reads. There is no new RF, NV or DMA behaviour. The CLDMA RX start (attempt 4) adds AP-side DMA into wired pages. It follows the stock order (before MD release) and has its own kill rules (5b).
