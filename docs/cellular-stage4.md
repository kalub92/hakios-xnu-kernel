# Cellular Stage 4: cellular as an everyday feature of the stay-in-iOS image (spec)

Written 2026-11-18. Analysis only: no code, no builds, no device. Guesses are marked **(guess)**. Line numbers are
from the tree as of HANDOFF-2026-11-17-night.md (md3d2, radio attempt 8 of 10).

Starting point (proven tonight, HANDOFF-2026-11-17-night.md:54-105): r1_md=9 + r1_md_radio=1 registers on AT&T LTE in
~1.2 s, binds the default bearer (EAPNACT -> +EIF ifup), brings up pdp_ip0 (IFT_CELLULAR, raw IP, MTU 1430), and a
userland tool did DNS + HTTP over it. Off sequence clean every time; Android IN_SERVICE afterwards.

Everything below keeps the Stage 2/3 safety rules (cellular-plan.md:133-138; cellular-stage3.md A.5, A.6, A.10, A.12):
read-only NV, RAM overlay, AT whitelist, IMEI never written, EFUN=1 only after the gates.

---

## 0. What is in the way today (code facts)

| Fact | Where |
|---|---|
| The modem runs inside `wait_hs1()`: one loop, `delay_for_interval(3 or 10 ms)` per pass | redwood_md.c:3589, :3747 |
| Each pass reads 15 CCIF SRAM words (boot trace) and the boot status 2x3 times, even after HS2 | redwood_md.c:3593-3610 |
| Time caps: 240 s radio (`radio_drive`), 255 s after EFUN=1 and 400 s md_ms (loop exit) | redwood_md.c:3400, :3710 |
| After the loop the thread powers MD1 down and exits | redwood_md.c:3987-3998 |
| "Unexpected traffic" stop rule uses run-cumulative counts (would fire eventually in an endless run) | redwood_md.c:3404 |
| MD exception with radio on -> off sequence; before radio-on -> loop `break` | redwood_md.c:3647-3669 |
| Gates evaluated once, after the 2e-1 script; gate 10 = battery >=30 % or charger, SoC < 50 C, **Wi-Fi off** | redwood_md.c:3692-3704, :2633 |
| EFUN=1 is refused unless md_level 7 + radio_req + radio_go | redwood_md.c:1535 |
| EFUN=0 waits up to 30 s; off sequence may take ~76 s worst case | redwood_md.c:2080, HANDOFF:16 |
| pdp_ip0 teardown is permanent (`cell_stopped`), interface never detached | redwood_cell.c:15-16, :518-522 |
| Kernel adds unscoped + scoped default routes itself; DNS only as sysctl `kern.redwood.cell_dns` (one server) | redwood_cell.c:311-323, :63, :425 |
| pdp_ip0 packet/byte caps per direction (`r1_md_if_pkts/kb`), 32-bit counters | redwood_cell.c:182, :247; redwood_md.c:4041-4047 |
| CLDMA RX ring 32 x 4 KB; TX ring 32 slots, caps | redwood_cldma.c:31, :38-42, :474 |
| NV overlay: 256 nodes / 16 MB cap, kalloc'd, gone at power-down | redwood_mdfs.c:6-10, :29-30, :490, :520 |
| Loader-return path powers MD1 down by MMIO only, **no EFUN=0** (`md_quiet`) | redwood_md.c:4003-4009, :3852; loader_return.c:396 |
| Side-button orderly exit: watcher sets `loader_live.stop` -> `loader_stop_poll` -> var save (<= 20 s) -> `loader_return` | mtkloader.c:2002-2016; loader_return.c:497-511; redwood_ufs.c:201, :907-938 |
| Watcher waits `var_save_ms/1000 + 15` = **35 s** default after asking, then snapshots + resets | mtkloader.c:1981, :2021-2029, :2173 |
| Without var_store, or no heartbeat for 30 s, the side button goes straight to snapshot + board reset (XNU never runs its exit code) | mtkloader.c:1979-1980, :2003-2004 |
| Every way back ends in a board reset (`reset_board()` after the snapshot, TOPRGU after `xnu_returned`) | mtkloader.c:2173, :1549-1558 |
| GIC SPI handlers exist for drivers | osfmk/arm64/gic_v3_irq.c:124 (`gic_v3_spi_attach`) |

