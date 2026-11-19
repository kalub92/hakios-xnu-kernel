# Camera launch: the lost first tap, the "dead" Camera, the double-painted frames

Status: analysis only. Nothing was implemented, built or run. Written 2026-11-18.

Sources:
- the last three sections of HANDOFF-2026-11-17-night.md;
- `logs/{g1,vm1..vm5,vd10,vd12,vd13,A}.log`, both parts: the timestamped console part, and the kept part after
  "kept for the host";
- the syslog lines in those logs (`backboardd`, `com.apple.xpc.launchd|`);
- `userland/src/{inject_media.c,inject_sb.c,rwtouch.c,camera_device.c}` and `userland/rc.jobs.*`;
- `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`;
- HANDOFF-2026-11-16.md (Updates 2-5);
- docs/open-mapple-{plan,gpu,kernel}.md;
- Open Mapple `MappleFramebuffer.cpp`, read only.

Labels:
- **[V]** verified: read from a log line or from code; the file and timestamp are given.
- **[I]** inference: says what it rests on and how sure it is.
- **[G]** guess.

Times in brackets are kernel seconds. Syslog wall-clock times were turned into kernel seconds as `kernel = wall − epoch +
~17 s`, where `epoch=` comes from the boot-args line. The offset was taken from vd12, where the rwtouch `swipe` at [244.24]
matches the backboardd Lock-button event at 16:42:16.38. It was checked on vd12 rec1/rec2 and on vd10 rec1/rec2 to within
±1 s.

## 0. Summary

1. **The lost first tap is not caused by the photo library, and the main thread is not blocked.** Five things show it:
   - The early open runs on a UTILITY global queue (`inject_media.c:1037`), not on the main thread.
   - The main-thread watchdog (`main_watch`, `inject_media.c:988`, reports any stall of 2 s or more) printed nothing in
     g1 or vm1-vm5. It does work: A.log shows lines at [265.87].
   - In vm2 the shot1 tap landed **inside** the library open (127.9-134.8) and was received: `[131.744018] tap 1 received`.
   - In every run, shot1 lands within ±0.25 s of the **first frame the Camera puts on the screen**: `swap #2` at
     [131.32-131.57], about 5.0 s after the launch, and that frame is still black. The tap was taken when it came 0.24 s
     after that frame (vm2) and lost when it came 0.18 s after it or earlier (vm1, g1, vm3, vm4, vm5).
   - The "20 s after launch" is 15 s of SpringBoard holding the launch notification (its main thread was hung), plus 5.2 s
     of the real launch.

   A real user would not get a dead shutter for 15-25 s. They would wait about 5 s on the first launch after a boot for
   the Camera to appear at all, and a shutter tap works from then on.
2. **In vd10 and vd12 the Camera did not die of its own fault. It was launched into a locked phone with the display off,
   and never came to the front.**
   - Auto-lock blanked the display during the scenario's long idle wait: `backlight-off ... displayState: off`, vd12 ≈[190].
   - The scenario's unlock was refused: `redwood: unlock: asked, answer 0`, vd12 ≈[208.9].
   - SpringBoard then launched the Camera anyway.
   - The Camera never configured a capture session: no `ClientIsActive`, no assetsd. Nothing was presented on screen
     until the next touch woke the display.

   The early video_test in vd12 only mattered because it pushed the scenario 102 s later (`!auto: start` [123.84]). The good
   runs launch the Camera at [126.4], before auto-lock can fire. Whether the app was suspended or killed is open; the log
   points to suspended (§2.4). One cheap run (T2) separates the two.
3. **The "double-painted camera frames" are 2-3 GL frames rendered by backboardd per camera picture after a home swipe**
   (HANDOFF-2026-11-16: 2.01 fr/pic with swipes, 1.0 without). Open Mapple plan item 2.6 (flip at frame end, vsync locked to
   the panel, present after the GPU) **would not plausibly fix it**:
   - our present already waits for the GPU (`MiniIOSurfaceDrawn` in `show()`);
   - `fb_rdma=1`, which paces presents on the panel's frame end, left fr/pic at 2.01 (runs r0 and r1b);
   - by our own driver's note, CoreAnimation does not use our vsync notifications.

   Item 2.6 is about tearing. It is medium in size and risk, and needs an MT6765 OVL0 shadow-register check first.

