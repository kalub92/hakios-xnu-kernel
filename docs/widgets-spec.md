# Home-screen widgets: why Weather and Calendar are white tiles, and the fix

Analysis and specification only (2026-11-19). Nothing in this document has been built or run. Tags: **[V]** checked
in a file, log or picture (with the place); **[I]** inferred from verified facts; **[G]** guessed or general iOS
knowledge that this tree cannot confirm.

## 0. Short answer

- **Top cause [V]: chronod is not a launchd job in our images.** `userland/build.sh:118-121` adds `com.apple.chronod`
  only when `WIDGETS=1`, and no current build sets it (the NOTE command, `ubuild-d.sh`, `btbuild3.sh`). SpringBoard asks
  for chronod's service and gets "No such process" (§2.2). With no chronod, no widget extension is ever started, and
  SpringBoard draws its empty placeholder: the white rounded squares.
- **The same port drew both widgets on 14 November [V]** when chronod was still in the job list:
  `logs/grid3.png` (the Weather tile says "No weather data", the Calendar tile shows "14" and "No events today"),
  `logs/grid3.log` lists `/redwood/LaunchDaemons/com.apple.chronod.plist`. The white tiles start with the runs
  built without it: `cs1` (18 Nov), `hd1` and `hc1` (19 Nov).
- **AMFI and the Sandbox are not the cause [V].** Widget extensions were started, checked in and drew on this kernel.
  Our stand-in already does what Open Mapple's AMFI IOService does, in a different way (§3).
- **Why chronod was dropped [V]:** CPU. With chronod, about 26 widget extensions start together and take one to two
  cores for minutes: `logs/G.log:1350` and the lines after it show WeatherWidget at 41-69%, CalendarWidgetExtension
  at 28-65%, GeneralMapsWidget at up to 93%, WorldClockWidget at 84-92% and StocksWidget at 72% of a core, from 190 s to
  more than 300 s.
- **The fix** needs only the small image (root.apfs plus a payload rebuild). Nothing forbidden is needed: no
  system-image rebuild and no persistent write. It has two parts:
  - put chronod back in the default job list;
  - stop lsd from registering the 27 widget extensions the home screen does not show (one more filter in the
    existing `lsd_enumerate` hook), so that chronod starts 2 extensions instead of about 26.
- **Calendar icon date [V/I]:** this uses different machinery. The dynamic icon is drawn inside SpringBoard
  (`SBCalendarIconImageProvider`) and does not involve chronod: `hc1` had no chronod and still drew "19". Why the
  number is missing before the first touch is not known. The diagnostic run below records when the provider reports a
  change (§6).

## 1. How a home-screen widget is drawn on iOS 16.5

| Step | Piece | Status on our port |
| --- | --- | --- |
| 1 | SpringBoard's home page `SBIconLocationRootWithWidgets` holds the widget icons (Weather and Calendar on page 1) | **running [V]**: `inject_sb.c:1061` handles that location; `fence3-idle.log` prints `home grid: SBIconLocationRootWithWidgets 3x4` |
| 2 | Each tile is a ChronoUIServices host view (`CHUISWidgetHostViewController`, in the shared cache **[V]**: `dyld_shared_cache_arm64.01/.05/.21`) that connects to chronod over the Mach service `com.apple.chronoservices` (also `com.apple.chrono.widgetcenterconnection`) | SpringBoard asks; **the lookup fails [V]** (§2.2) |
| 3 | chronod (`/System/Library/PrivateFrameworks/ChronoCore.framework/Support/chronod`, user mobile, on demand, `com.apple.chronod.plist` MachServices) keeps the widget descriptors and timelines. On a fresh /var it has no descriptor cache, so it asks **every** widget extension for its descriptors **[G for the reason; V for the effect]**: 26 extensions start in `fence3-idle.log` | **not a job [V]**: `build.sh:121`. Binary and plist are in the system image (`ios165/system.manifest:18570`, ChronoCore `Support/chronod`) |
| 4 | Widget extensions (`com.apple.widgetkit-extension`, `NSExtension` appexes in `<App>.app/PlugIns`) are known from the LaunchServices database that lsd builds; PlugInKit/ExtensionKit (pkd) gives them out, and launchd spawns them with RunningBoard managing them | lsd, pkd (`com.apple.pluginkit.pkd`, `build.sh:113`), runningboardd and containermanagerd are jobs **[V]**. Extensions were spawned when chronod existed **[V]**: `fence3-idle.log:2514` `[CalendarWidgetExtension]`, `:2571` `[WeatherWidget]` |
| 5 | The extension's data container (PluginKitPlugin class), which installd normally makes at install time. Here lsd's seed creates data containers through containermanagerd (`dataContainerCreatingIfNeeded:`, docs/boot-time-lsd.md §3). The kernel answers sandbox request 4 (container path) from `CFFIXED_USER_HOME` (`kern_minimac.c:126-148`), so PlugInKit's "invalid sandbox or container" check passes | **works [V]** (extensions ran and drew on 14 Nov; `minisandbox: request 4 ... answered` in every log) |
| 6 | The extension returns timeline entries with an archived SwiftUI view. chronod (or the extension) renders it with RenderBox into a surface that SpringBoard's host view shows **[G for the exact split]**. Here RenderBox draws with the CPU: `redwoodmetal.m:889-898` gives chronod and every `*Widget*` process no Metal device; `metal/texture.cpp:170-175` copies the widget's surface in | worked on 14 Nov **[V]** (grid3.png); CPU cost **[V]** (G.log) |
| 7 | Without chronod, the host view keeps its placeholder: a plain light rounded rectangle **[I]**. This matches the shots | **[V]** hc1-home.png, hd1-home.png, `HANDOFF-2026-11-17-night.md:228` (cs1) |

