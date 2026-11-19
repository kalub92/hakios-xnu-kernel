# Boot time 2: where a default boot's 111 s go, and what can be cut without the saved database

Status: analysis and specification only. Nothing was built, run, flashed or committed. Written 2026-11-19.
It follows `docs/boot-time-lsd.md` (the seed), `docs/app-launch-latency.md` (SpringBoard's start) and
HANDOFF-2026-11-17-night.md ("Boot time: lsd", "logd_helper and memory exits").

Labels: **[V]** verified (a log line with its time, or file:line). **[I]** inference, with its basis and how
sure. **[G]** guess. **[none]** means no log line exists for that interval. Times are kernel uptime in seconds.
Uptime counts from the SoC reset, not from the kernel's start: the loader's `timers: CNTPCT 0xd43ad40` (hd1) is
17.12 s at the jump to XNU. So "111 s" means 17 s before XNU plus 94 s after it.

The "today's default" boot here is R1 + R3 on, logd_helper dropped, the widget filter and chronod on. Its
first frame is wc5 `[111.838898] swap #2 ... at 111838 ms` and sc5 `111285 ms`. Runs used:
- hd1/hd2: logd_helper dropped, no widget filter. Their console part starts at 41.4 / 22.9 s.
- ft4/ft5: the same, with the sampler. ft4 starts lsd at load (EARLY).
- wc5/sc5: today's default. Their console part starts at ~88 s.
- ram1 has the only full XNU log from 17 s, because it panicked at 66 s. It is an older build (Nov 18 22:43,
  logd_helper still in), but it is the same up to ~25 s.
- lsa2/lsa3/d5/hc1 serve as controls.

## 0. Answers in short

1. **Timeline (§1).** Most of today's ~111 s falls into five intervals:
   - 17.1 s before XNU: preloader, LK and our loader, including a fixed 1 s banner pause.
   - 3.3 s of kernel and init, of which ~2 s is init's self-tests.
   - A **~13-15 s "launch storm" (20.5 → ~35 s)**: every job launchd starts at load crawls through dyld and its
     initialisers together.
   - **The seed (37.5 → 93.5 s, ~55 s)**: one serial chain lsd ⇄ containermanagerd ⇄ usermanagerd.
   - **17.5-19 s of SpringBoard building its UI** on its main thread, then backboardd's first GL frame.
     - The UI build has three stack-backed parts: CoreUI assets, IconServices/IconFoundation icon rendering with
       iconservicesagent at 67%, and UIKit/FrontBoard.
     - backboardd's first GL frame takes ~1.5 s.
2. **Corrections to what we believed:**
   - **backboardd is no longer on the critical path.** With logd_helper dropped, SpringBoard's main thread is
     already in the LaunchServices wait at 53 s (hd1, hd2 `sb hang` stacks in CoreServices/ControlCenterServices),
     not in `BKSDisplayServicesStart`. backboardd opens the display at 38.97 s (hd2).
   - **`swap #2` is a black frame** in every run (`0% of it not black`). The picture comes with `swap 3`,
     0.15-0.4 s later.
   - **The CPU runs at 2.2 GHz from 18.24 s to the end** (`r1cpu: MP0 back on ARMPLL_L: 2199 MHz`; at swap #2
     `core 0 runs at 2197 MHz`). The governor is off by default, and DRAM is at 3200 MT/s (dram1). There is
     nothing to gain from clocks.
3. **The boot waits; it is not CPU-bound (§2).**
   - About 1 of the 4 cores is busy for most of the boot, and never more than ~2.2.
   - Two serial chains decide the time: the launch storm (gate not yet identified, four hypotheses) and the seed.
   - Raw CPU, more cores or higher clocks do not help.
4. **Changes (§3)** need nothing forbidden: no `SYSTEM=1`, no persistent write.
   - Safe cuts inside the seed, about −15-20 s:
     - memoise lsd's persona list;
     - memoise `infoPlistSubset`;
     - hide ~100 unused **framework** plug-ins. Today only apps and app plug-ins are filtered, and the framework
       plug-ins (288) look like about half of the scanned bundles.
   - Small fixed delays, about −1.5-3 s.
   - The launch storm, about −10-14 s once lsd also starts at load. This is the largest unknown, so it needs one
     diagnostic run first.
   - Bundle groups the owner may choose to hide, ~0.1-1.1 s each (§3.3).
   - Only the combination reaches the target: **first frame under 80 s needs the storm fixed (or deferred
     plug-in registration) on top of the seed cuts.** The seed cuts alone give ~90-93 s **[I, medium]**.
5. **One silent diagnostic run, BT2 (§4).** 150 s, an RC file of ~1.1 KB, no unlock, touch, app, camera or
   sound. It measures:
   - per-core idle;
   - eMMC busy and wait time;
   - APFS lock and inflate time;
   - logd, launchd and dyld latency probes during the storm;
   - SpringBoard main-thread samples from 33 s to the first frame;
   - exclusive per-category seed times;
   - which directories lsd really scans;
   - the time of the first frame that is not black.

## 1. Timeline of a default boot

| Interval (s) | Length | What | Evidence | Label |
|---|---|---|---|---|
| 0 → ~16 | ~16 s | Boot ROM, preloader, LK. LK reads the 32 MB boot_a, which also holds Android's kernel and our payload | No line. The loader prints no time at its entry | **[none]** |
| ? → 17.12 | ≤ 1.5 s | mtkloader: display, pmsg, "Booting Darwin / XNU" banner, **`dbg_delay_ms(1000)`** (mtkloader.c:3596), copies the kernel (16.6 MB) and ramdisk (4.4 MB), jumps | `this core is running at 1499 MHz`; `timers: CNTPCT 0xd43ad40` = 17.12 s (hd1), 17.45 s (ram1) | V for the 1 s pause and the jump time; split with LK unknown |
| 17.12 → 17.65 | 0.5 s | XNU early start, untimed | ram1 first stamp `[17.644775] vm_cache_geometry_colors` | V |
| 17.65 → 18.25 | 0.6 s | Kernel to `bsd_init: done`. All 4 cores online by 17.81 (`4 of 4 processors are running`). r1cpu raises MP0 1499 → 2200 MHz at 18.24 | ram1 `[17.814247] cpu 3 online`, `[18.239978] r1cpu ...`, `[18.245419] bsd_init: done` | V |
| 17.90 → 18.16 | 0.25 s, on the path | The eMMC **speed probe**: reads 4 MB only to print `emmc: 4 MB in 254 ms` (redwood_emmc.c:1046-1058), inside `vfs_mountroot` | ram1 `[17.903188] emmc: xnusys at block` → `[18.156592] emmc: 4 MB in 254 ms` | V |
| 18.16 → 18.26 | 0.1 s | APFS mounts "System" (45065 files) on emmc0, then md1 "redwood" | `[18.173707] apfs: volume 0 "System"`, `[18.257255] ... "redwood"` | V |
| 18.25 → 20.42 | 2.2 s | **Our init** (userland/src/init.c `main`). It mounts the RAM /var, restores var.skel, prints the job list, then runs self-tests: `test_files`, `test_writable`, `test_memory`, `test_processes`, `test_dynamic` (hello_dyn), `test_devices` and `test_time` with its fixed `usleep(200000)` (init.c:1297). Then binds, inject.conf, vnodes, `exec /sbin/launchd.ios` | hello_dyn: `[18.486448] shared_region ... check_np failed` → `[19.386565] map_with_linking_np: [4(hello_dyn)]`. That is the first dyld launch: 0.9 s to map the shared cache. Launchd's boot tasks start at `[20.393150]`. Init's own lines carry no times | V for the ends; the split per stage has **[none]** |
| 20.39 → 20.42 | 0.03 s | launchd boot tasks, "Early boot complete" | `[20.420486]` | V |
| 20.45 → 20.55 | 0.1 s | launchd spawns every RunAtLoad/KeepAlive job, about 23 processes (pids 6-28). SpringBoard maps at 20.55 | `memorystatus_update ... pid 6..28` 20.454-20.538, `[20.546309] map_with_linking_np: [7(SpringBoard)]` | V |
| **20.5 → ~35** | **13-15 s** | **The launch storm.** Every process started at load reaches its own code only around 32-35 s (examples below the table). In 26.6-32.5 there is no line at all (hd2) | lines in the list below the table | V for each time; the cause is open (§2.2) |
| 25.7 → 34.9 | 9 s | **No on-demand daemon is spawned**, except cfprefsd at 25.67, which reached libSystem in 40 ms. Then pids 38-41 come at 34.95-36.23, a backlog released at once | hd2 `memorystatus_update(enter): pid 38` at 34.95, 39 at 35.46, 40 (lsd) at 35.77, 41 at 36.23 | V for the times; "backlog" is **[I, medium]** |
| 35.8 → 37.5 | 1.7 s | lsd is launched on demand. The first client is unknown **[none]**. dyld takes 1.1 s, our constructor runs at 36.91, then `createAndSeed: entered` | hd2 `[35.816768] map_with_linking_np: [40(lsd)]`, `[36.911237] !lsd: start pid 40`, `[37.452443] createAndSeed: entered` | V |
| ~33 → ~39 | | SpringBoard after its constructor: `BKSDisplayServicesStart` waits for backboardd, which opens the framebuffer at 38.97 and the GPU at 39.25 | hd2 `[38.973013] MiniIOMobileFramebuffer: backboardd[14] selector 57`, `[39.246258] RedwoodPVR: a client opened the GPU` | V for backboardd; when SpringBoard passed BKS is **[I, high]**: by 53 s it waits in CoreServices |
| ~39 → 93.5 | | SpringBoard's main thread waits on a lock in CoreServices, called from ControlCenterServices, until the seed ends. Heartbeat 0 the whole time | hd1/hd2 `[53.26] !SpringBoard hang ... (heartbeat 0)`, `#0 __ulock_wait`, `#2-6 (CoreServices)`, `#7 (ControlCenterServices)` | V at 53 s; start **[I]** |
| 37.5 → 39.8 | 2.3 s | `createAndSeedLocalDatabase:`: the store query and the first container work, before any bundle | `[39.80] lsd: createAndSeed: seeded 1 in 2.334 s` (hd1) | V |
| 39.9 → 92.8 | 51-53 s | **Built-in enumeration.** 164 apps; data containers 546-573 calls / 16.5-17.5 s; personas 1797-1878 / 8.4-8.8 s; infoPlistSubset 30-32 k calls / 8.9-9.4 s; signing 2402-2510 / 4.6-4.9 s. These are sums over nested calls | wc5 `[92.27] builtin enumeration: ok 1 in 51.16 s`, `[92.59] bundles 164 in 8.9 s; signing 2402 4.6 s ...` | V |
| 92.3 → 93.5 | 0.3 s | Upcall (answered), `setSeedingComplete: 1` | wc5 `[92.59]`, hd2 `[93.46]` | V |
| 40 → 48 | (in parallel) | mediaserverd's camera warm-up (r1cam=4): power on at 43.0, stream 44.0, stop 48.0. mediaserverd 80-92% of a core | hd1 kept `r1cam: power: on at 43012 ms`, ft5 `!sample top: mediaserverd 92%` | V, off the path |
| 93.5 → 95.0 | 1.6 s | backboardd's first (empty) swap | hd2 `[95.020398] swap 1 (#0): surfaces 0 0 0 0` | V |
| 95.0 → 98.9 | 3.9 s | SpringBoard: status bar 96.27, home grid 97.96 and 98.87 (with widgets) | hd2 lines | V |
| **98.9 → 106.6** | **7.7 s** | **SpringBoard's main thread, no line.** Samples: ft5 SpringBoard 95-97% (97-100 s), iconservicesagent 67% (103 s), SpringBoard 158-189% (106-110 s). Hang stacks at 103.5: IconFoundation/IconServices (ft5), CoreUI (hd1), UIKit/SpringBoard/FrontBoard (hd2). The frames are `<redacted>`, so they cannot be decoded | ft5 `[97.658449] !sample top: SpringBoard 97%`, `[103.676060] iconservicesagen 67%`, `[109.696873] SpringBoard 189%` | V for samples and stacks; what the work is: **[I]** icon rendering on a fresh icon cache (RAM /var), asset catalogs, layout |
| 106.6 → 107.2 | 0.6 s | First SpringBoard surfaces; icons and wallpaper in place | hd2 `[106.557452] SpringBoard[7] created surface 3`, `[107.214527] wallpaper: on top` | V |
| **107.2 → 109.4** | **2.2 s** | Between SpringBoard's last line and backboardd's first display surface | **[none]** | — |
| 109.44 → 110.98 | 1.5 s | backboardd's first GL work: surface 6, first draw 109.59, `eot kept: 1 programs`, 13 more surfaces, PMRD evaluatePolicy 110.24, backboardd `[BootUI] systemShellDidFinishLaunching ... Dismissing overlay` | hd2 `[109.588626] a draw by thread 403`, `[110.978493] swap #2 ... at 110978 ms` | V |
| 110.98 | | `swap #2`, **black** (`this frame on average red 1 green 1 blue 1; 0% of it not black`, in all runs). It is most likely the BootUI overlay **[I, medium]** | hd1/hd2/hc1/wc5/sc5/ft3/ft5/d5/lsa3 | V |
| 111.14 | | `swap 3`, surface 22, the home screen. Its colour is not printed, so the first non-black frame is ≤ +0.4 s **[I, high]** | hd2 `[111.135523] swap 3 (#2)`; the next report's frame is `red 157 green 178 blue 169; 100% not black` | V/I |

The launch storm, process by process:
- SpringBoard: exec 20.55, constructor ~33.2.
- tccd: first sandbox request 33.89.
- configd: network stack at 32.53.
- powerd: PMRD at 33.18.
- backboardd: exec 23.59, display at 38.97.
- containermanagerd: mapped 22.14, first request 37.26.
- **lsd** at load (ft4): exec ~20.5, constructor **36.70**. On demand (hd2): 35.82 → 36.91, which is 1.1 s.

Evidence lines: hd2 `[33.228442] !SpringBoard: WallpaperKit`, `[33.888136] minisandbox ... tccd`,
`[32.532978] MiniIONetworkStack: opened by configd`, ft4 `[36.70] lsd: start pid 15`.

Notes:
- **Today's default (wc5/sc5) shifts the tail:**
  - the seed starts at 38.3 instead of 37.5;
  - from the seed's end to the first frame takes 18.9-19.3 s instead of 17.5. The extra time is the widget
    extensions and chronod (wg2: CalendarWidgetExtension and WeatherWidget start at 97.9/98.7) **[I, medium]**.
- **No Wi-Fi, Bluetooth controller, modem or camera stream is brought up by the default boot-args**, so their
  fixed delays are not on the path:
  - `wifi=`, `connsys_*` and `r1_md` are absent;
  - r1cam=4 only reserves memory at 18.24;
  - mediaserverd's camera warm-up runs at 43-48 s, in parallel.
- **The pvr firmware load has a fixed 5 s delay after the root appears** (redwood_pvr.c:876-879). The GPU is ready
  at ~23 s and is first used at 39.2 s, so the delay is off the path today. It joins the path only if backboardd
  ever gets to the GPU before ~23 s.
- AMFI and the trust cache: the stand-in policy is registered at 17.75 s. Processes run "not code signed". There
  is no line and no sign of cost **[none]**. lsd checks signatures in its own process, in user space: signing
  totals 4.6-4.9 s of the seed.

## 2. Is the boot CPU-bound or waiting?

### 2.1 Core use over time

The sampler prints the top three processes by CPU, in % of one core, every 3 s. `!sample +N s: ... pid 0 cpu`
before lsd exists is kernel_task's cumulative time, because psgrep's `cpu_time(0)` reads it: 1371 → 2259 ms over
22.4-34.4 s, i.e. ~7% of a core. Sources: ft5 and ft4 kept logs, ft5 console from 88 s on, hd2 backboardd GL
stats.

| Window | Top three (ft5 unless noted) | Busy cores (top 3 + kernel_task; the rest < third each) | Label |
|---|---|---|---|
| 22-34 (storm) | SpringBoard 38-69%, then logd 19%, wifid 15-29%, configd 14-16%, sleepd 14-24%. At load, lsd uses **1%** (ft4: 30 → 139 ms CPU in 12 s) | **~1.0-1.3 of 4** | V numbers; the bound for the rest is I |
| 37-40 | backboardd 69%, lsd 57%, mediaserverd 49% | ~1.8 | V |
| 40-46 | mediaserverd 80-92% (camera warm-up), lsd 50-53%, containermanagerd 35% | ~1.7-1.9 | V |
| 46-93 (seed) | lsd 58-71%, containermanagerd 27-36%, usermanagerd 8-12% (+ kernel_task ~7%) | **~1.0-1.1** | V |
| 94-101 | SpringBoard 95-97%, launchd 29%, kernel_task 17-20% | ~1.4 | V |
| 103 | iconservicesagent 67%, SpringBoard 40%, kernel_task 11% | ~1.2 | V |
| 106-110 | SpringBoard 158-189%, ContextService 16%, kernel_task 14-16% | ~2.0-2.2 | V |
| 113-123 | backboardd GL stats: `4 cores 68% idle 18% kernel` (hd2 `[123.469801]`) | ~1.3 | V |

**Verdict:**
- The boot is a waiting, serial boot. About three of the four cores are idle for ~85 s of the 94 s after XNU
  starts **[I, high]**.
- Throughput fixes would not change the first frame, because the critical path is two single-threaded chains:
  - more cores (the MP1 cluster, `cpus=8`): no gain;
  - higher clocks: no gain, they are already at the maximum;
  - DRAM: no gain.
- What helps is shortening the chains, or running their parts in parallel.

### 2.2 The serial dependencies

1. **LK → mtkloader → XNU.** All serial. Inside, the 1 s banner pause and the 0.25 s eMMC speed probe **[V]**.
2. **init → launchd.** init's self-tests run before `exec launchd` **[V]**, ~2 s.
3. **The launch storm (20.5 → ~35).** Facts:
   - every process started at load reaches its own code at 32-37 s;
   - lsd started at load sat at 1% CPU for 12 s;
   - the same lsd started at 35.8 s needed 1.1 s;
   - no on-demand daemon is spawned between 25.7 and 34.9;
   - ~1 core is busy.

   So **something everyone shares is held, or is serialised, for ~13 s** **[I, high]**. Four candidates, none
   proven:
   - **H1, eMMC page-ins in series.** redwood_emmc.c reads in **PIO mode, polling the FIFO on the CPU, one request
     at a time under `emmc_lock`** (redwood_emmc.c:162, 183-222, 920-942) at 16 MB/s (`4 MB in 254 ms`) **[V]**.
     Every first touch of a shared-cache page (the cache is 2.5 GB, ios165/20F66...) goes through it. At 16 MB/s,
     13 s is ~200 MB. That fits about one busy core (the lock holder spinning) and lsd blocked without CPU
     **[I, medium]**.
     - Against it: cfprefsd, launched at 25.67 in the middle of the storm, made its first request 40 ms later.
       Its pages may simply have been warm already.
   - **H2, logd as a gate.** `os_log_create` makes a synchronous call to logd. With logd_helper, lsd's main thread
     was seen waiting in `os_log_create` until 48.75 s (ft2a/ft1b `lsd: wait: ... os_log_create+348`) **[V]**. If
     logd answers only from ~33 s, every process that logs in its initialisers waits **[I, medium]**.
   - **H3, launchd stalled.** No on-demand spawn in 25.7-34.9, then a burst. launchd at 25-33% of a core around
     34-37 s **[V]**. Why it would stall is unknown **[G]**.
   - **H4, dyld without prebuilt loaders.** The processes in inject.conf get a library inserted. dyld4 then cannot
     use the cache's prebuilt loader set, so it builds loaders and resolves symbols through the cache's export
     tries, with scattered LINKEDIT page-ins. Injected: SpringBoard, backboardd, lsd, containermanagerd,
     mediaserverd, iconservicesagent, bluetoothd **[I, low-medium]**. This makes H1 worse rather than standing
     alone.
4. **lsd on demand.** lsd is launched by its first client after the storm (35.8). Starting it at load gains
   nothing while the storm lasts: ft4 `createAndSeed: entered` at 37.33 against ft5 37.76 **[V]**.
5. **The seed.** One thread in lsd, waiting synchronously on containermanagerd (container create and set-info),
   which in turn reaches usermanagerd, and lsd's own persona calls to usermanagerd. lsd + containermanagerd +
   usermanagerd ≈ 1.0 core **[V]**.
6. **SpringBoard waits for the seed.** SpringBoard's main thread is in a CoreServices lock under
   ControlCenterServices from ≤ 53 s until the seed ends **[V at 53 s]**. LaunchServices' handlers wait on the
   seeding group (boot-time-lsd §1.2) **[V]**.
7. **SpringBoard's UI build.** It runs single-threaded on the main thread, part of it waiting on
   iconservicesagent, before the first surfaces **[V samples, I meaning]**.
8. **backboardd's first frame.** First GL context, CA programs and EOT compiles, ~1.5 s **[V times]**.

## 3. Changes, ranked

### 3.1 Ranking (gain = on the first frame, against today's ~111 s)

| # | Change | Expected gain | Risk | Forbidden? | Basis |
|---|---|---|---|---|---|
| C1 | **Persona memo in lsd.** Answer `-[UMXPCServer fetchListOfPersonasSynchronousWithError:]` from the first result for the rest of the boot. The hook is already installed: inject.c:2512 | **−5..8 s** (1797 calls, 8.4 s, synchronous on the seed's thread) | low. One user, and containermanagerd already has personas off | no | V calls/time, I exclusive share |
| C2 | **`-[MIBundle infoPlistSubset]` memo per object** (associated object, the same `NSDictionary`). Hook at inject.c:2511 | **−4..8 s** (30 k calls, 8.9 s, ~185 per bundle) | low-medium. The dictionary must be immutable, which it is during the seed | no | V calls, I hit rate |
| C3 | **Hide unused framework plug-ins.** About 100 of the 288 framework appex: 66 `*Diagnostic*`, 21 Lighthouse/PFL/MLRuntime, 15 FollowUp, 5 Engagement (list in scratch `fwappex.txt`, from ios165/system.manifest). Use the same enumerate hook, with directories under `/System/Library/*Frameworks/` | **−5..8 s** (~70-80 ms per bundle: the 27 widgets saved 1.8 s, the 26 apps 8 s) | low-medium. Sysdiagnose extensions, federated learning and follow-up UI are not used on the r1. BT2 must first show that these directories are scanned | no | I, medium |
| C4 | **Fix the launch storm**, after BT2 names the gate. H1: MSDC DMA, or at least a better PIO loop and bigger requests. H2: find what logd waits on at start. H3/H4 follow from BT2 | **−5..13 s** directly; more with C5 | H1: medium (kernel driver, read path only, writes untouched) | no | I |
| C5 | **lsd at load** (`EARLY=com.apple.lsd`, already in mkjobs.py:94), **after C4** | −0 alone (V, ft4). After C4, the seed starts at ~23-26 instead of 37.5: **−10..14 s** | low | no | V/I |
| C6 | **Fixed delays:** loader banner `dbg_delay_ms(1000)` → 0 (mtkloader.c:3596); eMMC 4 MB probe behind `emmc_probe=1` (redwood_emmc.c:1046-1058); init self-tests behind `init_tests=1` (init.c `main`) | **−1.3 s V** + **−0.5..1.5 s I** (init) | low (the chain image is rebuilt every run anyway) | no | V/I |
| C7 | **Owner-chosen app hiding** (§3.3) | 0.1-1.1 s per group, up to ~11 s for all of them | the owner's call: hidden apps lose their icon and cannot be opened | no | I (75 ms per bundle) |
| C8 | **Defer framework plug-in registration** until after the first frame: leave them out of the seed, then register them in lsd with `-[LSApplicationWorkspace registerPlugin:]`, in the background after `setSeedingComplete` | **−15..20 s** over C3's | **high:** poster extensions (CollectionsPoster, UnityPoster, ExtragalacticPoster) and UserNotifications UI may be needed at the first frame. The API's behaviour in a boot is unverified | no | G |
| C9 | **Stop non-critical jobs from starting at load** (nanoprefsyncd, nanoregistryd, BlueTool; bluetoothd and wifid only if SpringBoard does not call them synchronously). Wrap them in a "start after the first frame" launcher (mkjobs `LATE=`) | 0..4 s, and only if H1/H4 hold | low (Watch daemons) to medium (wifid, bluetoothd) | no | G |
| C10 | **backboardd GL pre-warm:** one off-screen CA frame when backboardd starts (~39 s, idle cores) | −0.5..1.5 s | medium | no | I (app-launch-latency §2.3) |
| C11 | **Prefetch SpringBoard's post-seed files during the seed** (Assets.car of the visible apps, CoreUI, SpringBoard's own), on an idle core and an idle eMMC | 0..3 s | low | no | G, needs BT2's eMMC idle data |
| C12 | **Make containermanagerd's per-container work cheaper** (546 calls × ~30 ms; containermanagerd is ~30% of a core, so the time is CPU in containermanagerd) | up to −8 s | unknown until BT2's stack sample of containermanagerd | no | I |
| — | Skip data-container creation for appex during the seed | ≤ −10 s | **high**: extensions such as the Calendar and Weather widgets may lose their container | no | not proposed before C12 |
| — | Higher CPU frequency or the governor | 0: already 2.2 GHz from 18.24 s (V). The governor (r1_gov) is off by default | — | — | V |
| — | More cores (`cpus=8`) | ~0: the critical chains are single-threaded | — | — | I |
| — | Saved database / prebuilt csstore / persistent icon cache | excluded (persistent write) / not recommended (boot-time-lsd §4.1 R5) | — | **yes** | — |

**Projection [I, low-medium]:**
- C1+C2+C3+C6 take the seed from ~55 s to ~35-40 s. The seed ends ~76-80 s and the first frame comes at ~94-98 s
  (default with widgets, +19 s).
- Adding C4+C5 starts the seed ~12 s earlier: **~82-86 s**.
- **Under 80 s** then needs one of these:
  - some of C7 (the owner's choice);
  - C8;
  - C10+C11 together with BT2 showing that the 7.7 s of SpringBoard work can be shortened.

### 3.2 Answers to the specific questions

- **SpringBoard/backboardd work in parallel with the seed:**
  - backboardd already is: its display is up at ~39 s.
  - SpringBoard is not, because its first LaunchServices call blocks the main thread.
  - Hooking ControlCenterServices to skip that call would only move the block to the next LaunchServices call
    (the icon model needs the database) **[I]**. Not proposed.
  - What can run in parallel: C10 (GL pre-warm) and C11 (prefetch).
- **Fixed delays in kernel bring-up or init:** only C6's three are on the path. pvr's 5 s and mediaserverd's
  camera warm-up are off it today.
- **Wi-Fi, Bluetooth, camera and modem:** none of their bring-ups is on the path with the default arguments.
  Their daemons (wifid, bluetoothd, BlueTool, mediaserverd) only add to the storm's load (C9).
- **Cheaper per-bundle seed:**
  - personas: C1, a cache inside lsd;
  - Info.plist: C2;
  - data containers: C12, which needs measuring first. A cache in our containermanagerd hook cannot answer a
    *create* on a fresh RAM /var. A lookup short-circuit for system apps returns no container, which is the
    high-risk row.
- **Pre-warming the dyld cache pages:**
  - It helps only where the eMMC is idle: init's 2 s, and the seed's 56 s if BT2 shows the eMMC idle then.
  - It adds no bandwidth inside the storm. C4 does.
  - A page list taken from BT2 and shipped in md1 would be a static file, not a persistent write.
- **CPU frequency:** no low start.
  - LK and the loader run at 1499 MHz until 18.24 s.
  - XNU raises MP0 to 2200 MHz before launchd (r1cpu lines, redwood_cpufreq.c `r1_cpufreq`).
  - Without `r1_gov=1` nothing lowers it until the run ends (hd1 kept: the only fall is "back to LK clocks
    before return").
  - The first 100 s run at 2.2 GHz.
  - Caveat: this needs `r1_cpufreq=2200` in the boot-args. Without it the cores stay at 1499 MHz (r1clk line).

### 3.3 Bundles the owner could choose to hide (the owner decides; nothing here is a recommendation)

Counts: the app plus its PlugIns appex, minus the 27 widget appex already hidden (ios165/system.manifest). Seconds
use ~70-80 ms per bundle **[I]**. They come off the seed and, a little, off SpringBoard's icon work.

| Group | Bundles | ≈ s |
|---|---|---|
| News | 11 | 0.8 |
| Books / Podcasts | 7 / 7 | 0.5 / 0.5 |
| Apple TV (AppleTV, Apple TV Remote, TVAccess/TVSetup/VideoSubscriberAccount view services) | 9 | 0.7 |
| Home (+ HomeUIService, HomeControlService, HomeCaptiveViewService) | 9 | 0.7 |
| Health (+ HealthPrivacyService, HealthENBuddy, HealthENLauncher, ExposureNotification VS) | 6 | 0.45 |
| Fitness | 2 | 0.15 |
| Find My (+ FindMyExtensionContainer, FindMyRemoteUIService) | 9 | 0.7 |
| Wallet (Passbook + 4 Passbook/payment services) | 9 | 0.7 |
| Stocks / Tips / Freeform / Watch (Bridge) | 3 / 5 / 1 / 2 | 0.2 / 0.4 / 0.1 / 0.15 |
| Measure, Magnifier, Compass (+ calibration VS) | 4 | 0.3 |
| Messages sticker and app-drawer apps (Animoji, FunCamera ×3, #images, Activity, Business Chat ×2) | 15 | 1.1 |
| Game Center (RemoteAlert, UIService, Widgets) / Sleep (2) / Family + Screen Time | 4 / 2 / 6 | 0.3 / 0.15 / 0.45 |
| Mail / Notes / Reminders / Music / Maps / Shortcuts / Voice Memos / Batteries / Hearing | 6 / 6 / 5 / 6 / 3 / 5 / 4 / 2 / 1 | 0.45 / 0.45 / 0.4 / 0.45 / 0.2 / 0.4 / 0.3 / 0.15 / 0.1 |
| Print Center, iCloud+, MusicRecognition, MobileStore, CTCarrierSpaceAuth, PDUIApp | 7 | 0.5 |
| **All of the above** | **156** | **~11-12** |

Caveats:
- docs/ holds screenshots of Health, Maps, Messages, Music, News, Reminders and Wallet, so the owner has opened
  those.
- Hiding the Wallet app is independent of the Settings row that is already hidden.

## 4. One silent diagnostic run: BT2

### 4.1 Goal

Fill every gap and decide between the storm hypotheses in one boot that matches today's default:
- per-core idle over time;
- eMMC MB, busy and wait per second;
- APFS stats at four points;
- when logd, launchd and dyld answer during the storm;
- SpringBoard's main thread from its constructor to the first frame;
- exclusive per-category seed times and which directories are scanned;
- init's stages and the loader's entry time;
- the first frame that is not black.

### 4.2 Build

- Non-BT userland with tasks D1-D8 below, all logging only and all behind the arguments named.
- `RC_JOBS=rc.jobs.bt2`. Kernel and payload rebuilt (D3, D6), loader rebuilt (D4).
- No `OSLOG`, no `EARLY`, no new defaults.

### 4.3 Run

`tools/r1-run.sh bt2 150 "bt2=1 proc_sample=1 lsdiag=1 sb_hang_every=2 mtkaudio=2 r1cam=4 pvr=6 pvr_gl=1 fbaspect=1 glwait=3 glthreads=1 cam_fps=10 fbscale=1 r1_clk=1 r1_cpufreq=2200 r1_gpu=680 glyuv=1 venc=4 mtkmic=2"`
- 150 s, under 230 s. A short run also lets the console tail start earlier.
- The switches come first. With the loader's ~99-character prefix they end at ~160 characters, inside the
  256 bytes rwtouch's `ifarg` reads. The whole line is ~330 characters, under the 607-character limit.
- `mtkaudio=2 mtkmic=2` are kept so the boot matches hd1/hd2/wc5. They bring the drivers up without playing
  anything. For a structural guarantee, drop both; nothing measured here depends on them.
- Silent:
  - no unlock tap, no touch, no button, no app launch, no camera open, no shutter, no sound_test;
  - the only camera activity is mediaserverd's automatic warm-up at ~43 s, as in every earlier silent run.

### 4.4 `userland/rc.jobs.bt2` (~1.1 KB; every line < 400 characters and < 63 words)

```
# BT2 (docs/boot-time-2.md): where a default boot's time goes. Silent: no unlock, no touch, no app, no camera, no sound. Run 150 s.
/redwood/bin/tolog /redwood/bin/psgrep -sample2 2 62 &
/redwood/bin/tolog /redwood/bin/psgrep -probe 3 6 &
/redwood/bin/cell_test ifarg r1_md_if=1 &
/redwood/bin/celld &
/redwood/bin/hello_dyn started by launchd
/redwood/bin/svc_test
/redwood/bin/surf_test
/bin/ps -A
/redwood/bin/mg_test
/redwood/bin/rwtouch say start wait 75 say end
```

- The middle six lines are the default rc.jobs and rc.jobs.homechk test programs, unchanged, for fidelity.
  cpu_test is left out because it would pollute the idle figures; homechk runs (wc5/sc5) do not have it either.
- The sampler and the probe start first, in the background, at ~21 s. The sampler runs 62 ticks × 2 s, to ~145 s.

### 4.5 Lines added (all kept; ≤ 190 characters each)

- **K1** (D4, loader): `loader: entered at %u ms, jumping at %u ms (CNTPCT)`.
- **K2** (D5, init): `init: %s at %.2f s`, for `start`, `var ready`, `tests done`, `binds done`, `exec launchd`.
- **K3** (D1, psgrep `-sample2 SECS TIMES`), one line per tick:
  `!s2 %.1f: idle %u%% usr %u%% sys %u%% | emmc +%u rd %.1f MB busy %u wait %u ms | %s %u, %s %u, %s %u, %s %u, %s %u | pi %s %u`
  - idle/usr/sys: sums over the 4 cores, from `host_processor_info(PROCESSOR_CPU_LOAD_INFO)`, out of 400;
  - emmc: deltas of `kern.redwood.emmc` (D3);
  - top five by CPU;
  - `pi`: the top process by `ri_pageins` delta (`proc_pid_rusage` RUSAGE_INFO_V2).
- **K4** (D1): at the first tick past 37, 94 and 113 s of uptime, and at the start:
  `!apfs %.0f: pagein %llu/%llu ms/%llu MB | dev %llu ms %llu MB | data %llu ms %llu MB | inflate %llu ms %llu->%llu MB | lock wait %llu held %llu ms`.
  It is parsed from `kern.apfs_stats` read into a 16 KB buffer. The old psgrep used 1 KB, which gave "Cannot
  allocate memory" in d5.
- **K5** (D2, psgrep `-probe SECS TIMES`): `!probe %.1f: oslog %u ms, lookup %u ms, spawn %u ms`.
  - `os_log_create` + `os_log_info`;
  - `bootstrap_look_up("com.apple.containermanagerd")`;
  - `posix_spawn` of `/redwood/bin/hello_dyn` until its exit.
- **K6** (D3, kernel): `kern.redwood.emmc` gains `busy %llu ms wait %llu ms waiters-max %u`.
  - busy = time with `emmc_lock` held;
  - wait = summed `lck_mtx_lock` wait.
- **K7** (D7, SpringBoard, `sb_hang_every=2`):
  - every 2 s of main-thread quiet, up to 45 lines: `sb s %.1f: %llx %llx ...` (12 unslid return addresses,
    slide removed as `psgrep` prints it);
  - once: `SpringBoard: constructor at %.2f s`;
  - once: `SpringBoard: main queue first answered at %.2f s`.
- **K8** (D8, lsd):
  - `lsd: first client pid %d (%s) at %.2f s`;
  - `lsd: self ms: dc %u personas %u plist %u signing %u entry %u` (exclusive time, with a per-thread nesting
    stack);
  - `lsd: scanned: apps %u, app plug-ins %u, framework plug-ins %u in %u dirs` (by directory class, in the
    enumerate hook).
- **K9** (D6, kernel framebuffer): `fb: first picture: swap %u at %llu ms, %u%% not black`. Checks swaps 2-8 and
  prints once, for the first swap with more than 5% non-black pixels.

### 4.6 Read-out

| Seen | Means |
|---|---|
| K1 `entered at` | LK's share of the first 17 s, against the loader's |
| K2 stages | init's 2.2 s split. The "tests done" minus "var ready" gap is C6's init gain |
| K3 in 21-35 s: emmc busy ≥ 800 ms per 1000 ms, wait ≫ busy, idle ≥ 250% | **H1**: the eMMC is the gate. C4 = DMA/PIO work, and C9 helps |
| K5 `oslog` in seconds until ~33 s while `lookup` and `spawn` are fast | **H2**: logd is the gate |
| K5 `lookup` slow in 25-35 s | **H3**: launchd is the gate |
| K5 `spawn` (hello_dyn, not injected) fast while injected daemons are slow, and K3 `pi` is high for the injected ones | **H4**: dyld without prebuilt loaders. C4 = inject later or into fewer programs |
| K3 in 40-93 s: emmc busy small | the eMMC is idle during the seed, so C11 (prefetch) is possible |
| K4 37 → 94 delta: lock wait and data ms | how much of lsd's 46% filesystem time is our APFS driver's lock or device |
| K8 `self ms` | C1/C2/C12's real gains (exclusive). They should add up to ≥ 80% of the 51 s enumeration |
| K8 `framework plug-ins` > 200 | C3/C8 apply. 0 means framework plug-ins are not scanned, and C3 is void |
| K8 first client | who starts lsd. If it is SpringBoard at ~35, C5 is the only way to start the seed earlier |
| K7 lines 33-53 in BackBoardServices, then CoreServices/ControlCenterServices, then CoreUI/IconServices/UIKit 93-107 | the SpringBoard phases with times. The 98.9-106.6 gap gets its frames, decoded on the host with tools/cache-name.py |
| K9 swap 3 at ≤ swap #2 + 0.4 s | the true first picture. Use K9 as the milestone from now on |
| `swap #2 ... at` within 110-113 s | the diagnostics did not slow the boot (wc5 111.8, sc5 111.3) |

## 5. Verified vs inferred, in one place

- **Verified:**
  - the loader jump at 17.12 s and its 1 s banner pause;
  - kernel milestones 17.65-18.25 and the 0.25 s eMMC probe on the mount path;
  - init 18.25-20.42 and its self-tests and `usleep(200000)`;
  - launchd's mass spawn at 20.45-20.55;
  - every storm-end time listed in §1, and lsd at load sitting at 1% CPU for 12 s;
  - no on-demand spawn between 25.7 and 34.9 (hd2);
  - lsd 35.8 → 36.9 → 37.5;
  - SpringBoard in the CoreServices wait at 53 s with logd_helper dropped;
  - backboardd's display at 38.97 s;
  - the seed's figures;
  - every post-seed line in §1, the black swap #2, and 2.2 GHz from 18.24 s;
  - the core-use samples in §2.1;
  - eMMC PIO under one mutex at 16 MB/s;
  - pvr's fixed 5 s;
  - the bundle counts in §3.3 and the 288 framework appex.
- **Inferred:**
  - the storm is one shared gate (high), and which gate it is (H1-H4: medium to low);
  - SpringBoard passed `BKSDisplayServicesStart` at ~39 s (high);
  - framework plug-ins make up about half of the scanned bundles (medium);
  - per-bundle cost ~70-80 ms;
  - the gains of C1-C12;
  - the extra ~1.5 s after the seed with widgets on (medium);
  - swap #2 is the BootUI overlay (medium).
- **No log line:**
  - LK's share of 0-17 s;
  - init's stages;
  - who launches lsd;
  - SpringBoard 33-53 s and 98.9-106.6 s (only 2 stack samples, redacted);
  - 107.2-109.4 s;
  - the colour of swap 3;
  - eMMC and APFS counters at any time of any boot.

## 6. Implementation tasks (Sonnet-sized)

Rules as in HANDOFF:
- non-BT build per run; `tools/r1-run.sh` directly;
- RC files < 8192 bytes, lines < 400 characters;
- switches in the first 256 bytes of the boot-args;
- tell the user before a device run; no sound; kept lines ≤ 190 characters.

### Diagnostics (all for BT2; logging only)

- **D1, psgrep `-sample2 SECS TIMES`** (userland/src/psgrep.c, next to `sample()` ~176).
  - CPU load: `host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, ...)` deltas.
  - eMMC: `kern.redwood.emmc` parsed with `sscanf`.
  - Top five by CPU, reusing the `cpu_time` loop.
  - Page-ins: `proc_pid_rusage(pid, RUSAGE_INFO_V2)` `ri_pageins` deltas.
  - K4 at uptimes 37, 94 and 113: `kern.apfs_stats` into a 16 KB malloc buffer.
  - Lines through `say_kept`. Only with `proc_sample=1`.
  - Pass: in BT2, 60-62 `!s2` lines and 4 `!apfs` lines; idle + usr + sys within 380-410 per line.
- **D2, psgrep `-probe SECS TIMES`** (psgrep.c).
  - Three timings with `mach_absolute_time`: `os_log_create`+`os_log_info`;
    `bootstrap_look_up(bootstrap_port, "com.apple.containermanagerd", &p)`;
    `posix_spawn` of `/redwood/bin/hello_dyn` + `waitpid`.
  - Pass: 6 `!probe` lines between 21 and 40 s.
- **D3, eMMC counters and probe switch** (xnu/files/bsd/dev/redwood_emmc.c).
  - Around `lck_mtx_lock(&emmc_lock)` (~920): `t0 = mach_absolute_time()` before, `emmc_wait += now - t0`
    after. On unlock, `emmc_busy += held`. Track the most waiters with an atomic counter.
  - Extend `emmc_sysctl` (1089).
  - Wrap the 4 MB probe (1046-1058) in `PE_parse_boot_argn("emmc_probe", ...)`. Default off; the line then reads
    `emmc: speed probe off`.
  - Pass: BT2's `!s2` lines carry `emmc +N rd`; the console has `emmc: speed probe off`; `swap #2` is still about
    0.25 s earlier than before.
- **D4, loader times** (mtkloader/mtkloader.c).
  - In `mtkloader_main` (3942), read `cntpct_el0` first and store it. Print K1 next to the existing `timers:` line
    (2869).
  - `boot_choice` (3596): `dbg_delay_ms(1000)` → `dbg_delay_ms(BANNER_MS)`, with `#define BANNER_MS 0` (C6).
  - Pass: K1 in the log header; `timers: CNTPCT` ≤ the previous value minus ~1 s (≈ 16.1 s).
- **D5, init stages** (userland/src/init.c `main` 1371-1432).
  - A helper `stage(const char *name)` that writes `init: %s at %.2f s` (from `mach_absolute_time`) with
    `sysctlbyname("kern.redwood.host_keep", ...)` and printf.
  - Call it at start, after `restore_var`, after `test_time`, after the binds, and before `run_rc`.
  - Gate the self-tests (`test_files` .. `test_time`) on boot-arg `init_tests` (default 1 now; C6 makes it 0).
  - Pass: 5 K2 lines in BT2; with `init_tests=0` the "exec launchd" line comes ≥ 1 s earlier.
- **D6, first picture** (xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp, near the swap #2 line ~1571 and the
  "this frame on average" code).
  - For swaps 2-8, run the existing average/non-black computation on the shown surface. The first time it is
    > 5% non-black, print K9 kept once.
  - Pass: one K9 line with swap ≥ 3 and ms ≤ swap #2's + 1000.
- **D7, SpringBoard sampler** (userland/src/inject_sb.c `hang_watch` 2184).
  - With `sb_hang_every=N`: after N s of quiet, suspend the main thread, collect pc, lr and 10 frame-pointer
    returns. Subtract `_dyld_get_image_vmaddr_slide` of the shared cache (as psgrep's slide), so that
    `ret & 0xfffffffff` minus the slide gives an unslid address.
  - One line `sb s %.1f: ...` per sample; up to 45; resume.
  - Print once `SpringBoard: constructor at %.2f s`, and `main queue first answered at %.2f s` when the heartbeat
    first moves.
  - Default (no argument): today's behaviour.
  - Pass: in BT2, ≥ 30 `sb s` lines between 33 and 108 s, and the "first answered" line.
- **D8, lsd** (userland/src/inject.c, lsd section ~2026-2520, `lsdiag=1` only).
  - (a) Self times: a thread-local stack of the category in progress in `lsd_timed*`. On return, add elapsed
    minus children to that category's self total. Print K8 `self ms` at `setSeedingComplete`.
  - (b) In the `MIFileManager enumerateURLsForItemsInDirectoryAtURL:` wrapper, classify the directory: under
    `/Applications/*.app/` → app plug-ins; under `/System/Library/` and `.framework/` → framework plug-ins; else
    apps. Count the items the original block is called with.
  - (c) First client: wrap `-[_LSDOpenClient willHandleInvocation:isReply:]` (0x181c00808, boot-time-lsd §1.2).
    For its first 3 calls, take the peer pid from the client's XPC connection (`xpc_connection_get_pid`) and the
    name from `proc_name`, and print K8 `first client`.
  - Pass: one `self ms`, one `scanned`, ≥ 1 `first client` line in BT2.
- **D9, `userland/rc.jobs.bt2`**: exactly §4.4.
  - Pass: `!auto: start` and `!auto: end`; the run ends `reason 0`; the kept log has no `dropped` > 0.

### Fixes (after BT2; each one run silent with rc.jobs.homechk, `homechk=1` first in the arguments, as wc5)

- **F1 (C1), persona memo** (inject.c, the `UMXPCServer fetchListOfPersonasSynchronousWithError:` hook).
  - Boot-arg `lsd_personas` (default 1 after F1 passes).
  - Cache the first non-nil result (retain it, copy the out error as nil) and return it for 300 s.
  - Pass:
    - `lsd: bundles ... personas N x.x s` with x.x < 0.5;
    - `setSeedingComplete: 1 at` ≥ 5 s earlier than wc5's 92.59;
    - Settings and Camera `launched by bundle id: 1`;
    - icon list lines identical to wc5.
- **F2 (C2), infoPlistSubset memo** (inject.c, `MIBundle infoPlistSubset`).
  - `objc_getAssociatedObject(self, key)`; on a miss, call the original and `objc_setAssociatedObject(...,
    OBJC_ASSOCIATION_RETAIN_NONATOMIC)`.
  - Boot-arg `lsd_plistmemo` (default 1 after it passes).
  - Pass: the `info.plist N x.x s` total ≤ 2.0 s; the seed ≥ 4 s shorter than F1's; same correctness lines.
- **F3 (C3), framework plug-in filter** (inject.c).
  - `lsd_fw_hidden[]`: the ~100 names (all `*Diagnostic*`, `*Lighthouse*`, `*PFL*`, `*MLRuntime*`, `DPML*`,
    `*FollowUp*`/`*Followup*`, `*EngagementExtension*`), matched as substrings in the enumerate wrapper for
    framework directories only.
  - `lsd_fwhide=1` (default after it passes); line `lsd: fwhide: %u of %u framework plug-ins hidden`.
  - Pass:
    - `fwhide: ≥ 90 of`;
    - `setSeedingComplete` ≥ 5 s earlier than F2's;
    - `swap #2 ... at` < 98000 ms (F1-F3 together);
    - home screen, Settings and Camera as wc5;
    - no new `FAULT` or respawn lines.
- **F4 (C6), fixed delays.** D3's probe off, D4's `BANNER_MS 0`, D5's `init_tests=0` default.
  - Pass: `timers: CNTPCT` ≤ 16.2 s; `init: exec launchd at` ≤ 19.4 s; `swap #2` ≥ 1.3 s earlier than the build
    before.
- **F5 (C4), the storm.** Spec it only after BT2:
  - H1: a separate spec for MSDC DMA (bounce buffer, read path only, `emmc_dma=1`). Pass: `emmc` rate
    ≥ 40 MB/s in K3, and the storm's end (SpringBoard constructor, K7) ≤ 28 s.
  - H2: logd start-up stacks.
  - H3: launchd sampling.
  - H4: a narrower inject.conf or later injection.
- **F6 (C5), lsd at load.** After F5: `EARLY=com.apple.lsd` as the build default (build.sh).
  - Pass: `createAndSeed: entered at` < 28 s; `setSeedingComplete` < 68 s; **`swap #2 ... at` < 80000 ms** (the
    target, without the saved database).
- **F7 (C7, only with the owner's list).** `lsd_hidden2[]` behind `lsd_hide=2`, the owner's groups from §3.3.
  - Pass: `hide: N of 175`; hidden icons gone; the rest as wc5.
