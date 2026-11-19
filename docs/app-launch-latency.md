# App launch latency: SpringBoard's 13.6 s, the app's 4.7 s, and the "background" capture session

Status: analysis and specification only. Nothing was built, run or committed. Written 2026-11-18.
Follows `docs/camera-launch-spec.md`, `docs/camera-session-stops.md` and HANDOFF-2026-11-17-night.md ("Camera launch",
"Camera session stops").

Labels: **[V]** verified: a log line with its time, or file:line. **[I]** inference: its basis and how sure.
**[G]** guess. Times in brackets are kernel seconds from the timestamped console part of a log. "+s" means seconds after
the request (`!auto: X`) unless it says otherwise.

Sources:
- logs `d1 d2 d3 reg2 lg1 t3a t3b g1` (console part and kept part);
- every other `logs/*.log` that has a `!SpringBoard hang` line or a launch after 135 s: about 110 boots, tabulated by
  script (bisA..C, cam2..9, mic0/1, play0/1, md*, set0..setI, cs1, btS1, p0, p1, fp1, vd0, vd1, mv1..5, q0b, m4, persist1..3,
  lc1..3, pb1/2, prof1/2, ...);
- userland/src/{inject_sb.c, inject.c, inject_media.c, inject_tweaks.c, eagl.c, rwtouch.c}, userland/root/etc/inject.conf,
  userland/rc.jobs.{pa,pt,mv9};
- xnu-src IOPMrootDomain.cpp and task_policy.c; xnu/files/bsd/dev/redwood_r1cam.c;
- the iOS 16.5 dyld cache, through `tools/dsc/objcimp.py` (methods by address and by name) and `tools/cache-name.py`.

## 0. Answers in short

1. **The 13.6 s is a boot artefact, not what a person tapping an icon gets.**
   - SpringBoard has not finished starting when the test asks. Its main queue is not drained even once from its
     constructor (~33 s) until [125.9] **[V]**.
     - First it waits on backboardd in `BKSDisplayServicesStart`, until about 57 s.
     - Then on a lock inside LaunchServices (CoreServices, called from ControlCenterServices) while `lsd` works
       from ~52 s to ~113 s.
     - Then it builds its UI (113 → 125.9).
   - Every queued block runs at [125.9-126.2]: two `show_camera_icon` timers that were due ~40 s and ~70 s earlier, the
     unlock, then the Camera notification.
   - Nothing is on the screen until SpringBoard's first frame at [130.4-131.6]. That frame comes at that time in every boot,
     with or without a launch, so a person has no icon to tap before then.
   - A request made after SpringBoard has settled is handled in **0.31-0.41 s** (Camera, 14 runs) and **0.13-0.15 s**
     (Settings, 16 runs) **[V]**.
2. **The 4.7 s is mostly the same boot window, not the app.**
   - In settled runs a cold Camera goes: process exec +0.10 s; session `ClientIsActive` +0.72-0.79 s; stream START
     +0.96-1.0 s; first preview picture +1.92-2.03 s. There is **no** stop and **no** restart **[V]** (13 runs; stop and
     restart checked in 9).
   - A cold Settings shows a changed screen +0.6-0.9 s after the request **[V]**. Those first frames have the home screen's
     average colour, so they are SpringBoard's zoom animation. The app's own first frame is not timed by any line yet.
   - In the boot window, the "first frame" (`swap #2`) is **SpringBoard's own first frame since boot**. It comes at
     [130.4-131.6] in runs that launch nothing as well **[V]**. 2.6 s are lost to the "background" interruption, and
     1.1-1.2 s to powering the sensor up a second time.
   - Shader compiles are **not** paid per app. Only backboardd (the render server) has GL: inject.conf **[V]**. Its
     first-use cost is paid once per boot, around SpringBoard's first frame, where the first GL frames cost 16.6 ms against
     6.5 ms later. A persistent cache would at best take ~1-2 s off that one boot-time frame **[I]**.
3. **"Background" is CMCapture's FigCaptureClientSessionMonitor.**
   - It combines two things published by SpringBoard/FrontBoard: the client's application state (BKS/RunningBoard) and the
     **FrontBoard display layout** (`FBSDisplayLayoutMonitor`). It also watches the device lock state. The method names are
     read from the 16.5 cache **[V]**.
   - The mediaserverd stop comes from the device vendor taking the device back: the
     `-[BWFigCaptureDeviceVendor takeBackDevice:forClient:informClientWhenDeviceAvailableAgain:]` frame on d2's
     `BT device_relinquish_streams` **[V]**.
   - None of our hooks replaces or short-circuits runningboardd, FrontBoard's layout publishing, or the app-state path.
     runningboardd is Apple's, with no library of ours in it **[V]**.
   - The delay comes from SpringBoard/backboardd being busy with their own first frame in the boot window **[I, medium-high]**.
     The interruption ends 8-56 ms after a boot-time `PMRD: changePowerStateWithTagToPriv(ON_STATE, b000b)` at
     [129.7-130.4]. That PMRD line comes in runs with no launch as well **[V]**. Its meaning is a guess **[G]**.
