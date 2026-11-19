# Sleep / low-power idle on the r1: spec (2026-11-19)

Analysis only. Nothing is implemented, built or run. Covers the STATUS.md "not yet done" item (docs/STATUS.md:66).

Tags: **[V]** = verified, meaning read in this repo, its logs, or haretic's source (file:line given). **[I]** = inference or estimate; the basis is given.
"haretic" = github.com/haretic/android-kernel-mt6765-rabbitr1 (main). Copies read for this spec: `docs/ref/dvfsrc/haretic-mt6765-spm-sleep.c`
(already in the repo), and `arch/arm64/boot/dts/mediatek/mt6765.dtsi`, `mt6765-rabbit-r1.dts`, `drivers/soc/mediatek/mt6765-mcdi.c` and
`drivers/power/supply/mt6370-charger.c` (fetched read-only into the session scratchpad, not into the repo).

**Bottom line.** Today the locked r1 is a fully awake SoC with its backlight off. All four XNU cores do WFI between work, but around a
dozen kernel threads poll hardware every 1 to 50 ms. A fifth core (the loader's watcher) busy-spins forever. The rails stay at whatever
the CPU frequency setting left them, up to 1.2 V, and Wi-Fi is held in constantly-awake mode. Nothing has ever measured the drain:
every logged battery reading was taken on USB power.
Stage (a) below is cheap and safe, and it probably recovers the largest share that is easy to get. Stage (b) gains real power only with
MediaTek's SSPM/MCDI protocol; haretic has solved that protocol and its code can be copied. Stage (d), SPM system suspend, is feasible on
paper (haretic does it on Linux), but it collides with our watcher/watchdog safety net and with iOS's whole sleep/wake machinery.
Recommendation: do (a), measure, do (b1). Treat (c)/(d) as a separate project and decide on them after (a)/(b1) numbers exist.

---

## 1. What the r1 does today when locked

### 1.1 Lock path
- [V] Lock = backboardd sets `DisplayBrightnessFactor` 0. Our hook turns that into `kern.redwood.backlight` = 0
  (userland/src/inject.c:1368-1404). The kernel writes the MT6370 backlight (redwood_hyn.c:533-561) and calls
  `redwood_r1_gov_screen(true)` (redwood_hyn.c:561). This only happens with `r1_bl=1..3` (inject.c:1374, redwood_hyn.c:755).
  Without `r1_bl` the backlight stays where LK left it.
- [V] System sleep is refused on purpose: `gSleepDisabledFlag = 1` unless `syssleep=1` (xnu/redwood.patch:1658-1667, IOPMrootDomain.cpp).
  The reason recorded there: powerd asks for sleep within seconds of locking. IOCPUSleepKernel then bounced straight back with one CPU
  and hung with more than one, because `GenericARMCPU::haltCPU` does nothing on the boot CPU (GenericARMPE.cpp:227-233).
- [V] The panel and display engine are not touched. Our port has no DSI/RDMA driver. The framebuffer is LK's, and vsync is a software timer
  (MiniIOMobileFramebuffer.cpp:7, 1806-1850). That timer re-arms every refresh for as long as the client keeps its vsync callback
  registered. [I] Whether backboardd drops the callback while the screen is dark is not known; nothing counts it.
  [I] So the panel stays powered, and DISP/RDMA keep scanning a 480x640x4 B framebuffer out of DRAM at about 60 Hz (~74 MB/s).

### 1.2 Cores and frequency
- [V] XNU runs on cores 0-3 (cluster 0, MPIDR 0x000-0x003). `cpus=4` is passed by tools/r1-run.sh:10, and the loader offers cores in
  `core_order` order (mtkloader/mtkloader.c:96-102).
- [V] **Core 4 (cluster 1) is the loader's watcher.** It is started with PSCI CPU_ON (mtkloader.c:2185-2199, log
  `watcher on cpu4: PSCI CPU_ON 0, running`). It loops forever: feed TOPRGU (mtkloader.c:2005), check the side button and heartbeat,
  then `dbg_delay_ms(100)` (mtkloader.c:2039). `dbg_delay_ms` is a pure busy loop on the counter (mtkloader/mtkdbg.h:81-86). So one A53
  runs at 100% at all times, at 919 MHz: MP1 per the logged `r1clk: MP1 sel 1 ARMPLL ... 919 MHz` lines.
- [V] Frequencies. LK leaves MP0 (cores 0-3) at 1499 MHz and MP1 at 919 MHz, with Vproc 800 mV, VSRAM 900 mV and VCORE 800 mV
  (logged `r1clk:` lines). `r1_cpufreq=2200` raises MP0 to the 2200 MHz row at Vproc 1.2 V (redwood_cpufreq.c:619-626).
  **Vproc is one rail for both clusters and the CCI** (haretic mt6765-rabbit-r1.dts comment at the cpufreq tables). So when MP0 is
  raised, the spinning watcher core also sits at 1.2 V.
- [V] `r1_gov=1` (redwood_cpufreq.c:1468-1586, started at 1637-1650) samples every 50 ms (line 1517). It drops to LK clocks
  (`r1_restore`, line 1118) when `r1_gov_off` is set by the screen-off call (lines 1477-1481, 1555). **Without `r1_gov=1` the screen-off
  call has no effect**: the `r1_cpufreq` rows stay raised (2200 MHz / 1.2 V) while locked.
  The stay arguments recorded in HANDOFF-2026-11-17-night.md:149 have `r1_cpufreq=2200` and no `r1_gov`. HANDOFF-2026-11-16.md:211
  records a stay image with `r1_gov=1`. Which one is on the device now is unknown.
- [V] The governor thread keeps waking every 50 ms while the screen is off (line 1517). Only its decisions change.

### 1.3 Idle: WFI
- [V] WFI idle is on: tunable `wfi` defaults to 1 (xnu-src osfmk/arm64/cpu.c:122), and `idle_enable` is TRUE unless `jtag=`
  (cpu.c:1140-1152). `machine_idle` → `Idle_context` → `cpu_idle` → `__builtin_arm_wfi()` (machine_routines.c:335-350, cpu.c:485-632).
  Our patch only removes the set/way cache flush before WFI (redwood.patch:2590-2607). HANDOFF-2026-11-16.md:191 says the same:
  "WFI idle is already on".