## 1. The lost first tap (problem 1)

### 1.1 Timeline of the six runs that tapped 20 s after the notification

All runs: `!auto: camera` at ≈[111.4], SpringBoard hang reported at ≈[103.1], scenario `wait 20 say shot1 tap 0.5 0.887`.

| run | launched by bundle id | 1st session START → stop | 2nd START | early open begins → opened | first frame (`swap #2`) | shot1 | tap 1 received |
|---|---|---|---|---|---|---|---|
| g1 | 126.370 | 127.774 → 129.622 | 130.326 | 130.655 → 136.240 (8450 ms) | 131.419 (black) | 131.541 | **lost** (139.923 is shot2) |
| vm1 | 126.367 | 127.702 → 129.510 | 130.284 | 130.665 → 135.199 (7548 ms) | 131.370 | 131.551 | **lost** (139.938) |
| vm2 | 126.356 | **none** (`ClientIsActive` only at 130.173) | 130.389 | **127.857** → 134.821 (7122 ms) | 131.317 (black) | 131.559 | **131.744 received** |
| vm3 | 126.538 | 127.805 → 129.697 | 130.518 | 128.025 → (open line not in the kept log) | 131.505 | 131.537 | **lost** (139.927) |
| vm4 | 126.553 | 127.796 → 129.673 | 130.586 | 130.867 → 135.607 (7526 ms) | 131.571 (black) | 131.545 | **lost** (139.962) |
| vm5 | 126.547 | 127.848 → 129.694 | 130.644 | 130.897 → 135.787 (7800 ms) | 131.564 | 131.589 | **lost** (139.996) |

**[V]** All from the console part. The lines are `!redwood: camera launched by bundle id: 1`,
`!redwood: camera: stream: START from the capture graph`, `!redwood: camera: stream: stop`,
`camera app: the early open begins`, `camera app: the photo library opened early in N ms`,
`MiniIOMobileFramebuffer: swap #2: surfaces in layers 0x1 now (0x0 before)`, `!auto: shot1` and
`camera app: tap 1 received`. "Black" is the 5 s stats line right after it, for example g1 `[131.421949] ... this frame on
average red 0 green 0 blue 0; 0% of it not black`. That line follows `[131.420888] 2 frames in 16952 ms`, so the screen had
been frozen since about [114.5].

**What this shows:**

- **[V]** The library open does not drop taps: in vm2 the tap was received while the open was 3.9 s in, with 3.1 s still
  to go.
- **[V]** No `camera app: main thread blocked` line in g1 or vm1-vm5. The watchdog is armed in `capture_hook` before
  `library_early` (inject_media.c:1145-1146), and it does fire elsewhere: A.log
  `[265.868461] camera app: main thread blocked 2233 ms`. So the main thread never stalled for 2 s or more. Shorter stalls
  are not excluded.
  - **The handoff's "Camera main thread busy 128-136" is not supported by any log line.**
- **[V]** The early open is not on the main thread: `library_early()` does `dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ...)`
  (inject_media.c:1037).
- **[V]** The tap was taken only where it came at least about 0.2 s after the Camera's first frame on screen (vm2 +0.242 s). It
  was lost at +0.18 s (vm1), +0.12 s (g1), +0.03 s (vm3), −0.03 s (vm4) and +0.02 s (vm5).
  - **[I, medium]** The tap is lost because the app has not finished launching. Its first frame on screen is still black,
    and the restarted capture session delivers its first kernel frames at the same moment:
    - g1 `[131.545584] frames of 1632x1224 from the kernel`;
    - vm4 [131.732];
    - vm2's only session had given its first preview picture at [131.535], 24 ms before its tap.
  - Which of the two (UI not ready, or capture not running) makes CameraUI ignore the tap is not known. There is no touch log
    in the Camera, so "touch not delivered" vs "touch ignored" is also open. T3 adds the lines.