Where a widget's pieces came from in failing runs: no `chronod` process, no `*Widget*` process, no
`com.apple.chronod.plist` in `/redwood/LaunchDaemons` (`hc1.log`, `hd1.log`, `cs1.log`: 0 hits) **[V]**.

## 2. Evidence

### 2.1 Kept logs, by run [V]

| run (date) | chronod plist listed | widget processes (`disabling freeze ... [*Widget*]`) | home-screen widgets |
| --- | --- | --- | --- |
| aud4 (13 Nov) | yes | 0 seen (console ring) | - |
| grid3 (14 Nov 13:17) | yes | console ring lost them (fbshot_w=120) | **drawn** (grid3.png) |
| combo3, default0, fence3-idle (14 Nov) | yes | 26 each | - |
| G (14 Nov 23:13) | yes | Weather, Calendar, Maps, WorldClock, Stocks, News... at 28-93% of a core each | - |
| cs1 (18 Nov), hd1, hc1 (19 Nov) | **no** | 0 | **white** |

The last log with chronod is `G.log` (14 Nov). `WIDGETS=` first appears in commit 5c3037a (17 Nov).

### 2.2 SpringBoard asks for chronod and finds nothing [V]

- `hd1.log:829` and `cs1.log`: `failed lookup: name = com.apple.chronoservices, flags = 0x200, requestor = SpringBoard[7], error = 3: No such process`.
- `hd1.log`: ten lines of `(user/501) <Error>: Could not find job with label com.apple.chronod`, from 01:29:02 on.
  `hc1.log` has none, but its console part starts later: the ring is 135 KB.

### 2.3 Extension failures seen with chronod (none of them Weather or Calendar) [V]

- `MailWidgetExtension` userabort, twice per run (`fence3-idle.log`, `combo3.log`, `default0.log`).
- `BooksWidgetExtension`: `extension check-in timeout`.
- `PassbookWidgetsExtension-iPhone`: corpse.
- `CalendarWidget.IntentsExtension` and `ScreenTimeWidgetIntentsExtension` are SIGKILLed by launchd at SpringBoard's
  `signal service` request. This is the normal idle exit **[I]**.
- No AMFI refusal and no sandbox denial for any extension. Sandbox request 6 (extension consume) is "allowed", first
  asked by `PhotosReliveWidget`.

These failures hurt only the hidden widgets (gallery, Today view). The fix's filter (§5) removes those extensions.

## 3. Open Mapple compared (question 2)

Their source is read-only at `scratchpad/open-mapple/xnu_kernel_mediatek_common`, under APSL (`APPLE_LICENSE`). No
code was copied.