Key consequence: **Android always finds the modem as LK left it, because every exit path ends in a board reset and LK
reloads/verifies md1img** (cellular-plan.md:23). EFUN=0 on exit is about a clean network detach and not leaving the PA
on during the save window, not about Android. That is why the exit design below may fall back to "power down without
EFUN=0" without harming Android.

---

## 1. Run-time structure: `r1_md=10` (service) and `r1_md=11` (service + radio)

Two short values, because the command line overflowed once (HANDOFF:81-83):

- `r1_md=10` = 9 (cldma + data + if + eif) + **service mode**, radio NOT requested: modem boots, SIM up, gates
  evaluated, modem kept alive radio-off for the whole uptime. Safe default, no RF.
- `r1_md=11` = 10 + `r1_md_radio=1`.
- Still required: `r1_md_imei_h=<hash>` (gate 9). Everything else defaulted. Example stay args add
  `r1_md=11 r1_md_imei_h=XXXXXXXX` (~35 chars).

### 1.1 Shape (no rewrite: a service branch in the existing loop)

Keep the single kernel thread and `wait_hs1()` loop. In service mode:

1. Boot -> HS1/HS2 -> FS -> `at_seq7` (2e-1) unchanged.
2. Gates (1.2). Pass + r1_md=11 -> `radio_go`. Fail -> stay radio-off (modem stays up so the AT/FS path keeps being
   exercised; `kern.redwood.cell_state = "gates failed: <list>"`).
3. Radio-on script `at_seqA` unchanged. RP_WAIT has **no 90 s give-up**: the modem keeps searching by itself.
4. Registered -> data phase as 3b, but the data phase never "finishes": new state `RP_SERVICE` replaces RP_HOLD.
5. Leave the loop only on: exit request (1.4), fatal watchdog (1.3), or radio-off request with MD power-down wanted
   (never in v1: radio-off = EFUN=0, MD1 stays powered).
6. Delete in service mode: the 240/255/400 s caps (redwood_md.c:3400, :3710), the 150 s md_ms test exit, the
   15-word trace and boot-status reads per pass after HS2 (read boot status once per 10 s instead), the `cell_done`
   sysctl end, the per-direction packet caps (replaced by the budget, section 4).
7. Logging: service mode must not fill the 64 KB kept log. `SAYM` lines only on state changes, rate-limited to
   ~20/min, plus one status line every 10 min. RESULT lines: `RESULT 4: <state> ...` on each state change.

Status for userland (read-only sysctls, polled by the daemon every 2 s; kernel writes, no locking beyond a seqcount):

```
kern.redwood.cell_state     string: off|booting|gates_failed|radio_off|searching|registered|data_up|denied|budget|stopped
kern.redwood.cell_sig       string: "<csq>,<rsrp_dbm>,<bars 0-4>"     (from +ECSQ/+CSQ, already parsed: ecsq_log redwood_md.c:1472)
kern.redwood.cell_op        string: operator short name from +COPS? / +CIEV: 10
kern.redwood.cell_rat       int: 7 = LTE (AcT)
kern.redwood.cell_addr      string: a.b.c.d (full; userland needs it) ; cell_dns: "dns1 dns2"; cell_mtu int
kern.redwood.cell_gen       int: bumped on every bearer up/down (daemon republishes when it changes)
kern.redwood.cell_bytes     quad: rx+tx bytes this boot (64-bit)
kern.redwood.cell_radio     int RW (root): wanted radio 0/1 (airplane / user toggle; default from r1_md)
kern.redwood.cell_budget_kb int RW (root): remaining allowance for this boot (section 4)
```

### 1.2 Gates: which stay hard preconditions on every boot

All ten are cheap and computed from this boot's own data, so all stay, evaluated once before the first EFUN=1
(gates_eval, redwood_md.c:2586). Changes:

| Gate | Every boot? | Change |
|---|---|---|
| 1 hs2_eind128, boot status | yes | none |
| 2 fs_clean | yes | also re-checked at runtime (1.3 W4) |
| 3 protected_reads (CALIBRAT/NVD_IMEI full length, zero mutations) | yes | runtime too (W4) |
| 4 overlay_checksum / eMMC refusal counter | yes | runtime too (W4) |
| 5 rpc (only 4005/400e/400d) | yes | runtime too (W5) |
| 6 no sysmsg after HS2 | yes | runtime: log + count only; stop only on a sysmsg id we do not know (**guess**: stock sends none in 11 min, stage3 A.10.6) |
| 7 channels | yes | runtime: rate-based (W6) |
| 8 SIM ready | yes | none |
| 9 IMEI hash | yes | none (needs r1_md_imei_h) |
| 10 physical | yes | split: battery >= 15 % or charger (was 30); SoC < 50 C; **Wi-Fi-off part becomes a user decision (D4)** |

### 1.3 Watchdogs that replace the time caps (service mode)

| # | Trigger | Action | Retries |
|---|---|---|---|
| W1 | MD exception (CCIF bit 16) | dump once; if radio on: EFUN=0 (5 s cap); MD1 power-down; state `stopped` | **0 restarts per boot** (1.3a) |
| W2 | AT engine dead: keep-alive `AT+CSQ` every 60 s idle; 3 consecutive timeouts | as W1 | 0 |
| W3 | CLDMA stop rule (`cldma_rx_poll` true: error bits, canary, HWO stuck) | pdp_ip0 down, EFUN=0, MD1 down | 0 |
| W4 | FS rule broken at runtime: write/delete on CALIBRAT/NVD_IMEI/X:/Y:, overlay CRC mismatch, eMMC refusal counter > 0, overlay full | EFUN=0 at once, then MD1 down | 0 |
| W5 | RPC op outside {4005, 400e, 400d}, or unanswered | EFUN=0, MD1 down | 0 |
| W6 | other-channel traffic: a channel never seen before outside {4,34,38,57,59,169,171}, or > 60 such packets/min | EFUN=0, stay radio-off (MD up) | 0 |
| W7 | battery < 10 % and no charger, or SoC >= 55 C | EFUN=0 (radio off, MD up) | back on at >= 20 % / <= 45 C, max 5 cycles per boot |
| W8 | budget exhausted (section 4) | pdp_ip0 down, EFUN=0 | until next boot / user raises it |
| W9 | `+CEREG` stat 3 with permanent cause #3/6/7/8 (existing `rej_perm`) | EFUN=0 for the rest of the boot | 0 |

1.3a Why no modem restart: after it ran, the MD image in DRAM is dirty (its own data sections), LK EMI-MPU-protects it,
and stock reloads md1img through Android's ccci on an MD reset **(guess about the stock path)**. We have no loader for it.
Restarting the same dirty image is untested and could assert or behave oddly. Stay down; the user gets it back with the
next boot. Revisit only with evidence that exceptions happen.

### 1.3b Registration loss, no service, RRC failures

- The modem handles cell reselection, re-attach and RRC failures itself while EFUN=1. **We never cycle EFUN to "help".**
- `+CEREG` 0/2/4, `+CGEV: NW DETACH`/`ME DETACH`/`ME PDN DEACT 0` -> pdp_ip0 down (link down, address and routes
  removed, interface stays attached), `cell_gen++`, state `searching`. Daemon withdraws the service keys.
- `+CEREG` back to 1/5 and `+EIAREG: ME ATTACH` (or `AT+CGACT?` shows cid 0 active) -> rebind: EAPNACT -> EPDN ->
  EIF up -> CGCONTRDP -> address (as 3b, redwood_md.c:3246 `data_drive`) -> pdp_ip0 up with the new address.
- Rebind failures: 3 in 10 min -> wait 10 min; 10 per boot -> data off for the boot (state `registered`, no bearer).
- Stat 3 temporary causes: leave it to the modem (T3402 retries); log once per 10 min.
- `+CSCON` changes: count only (RRC idle/connected flips are normal; HANDOFF:57).
- Needed in redwood_cell.c: replace the permanent `cell_stopped` (redwood_cell.c:518-522) with `cell_if_down()` /
  `cell_if_up(addr, peer, dns, mtu)` that can repeat. Address change = SIOCDIFADDR old + SIOCAIFADDR new.

### 1.4 Guaranteed shutdown on exit

Exit paths and what each must do (budget in brackets):