- [V] No platform idle hook: `GenericARMCPU::start` zeroes `ml_processor_info_t` (GenericARMPE.cpp:167-176). So `cpu_idle_notify` and
  `idle_timer_notify` are NULL, and `cpu_idle_latency` is 0. As a result `delay()` always blocks rather than spins when preemption is
  enabled (machine_routines.c:2451-2463, kern/clock.c:1575-1601). This is good: polling threads sleep rather than spin.
- [V] Wake sources in WFI: the EL1 virtual timer PPI 27, SGIs 0/1 (gic_v3_irq.c:76-79), and the only SPIs any driver attaches: PVR
  (redwood_pvr_drm.c:641) and redwood_lx (redwood_lx.c:464). An SPI is routed to the core that attached it (gic_v3_irq.c:272-273).
  **Every r1 peripheral driver polls; none uses its interrupt.**

### 1.4 What polls while locked (each poll wakes a core)
| Thread | Period | Source | Notes |
|---|---|---|---|
| Touch (hyn_thread) | 16 ms (`touch_poll_us`/`hyn_poll_us`) | redwood_hyn.c:249-300 | bit-banged I2C, ~0.4 ms CPU per read (line 259); never slows |
| Side button + wheel (r1keys) | 20 ms, or 33/10 ms with the wheel | redwood_hyn.c:419 | wheel = bit-banged I2C read of the OCH1970 every poll |
| IMU (`r1_imu=1`) | 40 ms | redwood_hyn.c:724 | bit-banged SPI |
| Haptics (`r1_vibr=1`) | 4 ms | redwood_haptic.c:1007 | 250 wakeups/s even when silent |
| BT bridge (`connsys_bt`) | 1 ms when idle (`bt_idle_ms`, default 0 → 1 ms) | redwood_connsys.c:2044-2046, 2163 | ~1000 wakeups/s |
| Wi-Fi en0 | 2 ms after traffic, 10 ms after 100 quiet polls | redwood_net.c:280-289, redwood_mtkwifi.c:1823-1850 | firmware set to **CAM (always awake)**, mtkwifi.c:1623-1630 |
| Modem service (`r1_md=10/11`) | 2 ms active, 20 ms idle | redwood_md.c:4335-4346 | svc2: "54 polls/s idle" (HANDOFF-2026-11-17-night.md:121) |
| CPU governor | 50 ms | redwood_cpufreq.c:1517 | also with the screen off |
| Audio | event-driven when idle; 5 ms while feeding; mic 20 ms (mtkmic≥2) | redwood_mtkaudio.c:1128, 2393-2425 | speaker path powers down after 1 s of silence (line 1131) |
| Battery | 10 s | redwood_mtkbatt.c:345 | |
| Thermal / DRAM look | 1 s / 10 s | redwood_cpufreq.c (thermal thread) | |
| Framebuffer vsync | 1/60 s while a callback is registered | MiniIOMobileFramebuffer.cpp:1806-1850 | [I] unknown when locked |

### 1.5 What stays powered while locked
| Block | State | Basis |
|---|---|---|
| Backlight | off with `r1_bl=1` | [V] §1.1 |
| Panel + DSI + RDMA/OVL (DISP domain) | on, scanning | [V] no driver touches them; [I] scanning continues |
| GPU (MFG domains, firmware MIPS) | on from first use until the run ends; never powered down | [V] redwood_pvr.c:1503, 1858-1863 ("nothing in v7.3 undoes it short of a restart") |
| Camera (CAM/ISP) | off after a session (`r1cam_linger_ms` default 0) | [V] redwood_r1cam.c:506, 584-588 |
| Audio codec / amplifier | off after 1 s of silence | [V] redwood_mtkaudio.c:1131, 2425 |
| Wi-Fi (CONN) | on; CAM power mode once joined | [V] redwood_mtkwifi.c:1623-1630 |
| Bluetooth | on with `connsys_bt`, polled 1 ms | [V] §1.4 |
| Modem MD1 | off unless `r1_md`; with `r1_md=10` the domain is up and radio-off (CFUN 4) | [V] HANDOFF-2026-11-16.md:364, -17-night.md:120 |
| DRAM | 3200 MT/s, DVFSRC not started (no down-shift) | [V] HANDOFF-2026-11-16.md:282 (`r1dram:` SHU0 3198, CTRL 0x8, LEVEL 0) |
| Watcher core 4 + cluster 1 + TOPRGU | running | [V] §1.2 |

### 1.6 Battery figures on record
- [V] No discharge figure exists anywhere: no run with the charger out. All 91 logs with `r1batt:` lines say `charger in`.
  HANDOFF/STATUS mention only "percentage held after unplug" (HANDOFF-2026-11-16.md:195), which was a display test.
- [V] With the charger in, at 100% / ~4.40 V, the gauge still reads a small **net discharge** of the cell:
  - logs/idle1.log: -50..-57 mA in 57-158 s, during the boot-time lsd rebuild at 2200 MHz.
  - logs/svc2.log: -12..-33 mA over 638-1780 s (no Wi-Fi, modem service radio-off, home screen).
  - Sample-to-sample spread at steady state: about ±2 mA.
  [I] This is the charger's regulation residue at "full" (status 4), not the system draw (§5.1).
- [V] Gauge scales: current 0.314 mA/LSB (redwood_mtkbatt.c:321, 378); cell 1000 mAh (redwood_mtkbatt.c:316; haretic dts
  `charge-full-design-microamp-hours = <1000000>`).
- [I] The charge counter as printed (`counter %#x`, CAR_CON0/1 raw) falls about 190k-200k counts per mAh. Basis: logs/svc2.log 60 s pairs,
  0x18e99 counts at -32 mA and 0xa9c7 at -13 mA. That is ~5 nAh per count. Calibrate it once against the current integral.

---

## 2. What MT6765 offers and how each state is entered

