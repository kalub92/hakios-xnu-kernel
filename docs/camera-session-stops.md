# Camera launch: who stops the capture session, and where the first touch goes

Status: analysis only. Nothing was built or run. Written 2026-11-18. Follows `docs/camera-launch-spec.md` (T3 results in
HANDOFF-2026-11-17-night.md, "Camera launch").

Labels: **[V]** verified (log line with time, or file:line); **[I]** inference, with its basis; **[G]** guess.
Times in brackets are kernel seconds; "+s" is seconds after launch as the T3 hooks print it (launch = [126.853] in t3b,
from `startRunning 0.660 s` at [127.513936]).

Sources: `logs/t3a.log`, `logs/t3b.log`, `logs/t3reg.log` (console part and the kept part after "kept for the host"),
`logs/vm1.log`, `logs/vm3.log`, `logs/vm4.log` (same launch, with `vdiag=1` in vm1/vm3), `userland/src/{camera_device.c,
inject_media.c,inject.c,inject_sb.c,rwtouch.c}`, `xnu/files/bsd/dev/{redwood_r1cam.c,redwood_input.c,redwood_hyn.c}`,
`ios165/rootfs/usr/libexec/backboardd` (strings only), the dyld cache via `tools/cache-name.py` and `tools/dsc/objcimp.py`.

## 0. Answer in short

- **Only one of the three stops is a real stop of the stream.** The stop at +3.530 is the late notice of a graph
  teardown that mediaserverd's capture graph did at [129.709729], 1.81 s after its START and 0.38 s after the first
  preview picture. The stops at +0.895 and +3.581 have no teardown behind them at all.
- **Who issues the real one: mediaserverd's capture graph (CMCapture), not our plug-in and not the kernel.**
  - Our plug-in only stops when the graph calls `stream_stop` and never posts a notice or an error of its own.
  - The kernel driver stops 35 ms after the plug-in has closed the device ("nobody reads").
  - The app never calls `stopRunning`.
  - No format change takes place: `format 0` both times.
  - In vm1 and vm3, the graph's error logger was hooked and logged nothing before that stop.
- **Why the graph stops is still open.** The two candidates left:
  - (a) a configuration commit by CameraUI on its session queue, which the T3 hooks do not see;
  - (b) a server-side interruption: the app-state check during launch, or system pressure.

  A runtime error is unlikely but not excluded. **One diagnostic run (D1, §3) settles it**: log the didStop payloads,
  the session's begin/commitConfiguration, AVFoundation's error/interruption notices, and mediaserverd's backtrace at
  `stream_stop`.
- The **stabilization hook** (`vfix`) is installed at [141.05] = +14.2 s (t3b). It is too late to cause any of these
  stops **[V]**.
- **Fix for the cost, whatever the cause:** keep the sensor powered for 3 s after the last close (r1cam "linger",
  camera-launch-spec T6b). A restart then costs ~0.1 s instead of ~1.2 s:
  - 0.95 s of each power-up is the bit-banged sensor table, [130.59] → [131.545];
  - the first live frame would come about 1 s sooner.
- **Lost first touch:**
  - It reached neither the app (no `touch phase` line until +13.008) nor, as far as can be told, our own swallow paths.
  - The likely drop point is inside backboardd's touch routing / SpringBoard's launch transition:
    - it is lost when the tap comes ≤ 0.18 s after the app's first frame (`swap #2`);
    - it is delivered at ≥ 0.24 s.
  - The same run D1 tells which, with `touch_lat=1` plus two small hooks (§5).

## 1. The t3b timeline (one session object 0x280cb4110)