| Path | Today | Stage 4 |
|---|---|---|
| Side button 3 s, var_store image, heartbeat live | `loader_stop_poll` -> var save (<= 20 s, ~12 s typical, mtkloader.c:1919) -> `loader_return` -> quiesce (MMIO power-down) [watcher waits 35 s] | **first** `redwood_md_stop_request()` (non-blocking: md thread starts pdp_ip0 down + EFUN=0), then var save as now, then `redwood_md_stop_wait(deadline = 8 s from the request)`, then `loader_return` (quiesce powers MD1 down if not already). Worst case 20 s save + 0 s (already over 8 s) + power-down < 1 s: inside 35 s |
| `loader_return_after` timer (test runs) | var save -> stops -> return | same two calls (loader_return.c:459) |
| Host `xl stop` | same as side button | same |
| Panic | `loader_return()` -> quiesce: MMIO power-down, implicit detach | unchanged in v1. Later (optional): from quiesce, write `AT+EFUN=0\r` into the AT ring and spin <= 2 s for OK (MMIO + memory only) **(guess that the ring path is panic-safe)** |
| Hang / no heartbeat 30 s / no var_store / 5 s extra hold | watcher snapshot + board reset, XNU runs nothing | unchanged: the reset is the guarantee (modem unpowered by the reset **(guess: TOPRGU system reset drops the MD1 domain; to verify in 4b)**) |

Inside `redwood_md_stop_request`: EFUN=0 with a **5 s** timeout (not 30), EFUN? once (2 s), then MD1 down regardless.
Typical: EFUN=0 OK in 416 ms (HANDOFF:57). The 8 s wait bound sits inside the watcher's 35 s with ample margin even
if the save takes its full 20 s. The everyday image must keep `emmc_write=1 var_store=1`, otherwise the side button
skips XNU's exit entirely (mtkloader.c:1979-2004): still safe (reset), just no detach.

---

## 2. NV persistence: recommendation = never persist (keep the RAM overlay, discard at power-down)

What the overlay loses each boot (cellular-stage3.md A.6, write list in A.6 table): learned items (last PLMN /
frequency list, AFC tracking value, counters, possibly the EPS NAS context / GUTI if kept in NV rather than on the
USIM **(guess)**), boot counters, SBP/config writes. Calibration and IMEI are never written (gate 3, W4).

Effect of running indefinitely without persistence:

- Within a boot: none. The overlay serves the modem's own writes back (redwood_mdfs.c:6-10). Growth is per distinct
  file (copy-on-write), observed 31 writes / 163 ops in 60 s (HANDOFF:58). Bounded by the set of files the modem
  writes **(guess: low MB)**. 4a measures it over 30 min; W4 stops cleanly if 256 nodes / 16 MB is ever hit.
- Across boots: each boot starts from Android's last saved NV. Cost: possibly a slower first cell search and/or one
  extra authentication on attach. Measured attach today: 1.2 s, so nothing visible. If the user stays in iOS for weeks,
  only the "learned" state ages; the network handles a stale GUTI with an identity request.
- Things that live on the SIM (EPSLOCI etc.) persist on the SIM for real; that is normal.

A private overlay file in our var store (not the NV partitions) is technically possible, but: it creates a third
NV state that diverges from Android's, it would be replayed into a modem whose real NV Android has since changed, and
it buys nothing measurable. **Do not build it.** Reconsider only if 4f/soak shows a concrete degradation (e.g. attach
> 10 s after long iOS-only periods). The user's rule (never write NV partitions) stays absolute; redwood_emmc.c keeps
refusing writes there.

---

## 3. iOS integration (no CommCenter)

### 3.1 What exists for Wi-Fi and what to reuse

- configd makes the Wi-Fi service by itself from the radio's IOKit object (iokit/Kernel/RedwoodWLAN.cpp, comment at
  net_test.m:33-34, :446-452); IPConfiguration runs DHCP and IPMonitor elects the primary service.
- `WIFID=1` adds configd, mDNSResponder(+Helper), trustd, securityd, nsurlsessiond, cfnetworkagent (build.sh:130-131).
  **Cellular needs the same jobs** (configd + mDNSResponder at least); the everyday image already has them when it has Wi-Fi.
- SCDynamicStore glue already written: net_test.m:67-82 (externs), `store` dump :148, `watch` :506. Reuse it in the daemon.
- No IOKit object for pdp_ip0, so configd will not make a service for it. The daemon publishes one, as VPN/PPP do.