### 2.1 Firmware chain
- [V] boot ROM → preloader → LK (stock) → our mtkloader → XNU (docs/mtk-porting.md:41-48). ATF (and GenieZone, EL2 = 2 in the
  loader's `timers:` line) sit below. PSCI is via SMC (`psci-method = smc`, docs/mtk-porting.md step 6; psci_smp.c:116-130).
- [V] LK loads the SPM firmware (spmfw partition) and hands it to ATF. The kernel picks an entry with `MTK_SIP_SPM_ARGS` and kicks it
  through DVFSRC START (spm-sleep.c:12-16; docs/dram-dvfsrc-spec.md:11-19).
  Our runs read `SPMFW 1` = loaded, not running (HANDOFF-2026-11-16.md:282).
- [V] The way back to Android is a TOPRGU warm reset with DDR reserve. The watcher feeds a 30 s TOPRGU every 100 ms
  (mtkloader.c:2850-2862, 2005); a missed feed resets into Android. The orderly end is `loader_return()` →
  `psci_smp_stop_others()` → loader → 1 s TOPRGU reset (loader_return.c; dram-dvfsrc-spec.md:83-84).

### 2.2 The states
| State | Entry | Who acts | Haretic uses it? |
|---|---|---|---|
| WFI (clock-gated core) | `wfi` | core | yes (state 0) [V] |
| CPU_SUSPEND core power-down, cluster-coordinated (MCDI) | PSCI CPU_SUSPEND `0x01010001`; ATF parks the core in `wfi; b .`, and the **SSPM MCDI task** powers it down, plus the cluster when told it is the last core | ATF + SSPM (mailbox 3 @ 0x10480000) | yes, behind `mt6765-mcdi enable` [V] mcdi.c:1-20, dtsi idle-states |
| CPU_OFF (hotplug) | PSCI CPU_OFF parks in `wfi; b .`; real power-off needs SiP `MTK_SIP_POWER_DOWN_CORE 0xc2000212` (x1 = cpu) from another core; without it the core can never be started again | ATF + SSPM | yes, mcdi.c:193-199 [V] |
| SODI / SODI3 / deep idle (screen-on/off SoC idle, SPM-managed) | stock vendor kernel only: SPM firmware scenarios entered from cpuidle with conditions on display/GPU/masters | SPM PCM firmware + SSPM | **no**: no sodi/dpidle in the haretic DT (grep: 0 hits) [V] |
| System suspend (SPM "suspend") | MCDI pause → SPM PCM WDT on → GIC masked except SPM SPI 181 → SSPM `SPM_SUSPEND` IPI → SiP `SPM_SUSPEND_ARGS 0xc2000220` (flags 0x80098, timer 600 s) → PSCI CPU_SUSPEND `0x01010005` on the boot CPU with **all other CPUs offline** → resume in reverse | ATF + SPM firmware (DRAM self-refresh, 26 MHz off, VCORE low) + SSPM | yes, `mem_sleep_default=deep` [V] spm-sleep.c:213-261, r1 dts chosen |
| PSCI SYSTEM_SUSPEND | ATF rejects the power level | — | no: "ATF rejects PSCI SYSTEM_SUSPEND's power level, so s2idle is no alternative" [V] r1 dts:209-211 |

- [V] DT idle-state parameters (haretic mt6765.dtsi:232-260; cpus 0-3 use `cpu_off_b`, cpus 4-7 `cpu_off_l`).
  Both have `arm,psci-suspend-param = <0x01010001>` and `local-timer-stop`, entry 100 us, exit 500 us. Min residency is 1500 us (b)
  and 4500 us (l). PSCI node: `arm,psci-1.0`, method smc (dtsi:282-285).
- [V] `local-timer-stop`: the core's arch timer does not wake it from power-down. Linux covers this with a broadcast timer, the
  MediaTek systimer at 0x10017000, GIC SPI 194 (dtsi:743-744).
- [V] Firmware facts haretic records:
  - ATF's power-down is "wfi; b .". Any interrupt other than the expected one parks the core there; the SPM then never sees it idle,
    and its watchdog resets the board (spm-sleep.c:23-26).
  - CPU_OFF while the MCDI task is paused "reset the board" (mcdi.c:13-16).
  - SSPM `AVAIL_CPU_MASK` is zero after boot (mcdi.c:7-10).
  - The suspend also needs the SPM firmware *running*, which needs DVFSRC START (spm-sleep.c:14-16). That is our `r1_dram` stage B,
    never run (docs/dram-dvfsrc-spec.md).
- [V] Wake sources, haretic DT (GIC numbers are SPI numbers; INTID = SPI + 32):
  | Source | Line |
  |---|---|
  | keypad (side button, KEY_POWER) | SPI 180 edge-falling via CIRQ (0x10204000, latches edges while the SPM has the GIC down), `wakeup-source` (dtsi:678-686, r1 dts:1074-1083) |
  | touch Hynitron | EINT 0 falling (r1 dts touch node) |
  | MT6357 PMIC (RTC alarm, PWRKEY) | EINT 144 level-high |
  | MT6370 (charger/backlight) | EINT 11 falling |
  | TCPC | EINT 41 level-low |
  | EINT controller | 0x1000b000, SPI 171 |
  | SPM | SPI 181 |
  | systimer | SPI 194 |
  | consys | SPI 267/269/270 |
  | Wi-Fi | SPI 268 |
  | BTIF | SPI 112/132/133 |
  | modem CLDMA | SPI 165 |
  | CCIF0/1 | SPI 148/149 |
  | MD WDT | SPI 261 |
  | TOPRGU wdt | SPI 139 |
  | pwrap | SPI 259 |

  The SPM replays a keypad wake (wake bit 2 in SPM_SW_RSV_0) as a pending IRQ (spm-sleep.c:186-211).

---

## 3. XNU idle and sleep on arm64, and what our port has

### 3.1 Apple's design
- **Idle** [V]: `machine_idle()` (machine_routines.c:335) → `Idle_context()` saves callee registers on the idle thread's stack
  (cswitch.s:366) → `cpu_idle()` (cpu.c:485). There it calls `cpu_idle_notify(TRUE, &timeout)`, the platform's chance to choose a
  deeper state, then runs `wfi` / `arm64_retention_wfi`. A core that loses power instead re-enters at `LowResetVectorBase`. The reset
  vector finds the cpu data by MPIDR and jumps to `cpu_reset_handler` (start.s:180-200), which only accepts `resume_idle_cpu` or
  `start_cpu`. `cpu_machine_idle_init()` sets the handler to `resume_idle_cpu` on every core as it comes up (cpu.c:1235). `resume_idle_cpu` → `arm_init_idle_cpu`
  (arm_init.c:829-870) restores the thread pointer, flushes the TLB, re-enables the virtual timer (`fiq_context_init` →
  `_enable_virtual_timer`, machine_routines.c:2372-2377), then `cpu_idle_exit(TRUE)` → `Idle_load_context()`.
  **It does not re-run `PE_init_cpu()`**: on Apple, AIC/FIQ state needs no per-core setup.
- **Offline**: `processor_exit` (processor.c:952-994) → `cpu_sleep()` (cpu.c:311-355): reset handler := `start_cpu`, `CleanPoC_Dcache`,
  then `PE_cpu_machine_quiesce` (non-APPLEARMSMP) → `IOCPU::quiesceCPU`. `processor_start` → `startCPU`.
  The lighter alternative is `sched_processor_enable(p, FALSE)` (processor.c:972): the core stays up but gets no threads.
- **System sleep**: IOPMrootDomain → `IOCPUSleepKernel` halts every non-boot CPU, then the boot CPU's `haltCPU` →
  `ml_arm_sleep()` (cpu.c:1002-1100). That waits for all cores' `ARM_CPU_ON_SLEEP_PATH`, stamps `wake_abstime`, and enters
  `arm64_prepare_for_sleep(true)`. Wake comes back through `start_cpu` with SleepState set. Apple expects:
  - the platform to save/restore its interrupt controller and timer;
  - a wake reason (IOPMrootDomain wake type/reason);
  - devices driven through IOService power states. Drivers that ignore power state changes keep running into and out of sleep unaware.

### 3.2 What our port has
| Piece | Ours | Where |
|---|---|---|
| idle | stock WFI, no idle notify | §1.3 |
| `cpu_sleep`/quiesce | `quiesceCPU` → `psci_smp_cpu_off()`: masks the vtimer, `gic_v3_irq_cpu_shutdown()`, CPU_OFF, WFI loop on refusal | GenericARMPE.cpp:212-219; psci_smp.c:237-250 |
| start | `startCPU` → `psci_smp_start_cpu` → CPU_ON at the reset vector; our start.s preamble sets VBAR/SCTLR for a cold core | GenericARMPE.cpp:221-225; redwood.patch:2832-2860 |
| per-CPU GIC/timer init | `pe_init_fiq` → `gic_v3_irq_cpu_init` (from `PE_init_cpu`, boot and `arm_init_cpu` only). It handles GICR_WAKER (gic_v3_irq.c:475-480). The loader patches one instruction in it so every core clears `ICC_CTLR_EL1.EOImode` | redwood.patch:3659-3670; docs/mtk-porting.md:109-112 |
| `haltCPU` | non-boot: `processor_exit`; boot: nothing | GenericARMPE.cpp:227-233 |
| system sleep | refused (`syssleep=1` to re-enable the bouncing) | redwood.patch:1658-1667 |
| end of run | `loader_return` → `psci_smp_stop_others` (SGI 1 to every started core; waits ≤ 3 s per core for stage OFF + AFFINITY_INFO; "left running" note otherwise) | psci_smp.c:266-360 |

- [V] At every run end, cores 1-3 already go through CPU_OFF (e.g. `psci: cpu 1 ends the system ...` in 123 logs). So CPU_OFF
  itself is proven. [I] Without MCDI they are parked in ATF's `wfi; b .`, not powered off; the warm reset clears them.
- [V] Runtime `processor_exit`/`processor_start` of a core has never been exercised (no log line, no caller).

### 3.3 What breaks if a core loses state
On a power-down (stages b-d, not WFI):
1. **GIC CPU interface** (ICC_SRE/PMR/IGRPEN1/CTLR, including EOImode). Lost [I: architectural]. `arm_init_idle_cpu` does not redo it,
   so the core comes back deaf to interrupts. The idle-resume path must call `gic_v3_irq_cpu_init()`: it is idempotent and carries the
   loader's EOImode patch.
2. **Redistributor**: SGI/PPI enables live in GICR (the GIC-600/500 always-on domain). [I] They are kept. The GICR_WAKER handshake
   on power-down/up is ATF's job; our `gic_v3_irq_cpu_shutdown` already sets ProcessorSleep before CPU_OFF.
3. **Timer**: CNTV_CTL/CVAL are lost, and the arch timer cannot wake the core (`local-timer-stop`). XNU has no broadcast timer, so a
   powered-down core with a pending timer would oversleep. Options: (i) only power down cores whose timer queue is empty / migrated
   (hotplug: `processor_shutdown` migrates timers); or (ii) program the systimer (SPI 194) for the earliest deadline before entering.
4. **Per-CPU data** (`cpu_data`, in DRAM) is kept. TPIDR_EL1, VBAR, SCTLR, TTBRs and TCR are rebuilt by the reset path (start.s plus
   our GENERIC_PSCI_SMP preamble). [I] PMU/KPC is not used on this board (board_config.h: "KPC is not enabled").
5. **SPIs routed to a dead core**: `gic_v3_spi_attach` pins an SPI to the core that attached it (gic_v3_irq.c:272-273). For hotplug,
   PVR's and lx's SPIs must be re-routed to core 0, or attached from core 0 only.
6. **The watcher** is not XNU's and is untouched by any XNU idle state. It keeps cluster 1 powered and Vproc loaded, and it is the
   only thing feeding TOPRGU. Any state that powers cluster 1 down, or stops DRAM (suspend), must first agree with it (§4 d).
7. **Loader return**: `psci_smp_stop_others` only signals cores in `psci_smp_started`, and it gives up after 3 s per core with a note.
   An offlined core (stage OFF set by `psci_smp_cpu_off`) is accepted at once, so the return survives hotplug. A core in an MCDI
   power-down must be woken by the stop SGI ([I] MCDI wakes cores on a pending IRQ). If not, the return costs 3 s per core and still
   completes. Haretic's warning: with MCDI enabled, a CPU_OFF while the MCDI task is paused resets the board. That means the return
   path must disable MCDI (pause, mask 0) **before** `psci_smp_stop_others`, or a watchdog reset replaces the orderly return. Either
   way the log survives (DDR reserve), but `var_store` saving would be lost.

---

## 4. Staged plan (each stage behind a boot-arg, off by default)

All new code reads one switch, `r1_slp=N` (bit mask). `r1_slp` absent = today's behaviour, byte for byte.
"Screen off" means the existing backlight-0 call (redwood_hyn.c:561), made into a global flag `redwood_r1_screen_off`.

### Stage (a): measurable idle, no new power states (`r1_slp` bits 0-3)
| # | Change | Files / functions | Wake sources | Est. saving | Hang / recovery | Interactions |
|---|---|---|---|---|---|---|
| a0 | Measurement only: one `r1slp:` line every 10 s (§5.2) | redwood_mtkbatt.c `r1batt_thread`; a helper `ml_cpu_irq_counts()` next to `ml_cpu_idle_time` in machine_routines.c (redwood.patch:2715) | — | 0 | read-only | none |
| a1 | Screen off → LK clocks (0.8 V) whether or not `r1_gov=1`; screen on → back to the `r1_cpufreq` rows (or leave it to the governor if on). Governor samples every 500 ms while off | redwood_cpufreq.c `redwood_r1_gov_screen`, `r1_restore`, `r1_switch`, `r1_gov_thread` | unchanged | [I] 1.2→0.8 V on a mostly-WFI 4-core cluster plus the spinning watcher and CCI (shared rail): ~30-80 mW ≈ 8-20 mA at 3.85 V. Basis: leakage+clock-tree scales roughly with V²-V³ | a failed switch already stops the governor (line 1582); thermal/`r1_latched` rules unchanged; latency to raise on unlock ~1.3 ms (logged `last switch` ~1000-1300 us) | none; unlock lag ≤ 1 governor sample (boost fires on the first touch) |
| a2 | Watcher stops spinning: CNTKCTL_EL1.EVNTEN on its own core, EVNTI = 15 (13 MHz counter → an event every ~5 ms); `dbg_delay_ms` → `wfe` loop **in the watcher only** | mtkloader/mtkloader.c `watcher_main` (around 1964-2040), mtkdbg.h | event stream (per-core register, does not touch XNU's cores) | [I] one A53 from 100% busy to ~0.1% duty: ~40-90 mW ≈ 10-25 mA. Basis: A53 active power at ~0.9 GHz is ~50-100 mW per core | if WFE never returns, TOPRGU (30 s) resets into Android: the same net as today | none for XNU; the side button exit still polls 10/s |
| a3 | Polling back-off while the screen is off: touch 16→50 ms (a tap ≥ 100 ms still seen twice); r1keys 20→50 ms and no wheel read; IMU paused; haptics 4→50 ms when idle; BT bridge idle 1→20 ms; governor 50→500 ms | redwood_hyn.c `hyn_thread`, `r1keys_thread`, `r1imu_thread`; redwood_haptic.c loop ~987-1010; redwood_connsys.c:2163; redwood_cpufreq.c:1517 | side button (polled), touch (polled) | [I] ~2-3k → ~100-200 wakeups/s; ~10-30 mW. Basis: each A53 wakeup at 1.5 GHz ~10-30 us plus 0.4 ms of bit-banging per touch read | worst case a missed short tap; the side button stays at 2 polls (≤ 100 ms press) | BT: HCI latency ≤ 20 ms only when nothing came in; A2DP traffic keeps the 0 ms path. Wi-Fi, modem and audio are **not** slowed in (a) |
| a4 | Park cores 1-3 while the screen is off **and** no audio feed **and** no camera: `sched_processor_enable(p, FALSE)` (no PSCI; the cores stay in WFI) | new small function in redwood_cpufreq.c called from the screen-off hook; mtkaudio feed flag and camera-active flag read | unchanged | [I] small (fewer migrations/IPIs, longer WFI on 1-3): ~0-10 mW. Main value: groundwork for b1 | none new: the cores stay up; unpark at screen-on | audio playback while locked: not parked (real-time threads); modem service thread and Wi-Fi poll all land on core 0 |
| a5 | GPU / peripherals: nothing to gate safely. Camera and audio already gate (§1.5); the GPU cannot be powered back up once off; DISP has no driver. Log PWR_STATUS instead (a0) | — | — | 0 | — | — |

Leave out of (a) and keep for later: Wi-Fi PS mode (CAM was chosen because unicast stopped 80 s after a join under PS, mtkwifi.c:1624-1626),
panel/DSI off, DVFSRC.

### Stage (b): PSCI core power-down (`r1_slp` bits 4-5)
- **b1, hotplug cores 1-3 while the screen is off** (replaces a4 when set).
  - Mechanism: `processor_exit(p)` → `cpu_sleep` → `quiesceCPU` → `psci_smp_cpu_off`. Then core 0 polls `AFFINITY_INFO` / stage OFF and
    issues SiP `0xc2000212` (x1 = cpu) to really power the core down (mcdi.c:193-199). `processor_start(p)` brings it back via CPU_ON
    at the reset vector (the proven boot path).
  - Files: GenericARMPE.cpp (`quiesceCPU`, `haltCPU`); psci_smp.c (new `psci_smp_power_down_core()`; clear the `psci_smp_started` bit
    so stop-others skips it, and set it again in start); gic_v3_irq.c (re-route attached SPIs to core 0 before exit); redwood_cpufreq.c
    (policy).
  - MCDI stays disabled. Haretic calls the SiP regardless of MCDI enable.
  - Saving [I]: three cores from WFI-clock-gated to off: leakage ~5-15 mW per core at 0.8 V → ~15-40 mW. Cluster 0 cannot go off
    while core 0 is up.
  - Hang modes: CPU_ON answers ALREADY_ON if the SiP did not power the core down. `psci_smp_start_cpu` then fails cleanly and the core
    stays out: iOS on fewer cores, no hang. A panic in the exit path goes to `loader_return`, which skips offline cores.
  - First test: one core, one exit/start cycle, with `psci:` lines.
- **b2, idle power-down via MCDI** (`cpu_idle_notify` hook → PSCI CPU_SUSPEND 0x01010001).
  - Needs: MCDI mailbox writes (`AVAIL_CPU_MASK`, `CLUSTER_CAN_POWER_OFF`, pause/ack); `gic_v3_irq_cpu_init` in the resume path
    (arm_init_idle_cpu or `cpu_idle_exit(TRUE)`); a broadcast timer (systimer SPI 194) or power-down only with no timer due within
    1.5 ms; MCDI disabled before `psci_smp_stop_others`.
  - Retention vs power-down: the DT offers only power-down (0x01010001). [I] MT6765 ATF has no retention state worth having over WFI.
  - Cluster-off: cluster 0 can power off only when cores 0-3 are all down. Cluster 1 never can while the watcher runs.
  - Saving [I]: over b1, core 0's leakage plus cluster 0's L2/SCU when all are down: ~10-30 mW.
  - Risk: SSPM-coordinated, with SPM watchdog resets on protocol mistakes (§2.2). Debugged blind. Do it only if b1 numbers say the
    remaining CPU share is worth it.

### Stage (c): display-off "deep idle" (`r1_slp` bit 6)
SODI3/dpidle are SPM-firmware scenarios that haretic does not use, and their vendor entry conditions are not in our references.
Do not attempt them. The practical (c) is "everything interrupt-driven + b2 with all four cores down", i.e. Linux's s2idle shape:
1. EINT driver for the pio EINT block (0x1000b000, SPI 171), with sysirq polarity (0x10200a80) for low/falling lines.
2. Side button on keypad SPI 180 (KP_MEM1 still read by the watcher, which keeps working).
3. Touch EINT 0. [I] The Hynitron in active mode asserts INT per report, so tap-to-wake works with the chip powered (a few mA of its own).
   No wake-gesture mode is known.
4. PMIC EINT 144 for the RTC alarm (MT6357 RTC AL_* registers; today we never write them: redwood_mtkbatt.c:415-416).
5. Wi-Fi SPI 268 / consys 267, BTIF 112, CCIF 148/149 for modem URCs. That means rewriting the polled RX paths (net_thread,
   BT bridge, md service) as interrupt-first. This is the bulk of the work.

Saving [I]: CPU wake rate near zero. The SoC still has DISP scanning, DRAM at 3200, CONN in CAM, so maybe 20-40 mW on top of (b).
Interactions: Wi-Fi association holds (no change in firmware state); BT links hold; A2DP playback keeps cores busy anyway; the modem
service must answer CCIF within its time budgets (today 20 ms polls).

### Stage (d): full system suspend (`syssleep=1` + `r1_slp` bit 7)
Follow haretic spm-sleep.c:213-261 exactly. Prerequisites:
- (i) SPM firmware running = DVFSRC START (dram-dvfsrc-spec stage B, never run);
- (ii) b1 for cores 1-3;
- (iii) **the watcher parked**: a new loader_live flag asks it to CPU_OFF itself, XNU restarts it with CPU_ON at `watcher_entry` after
  resume, and TOPRGU is fed by XNU on each wake;
- (iv) SPM wake period ≤ 20 s (< the 30 s TOPRGU) instead of stock 600 s, or TOPRGU paused around suspend (stock does this);
  that costs a wake every 20 s;
- (v) `GenericARMCPU::haltCPU` on the boot CPU does the SPM sequence and CPU_SUSPEND 0x01010005 with entry = reset vector. Resume
  re-inits GIC per-core state and returns into the `ml_arm_sleep`/`start_cpu` SleepState path; IOPMrootDomain gets a wake reason;
- (vi) GIC enables masked except SPM SPI 181, and keypad replay on wake.

Then every one of our polled drivers must stop cleanly before sleep and resume after: none registers for IOService power states today.
Wi-Fi: [I] association lost without WoWLAN (no Wi-Fi interrupt wake designed); iOS would re-join on wake.
BT: links drop unless connsys keeps them and wakes the AP via BTIF.
Modem: CCIF wake needed for incoming calls/SMS (calls are out of scope anyway).
Audio: iOS holds a power assertion while playing, so no suspend then.
Saving [I]: idle SoC ~5-10 mA total, versus probably 100+ mA today. Basis: typical MT6765 Android suspend figures; haretic dts:1003
quotes ~40 mA for the DRAM down-shift alone.
Hang: the SPM never answering means its PCM watchdog (30 s) resets the board → Android via ramoops marker. That recovers, but loses
`var_store` data since the last save.

---

## 5. Measurement without extra hardware

### 5.1 Charging: what the gauge can and cannot see
- [V] The MT6357 gauge measures **cell** current through its shunt: FGADC_CUR_CON0 at 0.314 mA/LSB, plus the CAR counter
  (redwood_mtkbatt.c:316-324). Neither sees VBUS.
- [V] With USB in, the charger (MT6370 on I2C5 @0x34, haretic r1 dts:1088-1121) feeds the system. The gauge then shows -12..-57 mA at
  100% (§1.6). **So battery-delta measurement while plugged in is impossible**: the number is the charger's float residue, not the
  load. Battery percentage is useless anyway (1% = 10 mAh; at 100 mA a 230 s run uses 6.4 mAh).
- What to do instead:
  1. **Software unplug (preferred, needs one verification).** Put the MT6370 charger in high-impedance mode for the measurement
     window, so the system runs from the cell with USB still connected. Restore it at the window end, in `loader_return`'s quiesce
     list, and on any failure.
     - Register: MT6370 CHG_CTRL1, 0x11 (haretic regmap 0x111; OTG_PIN_EN bit 1, OPA_MODE bit 0, FORCE_SLEEP bit 3 per
       mt6370-charger.c:57-61, 191). [I] HZ_EN is bit 2 in MediaTek's vendor mt6370 driver. **Verify against the vendor
       `mt6370_pmu_charger` header before writing.** Read-modify-write that one bit only; read it back.
     - Access: the same bit-banged bus the backlight already uses (redwood_hyn.c `bus_take(48, 49)`).
     - Gate: only if the cell reads ≥ 4.0 V.
     - Risk: a hang with HZ set leaves the phone not charging after the TOPRGU reset (a SoC reset does not reset the MT6370) until
       Android's charger driver rewrites CTRL1. Check after the first run with `adb shell dumpsys battery`
       (USB powered: true, status 2/5). No root needed.
  2. MT6370 ADC IBUS/VBUS (haretic `mt6370-adc`, `MT6370_CHAN_IBUS`) gives input power while plugged:
     system ≈ VBUS·IBUS·η/VBAT − I_cell. [I] Its LSB is coarse (tens of mA). Use it only as a cross-check.
  3. Hands-on: an unplugged stay image with `r1_slp` a0 logging, an hour locked, read the log over `logto=`. This is the only
     zero-risk true figure, and it costs the user's time.
- Run length with HZ:
  - The CAR counter resolves ~5 nAh/count [I §1.6], so quantization is irrelevant. Noise is load variance.
  - Steady samples spread about ±2 mA (§1.6), so a 60 s window at ~100 mA gives 1.7 mAh ± a few %.
  - A 10% difference between two states needs ≥ 60 s per state per run, and two runs.

### 5.2 Counters to add (a0), one kept line every 10 s
`r1slp: t=S scr=0|1 hz=0|1 chg=in|out car=0x… dcar=N (≈uAh) i=mA vbat=mV | idle c0..c3 % | irq/s c0..c3 timer/s ipi/s | pwr=0x…/0x… | mp0=MHz mp1=MHz vproc=uV | vsync=N`

| Field | Source |
|---|---|
| idle % | `ml_cpu_idle_time` deltas (already used by the governor) |
| irq/s, timer/s, ipi/s | `cpu_stat.irq_ex_cnt`, `timer_cnt`, `ipi_cnt` (arm/cpu_data_internal.h:94-98), via a new `ml_cpu_irq_counts()` |
| pwr | SPM `PWR_STATUS` 0x10006180 / `_2ND` 0x10006184 (redwood_md.c:167-168); decode with docs/ref/haretic-modem/mt6765-pm-domains.h (MD1 bit 0, DPY 2, IFR 6, MFG_CORE0 7, CAM 25, VCODEC 26, DISP/MFG/CONN/ISP per the PWR_STATUS_* macros) |
| vsync | `fVSyncCount` delta (MiniIOMobileFramebuffer.cpp) |
| clocks/rail | the `r1clk` reads |

Note: the r1_batt prints are every 60 s after the first six (redwood_mtkbatt.c:406). The new line must not depend on them.

### 5.3 THE silent unattended baseline run (`slp0`)
Purpose: today's locked vs unlocked behaviour with a0 counters and, if verified, a0+HZ cell current. No behaviour change.

- Build: kernel with a0 only (plus the HZ window if verified, `r1_slp_hz=1`), userland with this rc.jobs line (after the `idle=1` line,
  userland/rc.jobs:40). 26 words, ~150 characters; limits are 63 words (init.c:1116 `argv[64]`) and 400 characters:
  ```
  /redwood/bin/rwtouch ifarg slp0=1 say start wait 55 tap 0.5 0.06 wait 1 notify com.redwood.unlock say awake wait 40 button lock say locked wait 70 say end
  ```
- Command:
  ```
  tools/r1-run.sh slp0 230 "slp0=1 r1_slp=1 r1_slp_hz=1 r1_batt=1 r1_bl=1 r1_clk=1 r1_cpufreq=2200 r1_gov=1 pvr=6 pvr_gl=1 fbaspect=1 glwait=3 fbscale=1 cam_fps=0 r1_gpu=680 glyuv=1 nopill=1 buttons=3"
  ```
  - Run it with `tools/r1-run.sh`, **not** `r1-auto.sh`: r1-auto puts the extra args after its ~150-byte BASE, pushing them past byte 256
    (tools/r1-auto.sh:11, 20).
  - r1-run prepends 61 bytes (r1-run.sh:10), so every switch lies in bytes 61-~230.
- Silent:
  - no `mtkaudio`, so there is no audio sink and the unlock/lock clicks cannot play;
  - no `r1_vibr`, no `mtkmic`, no `connsys_bt`, no `wifi` (fewer variables; a Wi-Fi-on baseline is the second run);
  - no `autotest`, no SHOTS (r1-run adds neither).
- Timeline [I from idle1: `auto: start` ≈ 54 s; lsd rebuild over ≈ 114 s, HANDOFF-2026-11-16.md:270]:
  - awake window ≈ 110-150 s, home screen, backlight on;
  - lock at ≈ 150 s;
  - locked window ≈ 150-220 s;
  - end at 230 s.
  The `r1bl-hook: ... factor` lines give the true backlight timeline, so a 30 s auto-lock inside the awake window shows up and is
  windowed out.
- HZ window: on at 105 s, off at 222 s, and in loader_return. If the read-back of CTRL1 is not the expected value: no HZ, line
  `r1slp: hz REFUSED`.
- Pass (kept log):
  - `r1slp:` lines with `scr=0` before ≈150 s and `scr=1` after;
  - `hz=1` with `chg=in` and `i` ≤ -60 mA (system load now on the cell) during the window, `hz=0` after it;
  - `r1gov: ... ` low after lock;
  - no panic; Android back (r1-auto's check);
  - `adb shell dumpsys battery` afterwards shows USB powered and charging/full.
- Output: mean `dcar` (→ mA) per window, idle% and wakeups/s per core, PWR_STATUS per window. That table is the baseline every later
  stage is compared to.

---

## 6. Risks and recommendation
- Highest-value, lowest-risk: a1 (voltage), a2 (watcher), a3 (polling). [I] Together perhaps 20-60 mA of a guessed 100-250 mA locked
  draw. None adds a power state; all are reversible by the boot-arg.
- b1 is moderate: proven CPU_OFF/CPU_ON plus one SiP call. Runtime `processor_exit` has never run on this port; test it on one core first.
- b2/c are SSPM/SPM protocol work, debugged blind. Mistakes are board resets (recoverable, log kept), not bricks.
  Every polled driver would need an interrupt path for (c).
- d is a project of its own: DVFSRC start, watcher protocol, IOPMrootDomain wake plumbing, every driver's sleep/wake.
  [I] It would bring the r1 to phone-like standby (days instead of hours), and it touches the safety net that keeps the phone recoverable.
- **Recommendation:** a0 baseline (slp0) → a1+a2+a3 (+a4) and the same run again → b1 → stop and decide on b2/c/d with numbers in hand.
  Do not start (d) before DVFSRC stage B has run cleanly, and before the watcher has a park/restart protocol tested on its own.

---

## 7. Implementation tasks, stage (a) only (Sonnet-sized)
All behind `r1_slp` (bit 0 = a0 counters, bit 1 = a1, bit 2 = a2, bit 3 = a3, bit 4 = a4; `r1_slp=1` = a0 only). After every kernel
change: copy to xnu-src, `xnu-build.sh`, then `XL_RAMDISK=userland/root.apfs ./xl_x6826b build` (HANDOFF-2026-11-17-night.md md2g lesson).

1. **a0 screen flag + counters.**
   - Changes:
     - redwood_hyn.c `bl_sysctl`: set a global `redwood_r1_screen_off` next to `redwood_r1_gov_screen` (line 561).
     - machine_routines.c (via redwood.patch, next to `ml_cpu_idle_time`): `bool ml_cpu_irq_counts(int cpu, uint64_t *irq, uint64_t *timer, uint64_t *ipi)`.
     - redwood_mtkbatt.c `r1batt_thread`: when `r1_slp & 1`, log one `r1slp:` line per 10 s pass (§5.2 fields; the PWR_STATUS map is
       `ml_io_map(0x10006000)`, read-only).
   - Pass: ≥ 18 `r1slp:` lines in a 230 s run; `scr` flips with the `r1bl-hook` factor; idle% within ±5 of the `!redwood-gl: threads`
     idle%; no other log line changes when `r1_slp` is absent.
2. **a0b HZ window (`r1_slp_hz=1`), only after HZ_EN is verified in the vendor source.**
   - Changes: redwood_hyn.c new `r1_hz_set(bool)` on bus 48/49, RMW of CTRL1 bit, read-back; timed on/off from a thread; a
     `redwood_r1_hz_restore()` in loader_return.c's quiesce list.
   - Pass: `r1slp: hz on CTRL1 0x.. -> 0x..`, `hz=1 ... i=-<load> mA` lines, `r1slp: hz off` before `XNU returned`, and Android reports
     charging afterwards.
3. **a1 screen-off voltage.**
   - Changes: redwood_cpufreq.c. In `redwood_r1_gov_screen` (1477-1481), when `r1_slp & 2` and the governor is not running, take
     `r1_mtx` and `r1_restore()` on off. On on, re-raise `r1_row[]` via `r1_switch`, skipping if `r1_latched`. From the sysctl path,
     hand this to a thread_call rather than switching inline. With the governor on, sleep 500 ms per sample while `r1_gov_off`.
   - Pass: `r1slp: ... mp0=1499 ... vproc=800000` within 1 s of lock; `mp0=2200 vproc=1200000` within 1 s of unlock; `r1gov:` sample
     count growth drops 10x while locked; no `a switch up failed`.
4. **a2 watcher WFE.**
   - Changes: mtkloader/mtkloader.c `watcher_main`: once at start, `msr cntkctl_el1` with EVNTEN = 1, EVNTI = 15, EVNTDIR = 0. A local
     `watch_delay_ms()` = counter loop with `wfe` inside, used only at the line-2039 call, under `#if WATCH_WFE` set from `r1_slp`
     (boot_arg read like `sidebtn_test`). The MMU-off/`-mgeneral-regs-only` rules apply.
   - Pass: the loader log shows `watcher on cpu4: ... running`; a run with `sidebtn_test=110` still ends by the side-button path
     (`=== watcher: side button held, snapshot ===`); a 230 s run returns normally; the `r1slp` line on core 0 is unaffected.
     Optional: `watcher: wfe N wakes/s` in the snapshot.
5. **a3 polling back-off.**
   - Changes, each guarded by `r1_slp & 8 && redwood_r1_screen_off`:
     - redwood_hyn.c `hyn_thread` sleep 50 ms;
     - `r1keys_thread` 50 ms and skip the wheel read;
     - `r1imu_thread` skip;
     - redwood_haptic.c idle wait 50 ms when not `active`;
     - redwood_connsys.c:2163 idle wait 20 ms.
   - Pass: `r1slp` irq/s (sum of c0-c3) while locked falls ≥ 5x against slp0; a run with `button lock` then `button lock` wakes the
     screen (`r1bl-hook ... factor 1.000`); the hands-on check later is that a tap or the side button wakes it.
6. **a4 park (optional).**
   - Changes: redwood_cpufreq.c `r1_park(bool)` → `sched_processor_enable(processor_array[i], !park)` for i = 1..3, skipped if the
     mtkaudio feed or the camera is active.
   - Pass: `r1slp` idle% c1-c3 ≥ 98 while locked; `r1slp: park 3` / `unpark 3` lines; audio played while locked (with mtkaudio, not
     in the silent run) shows no underrun lines.
7. **Re-run slp0 with `r1_slp=31`** (same scenario, rename `slp1`).
   - Pass: the locked-window `dcar` mA (HZ) or, without HZ, the idle%/irq/clock evidence is better than slp0; no panic; Android back.