- **AMFI IOService, selector 6** (`iokit/Kernel/arm/MappleAMFI.cpp:1-15, 70-89`): an `AppleMobileFileIntegrity`
  IOService whose user client answers selector 6 (cdhash in the trust cache) with success. Their comment says that
  without it ExtensionFoundation fails with "Unable to find AMFI" and LaunchServices drops every appex whose point
  comes from the extension-point cache (posterkit.provider, widgetkit-extension). Selectors are numbered for 16.7.
  - **Ours** reaches the same result another way, in `xnu/files/bsd/kern/kern_minimac.c:72-108`. AMFI request `0x60`
    (query boot-arg state) answers `0x86` ("not enforcing"), so ExtensionKit skips verifying
    `extensions.plist.sig`, and the extension points are known.
  - Evidence [V]: widgetkit extensions were registered and spawned on 14 Nov, and the file has not changed since
    10 Nov (`git log`).
  - Selector 6 is not needed for widgets on 16.5. An IOService could still matter for anything that calls the user
    client directly, but no log has "Unable to find AMFI" (0 hits in `logs/`).
- **Sandbox op 4, container path** (`bsd/kern/amfi_stub.c:288-291, 302-337, 404-405`): they return the user's home
  directory. **Ours** returns the real container from `CFFIXED_USER_HOME` (`kern_minimac.c:126-148, 238-240`), which
  is more exact. Equivalent for PlugInKit.
- **Sandbox ops 5 and 6, extension issue and consume** (`amfi_stub.c:285-287, 339-385, 406-409`): they issue a real
  token and consume it into a handle, and say tccd needs this to grant Photos to assetsd.
  - **Ours** answers both with success and writes nothing (`kern_minimac.c:229-242`).
  - Relevant to "Photos grants" (open-mapple-plan 2.8); **not** to widgets. grid3 drew widgets with the current
    behaviour [V].
  - Not part of this fix. Their numbers are for 16.7; re-check them against 16.5 libsystem_sandbox before porting the
    idea (APSL, with notice).
- **Do widgets work on Open Mapple?** Unknown. Their tree mentions widgets only in the MappleAMFI comment above, with
  no docs, issues or screenshots about them.

## 4. Causes ranked (question 3)

1. **chronod not in the job list.** Verified as described above.
   - SpringBoard's lookup fails, no extension starts, and the tiles are white.
   - The only change between the drawn grid3 and the white cs1/hc1/hd1 that touches this path is chronod's removal.
     The others (lsd R1/R3, logd_helper dropped, mediaremoted) came on 18-19 Nov, and cs1 was already white on 18 Nov
     before R1/R3.
   - **Confidence: high.** The diagnostic run confirms it.
2. **After chronod is back, the next risk is CPU and the console ring [V for CPU, I for the effect].** 26 extensions
   drawing with the CPU on four A53 cores can push the widgets' first snapshot past the picture time and slow the
   home screen. The diagnostic run measures this.
3. **lsd changes R1/R3 (19 Nov) could alter appex registration [G, low].** MobileCal and Weather are not in
   `lsd_hidden[]` (`inject.c:2041`), and the hook only filters `/Applications` itself today (`inject.c:2392-2425`).
   The diagnostic run shows whether Weather and Calendar extensions start.
4. **Sandbox extensions (op 5) returning an empty token [G, low].** This could break a chronod→SpringBoard file
   handoff, but the same kernel drew widgets on 14 Nov.

## 5. Fix design (top cause)

- **F1. chronod on by default.**
  - In `userland/build.sh`, invert the switch: `WIDGETS=0` leaves chronod out, and by default it is in.
  - Fix the comment at `build.sh:118-120`. It says "the home screen is untouched", which `grid3.png` and
    `hc1-home.png` disprove.
  - The job is on demand (no RunAtLoad; `mkjobs.py` strips it anyway), so chronod starts when SpringBoard first asks.
  - Cost: one 2.3 KB plist in root.apfs.