### 3.2 The daemon: `celld` (userland/src/celld.c, ~300 lines, C + CF/SC)

Started from rc.jobs (`/redwood/bin/celld &`, like cell_test at rc.jobs:3), exits at once unless r1_md >= 10.
Loop every 2 s: read the sysctls of 1.1; when `cell_gen` changes or state leaves/enters `data_up`, republish.

Keys it publishes (one fixed service ID, e.g. `REDWOOD-CELL-0`, a UUID string constant):

```
State:/Network/Service/<id>/IPv4  = { Addresses = (a.b.c.d); DestAddresses = (peer); Router = peer;
                                       InterfaceName = pdp_ip0; }
State:/Network/Service/<id>/DNS   = { ServerAddresses = (dns1, dns2); InterfaceName = pdp_ip0; }   (guess: InterfaceName optional)
State:/Network/Service/<id>/Service = { PrimaryRank = "Last"; }                                   (guess: where IPMonitor reads the rank)
State:/Network/Interface/pdp_ip0/Link = { Active = true; }
```

Removed (`SCDynamicStoreRemoveValue`) when data goes down. No `Setup:` entries in v1 **(guess: IPMonitor accepts a
State-only service, as for PPP/VPN; 4c verifies)**. Fallback if it does not: also write
`Setup:/Network/Service/<id>` + `/Interface` (DeviceName pdp_ip0, Type "PPP"?) via SCPreferences, or (last resort)
the daemon withdraws the IPv4 key whenever `State:/Network/Global/IPv4` PrimaryInterface is en0.

Primary-service election: IPMonitor installs the default route of the best-ranked service and builds the DNS
configuration (dnsinfo) mDNSResponder reads. With `PrimaryRank = Last`, Wi-Fi wins whenever it has an address;
cellular is primary otherwise. Scoped routes/resolvers for pdp_ip0 remain, so apps that bind to cellular still work.

Kernel side, service mode: **stop adding the unscoped and scoped default routes** in `cell_if_attach`
(redwood_cell.c:311-323) — IPMonitor owns routes; a kernel default route would fight Wi-Fi's. Keep them for r1_md=9
(test mode, no daemon). Publish both DNS servers (CGCONTRDP f5/f6, already parsed).

mDNSResponder: resolves over pdp_ip0 once IPMonitor puts the DNS entity into dnsinfo **(guess, standard macOS/iOS
behaviour)**. 4c checks with `getaddrinfo` (the cell_test (d) step).

### 3.3 Interface type: keep IFT_CELLULAR, set expensive + constrained

- Kernel-side "cellular denied" is only `INP_NO_CELLULAR` / NECP policies. Per-app cellular-deny NECP policies are
  installed by NetworkExtension's path controller / CommCenter **(guess)**, none of which run here (not in IOS_JOBS,
  build.sh:107-131). So nothing denies by default **(guess; 4c verifies with an unbound socket and Safari in 4f)**.
- Keeping IFT_CELLULAR gives correct functional type (if.c:813-818, xnu-src), `IsWWAN` reachability, no ECN on
  cellular (dlil.c:8993), and lets discretionary background work (nsurlsessiond) avoid it.
- Expensive and constrained are NOT automatic (IFNET_IS_EXPENSIVE = `IFEF_EXPENSIVE` flag, if_var_private.h:1287).
  Set both at attach with `sock_ioctl(SIOCSIFEXPENSIVE)` and `SIOCSIFCONSTRAINED` (if.c:3860, :3902) on the same kernel
  socket used for SIOCAIFADDR. Constrained = Low Data Mode: apps/services that honour it hold back.
- Declaring it Ethernet-like instead would make it "wired"/unmetered: background downloads, iCloud/Photos/updates
  (if those daemons ever run) would use it freely. Only do that as a fallback if 4c/4f proves something refuses
  IFT_CELLULAR, and then keep expensive + constrained set (they are independent flags).
- IPv6: bearer is IPv4 only. Publish no IPv6 entity; kernel keeps dropping v6 (counted). No IPv6 default route on
  pdp_ip0. Happy-eyeballs falls back to v4.
- MTU 1430 from the network (cell_mtu); TCP MSS follows. Nothing else.

---

## 4. Data-use safety

Two layers, kernel hard, daemon soft:

1. **Kernel per-boot hard cap** (authoritative, survives a dead daemon): `kern.redwood.cell_budget_kb`, default from
   boot-arg `r1_md_budget_mb` (default **100**). Counted in `cell_output`/`cell_if_input` (64-bit, both directions).
   At 0: W8 (pdp_ip0 down, EFUN=0), state `budget`. Replaces the per-direction caps (redwood_cell.c:182, :247).
2. **Daemon daily allowance**: `/private/var/redwood/cell-usage` = `YYYY-MM-DD bytes`. At start: remaining_today =
   daily - used_today; writes `cell_budget_kb = min(kernel value, remaining_today)`. Rewrites the file every 1 MB.
   The file reaches the flash only with the var_store save on an orderly exit: after a panic/hang the usage since the
   last save is lost (undercount <= one boot's cap). Accept; the kernel cap bounds it.
3. iOS hold-back: expensive + constrained flags (3.3).
4. On/off: v1 = boot-arg (`r1_md=10` radio off, `=11` radio on) plus `kern.redwood.cell_radio` writable by root
   (ssh / a later toggle). v2 = the toggle of section 5.

Default suggestion: 100 MB per day, 100 MB per boot (decision D2).

---

## 5. UI (minimal v1, SpringBoard only)

- Feed: inject_sb.c reads the sysctls of 1.1 every 5 s on a dispatch timer (no IPC to celld needed).
- Hook (same `method_setImplementation` pattern as inject_sb.c:175, :939): `-[_UIStatusBar setData:]` in
  UIKitCore **(guess: selector name on iOS 16.5; confirm with a runtime method dump in SpringBoard first)**. Take the
  incoming `_UIStatusBarData`, `mutableCopy`, replace `cellularEntry` with a `_UIStatusBarDataCellularEntry`:
  `isEnabled = YES`, `displayValue` = bars 0-4, `rawValue`/`displayRawValue` = rsrp, `string` = operator,
  `type` = LTE **(guess: enum value; read it from a real data object or the class's `-description`)**,
  `status` = "service" **(guess)**. No service -> `string = @"No Service"`, bars 0. Radio off -> entry disabled.
  Leave Wi-Fi/battery/time untouched.
- Apps' status bars get their data from SpringBoard's status bar server **(guess for iOS 16)**: if bars appear in
  SpringBoard but not in apps, v1 accepts that.
- MobileGestalt telephony answers: if SB hides the cellular slot entirely (no `telephony` capability), v1 shows the
  operator only as the "service string" if possible; otherwise status bar is v2 (needs an MG answer override, risky:
  other daemons then expect CommCenter).
- Airplane mode -> EFUN=0/1: v2. celld watches the radios preference (`com.apple.radios.plist` AirplaneMode via
  SCPreferences callback **(guess: file/key)**) and writes `kern.redwood.cell_radio`. The Control Center toggle may not
  even show without telephony; not worth it for v1.
- Fakeable for radio-off tests: boot-arg `r1_md_fakesig=1` makes the kernel fill cell_sig/op/rat with fixed values
  (bars 3, "TEST", LTE) without a modem.

---

## 6. Power and heat

Today (redwood_md.c:3747): one wake every 3 ms while pdp_ip0 is up, 10 ms otherwise, each pass doing ~40 MMIO reads
(15 SRAM trace words + 6 boot-status reads + CCIF + CLDMA) -> **guess** ~10 us per pass = ~0.3 % of a core at 3 ms;
the larger cost is 333 wakeups/s keeping a core out of idle. The modem's own LTE draw (worse at rsrp -113 dBm, cell
edge, stage3 A.3) dominates either way **(guess)**.

Change, in this order:

1. (4a) Drop the per-pass trace/boot-status reads after HS2 (boot status once per 10 s).
2. (4a) Adaptive poll: 2 ms while any CLDMA RX/TX in the last 1 s; 20 ms otherwise (MD up, radio on or off). FS
   request latency at 20 ms is fine **(guess: stock FS daemon is a user process with similar latency)**.
   Raise RX ring 32 -> 128 entries (512 KB) so a burst arriving during a 20 ms idle sleep is not dropped.
3. (later, 4g) IRQ wakeup: `gic_v3_spi_attach` (gic_v3_irq.c:124) for CLDMA SPI 165 (level high, INTID 197) and
   CCIF0 SPI 148 (level low, INTID 180): handler masks the SPI and `thread_wakeup`s the md thread, which drains then
   unmasks; fallback timeout 100 ms. **(guess: INTID = SPI + 32 per the GIC note in redwood_wifi.c:4355.)**

Must not break:

- SRCCLKENA low byte 0x21 stays set while MD1 is on (redwood_md.c:3952); only md_power_off clears it (:3890).
- The CLDMA RX ring must be started before release and kept serviced: without it the MD storms AP_MD_PEER_WAKEUP
  (CCIF bit 20, every ~12 ms) and attach fails (HANDOFF:41, :59). Service mode: keep acking bit 20 (redwood_md.c:3627)
  and add a health counter: > 50 bit-20 edges in 10 s while pdp_ip0 is up -> log (and treat as W3 if sustained 60 s).
- Never gate the CCIF/CLDMA clocks or stop the RX ring while MD1 is on.
- The port has no AP suspend today **(guess)**. If one is added, MD1 must stay powered and its clock request honoured
  across it (haretic 1d6ee47b41, cellular-plan.md:13).

---

## 7. Staged plan

Radio approvals left tonight: 2 (attempts 9, 10). 4a-4e need **no RF**: r1_md=10 without radio, or a fake bearer.

| Stage | What | Lines (guess) | Test (unattended, kept-log lines) | RF? |
|---|---|---|---|---|
| **4a** service skeleton | r1_md=10/11 parse; service branch: no caps, `RP_SERVICE`, watchdogs W1-W7/W9 (W6 rate-based), keep-alive AT+CSQ, adaptive poll, trace reads dropped, 64-bit counters, status sysctls, log rate limit, overlay size in the 10-min status line | ~300 kernel | `XNU_SECS=1800` harness, `r1_md=10`, NOWIFI: PASS = `RESULT 4: radio_off` within 60 s, 3 status lines with keep-alive OK, overlay growth <= 1 MB/30 min, no W-line, CPU of md thread < 1 % (cputop), then normal exit + Android IN_SERVICE | no |
| **4b** exit path | `redwood_md_stop_request/_wait` in loader_stop_poll and loader_return_timer; 5 s EFUN=0 / 2 s EFUN?; quiesce unchanged | ~80 kernel | (1) var_store image + `sidebtn_test=120`: PASS = `r1md: exit: off N ms, MD1 down` before `var_store: exit save done`, total < 30 s; (2) `sidebtn_test=120 sidebtn_test_force=1` without var_store (MD1 up at reset): PASS = Android IN_SERVICE afterwards | no |
| **4c** iOS plumbing | repeatable `cell_if_up/down`; fake bearer `r1_md_iffake=1` (pdp_ip0 up with 192.0.2.2 peer 192.0.2.1 DNS 192.0.2.1, output counted and dropped, no modem needed); expensive+constrained; no kernel routes in service mode; `celld` publishing | ~120 kernel + ~300 celld | radio-off, configd+mDNSResponder jobs: (1) no Wi-Fi: `net_test store` shows the service, `State:/Network/Global/IPv4` PrimaryInterface pdp_ip0, unbound UDP send to 1.1.1.1 increments pdp_ip0 tx; getaddrinfo goes to 192.0.2.1 (tx count +1); SIOCGIFEXPENSIVE/CONSTRAINED = 1. (2) `wifi=1` with en0 up: primary = en0, the same send leaves on en0, pdp_ip0 keeps its scoped route. (3) fake down/up cycle x3: keys removed/republished | no |
| **4d** budget | kernel byte budget + W8; celld daily file | ~120 | fake bearer, `r1_md_budget_mb=1`: send 1.2 MB of UDP to a blackhole over pdp_ip0: PASS = `budget exhausted` line, pdp_ip0 down, keys withdrawn, file shows ~1 MB | no |
| **4e** status bar | inject_sb.c reader + setData hook; `r1_md_fakesig=1` | ~150 | radio-off + fakesig: screenshot (fbgrab) shows 3 bars + "TEST" + LTE on home screen; log line `redwood: status bar: cellular entry fed` | no |
| **4f** integration, **radio attempt 9** | nothing new: r1_md=11, celld, status bar, budget 20 MB, NOWIFI | ~30 glue | one run folds: registration, bind, keys, primary=pdp_ip0, cell_test unbound DNS + HTTP + getaddrinfo via mDNSResponder, status bar screenshot with real bars, 3 min hold with idle/fast poll switches, then `sidebtn_test` exit: EFUN=0 confirmed < 8 s, Android IN_SERVICE. PASS lines: `RESULT 4: data_up`, `celld: primary pdp_ip0`, `cell_test: RESULT ... http 200 gai ok`, `r1md: exit: off`, epoch check | **yes** |
| 4g (later, new approvals) | Wi-Fi + cellular together (coexistence, D4), 1 h soak, IRQ wakeups, airplane toggle, panic-path EFUN=0 | | | yes |

Attempt 10: hold in reserve to rerun 4f after a fix. Do not spend it on 4g tonight.

Kill / stop criteria:

- 4a: modem exception, FS error or W4 during a 30-min radio-off idle in 2 runs -> service mode not viable without more
  FS work; stop and analyse.
- 4b: the off sequence + power-down not done within 8 s in 2 runs, or Android not IN_SERVICE after the forced reset -> keep
  bounded test runs only (no everyday radio).
- 4c: IPMonitor elects no primary for the State-only service after both fallbacks (Setup entry; daemon-managed
  withdrawal) -> fall back to kernel-managed routes and accept "cellular only when no Wi-Fi configured".
- 4f: no registration with the same code that registered 4/4 times since attempt 5 -> stop, diff the log against md3d2.
- Any: a write attempt to a protected NV path, an IMEI mismatch, or an RPC outside the stock set -> stop the stage.

## 8. Top risks

1. **Long-run modem behaviour is unknown**: longest radio-on so far 64.9 s. Voice-centric UE without IMS may drop
   E-UTRA over time (stage3 A.4); fallback `AT+CEMODE=2` needs its own approval.
2. **Incoming SMS/calls while in iOS**: calls go unanswered (no IMS/VoLTE; AT&T routes to voicemail **(guess)**). SMS:
   if the modem stores to ME (NV) memory, an SMS received in iOS lands in the RAM overlay and is **lost** at power-down
   **(guess about storage)**. Add read-only `AT+CPMS?` to the 4a script to learn where it goes.
3. **Wi-Fi + LTE coexistence**: ch34 GMMGR->WMT not delivered (stage3 B). Band 12 unaffected; AT&T band 30 (2.3 GHz)
   is ISM-adjacent **(guess on the effect)**. Untested.
4. IPMonitor accepting a State-only service, and the status-bar private classes: both guesses, both tested radio-off.
5. Overlay growth over days (W4 ends cleanly, but the modem then stays off).
6. Budget undercount after a crash (bounded by the per-boot cap).
7. Boot-arg length (HANDOFF:81-83): r1_md=11 keeps it short; check the loader's appended args survive in 4a.

## 9. Decisions only the user can make (recommended default in brackets)

- D1. Radio on automatically in the everyday stay image (r1_md=11)? [Yes, but only after 4f passes; until then r1_md=10, modem up, radio off]
- D2. Data budget? [100 MB per day and per boot; lower if the plan is small]
- D3. Use cellular data when Wi-Fi is unavailable, by default? [Yes, Wi-Fi always preferred, interface marked expensive + Low Data Mode]
- D4. Allow Wi-Fi and cellular radio on together (relaxes gate 10's Wi-Fi-off rule)? [Yes after one approved coexistence run (4g); until then cellular radio only in images without wifi=1]
- D5. Status-bar indicators (bars, operator, LTE) via the SpringBoard hook? [Yes, SpringBoard only in v1]
- D6. Airplane-mode toggle? [Later (v2); v1 control = boot-arg + root sysctl]
- D7. Modem exception: stay down for the rest of the boot, no restart? [Yes]
- D8. Accept that calls are not received and SMS may be lost while in iOS? [Yes, after the read-only AT+CPMS? check]
- D9. Never persist modem NV, not even a private overlay file? [Never]
- D10. Android side: no change expected (every exit ends in a board reset; LK reloads the modem). Confirm nothing is wanted there. [No change]
- D11. Battery/heat thresholds for radio-on: >= 15 % or charger to start, off below 10 %, off at 55 C? [Yes]