- **[V]** In 5 of 6 runs the capture session is started, stopped about 1.85 s later, and started again about 0.7 s after that.
  Each START costs about 1.2 s to the first frame, because the kernel powers the sensor down and up again each time:
  - g1 kept log: `r1cam: power: off ... the CAM domain is off again`, then `r1cam: PMIC HWCID ... CAM domain: PWR_ON`;
  - START [130.326] to first frame [131.546].

  vm2 has a single later start. **The cause of the restart is unknown** (T3 logs `startRunning`/`stopRunning` callers).
  - It is not the early open: in vm3 the open began at [128.03], before the stop; in g1 it began at [130.66], after the
    restart; both restarted.

### 1.2 What the ~8 s library open is

- **[V]** On every boot the library is created from nothing:
  - `Photos.sqlite MISSING; Photos.sqlite-wal MISSING; Photos.sqlite-shm MISSING` in all ten logs;
  - `/private/var` is the kernel's RAM volume: vd13 head `[ ok ] mount a memory filesystem on /private/var`, and the comment
    at inject_media.c:565.

  The database is written to RAM, not to the eMMC.
- **[V]** It is assetsd's work, started by the Camera. assetsd is spawned 3.9-4.2 s after the launch, at the same moment in
  every run:
  - `map_with_linking_np: [45(assetsd)]` g1 [130.404];
  - vm1 [130.394], vm2 [130.291], vm4 [130.645], vm5 [130.741].

  The library answers 4.5-5.6 s after assetsd's exec: g1 [130.404 → 136.240], vm2 [130.291 → 134.821],
  vm4 [130.645 → 135.607].
- **[V]** The measured 7.1-8.5 s also covers the Camera's own `authorizationStatusForAccessLevel:`.
  - In g1 the block began at 127.79 (= 136.240 − 8.450), and "begins" is printed after that call returns, at [130.655]:
    2.9 s.
  - In vm2 the same call took 0.3 s (begins [127.857]).
- **[I, medium]** The CPU is not saturated: `[139.754123] redwood-gl: threads: 4 cores 62% idle 21% kernel` covers 129.75-139.75
  in g1, and the same 62% shows in vm2 and vm4. So the time is spent waiting:
  - on assetsd's mostly serial database creation;
  - on its first-use page-ins of PhotoLibraryServices/CoreData from the eMMC system volume;
  - in the Camera, on tccd answering while the Camera's own launch also queries it.

  None of these was measured separately.
- **[I, medium]** A second Camera launch in the same boot should be fast: assetsd stays alive (g1 `programs ... assetsd[45] 0` at
  [159.85]-[200.05]) and Photos.sqlite then exists. **No run relaunched the Camera; not measured.** The warm path is what tap 2
  shows: `THE PHOTO IS IN THE LIBRARY, 267 ms` vs 660 ms for tap 1 (g1).

### 1.3 What a real user sees

- **[V]** Of the 20 s between `notify com.redwood.camera` [111.38] and the launch [126.37], 15 s is SpringBoard. The notification
  handler runs on SpringBoard's main queue (`inject_sb.c:2263`), and SpringBoard's main thread was hung: `[103.105889]
  !SpringBoard hang: the main thread has not answered for 20 s`. It started answering again at [126.09]
  (`!icons: still-camera added`). This happens only right after boot, and a user tapping the icon goes through the same
  main thread, so they would wait for it too.
- **[I, medium-high]** From the real launch, the Camera shows its first (black) frame after about 5.0 s and a live preview soon
  after (`[136.678] 154 frames in 5019 ms ... 100% of it not black`). Taps work from about 0.2 s after the first frame. So a
  user gets about 5 s of a frozen or black screen on the first Camera launch after each boot, with no shutter to tap, and
  not 15-25 s of a dead shutter. The library open does not delay the shutter; it only makes a very early first save wait
  for assetsd (still_to_library already allows 20 s).