- **F2. Only the home screen's widget extensions are registered.**
  - In `userland/src/inject.c`, `lsd_enumerate()` already sees every `<App>.app/PlugIns` directory **[V]**:
    `lsd: enumerate: /Applications/AAUIViewService.app/PlugIns` in `hc1.log`, `hd1.log`, `ft3.log`, `d5.log`.
  - Add a second list, `lsd_widget_hidden[]`. When the directory path ends in `.app/PlugIns` and `lsd_widgets` is on,
    skip those leaves.
  - Boot-arg `lsd_widgets=0` turns the filter off; `#define LSD_WIDGETS_DEFAULT 1`. Parse it in `lsd_hooks()` like
    `lsd_hide`, and install the hook when `lsd_hide || lsd_widgets || lsdiag`.
  - The list is the 27 widgetkit appexes in the image other than the two on the home screen (from their
    `NSExtensionPointIdentifier`, [V]): AppStoreWidgetsExtension, BatteriesWidgetExtension, GCWidgets,
    PhotosReliveWidget, LockScreenPeopleWidget_iOSExtension, PeopleWidget_iOSExtension, ScreenTimeWidgetExtension,
    SleepWidgetExtension, TVWidgetExtension, BooksWidgetExtension, FindMyWidgetItems, FindMyWidgetPeople,
    FitnessWidget, HealthMedicationsWidgetExtension, HomeWidget, GeneralMapsWidget, MailWidgetExtension,
    com.apple.mobilenotes.WidgetExtension, WorldClockWidget, MusicWidgets, NewsTag, NewsToday2,
    PassbookWidgetsExtension-iPhone, PodcastsWidget, RemindersWidgetExtension, StocksWidget, TipsWidget (each with
    `.appex`).
  - Kept: `WeatherWidget.appex`, `CalendarWidgetExtension.appex`, and every non-widget appex (the Intents extensions
    included).
  - Cost: the gallery and Today view offer only these two widgets. That is acceptable on a 3x4 home screen, and
    `lsd_widgets=0` restores the rest.
- **F3 (only if the fix run still shows the two extensions above ~30% for minutes):** look at why WeatherWidget keeps
  re-rendering, for example a location or network timeline reload loop.
  - Out of scope until measured. G.log predates mediaremoted, whose absence also burned CPU (docs/x6826b-emmc.md).
- **Nothing forbidden is needed** for F1 or F2: there is no `SYSTEM=1` and no var_store or emmc write. A persisted
  chronod descriptor cache would also stop the start-everything burst, but it needs a persistent /var, which is
  forbidden. F2 makes it unnecessary.

## 6. One silent diagnostic run (question 4), "wg1"

- **Purpose:**
  - confirm cause 1 with stock chronod and no F2;
  - measure the extensions' CPU on today's build;
  - see the Calendar icon with no touch at all.
- **Silence:** there is no touch, no tap, no unlock, no camera, no tone and no player in the scenario.
  - `mtkaudio=2` stays in, as in the silent `hc1`, because no recent run boots without it.
  - The microphone and encoder args are dropped.

**Build** (small image only):

```
WIDGETS=1 scratchpad/ubuild-d.sh "" rc.jobs.widgets
XL_RAMDISK=userland/root.apfs ./xl_x6826b build
```

Both are needed: the handoff's md2g lesson is to rebuild the payload after any image change. Check that
`userland/root/LaunchDaemons/com.apple.chronod.plist` exists and that `root/bin` has `psgrep`, `tolog` and `rwtouch`.