4. **Fixes, ranked by gain per effort:**
   1. Time launch tests after SpringBoard has settled (harness).
   2. Make the sensor power-up faster: r1cam linger on by default, and a faster bit-banged I2C. Up to ~1 s on every cold
      Camera open.
   3. Find out why `lsd` rebuilds for 60 s at every boot, even with a persisted /var. This makes boot-to-usable ~60 s
      shorter.
   4. Measure, then maybe pre-warm, backboardd's first-frame shader work.
   - Nothing to do in the capture path.
   - **One run (D4, §5)** confirms all of this: a Camera launch right after SpringBoard's first frame, a Settings cold
     launch, and a Camera warm relaunch, with the new lines.

## 1. Question 1: the 13.6 s

### 1.1 The facts across boots

| Milestone | Times (kernel s) | Runs | Label |
|---|---|---|---|
| SpringBoard exec (`map_with_linking_np: [7(SpringBoard)]`) | 19.65-19.89 | 12 old full-console logs | V |
| backboardd exec | 22.29-22.82 | same | V |
| `!SpringBoard hang: ... 20 s (heartbeat 0)` #1, stack `BKSDisplayServicesStart` → `_BKSDisplayGetMainScreenInfo` → `mach_msg` | 52.9-53.8 | mic0, play0/1, md2c1-3, rtc2, set2, set4, bisA, cam3-6, gpu4-7 | V |
| `lsd[N] denied attempt to escape CPU monitor` lines, about 12 a second | first ~51-57, last **112.0-114.1** (recent builds); 117-132 in builds before Nov 17 | ~100 boots | V |
| hang #2 (30 s after #1), still **heartbeat 0**, stack `__ulock_wait` ← libsystem_platform ← 5 frames of CoreServices ← ControlCenterServices ← `BSCollectionFilter` ← ControlCenterServices ... ← SpringBoard | 103.1-104.0 | d2, g1, bl1, mic0, set*, md*, lc*, pb*, prof* | V |
| `MiniIOMobileFramebuffer: swap 1 (#0): surfaces 0 0 0 0` (backboardd's first swap: nothing in it) | 113.6-116.1 | same | V |
| `!redwood: status bar: none for this screen` (SpringBoard builds its status bar) | 114.7-117.2 | same | V |
| `!redwood: home grid: SBIconLocationRoot 3x4` (home screen layout) | 116.8-119.8 | same | V |
| first SpringBoard IOSurfaces (`SpringBoard[7] created surface 1`) | d2 125.506 | d2 | V |
| `!icons: still-camera added` twice, 0.26 s apart | 125.5-127.6 | all | V |
| request handled (`camera launched by bundle id`) | d2 126.154, g1 126.370, d3 126.196 | | V |
| backboardd's first GL draw (`redwood-gl: pvr: a draw by thread 355`) | d2 128.911 | d2 | V |
| `PMRD: changePowerStateWithTagToPriv(ON_STATE, b000b)` | 129.67-130.37 | d2, d3, t3a, t3b, g1, lc1, pb1 | V |
| `swap #2: surfaces in layers 0x1 now`, after "2 frames in 16-17 s": the first picture on screen | 130.4-131.6 **without any launch** (lc1 130.4, pb1 131.0, prof1 131.0, rtc1 131.3); 130.4-131.5 with the Camera | | V |

How to read it:

- **[V]** The heartbeat is a `dispatch_async` to the main queue every second (inject_sb.c:1458-1462). `heartbeat 0` at
  [103] means not one main-queue block ran between the watcher's start in the constructor and 103 s.
  - The watcher reports at most twice, 30 s apart (inject_sb.c:1458, 1496). So nothing tells when the main queue first
    answered.
  - The `show_camera_icon(12)` and `show_camera_icon(40)` timers (inject_sb.c:1744, 2154-2155) are main-queue
    `dispatch_after`s, due ~12 s and ~40 s after the constructor. Both ran at [125.88] and [126.14] (d2). So **the main
    queue was first drained at ~125.9**.
  - The unlock (posted at ~107.5) and the Camera notification (posted at [112.54]) both waited behind them
    (inject_sb.c:2269, 2304). `!auto: camera` [112.54] → `launched by bundle id` [126.15] = 13.6 s of queueing.
- **[V]** The constructor ran at about 33 s, inferred from the first report at 53 s after 20 s of quiet. So SpringBoard
  spent ~13 s between exec (19.7) and our constructor, probably in dyld and initialisers **[I]**.
- **[I, high]** From ~33 s to ~57 s the main thread waits for backboardd's display service (hang #1 stack).
- **[I, high]** From ~57 s to ~113 s it waits on a lock in LaunchServices while lsd works. Three things support it:
  - hang #2's stack: a ulock wait in CoreServices under ControlCenterServices's module/bundle filter;
  - lsd's CPU-monitor lines end 1-2 s before `swap 1` and the status bar line, in every boot;
  - HANDOFF-2026-11-16 line 233 says the same: "lsd rebuilding its database on the fresh /var keeps a core busy ~55 s and
    delays SpringBoard". idle1 notes the governor at 100% high with no switch until ~114 s (line 270).
- **[V]** The kernel prints the CPU-monitor line when a process without `com.apple.private.kernel.override-cpumon` asks to
  disable its per-thread CPU monitor (xnu task_policy.c:2799-2868). lsd asks about 12 times a second for ~60 s.
  - **[I, medium]** One request per unit of database work. "Rebuild" is inferred from that and from the fresh /var. No lsd
    log line says it.
- **[I, medium]** From 113 to 125.9 SpringBoard finishes starting on its main thread: status bar, home grid, icon model,
  wallpaper and lock screen.
  - Between the grid line (~118) and the first surfaces (125.5) there is **no line at all**: 7 s unaccounted.
  - No CPU figure covers it. The first `redwood-gl: threads` line is at [139.4] (covering 129.4-139.4: 62% idle, 21%
    kernel, empty program list); backboardd's GL statistics start only with its GL.
- **[V]** With the persisted data volume (`var_store=1`: persist2 generation 68, persist3 generation 70, Nov 14 build) lsd
  still made 489-649 such lines from 61 to 119 s, and SpringBoard was just as late (icons at 137-138).
  - **So it is not (only) the RAM /var.** Why lsd redoes its work at every boot is open (§4, F3).

### 1.2 What a person would see

- **[V]** The display shows no frame from iOS before SpringBoard's first frame at ~131 s (`2 frames in 16613 ms` at d2
  [131.05]: the only earlier swap, at 114.4, has no surfaces). There is no icon to tap before ~131 s.