- Warm launches (Camera already used in this boot): not measured.

### 1.4 Proposal

- **No change to `library_early`.** It is already off the main thread, it is not what drops the tap, and the launch-to-first-frame
  time is the same whether the open starts at +0.3 s (vm2 4.96 s, vm3 4.97 s) or at +4.3 s (g1, vm1, vm4, vm5: 5.00-5.05 s).
  - Skipped: pre-creating the library at boot, and a persistent PhotoData. Add them only if T3 shows the first tap waiting for
    the library.
- **Harness:** time taps from the app's first frame, not from the notification. In practice keep the 35 s first shot (rc.jobs.mv7/mv8).
  Also wake the screen before unlocking (T1).
- **Diagnostics (T3)** for the two real unknowns: what CameraUI does with a touch before it is ready, and why the session
  restarts.
- **Optional, later (T6):** keep the sensor powered for 3 s after `stream: stop`. The restart would then cost about 0.1 s instead of
  about 1.2 s. This is kernel work in `xnu/files/bsd/dev/redwood_r1cam.c` (where `the CAM domain is off again` is printed). Do it
  only if T3 shows the restart happens on every launch.

## 2. The Camera that "died" 5 s after launch (problem 2)

### 2.1 Side by side

| | vd10 (died) | vd12 (died) | good runs (g1, vm1-vm5) |
|---|---|---|---|
| boot-args | `vdiag=1 venc=2 venc=3 pvr_stats=1 wifi=1`, no mtkmic | `vdiag=1 venc=4 mtkmic=2`, no autotest | various; vm2/vm3 = vd12's set plus autotest |
| jobs before the scenario | none (`wait 312`) | video_test inproc + encode (the scenario started at [123.84]) | none |
| `!auto: camera` → launched | [338.473] → [338.825] (0.35 s) | [214.070] → [214.412] (0.34 s) | [111.4] → [126.4] (15 s, SpringBoard hang) |
| `watched (at launch)` | +0.63 s | +0.60 s | +1.2 s |
| `early open begins` | +0.74 s | +0.71 s | +1.5 to +4.5 s |
| `device set: ClientIsActive` | **never** | **never** (after launch) | +1.1 to +3.8 s |
| `vdiag: configure:` (vdiag runs) | never | never (kept log) | vd13: right after the early open |
| assetsd spawned | **never** | **never** | +3.9 to +4.2 s |
| `camera: stream: stop` | [343.888] = device made [338.609] + 5.28 s | [219.468] = device made [214.211] + 5.26 s | after a real stream |
| later Camera lines (`HEVC switches` at +12 s, `setPreferredVideoStabilizationMode: wrapped` at +14 s, watchdog) | none | none | present |
| frames on screen across the launch | `[369.569] 3 frames in 179576 ms` (none from ~190 to 369) | `[245.130] 3 frames in 55293 ms` | `154 frames in 5019 ms` 5 s after the first frame |
| display / lock state | blank (inferred, §2.2) | **blank and locked** (verified, §2.2) | on |

**[V]** for every cell except where it says inferred.

- vd12's `ClientIsActive` [53.28] and `stream: stop` [58.35] are mediaserverd's boot warm-up still. They are present in every
  run: g1's kept log has `still: tap 0 ... the warm-up` too.
- That stop follows the last `ClientIsActive` by exactly 5.0 s, the same as the stop after Settings opens in the vm runs:
  `[172.35] ClientIsActive` → `[177.35] stream: stop`. So "stream: stop" is mediaserverd's 5 s idle teardown. It says
  nothing about when the Camera stopped.

### 2.2 The display was off and the phone locked when the Camera was launched

vd12 [V], from the syslog lines in vd12.log:
- `16:41:22.131722 ... UISensor mode changing to: <score: 4108; ... reason: SB + backlight-off + pocketTouches; proximityDetectionMode: locked; digitizerEnabled: N`
  ≈ [190.0], about 66 s after `!auto: start`, during `wait 85`. `Set Blanked: YES` follows.