**Boot-args** (the r1-run.sh 3rd argument, 172 bytes; `wchk=1` sits at byte 96-102 of `kern.bootargs`, inside
rwtouch's 256-byte `ifarg` window):

```
wchk=1 mtkaudio=2 r1cam=4 pvr=6 pvr_gl=1 fbaspect=1 glwait=3 glthreads=1 cam_fps=10 fbscale=1 r1_clk=1 r1_cpufreq=2200 r1_gpu=680 glyuv=1 lsdiag=1 fbshot_at=160 fbshot_w=70
```

Run with `tools/r1-run.sh wg1 215 "<args above>"` (main session only).

- `fbshot_at=160`: `hd1` used the same value and got its picture at 168.6 s, before the display slept at ~170 s.
- `fbshot_w=70`: 22 KB, which does not flood the ring.

**Scenario** `userland/rc.jobs.widgets`: 7 lines, each under 400 characters, about 1 KB in all. The timings assume
rwtouch starts at ~41 s (`rc.jobs.homechk` comment) and the first frame at ~111 s.

```
# Widgets diagnostic (docs/widgets-spec.md section 6), wchk=1 FIRST in the boot-args. Silent: no touch, no sound. Shot by fbshot_at=160.
/redwood/bin/rwtouch ifarg wchk=1 say wstart wait 95 notify com.redwood.widgets say w1
/redwood/bin/tolog /redwood/bin/psgrep chronod Widget pkd
/redwood/bin/tolog /redwood/bin/psgrep -cpu 20
/redwood/bin/rwtouch ifarg wchk=1 notify com.redwood.widgets say w2 wait 15 notify com.redwood.widgets say w3
/redwood/bin/tolog /redwood/bin/psgrep -cpu 15
/redwood/bin/rwtouch ifarg wchk=1 say end
```

Timeline: w1 at ~136 s, then the process list, CPU 136-156 s, w2 at ~157 s, the picture at ~160-168 s, w3 at
~172 s, CPU 172-187 s, and end at ~188 s. That is well inside 215 s.

**Log lines to add** (`userland/src/inject_sb.c`, active only with `wchk=1`; kept lines through `keep_line`, each
capped):

- `sbw: <tag> view <Class> <w>x<h> hidden <0|1> alpha <a> contents <0|1> sublayers <n>`: on the notification
  `com.redwood.widgets`, walk every window's views (as `layers()` does). Print each view whose class name contains
  `Widget`, `CHUIS`, `Avocado`, `LiveIcon` or `Calendar`. Cap 16 lines per notification; tag = w1, w2 or w3 in order.
- `sbw: calendar delegate <Class>`, once per class: at load, every class that conforms to the protocol
  `SBCalendarIconImageProviderDelegate`. Use `objc_copyClassList` with `class_conformsToProtocol`, and run it from
  the existing SpringBoard-ready dispatch so the classes are loaded.
- `sbw: calendar icon changed at <s> s (<n>)`: from a wrapper on `calendarIconImageProviderHasChanged:` in those
  classes. Use `class_getInstanceMethod` with `method_setImplementation`, then call the original. Cap 8.
  `replace_method()` is not usable here, because it wants a known address.

**Existing lines that answer the questions:**

| Question | Kept / console line | Cause 1 confirmed when |
| --- | --- | --- |
| chronod job present | console: `/redwood/LaunchDaemons/com.apple.chronod.plist` listing | present |
| SpringBoard reached chronod | console: `failed lookup: name = com.apple.chronoservices` | **absent** |
| extensions started | tolog psgrep lines (pid, path) with `.../WeatherWidget.appex/WeatherWidget` and `.../CalendarWidgetExtension.appex/CalendarWidgetExtension`; console `disabling freeze for pid N [WeatherWidget]` | both present |
| host views drawn | `sbw: w2 view CHUIS... contents 1` or `sublayers >0` for two tiles | yes |
| CPU | `psgrep -cpu` top-ten lines at 136-156 s and 172-187 s | recorded (compare G.log) |
| Calendar icon before any touch | `sbw: calendar icon changed at ...` and the `sbw:` Calendar/LiveIcon view lines | recorded |

**The picture** (`wg1` shot, 75x100):

- Pass: the left tile is not white (blue-grey, "No weather data" or a temperature), and the right tile is white with
  a red day name, a large date number and a line of text, like `grid3.png`.
- Fail and cause 1 not confirmed: both tiles are still plain white with chronod and both extensions running. In that
  case look at the `sbw:` contents lines and the extensions' exits.

## 7. Calendar icon without its date before the first touch (question 5)

- **Not the same machinery [V]:**
  - The icon is `SBCalendarApplicationIcon` with `SBCalendarIconImageProvider` and its delegate method
    `calendarIconImageProviderHasChanged:`. All three strings are in the shared cache (`.01`, `.03`, `.21`), so the
    icon is drawn in SpringBoard.
  - `hc1` (no chronod) drew "19". `hd1` (no chronod, no touch) did not. `hd3` (no chronod, touched) did
    (`HANDOFF-2026-11-17-night.md:403`).
- **What is known [V]:**
  - The missing date goes with "no touch at all".
  - `hd1`'s picture was the last frame before the display slept (168.6 s).
  - In the no-touch run `hd2`, nothing swapped after 123 s.
- **Hypotheses:**
  - **[I] (likeliest):** the provider's dated image arrives only after a later event, such as a day or time-change
    notice or a redraw that the first interaction causes, and nothing asks for it while the home screen sits untouched.
  - **[G]:** the image is committed but not yet composited, because no further frame is drawn when nothing changes.
- **How wg1 separates them:**
  - "changed at" before ~123 s with the shot still blank points to compositing.
  - No "changed" line points to the provider never being told.
  - A "changed" line between 123 and 168 s, with no swap after it, also points to compositing.
- **Fix:** none designed until wg1 has run. If the provider is never told, the smallest fix is to post the
  provider's own refresh once, from `inject_sb.c`, after SpringBoard's first frame. The selector to call has to be
  read from the class at run time, not guessed.

## 8. Implementation tasks (Sonnet-sized)

Device runs are launched by the main session; implementers build only.

1. **T1, scenario file.** Create `userland/rc.jobs.widgets` exactly as in §6. Check: `wc -c` is under 8192, the
   longest line is under 400 characters, and `build.sh` copies `psgrep` and `tolog` because the file names `psgrep`
   (`build.sh:518`).
2. **T2, SpringBoard diagnostics.** In `userland/src/inject_sb.c`, add the three `sbw:` lines of §6, gated by
   `wchk=1` (read `kern.bootargs` into a 1024-byte buffer, as at `inject_sb.c:2257`):
   - the notification handler next to `com.redwood.layers` (`inject_sb.c:2272`);
   - the view walker, reusing the window loop of `layers()` (`inject_sb.c:858`);
   - the delegate scan and wrapper.
   Pass: it builds; without `wchk=1` nothing is registered and no `sbw:` line appears.
3. **T3, diagnostic build.** Build as in §6 (`WIDGETS=1 ubuild-d.sh "" rc.jobs.widgets`, then the payload).
   - Check that `root/LaunchDaemons/com.apple.chronod.plist` is there, that `root/etc/rc.jobs` is the widgets file,
     and that the chain image still fits the 32 MB boot partition (chainboot does not refuse it).
   - The main session runs wg1 and reads the §6 table and shot.
4. **T4, fix F1.** In `userland/build.sh`, make chronod default (`[ "$WIDGETS" != 0 ] && IOS_JOBS=...`), and update
   the header comment (line 16) and the comment at 118-120.
   - Pass: a default build's `root/LaunchDaemons` has `com.apple.chronod.plist`; `WIDGETS=0` removes it.
   - `btbuild3.sh` inherits the change.
5. **T5, fix F2.** In `userland/src/inject.c`, add `lsd_widget_hidden[]` (the 27 names of §5), `LSD_WIDGETS_DEFAULT 1`,
   the `lsd_widgets=` parse in `lsd_hooks()` and the `.app/PlugIns` branch in `lsd_enumerate()`.
   - Keep the `/Applications` branch unchanged. Install the hook when `lsd_hide || lsd_widgets || lsdiag`.
   - Add one kept line when the counter first reaches the list's size:
     `lsd: widgets: 27 of 27 widget extensions hidden`. If the run ends with fewer, print
     `lsd: widgets: N hidden` from `lsd_set_complete` under lsdiag.
   - Add `lsd_widgets` to the `lsd: start ...` line.
   - Pass: it builds.
6. **T6, fix run** (main session, `wg2`). Same build as T3 but with T4 and T5 instead of `WIDGETS=1`, the same
   scenario and the same boot-args. Pass when the kept log shows:
   - `lsd: widgets: 27 of 27 widget extensions hidden`;
   - the psgrep list with chronod, WeatherWidget and CalendarWidgetExtension, and no other `*Widget*` process;
   - no `failed lookup: name = com.apple.chronoservices`;
   - `sbw: w2 ... contents 1` (or sublayers > 0) for both tiles;
   - in `psgrep -cpu` at 172-187 s, no widget process above 30% and total CPU close to wg1's idle figure without
     the extensions;
   - `swap #2 ... at` within 111 ± 3 s, `XNU returned ... reason 0`, and the device back in Android.

   The shot should look like `grid3.png`: Weather tile filled and Calendar tile with the date, today's number and
   "No events today". A regression check in the same session: `rc.jobs.homechk` (`homechk=1`) still shows
   `settings launched by bundle id: 1` and `camera launched by bundle id: 1`.
7. **T7, docs.** Add the result to `docs/STATUS.md`, and to the newest HANDOFF with run names. Push STATUS.md as
   the standing task asks. This spec stays uncommitted until the owner says otherwise.