- **[V]** A request after SpringBoard has settled is quick. Request → `launched by bundle id`:

| | runs | request at | handled after |
|---|---|---|---|
| Camera | p0, p1, fp1, vd0, vd1, mv1, md3c1, md3d1, mdchk, mdchk2, mdchk3, q0b, m4, swwait | 145-193 s | 0.31-0.41 s |
| Settings | set0-setI, cs1, btS1, san2 | 145.3-146.3 s | 0.13-0.15 s |
| Settings, from the open Camera | vm1-vm5, reg2, lg1 | 171.9 / 186.4 s | 0.46-0.70 s |

- **[G]** Between ~131 s and ~145 s nothing has been measured. A tap right after the first frame might still meet a busy
  main thread (the wallpaper `dispatch_after` of 15 s, widgets, the first snapshots). D4 measures it with a request at
  ~132.5 s.
- Answer: a person who taps an icon once the home screen is up waits a fraction of a second for SpringBoard. The 13.6 s
  exists only because the test posts its request during SpringBoard's start. **Boot to usable** is the real cost: about
  131 s, of which lsd's ~60 s window is the largest part.

## 2. Question 2: where launch time goes

### 2.1 Settled cold launches (the user's case) **[V]**

| Phase | Camera (13 runs; START/stop read in 9: p0 p1 fp1 mv1 vd0 vd1 md3c1 q0b m4) | Settings (16 runs) | Line |
|---|---|---|---|
| request handled, process exec'd | +0.10 | +0.13-0.15 | `map_with_linking_np: [N(Camera)]` |
| `launchApplicationWithIdentifier:` returned | +0.31-0.41 | +0.13-0.15 | `launched by bundle id` |
| capture session asks for the device | +0.72-0.79 | | `device set: ClientIsActive` |
| stream START | +0.96-1.00 | | `stream: START from the capture graph` (p0 146.554, md3c1 169.092 ...) |
| first preview picture handed to the graph | +1.92-2.03 | | `PrimaryScaler: the first picture given` |
| first changed frame on screen | +1.0-1.5 | +0.6-0.9 | first `swap N (#N-1)` after the idle `2 frames in 14-16 s` |
| stream stop / restart | **none** for ≥ 25 s | | (mv1's stop at +30.5 is the scenario) |

- **[V]** The first frames after a request have the home screen's average colour: p0 `red 155 green 176 blue 167`, set3
  `158 179 169`. The Camera preview is about 120-131 / 90-103 / 92-103. So the first frames are SpringBoard's zoom
  animation. When the app's own first frame is on screen is **not** in any log.
- **[V]** From START to the first picture takes ~1.0 s. It is the sensor power-up, and 0.95 s of it is the bit-banged register
  tables (camera-session-stops §0, reg2 `power: on at 130478 ms` → `stream is on at 131590 ms`). The bit-bang uses
  `delay(5)` per SCL/SDA edge (redwood_r1cam.c:281-306) for 422 writes. **This is the largest single cost of a cold
  Camera open after SpringBoard.**
- Warm relaunch of the Camera (process alive, suspended): **never measured**. D4 does it.
- Photos: no launch with timing in any kept log. The only Photos launch, soak.log, aborted. Not answerable from logs.

### 2.2 The boot-window Camera launch (d2/t3b/d1/d3) **[V]** unless marked

| Kernel s (t3b) | Since handled | What |
|---|---|---|
| 112.5 | | `!auto: camera` posted; it waits on SpringBoard's main queue (§1) |
| 126.15 (d2) | 0 | handled |
| 126.36 (d2) | +0.21 | exec |
| 126.50 (d2) | +0.35 | our constructor in Camera (`t3 hooks` line; dyld plus the app's own initialisers before it) |
| 126.84 (d2) | +0.69 | `beginConfiguration (main 1) 0.352 s after launch` ("launch" = our constructor) |
| 127.51 | | `startRunning 0.660 s` |
| 127.70 | | `ClientIsActive`; the graph builds; START [127.90]; sensor power-up; first picture [129.33] |
| 127.76 (d1/d3: +0.88-0.91 app-s) | | **`AVCaptureSessionWasInterruptedNotification` reason 1** (d1/d3 kept) |
| 129.71 | | the device vendor takes the device back: `stream: stop` (§3.1). The power-up just done is thrown away |
| 129.7-130.4 | | boot-time `PMRD ... ON_STATE` (also in runs with no launch) |
| 130.0-130.4 | | interruption ends (d1 `InterruptionEnded` +3.494 app-s ≈ [130.03]; d3 restart [130.115], 8 ms after PMRD [130.107]; t3b restart [130.426], 56 ms after PMRD [130.370]) |
| 130.51 | | START again; the second power-up: 0.95 s of tables |
| 131.54 | | `swap #2`: **SpringBoard's first frame since boot** (it comes at this time without a launch too) |
| 131.7 | | first live picture; a second interruption is possible (d1: +3.523 → ended +4.875 = [131.41], 0.2 s after swap #2) |
| 130.9 (d3) | | first touch delivered to the app: 0.47 s after swap #2 |

So the boot-window ~5 s splits as follows:
- ~0.65 s of app start to `startRunning`. This is the same in settled launches.
- ~2.7 s inside the "background" interruption. It ends with SpringBoard's/the system's boot milestones, not with the app.
- ~1.2 s of the second sensor power-up.
- The first frame is SpringBoard's, timed by its own start.

None of the 5 s is the shared cache or our constructors in a measurable way: exec → our constructor 0.15 s, constructor
→ first `beginConfiguration` 0.35 s.

### 2.3 Shader compilation: whose, when, and would a cache help

- **[V]** Only backboardd loads the GL stack:
  - etc/inject.conf (`userland/root/etc/inject.conf`) puts `libredwood.dylib` in backboardd and a few daemons;
  - eagl.c:3: "server (backboardd; libredwood.dylib loads this ...)".
  - Camera and Preferences get `libredwood-media` / `libredwood-tweaks`, which have no GL.
- **[I, high]** Apps draw through CoreAnimation, and the render server in backboardd turns their layers into GL. **No app
  pays its own first-use shader compilation.** The EOT/load-op caches of swipe-profile.md §6 live once, in backboardd, for
  the whole boot.
- **[V]** backboardd's GL starts with SpringBoard's first frame:
  - first draw [128.91] (d2);
  - `!MESA: pvr: eot kept: 1 programs` at [129.07];
  - then 126 EOT misses in 129-139 s and 0-1 per 10 s after.
  - The first 10 s window: `277 frames in 10 s, 16.6 ms each ... 11.3 in glFlush`, against 6.5 / 4.2 ms in every later
    window.
  - **[I, medium]** ~7 ms × 277 ≈ 2 s of extra render-thread time in 129-139 s is first-use work: GLSL compiles of CA's
    ~23 programs (`27 shaders, 23 programs` in the 800-frame line), Zink/pvr pipeline and EOT compiles, BO creation, first
    texture uploads. How it splits is not measured.
- **[I, medium]** The EOT key holds the PBE words, which hold surface addresses (swipe-profile §6). So a new app's new
  render targets cost EOT compiles. 126 misses at ~0.2-0.5 ms each (from lc1: 741 profiler samples at 2.3 kHz over 1478
  misses) is **~30-60 ms**: small.
- A persistent on-disk cache (Mesa disk cache for Zink/pvr, kept in /private/var with var_store) or a pre-warm frame would
  help **only SpringBoard's first frame at boot** (≤ ~2 s), not later launches. Rank it after a measurement (F4).
- Lines missing to tell: per-compile time in backboardd, and the app's own first frame (§6, T-B4, T-B5).

## 3. Question 3: why the app is "background" for seconds

### 3.1 The signal, from the 16.5 cache **[V]**

`python3 tools/dsc/objcimp.py` and a selector search over CMCapture/FrontBoard/SpringBoard give:
- CMCapture `FigCaptureClientSessionMonitor`:
  - `-initWithClientAuditToken:forThirdPartyTorch:applicationAndLayoutStateHandler:interruptionHandler:` 0x1a1f6b748;
  - `-updateApplicationState` 0x1a2038fd4;
  - `-applicationState` 0x1a1f827bc;
  - `-layoutMonitor:didUpdateLayout:` 0x1a20393a4;
  - `-deviceLockStateMonitor:didUpdateDeviceLockState:` 0x1a2039628.
  - Its selector table also has `_isBKSApplicationStateMonitoringRequiredForClient`, `_isLayoutMonitoringRequiredForClient`,
    `_isDeviceLockStateMonitoringRequiredForClient`.
- CMCapture `FigCaptureDisplayLayoutMonitor`: `-initWithFBSDisplayLayoutMonitorCreateFunction:displayType:` 0x1a20c5bd8,
  `-isOnHomeScreen`, `-isOnLockScreen`. `BWFigCaptureSession` also has `-layoutMonitor:didUpdateLayout:` (0x1a20950b4).
- FrontBoard (inside SpringBoard) `FBMainDisplayLayoutPublisher`:
  - `-flush` 0x1a26078b0;
  - `-transitionAssertionWithReason:` 0x1a260b6ec;
  - `-isTransitioning`, `-currentLayout`, `-setBacklightLevel:` 0x1a26110d4.
- SpringBoard's layout element updates:
  - `-[SBSwitcherController _updateDisplayLayoutElementForLayoutState:]` 0x1c4d489fc;
  - `-[SBLayoutElementViewController _updateDisplayLayoutElementVisibility]` 0x1c4a1abf4;
  - `-[SBFullScreenSwitcherSceneLiveContentOverlay _updateDisplayLayoutElementActive]` 0x1c4c22150.
- The stop's own path (d2 kept, `cam_bt=1`):
  - `BT device_relinquish_streams` includes `-[BWFigCaptureDeviceVendor takeBackDevice:forClient:informClientWhenDeviceAvailableAgain:]+0xdc`;
  - `BT stream_stop` runs from CoreFoundation's notification callout into `-[BWFigCaptureDevice dealloc]` →
    `-[BWFigCaptureStream dealloc]`. objcimp names the nearest method at or before an address, so the deeper frames are
    approximate.

So:
- The client is "foreground" for mediaserverd when its **application state** (RunningBoard/FrontBoard via BKS) and the
  **display layout** that FrontBoard publishes for SpringBoard agree. The lock state counts too.
- When they do not, the session monitor's interruption handler fires: reason 1 `VideoDeviceNotAvailableInBackground`.
  The vendor takes the device back and the stream is torn down.
- When the state turns foreground, `informClientWhenDeviceAvailableAgain` sends `InterruptionEnded`, and CameraUI
  restarts the session.

**[I, medium]** Which of the inputs was "background" at +0.9 s is not logged. The candidates, in order:
1. The display layout still without the Camera, or still "transitioning" while SpringBoard animates the launch. SpringBoard
   is also drawing its own first frame at the time. The second interruption ending 0.2 s after `swap #2` (d1) fits a layout
   published when the launch transition completes.
2. The app state before the scene is foreground.
3. The lock state or backlight just after the queued unlock. The first ending is within 8-56 ms of the boot-time PMRD
   ON_STATE, which is an `evaluatePolicy` adjust (tag 0xb = kCPSReasonEvaluatePolicy, IOPMrootDomain.cpp:219, 8835) and
   happens without a launch too.

D4's mediaserverd and SpringBoard lines name which one it is (§5).

### 3.2 Our code that sits on these paths, and whether it can delay them

| Path | Ours? | Where | Can it delay "foreground"? |
|---|---|---|---|
| runningboardd (app state, assertions) | Apple's binary, plist `com.apple.runningboardd.plist`; no library of ours (inject.conf has no runningboardd) | userland/root/etc/inject.conf | **no** **[V]** |
| FrontBoard scene/layout publishing (in SpringBoard) | SpringBoard has `libredwood-sb` and `libredwood-tweaks`. Their replaced methods: `SBLockdownManager _setupActivationState`, `SBSetupManager _setSetupRequiredReason:`, `XBApplicationSnapshotManifestImpl +_outputFormatForSnapshot:` (JPEG), `SBLockHardwareButton longPress:`, `SBMainDisplayPolicyAggregator _allowsCapabilityPosterSwitcher...`, `SBAppSwitcherPageView _createOrDestroyWallpaperOverlayView`, `SBHDefaultIconListLayoutProvider makeLayoutForIconLocation:`, UIDevice/_UIBatteryView battery getters; CADisplayLink only with `dl_dump=1`/`wk_trace=1` | inject_sb.c:2162-2197, 1969-1985 | **no direct effect** **[V]**: none touches scenes, transitions or the layout publisher. Indirect only: anything that keeps SpringBoard's **main thread** busy delays its transition and layout. In the boot window that is iOS's own start (§1), plus the queued blocks of ours that run first at 125.9: the icon timers, the 15 s wallpaper `put_wallpaper(0)` (inject_sb.c:2240-2242), the unlock (2269, 2356). Their cost is not measured **[G: small]** |
| The test's own requests | `com.redwood.unlock` and `com.redwood.camera` on the main queue | inject_sb.c:2269, 2304 | yes, as an artefact: the launch is requested while the unlock transition is still running **[I]** |
| backboardd window server | logging pass-throughs of `is_ready`, `ignore_update_p`, `begin_update`, `finish_update`, `render_update`; `renderer` returns NULL only after a failure | inject.c:1291-1325, 128-209 | **no** **[V]**: each calls the original and returns its answer |
| backboardd touches | `cancelTouchesOnDisplay:` is Apple's, called by SpringBoard at the launch (d3 [126.44]); our DOWN path only logs | inject.c (touch_lat) | no, for the app state; it explains the lost first taps (camera-session-stops §5) |
| framebuffer | `SwapWait` answers success at once; vsync is a free-running timer | MiniIOMobileFramebuffer.cpp:1923-1928, 1802-1813 | **[I, low]** It can only make CA think frames are done sooner, not later. If FrontBoard waited for a "frame displayed" callback that our driver never sends, the transition would hang, and it does not |
| brightness | `BrightnessSystem setProperty:forKey:` / `copyPropertyForKey:` mirror into our sysctl and call the originals (only with `r1_bl`, not in these runs) | inject.c:1430-1480 | no **[V]** |

Conclusion **[I, medium-high]**: none of our stubs short-circuits the foreground signal. In the boot window the signal is
late because SpringBoard and backboardd are still starting: SpringBoard's main queue only began to run at 125.9, and
its first frame is at ~131. The settled launches do not interrupt at all (§2.1). So this is not a capture-path bug, and
it needs no capture-path fix.

## 4. Fixes, ranked by expected gain per effort

| # | Fix | Gain | Effort / risk | Basis |
|---|---|---|---|---|
| F1 | **Harness: request launches after SpringBoard has settled.** Wait for `swap #2` + 2 s or a fixed ≥ 135 s, not "start + 85 + 6". Keep one boot-window case only as a boot test | Removes the 13.6 s, the interruptions, the restart and the lost first taps from every launch measurement; measurements become what a user gets | rc.jobs edits only; no risk | §1.2, §2.1 |
| F2 | **Sensor power-up faster on every cold Camera open.** (a) `r1cam_linger_ms=3000` on by default: exists, measured -1.11 s on a restart (lg1), and a warm relaunch within 3 s skips the tables. (b) A boot-arg for the bit-bang edge delay (`r1cam_i2c_us`, default 5): 2 µs would cut the 0.95 s of tables to ~0.4 s **[I]**; a hardware I2C controller to ~0.05 s | (a) -1.1 s on a relaunch or restart; (b) -0.5 s (2 µs) to -0.9 s (HW I2C) on **every** cold open, which is about half of the settled 1.95 s to the first picture | (a) a default; caveat in HANDOFF (sensor powered ≤ 3 s more). (b) small and boot-arg gated; the risk is I2C errors with the SoC's weak internal pull-ups (camera-session-stops §4; redwood_r1cam.c:281-306) | §2.1 |
| F3 | **Find why lsd does ~60 s of work at every boot**, even with var_store's persisted /var (persist2/3). Candidates [G]: the root volume (`rd=md0`, a new identity each boot), the build/version keys lsd checks, or the database path being outside what var_store keeps. First step static: find the csstore in a var_store image and lsd's rebuild triggers in its strings | SpringBoard usable ~55-60 s sooner after boot (113 → ~55 s for the end of the LaunchServices wait) | unknown, investigation first; changing lsd's database is risky | §1.1 |
| F4 | **backboardd first-frame shader work**: measure first (T-B4). If CA's compiles are ≥ 1 s, pre-warm at backboardd start (one offscreen frame with CA's common programs), or turn on Mesa's disk cache in a var_store-kept directory | ≤ ~2 s, once per boot, on SpringBoard's first frame only | medium; a cache in /var interacts with var_store; pvr's pipeline cache support is unverified | §2.3 |
| F5 | EOT cache keyed without surface addresses (addresses patched at emit) | ~30-60 ms per new app's first frames | medium Mesa change | §2.3 |
| — | Faking foreground for the capture monitor, or pushing `ClientIsActive` early | none in settled launches; would break iOS's privacy semantics | not proposed | §3 |

## 5. The one diagnostic run (D4)

Goal, one boot:
- H1: SpringBoard's main queue first runs at ~126, and a request right after its first frame is handled in < 0.5 s.
- H2: a launch after SpringBoard's first frame is not interrupted, and the monitor's inputs turn foreground before the
  session starts. In the boot window, the input that is late is named.
- H3: backboardd's compile cost sits at the first frame, and later launches compile little.
- H4: the warm relaunch of the Camera, and a cold Settings for comparison.

**Build:** non-BT, `RC_JOBS=rc.jobs.lat`, with tasks T-A1, T-A2, T-B1..T-B5 below (all logs only, all behind the boot-args
named).

**Run:** `tools/r1-run.sh d4 230 "<BASE> vdiag=1 cam_bt=1 touch_lat=1 sb_hang_every=5 glcompile=2"`.
- BASE is the camera-session-stops base: `mtkaudio=2 r1cam=4 pvr=6 pvr_gl=1 fbaspect=1 glwait=3 glthreads=1 cam_fps=10
  fbscale=1 r1_clk=1 r1_cpufreq=2200 r1_gpu=680 glyuv=1 autotest=1 venc=4 mtkmic=2`.
- No linger, Wi-Fi, `pvr_stats` or `var_store`.
- Command line: d1's was 335 characters; this adds 34. md3b1 overflowed at ~475.

**`userland/rc.jobs.lat`** (lines of 246 and 134 characters; times from `say start` ≈ 21.5 s):

```
# D4 (docs/app-launch-latency.md): Camera right after SpringBoard's first frame, Settings cold, Camera warm.
/redwood/bin/rwtouch say start wait 85 tap 0.5 0.06 wait 1 notify com.redwood.unlock wait 25 say camera notify com.redwood.camera wait 8 say shot1 tap 0.5 0.887 wait 20 say home1 button home wait 6 say settings notify com.redwood.settings wait 12
/redwood/bin/rwtouch say home2 button home wait 6 say camera2 notify com.redwood.camera wait 4 say shot2 tap 0.5 0.887 wait 10 say end
```

| Marker | At about | Tests |
|---|---|---|
| unlock | 107.5 | still queued behind SpringBoard's start (H1 control) |
| `!auto: camera` | 132.5 | just after SpringBoard's first frame (~131): handling time, interruption or not |
| shot1 | 140.5 | 8 s after the request: must be `tap 1 received` |
| `!auto: settings` (after home1) | 166.5 | cold Settings, settled |
| `!auto: camera2` (after home2) | 184.5 | warm Camera: same pid, or a new `map_with_linking_np` |
| shot2 | 188.5 | 4 s after the warm request |
| end | 198.5 | run ends at 230 s, under 250 s |

**Read-out:**

| Seen | Means |
|---|---|
| `SpringBoard: main queue first answered at +N s` ≈ 126 - 33; `sb hang at` lines in BackBoardServices until ~57, in CoreServices until ~113, then in SpringBoard/UIKit frames | H1 confirmed; the 113-126 frames name SpringBoard's last startup work |
| `SpringBoard: notification com.redwood.unlock received (off main)` at ~107.5 and `redwood: unlock: asked` at ~126 | the queueing is SpringBoard's start, not the notification |
| `camera launched by bundle id` < 0.5 s after `!auto: camera` | H1: a user's tap is not delayed |
| no `notice AVCaptureSessionWasInterruptedNotification` for the 132.5 launch, exactly one `stream: START` | H2: interruption only during SpringBoard's start |
| an interruption at 132.5 anyway | SpringBoard is still settling at 132 s; then the `client monitor` lines name the input (state value, layout without `com.apple.camera` or `transitioning 1`, lock state) |
| `mediaserverd: client monitor: ... applicationState 8` (or whatever value) and `layout ... com.apple.camera` lines before `ClientIsActive = true` | which signal gates "foreground", with times |
| `redwood-gl: compiles 10 s:` large in the window of SpringBoard's first frame and small for the later launches | H3; the total says whether F4 is worth it |
| `camera app: notice UIApplicationDidBecomeActiveNotification at +S` and `first idle main run loop at +S` for both Camera launches, `settings app:` the same | the app's own first frame / activation, the missing line of §2.1 |
| camera2: no new `map_with_linking_np: [N(Camera)]`, `startRunning` on the same session object, `PrimaryScaler: the first picture` < 1.2 s after the request | warm relaunch numbers (H4); a new pid means the app was killed and this is a cold launch with warm caches |

## 6. Implementation tasks (Sonnet-sized, logs only, all gated)

Rules as in HANDOFF: non-BT build per run, `tools/r1-run.sh` directly, rc.jobs lines < 400 characters, tell the user
before a device run. Keep each new keep/host line short: the kept log is 64 KB.

### T-A1: SpringBoard hang watcher, compact and repeated (userland/src/inject_sb.c, `hang_watch` 1452-1499, `watch_for_hangs` 1502-1516)

- Boot-arg `sb_hang_every=N` (default 0 = today's behaviour).
- With N > 0, report every N s of quiet, up to 30 reports, as **one line** each:
  `sb hang at +S: <12 unslid return addresses in hex>`, unslid with `slide` as elsewhere in the file. The host names them
  with `tools/cache-name.py` / `tools/dsc/objcimp.py`.
- When the heartbeat first moves after any quiet ≥ 2 s, print once:
  `SpringBoard: main queue first answered at +S s (after the constructor), quiet Q s`.
- Pass, kept log of D4:
  - that line with S ≈ 90-95;
  - ≥ 10 `sb hang at` lines;
  - the decoded frames show `BKSDisplayServicesStart` early and CoreServices/ControlCenterServices later.

### T-A2: SpringBoard: when requests arrive, and when the layout is published (inject_sb.c)

- In the constructor, add a second `notify_register_dispatch` for `com.redwood.camera`, `com.redwood.settings` and
  `com.redwood.unlock`, on a **global** queue. Each prints `SpringBoard: notification NAME received (off main)`.
  - The existing main-queue handlers are unchanged.
  - The unlock handler's `say("redwood: unlock: ...")` (inject_sb.c:555-565) becomes `keep_line` so it reaches the kept log.
- Gate on `vdiag=1`: wrap `-[FBMainDisplayLayoutPublisher flush]`, with `method_setImplementation` and the original
  called first.
  - For the first 40 calls after any of those notifications, print
    `SpringBoard: layout flush: transitioning %d, %.240s`, using `isTransitioning` and the description of `currentLayout`.
  - Also wrap `-transitionAssertionWithReason:` and print `SpringBoard: layout transition assertion: %.80s`.
- Pass:
  - the three `received (off main)` lines at ~107.5, ~132.5 and ~166.5;
  - `layout flush` lines that name `com.apple.camera` within a second of `camera launched by bundle id`.

### T-B1: mediaserverd: FigCaptureClientSessionMonitor inputs (userland/src/inject_media.c, `image_added` 283 / the mediaserverd branch 1866-1910)

- When CMCapture is loaded and `vdiag=1`, wrap these methods of `FigCaptureClientSessionMonitor`:
  - `updateApplicationState`;
  - `layoutMonitor:didUpdateLayout:`;
  - `deviceLockStateMonitor:didUpdateDeviceLockState:`.
- Call the original, then print
  `mediaserverd: client monitor: SEL, applicationState %d, arg %.200s` (first 40 calls).
  - The application state is the return of `-applicationState`; check its type encoding before calling it.
  - The argument is the layout or lock-state object's description.
- Pass, D4 kept log:
  - at least one line before the 132.5 launch's `ClientIsActive = true`;
  - in a boot-window run, a state change within 0.1 s of `AVCaptureSessionInterruptionEndedNotification`.

### T-B2: Camera activation and first idle (inject_media.c, `camera_app_probe` 1300-1330, vdiag only)

- Observers (queue nil) for `UIApplicationDidBecomeActiveNotification`, `UISceneWillEnterForegroundNotification` and
  `UISceneDidActivateNotification`. Each prints `camera app: notice NAME at +S` with the existing `app_launch_ns`.
- A `CFRunLoopObserver` on the main run loop, `kCFRunLoopBeforeWaiting`, removed after it fires once:
  `camera app: first idle main run loop at +S`.
- Also `camera app: constructor done at +S`, at the end of `redwood_media_init`'s Camera branch (1849-1860).
- Pass: all three kinds of line for both Camera launches in D4. The warm relaunch shows `DidBecomeActive` again with no
  new constructor line.

### T-B3: the same for Settings (userland/src/inject_tweaks.c, constructor 501)

- If `getprogname()` is `Preferences` and `vdiag=1`, add the same three notices and the first-idle observer, as
  `settings app: ...`. The launch time is taken in the constructor.
- Pass: `settings app: first idle main run loop at +S` in D4, with S < 1.5.

### T-B4: backboardd compile time (userland/src/inject.c `my_compile_shader` 421-440; the eagl.c 10 s line near 1565)

- With `glcompile=2`, time `orig_compile_shader` with `mach_absolute_time` and add up the count, total and maximum.
- Every 10 s, next to the existing `redwood-gl: pvr: N frames in 10 s` line, print
  `redwood-gl: compiles 10 s: N, total M ms, longest K ms`.
- Also print once `redwood-gl: first frame: first draw at T1, its glFlush done at T2`. The first draw is already logged
  (`a draw by thread`).
- `glcompile=1` keeps today's meaning: the source text.
- Pass: the compile line for the window of SpringBoard's first frame (~129-139) and for the later launches.

### T-B5: rc.jobs.lat (userland/rc.jobs.lat)

- Exactly the two rwtouch lines of §5, with the comment.
- Pass: `!auto: camera`, `!auto: settings`, `!auto: camera2` and `!auto: end` all in the console, and the run ends
  `reason 0`.

### T-C1 (after D4, only if it confirms F2): r1cam I2C edge delay (xnu/files/bsd/dev/redwood_r1cam.c 281-306)

- Boot-arg `r1cam_i2c_us` (default 5) in place of the literal `delay(5)` of `cam_bb_scl`/`cam_bb_sda`.
- Pass, in an rc.jobs.lat run with `r1cam_i2c_us=2`:
  - `sensor: the tables` → `all acknowledged` with no NAK lines;
  - `power: on at A ms` → `stream is on at B ms` with B - A < 600 (today 1110);
  - `PrimaryScaler: the first picture` < 1.5 s after a settled Camera request;
  - an rc.jobs.mv9 regression with 3 photos in the library.

### T-C2 (after D4): harness defaults (F1)

- In rc.jobs.pa, .mv7-.mv9 and the autotest of rc.jobs, move `notify com.redwood.camera` to ≥ 135 s after boot:
  `wait 25` in place of `wait 5` after the unlock.
- Turn `r1cam_linger_ms=3000` on in the stay image args only if the user accepts the 3 s powered tail.
- Pass: in rc.jobs.mv9, `camera launched by bundle id` < 0.5 s after `!auto: camera`, one `stream: START` before shot1,
  and three `THE PHOTO IS IN THE LIBRARY`.