- `rwtouch: notification com.redwood.unlock: posted` then `redwood: unlock: asked, answer 0` (vd12.log:664), next to
  launchd 16:41:41 ≈ [208.9]. `unlock()` (inject_sb.c:542) prints "asked" only when `isUILocked` is YES. So the UI was locked
  and `unlockUIFromSource:withOptions:` refused.
- `16:41:46.229719 ... UIKitApplication:com.apple.camera ... launching: launch job demand`, `Successfully spawned Camera[49]`,
  `16:41:46.777981 ... new scene host settings ... <sceneID:com.apple.camera-default>` ≈ [214.6]. The app's scene was made
  while the phone was locked and blanked.
- The display came back only at the scenario's `video` swipe: `16:42:16.383727 [Button] Lock page:0xC usage:0x30
  downEvent:1`, `Main will unblank`, `changeSource: LockButton` ≈ [244.24]. It showed the lock screen (`[250.190] this
  frame on average red 123 green 148 blue 141`). The Camera's preview colour in the good runs is about 117/94/96.
  - So the scenario's swipe and taps went to the lock screen, and `scan: 0 movie file(s)`.
  - **[V]** a touch on a blank screen wakes it (this swipe, and rec2 at ≈[255.96] → `16:42:28.096638 [Button] Lock`).
  - The lock screen blanks again 6.1 s after a wake (16:42:16.4 → 16:42:22.5, and 16:42:28.1 → 16:42:34.2).

vd10 [I, high]:
- vd10's syslog window starts at 16:20:07 ≈ [374], so it does not cover the launch.
- Its framebuffer lines have the vd12 signature: `[369.569277] MiniIOMobileFramebuffer: 3 frames in 179576 ms`, then a
  3%-lit frame (`red 9 green 9 blue 9; 3% of it not black`, the same frame vd12 showed at the wake), then the lock-screen
  colours at [374.62].
- The scenario idled 312 s, far beyond the 60 s auto-lock. In vd12 the display blanked about 66 s into the idle. In vm2/vm4
  it blanked about 60 s after Settings opened at [172]: syslog 17:12:13 and 17:31:00 ≈ [232].
- The 60 s auto-lock figure is **[I]** from those runs.

Why the good runs escape [V]: their Camera launch at [126.4] is the first thing SpringBoard does after its boot hang ends,
long before the 60 s idle has passed.

### 2.3 Ruled out

- **mtkmic=2:** ruled out (handoff: vm2-vm4 have it and survived).
- **The early video_test or mediaserverd's camera use:**
  - vd12's mediaserverd showed nothing unusual: the encoder lines, then the normal warm-up;
  - vm2/vm3 (same boot-args, no early jobs) survived;
  - vd10 had no early jobs and died the same way.
  - Its only effect was the 102 s delay.
- **A crash the hooks would see:** there is no `FATAL SIGNAL`, `EXCEPTION THROWN`, `UNCAUGHT EXCEPTION` or `main thread blocked` line, and
  `camera_app_guard()` is installed in the constructor (inject_media.c:1700).
- **The early open on its own:** vd13 (survived) also began its open +0.1 s after `watched` (kept log: `watched` → `asks
  statfs` → `early open begins` → `vdiag: configure:`).

### 2.4 Killed or suspended?

- **[I, medium]** Suspended, not killed.
  - Every hook line stops at once: the watchdog, the 12 s and 14 s timers, and no assetsd spawn although `sharedPhotoLibrary`
    had just been called. That fits a whole-task suspension (RunningBoard: an app with a scene that never comes to the front).
  - launchd's log in vd12 has the Camera's spawn at Notice level, and **no exit line** for Camera[49] through 16:42:42
    ≈ [259]. launchd normally logs exits at Notice level.
  - A kill by a TCC `abort_with_payload` or a SIGKILL would also bypass our signal handler. So the hook lines alone cannot
    tell the two apart.
- **[G]** The absence of `Camera[49]` in `redwood-gl: programs` proves nothing: that list holds the top 5 and ties at 0.

### 2.5 The one cheap run that separates them (T2)

A late start with the Camera launched while the phone is locked, then a wake and unlock without relaunching it. `rc.jobs.lk`
(non-BT build, `tools/r1-run.sh` directly, 260 s, BASE + `venc=4 mtkmic=2`):

```
/redwood/bin/rwtouch say start wait 150 say camera notify com.redwood.camera wait 20 say wake tap 0.5 0.06 wait 1 say unlock notify com.redwood.unlock wait 15 say shot1 tap 0.5 0.887 wait 10 say end
```

Read it as follows:
- **Suspended:**
  - before `!auto: wake` there are no Camera lines after `early open begins`;
  - after `!auto: unlock` the **same pid** goes on: `device set: ClientIsActive`, `map_with_linking_np: [N(assetsd)]`,
    `the photo library opened early in` with a large number, `4 HEVC switches` printed late;
  - shot1 gives `tap 1 received`.
- **Killed:**
  - a launchd `exited` line for the Camera's pid before `!auto: wake`;
  - after the unlock, no Camera lines, or a new `map_with_linking_np: [M(Camera)]` with a new pid.
- **Control:** the display was blank at the launch: a `backlight-off` syslog line before `!auto: camera`, and
  `redwood: unlock: ...` once T1's log line is in.

## 3. "Camera frames painted twice after a home swipe" and Open Mapple item 2.6 (problem 3)

### 3.1 What the symptom is

- **[V]** The measurements behind the STATUS item (HANDOFF-2026-11-16.md):
  - q0: "steady camera = pairs of frames ... A then B 23-28 ms later into the OTHER swap surface";
  - w0 autotest (with home swipes): `2.01 fr/pic`, swaps 57/s;
  - n0 (the same without the home swipes): `1.0 fr/pic`, 39 swaps/5 s;
  - Update 5: one home swipe before the Camera leaves the render server swapping at 59 Hz;
  - HANDOFF-2026-11-17-night line 193 (sz2/sz3): `fr/pic 3.02`.
- **[I, medium]** So "painted twice" means backboardd renders each camera picture 2-3 times, which costs CPU and GPU. It is
  not a visible doubling: screenshots are normal (q0, r1b). The STATUS line does not say which of the two meanings it has,
  and this is the only measurement of it.

### 3.2 Would 2.6 fix it? Not plausibly

Item 2.6 is "frame-end flip, vsync locked to the panel, present only after the GPU is done".

- **Present only after the GPU is done:** already ours [V].
  - `IOMobileFramebuffer::show()` waits `MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS)` before copying
    (MiniIOMobileFramebuffer.cpp:1415-1420).
  - The next swap is let through only after the copy (1547-1558).
  - `glwait=3` also makes the surface lock wait for the pvr fence.
- **Present paced on the panel:** tried [V]. `fb_rdma=1` waits for DISP_RDMA0's frame-end bit before showing (1440-1451).
  HANDOFF-2026-11-16: "r0 (off): ... 2.01 fr/pic ... r1b (on): ... 2.01". No change.
- **Vsync locked to the panel:** our vsync callback is a free-running timer (`armVSync`, 1802-1813). By the driver's own
  note (MiniIOMobileFramebuffer.cpp:426), "CoreAnimation does not go by the refresh notifications of this driver -- it never asks for
  them". **[I, medium]** CA's pacing therefore cannot change when that timer is phase-locked.
- **[I, medium]** The extra frames are CA's own decision, made upstream of the framebuffer. HANDOFF-2026-11-16 Update 3 found no
  window committing at that rate and no SpringBoard animation to blame. CA's frame-rate arbitration state after a swipe
  remains the open lead (the Update 5 plan: `caenv=CA_PRINT_FRAME_RATE_ARBITRATION=1 caenv=CA_PRINT_FRAME_INFO=1`).
- One related difference **[V, read in Open Mapple]**: `MappleFramebuffer.cpp`'s header says CA (`IOMFBDisplay::can_update_`)
  renders into a page again only once `SwapWait(swap, 0x80000000)` returns 0. Theirs answers retired/displayed/busy; ours
  answers success at once (1923-1928). **[I, low]** That gates **when** CA may draw, not **whether** it draws again. It
  could only matter if CA's second render is a retry of a frame it thinks was not displayed. T5 tests that cheaply, before any
  port.

### 3.3 What 2.6 would change in our driver (for tearing), size and risk

`xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`, all behind a new boot-arg, with the old path kept as the default:
1. **Flip at the frame end.** Today `fb_flip=1` writes `OVL0 L0_ADDR` (0x1400b000 + 0xf40) right after the copy, at any point in the
   scan (1541-1545). `fb_flip` is not in BASE args, so the default path copies into the live framebuffer and can tear in
   bands (the comment at 564-570).
   - Change: copy into the back picture first, then wait for the RDMA0 frame-end bit (as `fb_rdma` already does at
     1440-1451), then write L0_ADDR with interrupts off.
   - Prerequisite: read whether MT6765 OVL0 latches L0_ADDR through shadow registers at frame start. The register and
     bit are not known here **[G]**; open-mapple-gpu.md §6 item 4. If it does latch, the write is already tear-free and
     only the timing of the copy matters.
2. **Vsync phase-locked to the panel.** `armVSync()` aligns `fVSyncTime` to the last measured RDMA frame end plus k periods,
   instead of `+= period`. About 30 lines; low risk; no expected effect on CA (§3.2).
3. **Real SwapWait.** Keep a ring of swap ids with their state (queued / shown); selector 6 answers busy (`kIOReturnBusy`)
   when bit 31 is set and the swap is not shown yet, otherwise blocks up to `in[2]` ms.
   - About 80-120 lines. **Medium risk**: a wrong answer stalls backboardd's render thread (the screen freezes), so it needs
     a timeout and a counter line.

Size: 150-250 lines, one file, 2-3 device runs. Risk: medium (black or frozen panel on a mistake; reverted by the boot-arg).
Gain: possibly tear-free frames. For the double paint, none expected. **Recommendation:** do not start 2.6 for this
symptom. Run T5 first, and treat 2.6 as a tearing item only after the shadow-register read.

## 4. Implementation tasks (prioritized, Sonnet-sized)

Rules as in HANDOFF: non-BT builds per run via `ubuild.sh` with `RC_JOBS`, `tools/r1-run.sh` directly, no Wi-Fi, no
`pvr_stats`, no `vdiag` unless stated. BASE = `mtkaudio=2 r1cam=4 pvr=6 pvr_gl=1 fbaspect=1 glwait=3 glthreads=1 cam_fps=10
fbscale=1 r1_clk=1 r1_cpufreq=2200 r1_gpu=680 glyuv=1 autotest=1`.

### T1 (P1, harness, ~10 lines): wake before unlock; record the lock state at launch

- **`userland/src/inject_sb.c`**, the `com.redwood.camera` handler (line 2263):
  - before launching, read `-[SBLockScreenManager isUILocked]` the way `unlock()` does (inject_sb.c:542-560);
  - change the line to `keep_line("redwood: camera launched by bundle id: %d (UI locked %d)", ok, locked)`.
  - No other behaviour change: do not refuse the launch.
- **Every `userland/rc.jobs.*` with `notify com.redwood.unlock`** (pa, mv2, mv5, mv6, mv7, mv8, and any other found by grep):
  insert `tap 0.5 0.06 wait 1` before `notify com.redwood.unlock`.
  - A touch wakes a blank screen (§2.2), and at 0.06 it lands on the status bar or the lock screen's top.
  - The unlock must follow within 6 s of the wake (the lock screen blanks again after 6.1 s).
- Pass, in the kept log of a late run (rc.jobs.mv2's `wait 312`):
  - `redwood: camera launched by bundle id: 1 (UI locked 0)`;
  - `!redwood: camera: device set: ClientIsActive` within 3 s of it;
  - `camera app: the photo library opened early in`;
  - no `redwood: unlock: asked, answer 0`.

### T2 (P1, one diagnostic run, no code): killed or suspended

- Add `userland/rc.jobs.lk` exactly as in §2.5. It deliberately keeps the old order: no wake before the launch.
- Build it with T1's log line, so that `(UI locked 1)` confirms the setup.
- Pass: one of the two outcomes in §2.5 is unambiguous in the log.
- Then update docs/STATUS.md:
  - drop "Camera closed about 5 s after launch ... cause unknown", or reword it as "harness: launched while locked";
  - reword the lost-tap line per §1.3.

### T3 (P2, ~60 lines in `userland/src/inject_media.c`, Camera only): why a tap is lost, and why the session restarts

- `library_early()`: print two more lines with ms since `began`:
  - after `authorizationStatusForAccessLevel:`: `camera app: early open: authorisation %ld after %llu ms`;
  - after `sharedPhotoLibrary`: `camera app: early open: shared library after %llu ms`.
- `camera_app_probe()`: wrap `-[AVCaptureSession startRunning]` and `-[AVCaptureSession stopRunning]` with
  `method_setImplementation`, the same way as `capture_photo`. Each call prints
  `camera app: session startRunning|stopRunning (main %d)` and then `camera_frames("session start"|"session stop")`.
  Cap it at 6 calls.
- Touches: wrap `-[UIApplication sendEvent:]`. For the first 40 events of type touches (`[event type] == 0`), print
  `camera app: touch phase %ld at %.3f s after launch`. Use `allTouches anyObject` `phase`, and take the launch time in the
  constructor.
  - Keep it to the first 40 so the kept log stays small.
  - Remove it after the run, or gate it behind the existing `vdiag=1` check (`vdiag_on`) if it is to stay.
- Run: rc.jobs.pa (shot1 at notify+20 s), BASE + `venc=4 mtkmic=2`, no vdiag (or vdiag=1 if the touch hook is gated on it).
- Pass (all in the kept log):
  - two `session startRunning` lines and one `stopRunning` line, with frames (decode with `tools/cache-name.py`), or a single
    start;
  - for shot1, either `touch phase 0` / `touch phase 3` near the first `swap #2` and no `tap 1 received` (the touch reached
    the app and was ignored), or no touch line at all (not delivered);
  - the two new early-open lines, which split the 7-8 s.