| Kernel s | +s | Who | Line | Label |
|---|---|---|---|---|
| 127.513936 | 0.660 | app (session queue, `main 0`) | `startRunning`, frames `CameraUI+0x15b108 ← +0x15cfec ← +0xe2e4` (path A) | V |
| 127.699038 | | plug-in in mediaserverd | `device set: ClientIsActive`, `device_request_streams (1)`, `stream: start (format 0)` (worker started) | V |
| 127.752238 | | plug-in | `stream: format 0`, then gets of VideoOutputConfigurations / SupportedColorSpaces | V |
| 127.755598 | 0.895 | app (main) | `_handleDidStopRunningNotificationWithPayload:` **(stop #1)** | V |
| 127.781236 | 0.928 | app (session queue) | `startRunning`, frames `CameraUI+0x15b108 ← dispatch block` (path B) | V |
| 127.794-127.858 | | plug-in | `processing_session_create`, rate/output configs, `session: invalidate/finalize` | V |
| 127.902959 | | graph | `stream: START from the capture graph` | V |
| 127.940-129.04 | | kernel r1cam | CAM domain on, sensor Hi846 tables, `serving: the stream is on` | V |
| 129.328648 | | plug-in | `PrimaryScaler: the first picture given (1504x1128)` | V |
| **129.709729** | | graph → plug-in | **`stream: stop`** | V |
| 129.741099 | | plug-in worker | `the frames stopped (7)` (device closed) | V |
| 129.744337 | | kernel | `r1cam: serving: nobody reads: the stream stops after 20 frames` | V |
| 129.752060 / .756894 | | graph | `device: streams given back (1)`, `device set: ClientIsActive` | V |
| 129.80 | | kernel | `r1cam: power: off` … `the CAM domain is off again` | V |
| 130.238576 | 3.489 | app (main) | `_handleDidStartRunningNotificationWithPayload:` (late: the stream already stopped 0.53 s ago) | V |
| 130.383229 | 3.530 | app (main) | `_handleDidStop...` **(stop #2 = the 129.71 teardown, 0.67 s late)** | V (time), I (pairing) |
| 130.426413 | 3.572 | app (session queue) | `startRunning`, path A (`_commitSessionConfigurationIfNecessaryWithLogReason:`) | V |
| 130.433800 | 3.581 | app (main) | `_handleDidStop...` **(stop #3)**; the server has not begun anything since 129.757 | V |
| 130.440455 | | graph | `device set: ClientIsActive`, `device_request_streams (2)`, the whole configuration again | V |
| 130.445348 | 3.592 | app (session queue) | `startRunning`, path B | V |
| 130.511487 | | graph | `stream: START from the capture graph` | V |
| 130.535-131.661 | | kernel | power-up again; 0.95 s of sensor tables; `serving: the stream is on` [131.662] | V |
| 131.544277 | | backboardd | `swap #2: surfaces in layers 0x1 now` (first frame after 16.8 s, black) | V |
| 131.564051 | | harness | `!auto: shot1` (tap 0.5 0.887, 60 ms) | V |
| 131.778456 | 4.920 | app (main) | `_handleDidStart...` | V |
| 139.861128 | 13.008 | app | first `touch phase 0` (= shot2) | V |
| 141.050856 | 14.2 | app | `vdiag: setPreferredVideoStabilizationMode: wrapped, forcing Off` | V |

The callers were named with the 16.5 cache **[V]**:
- `0x1cdd98108` is `-[CAMCaptureEngine _sessionQueue_startWithRetryCount:retryInterval:logReason:completion:]+0x26c`.
- `0x1cdd99fec` is `-[CAMCaptureEngine _commitSessionConfigurationIfNecessaryWithLogReason:]+0x1e8`.
- `0x1cdc4b2e4` is CameraUI+0xe2e4. `objcimp.py` names `-[CAMShutterIndicatorView _commonCAMShutterIndicatorViewInitialization]+0x22c`
  as the nearest method before it, which is probably a block or C function after it **[I]**.
- `0x19b16cc3c` is PhotoLibraryServicesCore+0xcc3c, a dispatch trampoline.

So the app starts the session in two ways:
- **A**: commit the configuration, then start.
- **B**: a dispatched start-with-retry.

Each didStop is answered by one of them within 11-42 ms. CameraUI restarts on its own, by design. Nothing in our hooks
has to do with that.

Same pattern in the other runs:
- **t3reg** (kept part):
  - start +0.617 (A), didStop +0.876, start +0.908 (B);
  - didStart +3.493, didStop +3.534, start +3.619;
  - didStart +4.889.
  - That is **2** didStops: no third one **[V]**.
- **vm1, vm3, vm4, g1, vm5**: graph START → `stream: stop` 1.81-1.89 s later, every time at [129.51-129.70]. The restart
  `ClientIsActive` follows at [130.19-130.49] (camera-launch-spec §1.1, and vm1 [129.509781] / [130.193601], vm3 [129.697175] / [130.438884],
  vm4 [129.673441] / [130.488084]) **[V]**.
- **vm2** had **no** first session: one start at [130.17], never stopped. Its Photos authorisation answered in 0.3 s
  instead of 2.8-3.1 s (camera-launch-spec §1.2) **[V]**.

## 2. Each stop: who issued it

### Stop #2 (+3.530, the real one: graph teardown at [129.709729])

Excluded, with evidence:
- **The app**: the T3 wrapper on `-[AVCaptureSession stopRunning]` (`inject_media.c:1188-1196`) logs the first 6
  start/stop calls, and t3b has 4 starts and 0 stops **[V]**.
- **Our plug-in as issuer**: `stream_stop()` (`camera_device.c:1918-1931`) runs only when the graph calls it, or from
  `stream_invalidate()` (`:1987-1990`). The worker's own end (`:1867-1875`) only prints `the frames stopped`, which came
  31 ms *after* `stream: stop`. The plug-in posts no CoreMedia notification at all: its only `notify_post` is the still
  file's (`:1428`) **[V]**. It cannot tell the graph "stopped" or "error".
- **A format change**: `stream set: FormatIndex` / `stream: format 0` in both builds ([127.752], [130.458]). The same
  property gets and sets in the same order **[V]**.
- **The kernel**: `r1cam: serving: nobody reads` [129.744337] follows the device close. It is the
  `cam_dev_opens == 0` arm of the serving loop (`redwood_r1cam.c:1576-1577, 1584`). No `NO FRAME FOR 3 s` line **[V]**.
- **The stabilization hook**: installed at +14.2 s **[V]**. The `-12786 Setup VIS Processor` node error belongs to the
  *second* session, at [132.03] in vm1, [132.33] in vm3 **[V]**.
- **A graph node error before the stop**: vm1 and vm3 ran with `vdiag=1`. `-[BWLimitedGMErrorLogger logErrorNumber:errorString:]`
  and `+[NSException exceptionWithName:reason:userInfo:]` were wrapped before the first graph configured: the kept order
  puts them before `BWMultiStreamCameraSourceNode configure:` #1 at [127.545] in vm1. Neither logged anything until
  [132.03] **[V]**. Errors that do not go through that logger are not excluded.

What is left, ranked:
1. **[I, medium] CameraUI commits a new session configuration on its session queue**, and the server tears the
   running graph down for it.
   - Basis: the restart at +3.572 is path A, `_commitSessionConfigurationIfNecessary...`. In vm2, where the
     first start never happened, the only start came at the same ~3.5 s.
   - Against: the commit we can see (+3.572) comes 0.7 s *after* the teardown, and the server rebuilds only after the
     app's next `startRunning`.
   - The hooks wrap neither `beginConfiguration` nor `commitConfiguration`, so an earlier commit at ~[129.7] would be
     invisible.
   - What could trigger it: the photo-library authorisation answering, with t3b `authorisation 3 after 2765 ms` at [130.726]
     and its request at ~[127.96]; or the preview/capture-mode setup.
2. **[I, medium-low] Server-side interruption.** FigCaptureSession stops a session whose client is not foreground-active
   (`VideoDeviceNotAvailableInBackground`), or under system pressure.
   - Basis: SpringBoard was hung until [126.09] and launched the app at [126.37], and the app's scene was still
     coming up: its first frame came at [131.54].
   - Against: an interruption normally ends with a server-side resume, not an app restart.
   - Its notices (`AVCaptureSessionWasInterruptedNotification`) are not hooked.
3. **[I, low] Graph runtime error not logged by `BWLimitedGMErrorLogger`.** It would show as
   `AVCaptureSessionRuntimeErrorNotification` and an error in the didStop payload.

### Stop #1 (+0.895) and stop #3 (+3.581)

- **[V]** No graph teardown accompanies either one:
  - Stop #1 falls inside the first graph build: no `stream: stop`, no `streams given back`. The build simply
    continues ([127.794] `processing_session_create`) and starts the stream at [127.903].
  - Stop #3 comes 9 ms after the app's start at +3.572, before the server's next `ClientIsActive` [130.4405].
- **[I]** They are AVCaptureSession-level notices for a start that was superseded: stale or duplicate notices, not
  stream stops. They are harmless apart from making CameraUI call `startRunning` once more (path B). t3reg has no stop
  #3, so they depend on timing.
- The payload logging in D1 confirms or refutes this. A superseded start carries no error; a real stop carries an
  error or a reason.

## 3. The one diagnostic run that settles it (D1)

Build: the normal non-BT build, with the following changes. All are logs only and call the originals.

1. `inject_media.c`, in `t3_hooks()` (`:1215-1270`):
   - In the block wrapper (`:1231-1249`), add `%.300s` of `[payload description]` to the line.
   - Wrap `-[AVCaptureSession beginConfiguration]` and `-[AVCaptureSession commitConfiguration]` like `session_start`.
     Log the first 10 calls with time, `main %d` and `camera_frames(...)`.
   - In `camera_app_probe()` (`:1272`), register `NSNotificationCenter` observers (object nil, queue nil, so they run on
     the posting thread) for `AVCaptureSessionRuntimeErrorNotification`, `AVCaptureSessionWasInterruptedNotification`,
     `AVCaptureSessionInterruptionEndedNotification`, `AVCaptureSessionDidStopRunningNotification`. Each prints
     `camera app: notice NAME at +S: userInfo %.300s`.
2. `camera_device.c` (runs in mediaserverd):
   - Add boot-arg `cam_bt=1` (with `bootarg_number`). It makes `say_bt()` (`:125-148`) print for `stream_stop`,
     `device_relinquish_streams` and `stream_start` (first 3 each) without needing `CAM_TRACE`.
     `backtrace()` needs no `CAM_TRACE_STUB`.
   - In `device_set_property()` (`:2108-2114`), print the `ClientIsActive` value each time:
     `device set: ClientIsActive = true|false`.
3. Run: `tools/r1-auto.sh t4 "<BASE> venc=4 mtkmic=2 vdiag=1 cam_bt=1 touch_lat=1" 200` with rc.jobs.pa (shot1 at notify+20 s, as t3b).
   Add the touch hooks of §5 in the same build, so that one run answers both questions.

Read-out:

| Seen in D1 | Means | Fix |
|---|---|---|
| A `commitConfiguration` on the session queue just before the `stream: stop`, and `BT stream_stop` frames through a CMCapture "configuration"/"commit" path | cause 1 | Look at what the commit changes (its caller frames): if it is the library authorisation, answer it before launch (pre-warm, camera-launch-spec T4); if it is CameraUI's mode/preview setup, accept it and rely on the linger |
| `notice AVCaptureSessionWasInterruptedNotification ... reason N` | cause 2 | N=1 (background): the app is not foreground-active for mediaserverd in its first ~3 s; look at the app-state SpringBoard publishes at launch (inject_sb launch path). N=5 (system pressure): our thermal/pressure reporting |
| `notice AVCaptureSessionRuntimeErrorNotification ... AVErrorKey ... -118xx/-127xx` or a didStop payload with a non-zero error | cause 3 | the error code and the `BT stream_stop` frames name the node; fix the plug-in property/output it complains about (`camera_device.c`) |
| Stops #1/#3 payloads carry no error and no reason | stale notices, as §2 says | none needed |

## 4. Fix independent of the cause: r1cam linger (T6b)

- Where: the close path of `redwood_r1cam.c`, which ends the serving loop on `cam_dev_opens == 0`
  (`:1576-1577`, then `say("power: off")` at `:696`).
- What: replace the immediate sensor/CAM-domain power-off with a `thread_call` 3 s later, cancelled by the next open.
  On an open within the 3 s, skip `cam_sensor` init (tables) and go straight to SENINF/CAMSV programming and
  `stream on`.
- Expected: t3b's restart [130.511] START → [131.703] first kernel frame (1.19 s) becomes ~0.15 s. The first live
  preview then comes ~1 s earlier on every launch that restarts, which is 5 of 6 runs plus t3a/t3b/t3reg.
- Risk: low. The sensor stays powered 3 s more after the Camera closes, and the thermal guard is unaffected.

## 5. The lost first touch

### 5.1 What is known

- **[V]** t3b: `!auto: shot1` [131.564051], 20 ms after `swap #2` [131.544277]. The app's `sendEvent:` hook was installed
  in the constructor (`inject_media.c:1272-1276`, line `t3 hooks: session 1, sendEvent 1`). The first `touch phase`
  line is +13.008 (shot2). So the shot1 touch never reached `-[UIApplication sendEvent:]` in the Camera.
- **[V]** t3a: shot1 1.25 s after `swap #2` → `touch phase 0 at 6.045 s`, `tap 1 received`.
- **[V]** camera-launch-spec §1.1: lost at +0.18/+0.12/+0.03/−0.03/+0.02 s from `swap #2`, received at +0.24 s.
- **[V]** The tap does not come from the touch panel: `rwtouch` writes DOWN, 60 ms, UP into `/dev/redwood-touch`
  (`rwtouch.c:172-179`). The panel driver had 0 reports (`hyn: so far 0 reports` [134.049220]).

### 5.2 Where a touch can be dropped on its way, with what would show it

| # | Place | Code | Can it drop this tap? |
|---|---|---|---|
| K1 | panel driver → queue (real finger only) | `redwood_hyn.c:232,241` → `redwood_touch_post` `redwood_input.c:141-145` | no: not in the rwtouch path **[V]** |
| K2 | queue full: the oldest sample is dropped | `redwood_input.c:118-120` (512 records) | no **[I]**: a reader (backboardd's input thread) is blocked in `touch_read` (`:165-170`); two samples cannot fill 512 |
| K3 | `write` with a bad type → EINVAL | `redwood_input.c:197-199` | no: DOWN/UP are valid |
| B1 | backboardd input thread reads the sample | `inject.c:1879-1884` (`count < 4` logs only the first four of the boot) | — |
| B2 | **display-wake swallow**: when the event server last set `TouchDetectionMode` low byte 0xfe, a DOWN becomes a lock-button press and the rest of that finger is dropped | `inject.c:1890-1905` (mode from `service_set_property` `:1637-1647`) | **[I] no**: it would have pressed the lock button with the display on, which sleeps the display, and shot2 13 s later reached the app as a normal touch. Not excluded by a log line: the mode lines (`redwood: input: touch detection mode`) carry no timestamp |
| B3 | a MOVE/UP for a finger not down is dropped | `inject.c:1748-1749` | only if the DOWN was lost first |
| B4 | dispatch into the event system, result unchecked | `inject.c:1777` `IOHIDVirtualServiceClientDispatchEvent` | unknown: nothing logs whether it was accepted |
| B5 | **backboardd's touch routing** (Apple code in `/usr/libexec/backboardd`): hit-test at the touch point to a CA context and its process, deferral rules, and touch cancellation on request. Selectors present (strings): `destinationsForEvent:sender:`, `_determineContextsAtHitTestPoint:display:processingInfo:entered:within:exited:`, `_postEvent:sender:dispatcher:destination:cancelled:`, `cancelAndSuppressTouchesOnDisplay:reason:`, `cancelTouchesOnDisplay:`; classes `BKHIDDirectTouchEventProcessor`, `BKTouchDestination`, `BKTouchDeliveryPolicyServer` | not ours | **[I, most likely]** while the app's scene is coming up, the point hit-tests to SpringBoard's transition window or the app's context is not yet touch-routable, or SpringBoard has asked backboardd to cancel/suppress touches for the launch transition |
| S1 | SpringBoard gets the touch and drops it (interaction ignored during the app-launch transaction) | not ours; `inject_sb.c` has no `sendEvent:` hook **[V]** (grep) | **[I]** possible: same window as B5 |
| A1 | Camera's `sendEvent:` | hooked, `inject_media.c:1198-1212` | **[V]** never called for shot1 |

The 0.2 s window after the first frame matches a launch transition (S1/B5) better than any of our own code. B2 is
the only one of ours that drops a DOWN on purpose, and it would have locked the display.

### 5.3 What to log (in D1)

- `touch_lat=1` (existing):
  - kernel `tl: p TYPE FINGER X Y at US` per sample (`redwood_input.c:122-127`);
  - backboardd `tl: i TYPE posted US read US sent US` (`inject.c:1906-1909`).
  - Proves K1-K3 and B1/B3/B4's entry.
- `inject.c`, at each DOWN, for the first 30 DOWNs after boot+100 s: `input: DOWN at US, mode 0x%x, sent|wake`.
  This settles B2 with a timestamp.
- `inject.c` (backboardd), once the event server runs:
  - for every class from `objc_copyClassList` with the prefix `BK` that implements `cancelAndSuppressTouchesOnDisplay:reason:`,
    `cancelTouchesOnDisplay:` or `cancelTouchesWithIdentifiers:`, wrap the method and print
    `backboardd: SEL from pid? reason %.120s at US`;
  - also print one line listing the classes found, for later hooks of `destinationsForEvent:sender:`.
- `inject_sb.c`: copy `send_event` from `inject_media.c:1198-1212` into SpringBoard. Log touch events for 40 events
  after `camera launched by bundle id`, as `SpringBoard: touch phase N at US (ignoring %d, key window %s)`, with
  `-[UIApplication isIgnoringInteractionEvents]` and the key window's class.

Read-out:
- `tl: i ... sent` at ~[131.58] and nothing in SpringBoard or the app → B5 (backboardd routed it nowhere, or cancelled it).
  A `cancelAndSuppressTouches... reason` line in the window names the requester.
- A `SpringBoard: touch phase 0` at ~[131.58] → S1: SpringBoard owned the point during the transition.
- `input: DOWN ... wake` → B2: our bug. Only treat 0xfe as wake while the display is off.
- No `tl: i` at all → K/B1 (unexpected).

Fix: for S1/B5 this is iOS's own launch-transition behaviour, stretched by our slow first frame. No code fix is
proposed; the harness already times taps from the first frame (camera-launch-spec §1.4). The r1cam linger and a faster
launch shorten the window a user can hit.

## 6. Implementation tasks (Sonnet-sized)

**C1 — session notices (userland/src/inject_media.c, `t3_hooks`, `camera_app_probe`).**
Payload text in the didStart/didStop wrapper; begin/commitConfiguration wrappers (first 10); four NSNotificationCenter
observers.
Pass (kept): `camera app: session 0x... _handleDidStopRunningNotificationWithPayload: 0.8xx s after launch (main 1) payload {...}`
and at least one `camera app: commitConfiguration (main 0) N.NNN s after launch` line. Any interruption or runtime error
shows as `camera app: notice AVCaptureSession... at +S: userInfo {...}`.

**C2 — mediaserverd backtraces and ClientIsActive (userland/src/camera_device.c).**
`cam_bt=1` enables `say_bt` for `stream_stop`, `device_relinquish_streams` and `stream_start` (first 3 each), independent
of `CAM_TRACE`. `device_set_property` prints the ClientIsActive value.
Pass (console/kept): `BT stream_stop: 1a1... 1a1...` before `stream: stop` at ~[129.7]; decode it with
`python3 tools/cache-name.py` and `tools/dsc/objcimp.py` into a named CMCapture method in the report. Also expect
`device set: ClientIsActive = true` at ~[127.7] and `= false` at ~[129.76].

**C3 — touch path logs (userland/src/inject.c, userland/src/inject_sb.c).**
- DOWN line with mode (first 30 after +100 s).
- backboardd cancel/suppress wrappers found by a class scan, plus one class-list line.
- SpringBoard `sendEvent:` wrapper (40 touch events after the camera launch).

Pass (kept): `input: DOWN at ... mode 0x1, sent` for shot1 at ~[131.58]; the class-scan line; and for shot1 either a
`SpringBoard: touch phase 0` line or a `backboardd: cancel...` line or neither (B5). Each tells the D1 read-out (§5.3)
which case it is.

**C4 — run D1 (device, tell the user first).** As §3.3; rc.jobs.pa; one run.
Pass:
- the run returns with reason 0;
- the kept log has the C1/C2/C3 lines around [127.5-131.8];
- the report fills in §3's and §5.3's read-out tables with the line that decided each.

**C5 — r1cam linger (xnu/files/bsd/dev/redwood_r1cam.c), after D1, regardless of its outcome.**
Delayed power-off (`r1cam_linger_ms=3000` boot-arg, 0 = today), and a fast re-open that skips the sensor tables while
powered.
Pass (kept):
- `r1cam: linger: reopened after N ms, sensor still powered` at the restart;
- no second `r1cam: sensor: the tables (init 381 ...)` line within 3 s of `serving: nobody reads`;
- `r1cam: serving: the stream is on` ≤ 0.3 s after the second `stream: START from the capture graph`;
- `tap 1: THE PHOTO IS IN THE LIBRARY` in an rc.jobs.mv7 regression run.

**C6 — fix per D1's read-out** (one of: library/authorisation pre-warm, app-state at launch, plug-in property). Sized
once D1 has named the cause.
Pass: in an rc.jobs.pa run, exactly one `stream: START from the capture graph` before the first `swap #2`, and no
`stream: stop` before shot2.