### T4 (P3, skipped by default): library pre-warm

Do it only if T3 shows `authorisation` or `shared library` delaying the **first tap's save** past about 2 s in a realistic
first launch. Otherwise nothing: the shutter does not wait for the library (§1.1, vm2).

### T5 (P3, 1-2 runs, boot-args only): does the double paint depend on present timing at all?

- Two runs of the existing swipes-then-Camera autotest (the one that gave `2.01 fr/pic`):
  - (a) BASE plus `caenv=CA_PRINT_FRAME_RATE_ARBITRATION=1 caenv=CA_PRINT_FRAME_INFO=1` (HANDOFF-2026-11-16 Update 5);
  - (b) BASE plus `fb_beat=0 fb_queue=1`.
- Pass: the `fr/pic` lines.
  - If (b) still reads about 2.0, present timing is not the cause (expected), and 2.6 is closed for this symptom.
  - (a) names CA's preferred rate or frame reasons in the log; if they do not reach the log, say so in one line.

### T6 (P4, kernel, only for tearing or launch latency, not for problems 1-3 as such)

- (a) 2.6 part 1 per §3.3 (flip at frame end), after reading the MT6765 OVL0 shadow register.
- (b) r1cam linger: keep the sensor powered for 3 s after `stream: stop` (`redwood_r1cam.c`) if T3 shows the session
  restart on every launch.
  - Pass for (b): the second `START` → `frames of 1632x1224 from the kernel` drops from about 1.2 s to under 0.3 s.
  - `swap #2` minus `camera launched by bundle id` drops below 4 s.
