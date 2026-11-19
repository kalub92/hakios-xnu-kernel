# Screen scale: where 3x and 1x come from, and one consistent choice (spec, 2026-11-19)

This is analysis and a spec only. Nothing was built, nothing was run on the device, and nothing was committed.
Labels: **[V]** means verified here (from the code, the cache disassembly or a log line). **[I]** means inferred.
**[G]** means a guess. Apple addresses are unslid iOS 16.5 (20F66) iPhone10,3 dyld-cache addresses.

How Apple's code was read:
- `tools/dsc/textdump.py` + `otool -tV` to disassemble an image.
- Four scratch helpers built on `tools/dsc/dsc.py`, kept in the session scratchpad
  (`/private/tmp/claude-501/-Users-calebstultz-Developer-rabbit-i-OS/fd2e78b0-357a-4fd0-9b31-45f2cd33b5ce/scratchpad/scale/`), none in the repo:
  - `lsm.py`: lists ObjC methods by name, built on `tools/dsc/objcimp.py`'s `scan`.
  - `an.py`: annotates the disassembly. Unlike `tools/dsc/annot.py`, whose stub range is hard-coded to CMCapture, it names
    selector stubs, export stubs and `adrp/add` branch islands, plus strings and doubles.
  - `xref.py`: finds `adrp+add` references to a string or CFString.
  - `nm.py`: names the frames of `ustack` lines through the export table.
- `tools/cache-name.py` mis-names some addresses: it gave PencilKit/ProVideo where the export table gives SwiftUI. `nm.py` was
  used for the stacks below.

## 0. Answers in short

1. **The 3x comes from SpringBoardHome's own screen-type table, not from MobileGestalt [V].** Our 375x500-point screen matches no
   iPhone screen type. The lookup's fallback for a phone with no home button and a screen smaller than 375x812 picks
   **screen type 4**. Type 4's home icon variant is hard-coded as `0x690` = **56 pt @3**. MobileGestalt (`main-screen-scale`,
   `ArtworkDeviceScaleFactor`) and UIScreen both say 1 here. They come from the device tree. Our kernel rewrites it to the
   `fbscale` value (`/chosen display-scale`, `/product artwork-scale-factor`).
2. **Only the home-screen icon grid is at the wrong scale.**
   - Everything else that renders reads the process's UIScreen scale, which is 1: app layers, text, SF Symbols, blur, snapshots,
     and the widget hosting **[I, high]**.
   - Widgets are not rendered as bitmaps at all. The extensions build **SwiftUI view archives** (WidgetKit `Archiver`, SwiftUI
     `_ArchivedViewHost` frames in the stacks) **[V]**. The two full-core bursts are SwiftUI layout plus date formatting (ICU,
     CFDateFormatter), not pixels. **The mismatch explains neither burst [I, high].**
   - There is a wider mismatch the hook cannot fix: the rootfs asset catalogs are **thinned to @3x** **[V]**. Every app bitmap
     asset is minified 3:1 at 1x, but only 1.5:1 at 2x.
   - The cost of the icon mismatch is quality, not resources: about 1 MB of extra icon memory, and unmeasured iconservicesagent
     work at 9x pixels.
3. **Recommendation: everything at 2x.**
   - The panel is **480x640**. We present 375x500 points, which is 1.28 panel pixels a point.
   - 1x renders 375 px and enlarges it 1.28 times: soft text, and 3x art shrunk 3:1. 2x renders 750 px and reduces it 0.64 times:
     sharper text, and art shrunk only 1.5:1. 2x is also a scale iOS 16 actually ships; no iOS 16 iPhone is 1x.
   - Cost: 4x fragment pixels and about +2 ms of kernel scaling per frame. The GPU's headroom at 4x pixels is the open question.
     The one run (section 5) decides it. If 2x fails its thresholds, the same icon hook makes **1x** consistent instead.
4. **Changes:**
   - Kernel: none. `fbscale=2` exists, and is the default with `fbaspect=1`.
   - One new SpringBoard hook (`sb_iconscale`, default on).
   - One diagnostic line (`scalechk=1`) and one scenario.
   - No system-image rebuild and no persistent write. Everything is in the payload (`/redwood`) or a boot-arg.
5. **One silent run of about 205 s:** 2x with the hook, a 9-swipe-pair measurement after the widget bursts, and one poke-driven shot.
6. **The second widget burst at about 139 s** starts when the extensions are resumed after a RunningBoard suspension at about
   123 s **[V]**. Unlock is excluded: wc1/wc2 had no touch **[V]**. Leading suspects are chronod's
   `initialReloadFailureRetry`, a DAS budgeted activity, or keybag first-unlock. Log chronod's stacks at 1 s ticks over
   132-148 s.

## 1. Where 3x and 1x come from

### 1.1 The 3x: SpringBoardHome's screen type → icon variant (Apple) [V]

| Step | Symbol / address | What it does |
|---|---|---|
| a | `+[SBHDefaultIconListLayoutProvider currentDeviceScreenType]` 0x1c4538518 → 0x1c44ec4a8 | `dispatch_once`. The block invoke is at 0x1c453ad1c and its result is cached at 0x1e2549ec0 |
| b | block 0x1c453ad1c | Inputs: `__sb__mainScreenReferenceBounds` and `__sb__mainScreenScale` (SpringBoardServices; in SpringBoard, otherwise `UIScreen`), `SBFEffectiveArtworkSubtype`, `SBFEffectiveDeviceClass`, `SBFEffectiveHomeButtonType` (SpringBoardFoundation). It passes them to the lookup 0x1c4558634, the `_SBHScreenTypeLookup` named in inject_sb.c:991 |
| c | lookup 0x1c4558634 | Exact pass first (enumeration 0x1c44d2738). For no match on a phone with no home button, it walks size floors (`BSSizeGreaterThanOrEqualToSize`) over the size table at 0x19155ff30 (via 0x1dae36a48): 430x932 → 0x17, 428x926 → 0xd, 414x896 → 5, 393x852 → 0x15, 390x844 → 0xb, 375x812 → 3, **otherwise 4**. 375x500 → **4**. The override `overriddenScreenType` (0x1c4558a20) applies only with `os_variant_has_internal_content`, so it is not usable |
| d | `-[SBHDefaultIconListLayoutProvider makeLayoutForIconLocation:]` 0x1c4539588 | Calls 0x1c463d3cc(screenType, options) for the icon-info structs (sret at sp+0x4c0: {w, h, **scale** at +0x10, radius}). The scale reaches `setIconImageInfo:forGridSizeClass:` (call at 0x1c453a298) |
| e | 0x1c463d1e8 → 0x1c463d0a0 | Variant code by screen type (jump table 0x1c46ef6a8). Types 2, 3, 6, 9, 11, 14, 15, 17, 20, 21, 24 → 0x708 (60@3). Types **4**, 10, 12, 16, 18, 22 → **0x690 (56@3)**. Types 5, 13, 19, 23 → 0x780 (64@3). Types 0, 1, 7, 8 → @2. Codes 0x690/0x708/0x780 → scale **3.0**; codes 0x70-0xa7 → 2.0. 0x1c463d12c gives the points (0x690 → 56) |
| f | getters | `-[SBIconListGridLayoutConfiguration iconImageInfoForGridSizeClass:]` **0x1c451e558** holds the value. `-iconImageInfo` 0x1c453f648 forwards to it with nil. `-[SBIconListGridLayout iconImageInfo]` 0x1c44ce7fc and `iconImageInfoForGridSizeClass:` 0x1c44ce25c forward to the configuration (ivar +0x20) by `objc_msgSend`. `-[SBIconView iconImageInfo]` 0x1c44da5f0 takes it from the list layout or icon image cache |

- **[I]** Type 4 is most likely the Display-Zoom iPhone X class (320x693 @3, which is in the size table at +0x40). It is the
  smallest phone with no home button, so it is the bottom of the chain.
- Consequence: the scale is fixed by the screen type. It is 3 with `fbscale=1` and still 3 with `fbscale=2`. Presenting
  different MobileGestalt values cannot change it. Matching a different type needs a real iPhone size, and a home-button type
  (0 = 56@2) would bring back the home-button UI.
- **[V]** In the logs (wc1-wc3, no fix):
  - `sbw: calendar image #1 56x56@3 ... 168x168` while `UIScreen` = 1 (`screen@1`), and the icon layer is
    `56x56 cs 1.0 ...: 168x168`.
  - A second consumer asks at the screen's scale: `#2 48x48@1`. So the grid is the odd one out.
- **[V]** At the 103 s hang the SpringBoard main thread is inside `SBHGetApplicationIconImage+0x90` → IconServices → a sync XPC
  to iconservicesagent (wc2, wc4, wg2, ft5). **[I]** The ordinary app icons are generated at the same 56@3 = 168 px.

### 1.2 The 1x: device tree → MobileGestalt → UIScreen (ours, then Apple) [V unless marked]

| Where | File:line / address | Value at our BASE (`fbaspect=1 fbscale=1`) |
|---|---|---|
| Identity product node | xnuloader/identity.h:51 (`artwork-scale-factor`, data offset 118 = 3), :89 (`artwork-device-subtype`) | 3, then the loader overrides the subtype |
| mtkloader | mtkloader/mtkloader.c:844-849 (`artwork-device-subtype` forced to 2436), :931 (`/chosen display-scale` = 2), :985 (`/product` from identity: `artwork-scale-factor` 3) | 2 / 3 / 2436 |
| Kernel display service | xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp:132-134 (1125x2436, 458 dpi defaults), :146 `fbscale`, :165 `fbaspect` | |
| | :674-697: with `fbaspect` or `fbscale` != 3, the size becomes 375*s x round(375*640/480)*s, **and it writes `/chosen display-scale` and `/product artwork-scale-factor` = s** (s = `fbscale`, else 2) | 375x500, both 1 |
| | :721 log `display %ux%u on a panel of %ux%u` | wc1: `display 375x500 on a panel of 480x640` |
| | :1877-1885 `GetDisplaySize` = fWidth x fHeight; :1894-1899 dot pitch 458*w/1125 | 375x500; 152 dpi |
| MobileGestalt | libMobileGestalt: `IODeviceTree:/chosen` `display-scale` read at 0x19c531cd0 (CFString 0x1d7366c00). Key `main-screen-scale` 0x1d7369d00 referenced at 0x19c539b00. `/product artwork-scale-factor` read at 0x19c52ba1c (0x1d736b320). Key `ArtworkDeviceScaleFactor` 0x1d736ca00 at 0x19c541748 | 1 / 1 |
| backboardd | userland/src/inject.c: **no** scale hooks (grep for scale / MainScreenInfo / contentsScale: none) | |
| UIScreen | **[I]** `BKSDisplayServicesGetMainScreenInfo` (the hang #1 stack) → backboardd's CADisplay, plus MobileGestalt's scale | `screen@1` in sbw lines |

- The status bar, keyboards and widget sizes key on points or pixel width, not scale. inject_sb.c:899-985 already replaces the
  Fallback status-bar provider for any pixel width.
- `fbshot`/`touch` work in fractions of the panel. Touch needs nothing.

## 2. What else is computed at which scale

| Thing | Scale used | Evidence | Note |
|---|---|---|---|
| Home grid and dock icon images, Calendar provider | **3** (screen type) | [V] 1.1 | The only real mismatch. inject_sb.c:1268-1316 (`calfix`) patches only the Calendar provider |
| Other SpringBoard icon consumers (`48x48@1`) | 1 | [V] wc1 | |
| Icon layers (`contentsScale`) | 1 | [V] `cs 1.0` | |
| Widget extensions (Calendar, Weather) | process UIScreen = 1; output is a **view archive** | [V] stacks: `WidgetKit Archiver.ArchivingDelegate`, `SwiftUI _ArchivedViewHost`, `_ShapeView` (119.9 s); 2nd burst ICU `DateFormatSymbols`, `CFDateFormatter`, CFLocale numbering, `__NSStringDrawingCore`, `TextLayoutProperties`. [V] WidgetKit, ChronoCore and ChronoUIServices import `mainScreen`/`scale` (methname strings) | Render cost is layout and formatting, almost free of the pixel count [I, high]. Not a cause of the bursts |
| Widget hosting in SpringBoard (CHUIS) | 1 (`_effectiveSizePixelAlignedForDisplayScale:` takes the screen's) | [I] | |
| App content: layers, text, SF Symbols (vector) | 1, consistent | [I] | |
| **App bitmap assets** (Assets.car) | **rootfs is thinned to @3x**: Preferences 4x@3 / 1x@1; SpringBoard 30@3 13@2 5@1; SpringBoardHome 44@3 4@2 2@1; Photos 6@3 1@1 (`assetutil --info`) | [V] | At 1x every `UIImageView` image is minified 3:1 by CA (bilinear, no mipmaps: thin strokes lost, as on the Calendar icon) [I]. At 2x it is 1.5:1 |
| Snapshots (XBApplicationSnapshot), launch zoom | UIScreen scale | [I] | inject_sb.c hooks only the snapshot output format |
| Wallpaper | our JPEG is **375x812 px** (build.sh:403), `resizeAspectFill` into 375x500 pt | [V] | 1:1 at 1x; enlarged 2x at 2x. It is a 0.4-blurred render, so this is harmless |
| Dock/folder blur | backboardd's backdrop at display scale | [I] | |

Cost of the mismatch, from the logs:
- **Memory:** an icon image is 168²·4 = 110 KiB instead of 12 KiB (1x) or 49 KiB (2x). The home screen holds about 11 icons
  plus the Calendar one: page 1 has 7 icons (`icon list ... 7 icons`), the dock 4 **[V]**. That is about **+1.1 MB** in
  SpringBoard, plus iconservicesagent's copies **[I]**. The `memory:` lines show 1548-1555 MB available **[V]**. Negligible.
- **CPU:**
  - iconservicesagent composites each icon at 9x the 1x pixels **[I]**. Its CPU was never sampled, so it is not measured. The
    run samples it.
  - The widget bursts do not depend on scale. The first burst ran ~98-123 s (CPU 7.4 s at 116.9 s, 12.0 s at 122.9 s). The
    second ran ~139-171 s and took 30 s of CPU for Calendar (wc2). They are layout work **[V stacks, I attribution]**.
- **GPU:** 11 × 110 KiB is uploaded once. Per-frame minification of 11 icons is negligible **[I]**.

## 3. The right single scale for this panel

Facts:
- Panel 480x640 **[V]**. Presented 375x500 pt **[V]**. Ideal render scale 1.28.
- The kernel scales every frame onto the panel with a 2-tap bilinear filter (`fb_filter=1`, :224) on 3 threads (:232).

| | 1x (`fbscale=1`) | 2x (`fbscale=2`, the default with `fbaspect=1`) |
|---|---|---|
| Pixels rendered | 375x500 = 0.19 M | 750x1000 = 0.75 M (**4x**) |
| To the panel | enlarge 1.28: soft text | reduce 0.64: sharper text, slight aliasing of 1-px features with the 2-tap filter [I] |
| 3x assets | 3:1 minify, strokes lost | 1.5:1 |
| iOS support | no iOS 16 iPhone is 1x [I] | standard |
| Kernel scaler per frame | **1.75 ms** (wc5 1746-1891 µs, reg1 1729-1783, lc2 1713) [V] | **3.7-4.9 ms** (camgl1 3701-3762, ws6-ws10 4008-4865; older builds) [V, not the same build] |
| Render thread (swipes, lc2) | GL frame 4.4 ms, 3.3 ms in glFlush, idle 91%, 21 swaps/s peak [V] | CPU per pass is driver work, independent of draws (pb1) [V]. Pixel count was never tested |
| GPU fragment | lc2 swipe window: 5186 fragment jobs, kick-to-done median 1.5 ms (least 0.36) [V] | ×4 if fill-bound [I]. Worst case it becomes the limit (≈10-14 swaps/s) [G]. Best case it stays under the swap budget |
| Memory | `19 surfaces 3 MB` [V] | ≈12 MB of surfaces, plus 4x CA backing stores [I]. Available 1.5 GB [V] |

- 240x320 @2 (1:1 with the panel) is rejected: it is too narrow for iOS layouts, which need at least 320 pt.
- 480x640 @1 is rejected: everything would be 22% smaller on a small panel.

**Recommendation: 2x everywhere, conditional on the run's thresholds (section 5).** Values to present:

| Key | Value | Source |
|---|---|---|
| Display size (IOMFB `GetDisplaySize`) | **750 x 1000** | kernel, `fbscale=2 fbaspect=1` |
| `/chosen display-scale` → MG `main-screen-scale` | **2** | kernel :674-697 |
| `/product artwork-scale-factor` → MG `ArtworkDeviceScaleFactor` | **2** | kernel |
| Dot pitch | 305 | kernel (458·750/1125) |
| `artwork-device-subtype` / `ArtworkDeviceSubType` | **2436** (unchanged: no-home-button gestures, safe area) | mtkloader:844 |
| `DeviceClass` / `ProductType` | iPhone / **iPhone10,3** (unchanged: the dyld cache) | identity |
| UIScreen `scale` | 2 (follows) | |
| SpringBoardHome screen type | 4 (cannot change) | |
| Home icon `SBIconImageInfo` | **56x56 @2** (hook) | new, section 4 |
| Points | 375 x 500 (unchanged: grid, status bar and keyboards keyed on points) | |

If 2x fails, apply the same table at 1x: `fbscale=1`, MG 1/1, display 375x500, icons 56@1 through the hook.

## 4. Changes (each behind a boot-arg), risks, and what is not needed

| # | Change | Where | Switch |
|---|---|---|---|
| C1 | 2x display (exists, no code) | kernel | `fbscale=2` / omit; `fbscale=1` for A/B |
| C2 | **Icon scale = screen scale**: replace `-[SBIconListGridLayoutConfiguration iconImageInfoForGridSizeClass:]` (0x1c451e558) with `replace_method`. The original returns a 4-double HFA {w, h, scale, radius}. Return it with `scale` = `[[UIScreen mainScreen] scale]` when that is ≥ 1 and different. One kept line on the first change | userland/src/inject_sb.c (install next to the `make_layout` hook, :2468) | `sb_iconscale=0` off (default on) |
| C3 | One scale summary line: UIScreen scale/nativeScale, MG `main-screen-scale` and `ArtworkDeviceScaleFactor` (`dlsym MGCopyAnswer`), `currentDeviceScreenType`, the root list layout's `iconImageInfo` | inject_sb.c, with the `say_icon_lists` timer (40 s after the layout) | `scalechk=1` |
| C4 | `calfix` (exists) becomes a no-op once C2 is on (asked == screen). Keep it until C2 is proven, then delete `cal_fix_start` | inject_sb.c:1268-1316, :2560-2569 | `calfix=0` |
| C5 | Status-bar provider (exists) is needed at both scales | inject_sb.c:952-985 | `sb_statusbar=0` |
| C6 | Scenario rc.jobs.scale | userland/ | `scalechk=1` |
| opt | Wallpaper rendered at 750x1000 (build.sh:403 args); kernel area filter for the downscale (`fb_filter=2`) | | only if the shot shows blur or aliasing |

Risks:
- **GPU fill at 4x** is the main risk and is unmeasured. Mitigation: the run's thresholds, and C1 stays a boot-arg.
- **Layouts that assume iPhone X metrics:**
  - The grid, insets, safe area (2436 subtype: 44/34 pt), keyboard and widget sizes are in points and do not change with scale **[I]**.
  - The screen type stays 4 in both modes **[V]**.
- **Status bar:** the pixel width is 750, not 1125, so UIKit picks the Fallback. C5 replaces it, as it does at 375 **[V comment
  :899-905]**.
- **"Unknown scale":** 2 is a shipping scale; 1 is the unusual one. The unknown thing is the 750x1000 pixel size, but layout is in
  points **[I]**. Earlier r1 runs at `fbscale=2` (camgl1, ws6-ws10, older stack) had Camera and SpringBoard working **[V]**.
- **CoreUI:** with `ArtworkDeviceScaleFactor` 2 and only @3x renditions present, it picks @3x, the same as today at 1x **[I]**.
- **Stale MobileGestalt cache:** MobileGestalt keeps a cache (`.../systemgroup.com.apple.mobilegestaltcache/.../com.apple.MobileGestalt.plist`,
  CacheExtra) **[V strings]**. With `var_store=1` a persisted /var could carry a stale scale **[G]**. Run without `var_store`,
  which is the default.
- **The C2 hook:** it is installed in the constructor at ~33 s, before the layout at ~113 s. SBHIconImageCache and SBIconView take
  their info via `objc_msgSend` through the hooked getter **[V disassembly 1.1f]**. The ABI is the same 4-double HFA that
  `sbw_image` already uses.

**Not needed:** a system-image rebuild or any persistent write. C1 is a boot-arg. C2, C3 and C6 are in libredwood-sb.dylib and
`/redwood/etc/rc.jobs`, inside the payload (`root.apfs` → `xl` payload). The eMMC system image is untouched, and so is the loader.

## 5. The one diagnostic run (silent, ≤ 230 s)

- **Build:** the default userland with C2, C3 and `RC_JOBS=rc.jobs.scale`. build.sh:520 copies tolog and psgrep because the file
  names psgrep. Then the payload.
- **Command:** `tools/r1-run.sh sc1 225 "<args>"`. The args:

```
scalechk=1 wchk=1 fbscale=2 mtkaudio=2 r1cam=4 pvr=6 pvr_gl=1 fbaspect=1 glwait=3 glthreads=1 cam_fps=10 r1_clk=1 r1_cpufreq=2200 r1_gpu=680 glyuv=1 pvr_stats=1 swprof=1 proc_sample=1 fbshot_at=190 fbshot_w=70 loader_return_after=225
```

- The fixed prefix of the boot-args is about 113 bytes (`debug=0x8 ... emmc_blocks=4966400 `), so `scalechk=1` sits at about
  byte 114, inside the first 256.
- `wchk=1` is there for the `sbw:` calendar lines and the `com.redwood.wpoke` window.
- There is no Camera, no shutter and no sound. The unlock and swipes are silent, as in the swprof runs.

**rc.jobs.scale** (about 1.6 KB, every line under 400 characters; rc starts at ~22 s, the expected times assume that, and the
`say` lines are the anchors):

```
# Screen-scale check (docs/screen-scale-spec.md section 5). scalechk=1 FIRST in the boot-args. Silent: no camera, no shutter, no sound.
/redwood/bin/rwtouch ifarg scalechk=1 say sstart wait 73 say s_icons
/redwood/bin/tolog /redwood/bin/psgrep -sample 2 10 iconservicesagent &
/redwood/bin/rwtouch ifarg scalechk=1 wait 22 say s_widgets
/redwood/bin/tolog /redwood/bin/psgrep -sample 3 20 CalendarWidgetExtension &
/redwood/bin/rwtouch ifarg scalechk=1 wait 15 say s_chrono
/redwood/bin/tolog /redwood/bin/psgrep -sample 1 16 chronod &
/redwood/bin/tolog /redwood/bin/mg_test main-screen-scale main-screen-width main-screen-height ArtworkDeviceScaleFactor ArtworkDeviceSubType DeviceClass ProductType
/redwood/bin/rwtouch ifarg scalechk=1 wait 38 tap 0.5 0.06 wait 1 notify com.redwood.unlock wait 5 say s_swipes
/redwood/bin/rwtouch ifarg scalechk=1 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35
/redwood/bin/rwtouch ifarg scalechk=1 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35
/redwood/bin/rwtouch ifarg scalechk=1 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35 say s_swipes_end
/redwood/bin/rwtouch ifarg scalechk=1 wait 8 notify com.redwood.wpoke say s_poke wait 10 say end
```

Expected timeline:

| ~s | What |
|---|---|
| 95-115 | iconservicesagent sampled |
| 117-177 | Calendar extension sampled. Both widget bursts land here |
| 132-148 | chronod sampled at 1 s ticks (question 6) |
| ~133 | MobileGestalt answers |
| ~172 | unlock |
| ~178-189 | 18 swipes (9 pairs), after the bursts, ending on page 1 |
| ~197 | the poke: one 3x3 window, one swap. `fbshot_at=190` takes the picture in the 5-s report that this swap produces. The display reports only while it swaps (wg1). Swipe swaps after 190 would also do |
| ~207 | end; the loader returns at 225 |

Pass criteria (kept-log lines):

| # | Line | Pass |
|---|---|---|
| P1 | `MiniIOMobileFramebuffer: display 750x1000 on a panel of 480x640` | exact |
| P2 | `mg_test: main-screen-scale = ... +2.0`, `ArtworkDeviceScaleFactor = ... 2`, `ProductType ... iPhone10,3` | values |
| P3 | `redwood: icon scale: 56x56@3 -> @2 (UIScreen 2)` (C2) | present once |
| P4 | `sbw: calendar image #1 56x56@2 (asked 2) ... 112x112`, then `calendar image icon layer d2 CALayer 56x56 cs 2.0 ...: 112x112` | asked 2 proves C2 at the source; 112 px at cs 2 is 1:1 |
| P5 | `redwood: scale: UIScreen 2 ... MG 2/2 ... screen type 4 ... icon 56x56@2` (C3) | present |
| P6 | `redwood: status bar: ... the iPhone X's provider instead`; `redwood: icon list SBIconListView 375x500: 3x4, insets 60,27,196,27` | layout unchanged in points |
| P7 | During the swipes (~178-190): `MiniIOMobileFramebuffer: N frames in ~5000 ms (S a second), D us each in the driver` | **S ≥ 15** (1x: 20-21) and **D ≤ 4500** (1x: 1750) |
| P8 | `!redwood-gl: pvr: N frames in ..., X ms each` for the swipe window; `pvr: stats: fragment: kick to done: ... M middle` | **X ≤ 9** (1x lc2: 4.4). M is for information (1x: 1525 µs). `GPU resets 0, faults 0` |
| P9 | `memory: ... (N surfaces S ...)` | for information (1x: 19 / 3 MB); available ≥ 1300 MB |
| P10 | fbshot picture | decodes; icon text and the Calendar "19" legible; both widgets drawn; no red status bar |
| P11 | `XNU returned to mtkloader: reason 0 (end)` | present |
| info | `!sample top:` iconservicesagent ticks; CalendarWidgetEx ticks (burst start and length at 2x vs 139/171 at 1x); chronod ustacks 132-148 | for questions 2 and 6 |

- If P1-P6 pass but P7/P8 fail: keep **1x with C2** (`fbscale=1`, C2 on), and say so in STATUS.
- If P7 and P8 pass: drop `fbscale=1` from the documented BASE.

## 6. The second widget burst at ~139 s (brief)

Facts **[V]**:
- The first burst starts with the extension launch: `disabling freeze` for pid 44 at 97.9 s, CPU 7.4 s by 116.9 s.
- At 122.9 s the extension is suspended (`the task suspended 1 time(s)`, runningboardd 38%). It stays suspended through 137.97.
- At 140.98 it is resumed (`suspended 0`, 3 threads instead of 4) with chronod at 24-50%. It renders for 30 s of CPU.
- The second burst starts at the same uptime in wc1-wc3 and is common to Calendar and Weather, so the trigger is in chronod.
- No touch in wc1/wc2, so it is not the UI unlock.
- Its stacks are date formatting and text layout.

Candidates, all named in ChronoCore's strings **[V strings, I relevance]**:
1. `initialReloadFailureRetry` (MobileSnapshotReloadConfigurationPolicy): the first render was cut by the suspension and is
   retried after a delay. This fits the "resume 16 s after suspend" pattern.
2. A `DAS budgeted refresh activity` started by dasd at a fixed delay after boot.
3. Keybag first unlock: `%{public}s notified of first unlock; adjusting responses`. With no passcode it can come at boot
   without a UI unlock.
4. `SignificantTimeChangeNotification` / `localeChangeCoordinator` / `NetworkEnabledAfterBootNotification`. Less likely: no
   Wi-Fi in wc1-wc3, and no clock set after the `epoch=` boot-arg **[I]**.
5. Placeholder pre-caching (`Removing pre-caching startup reason`, `MobilePlaceholderReloadStrategy`).

What to log:
- The chronod `psgrep -sample 1 16` in the run above. The thread that wakes at ~138-140 names the path when decoded (slide =
  the `slide` in the `!sample +N s` line; name the frames by export). The frames that would point to each candidate:
  - BackgroundSystemTasks or DuetActivityScheduler frames: DAS.
  - libnotify `_notify_dispatch_handler`: a Darwin notification.
  - a libdispatch timer under ChronoCore: the retry.
  - MobileKeyBag: first unlock.
- If that is not conclusive, one more run with `chronod` added to the test build's libredwood-sb INJECT line and `poster_log=1`.
  The reader at inject_sb.c:1953 keeps only error-like words, so add a `chrono_log=1` keyword set: `reload`, `reason`, `DAS`,
  `activity`, `Significant`, `locale`, `unlock`, `Network`, `pre-caching`, `retry`, `timed out`, `budget`. wc3's OSLOG route gave
  nothing; DT mode in-process is the other way to get chronod's own words.

## 7. Implementation tasks (Sonnet-sized)

**T1: icon scale hook (C2).** userland/src/inject_sb.c.
- Add `typedef struct { double w, h, scale, radius; } icon_info;` and `static icon_info (*orig_icon_info)(id, SEL, id);`.
- In `icon_info_for_class(id self, SEL sel, id cls)`, call the original. If `sb_iconscale` is on and `[[UIScreen mainScreen] scale]`
  is ≥ 1 and differs, set `.scale`, and `keep_line("redwood: icon scale: %gx%g@%g -> @%g (UIScreen %g)", ...)` once.
- Install it with `replace_method("SBIconListGridLayoutConfiguration", "iconImageInfoForGridSizeClass:", 0x1c451e558UL, ...)` next
  to :2468, unless the boot-args hold `sb_iconscale=0`. Use the same boot-arg parse style as `sb_statusbar=0` at :2521.
- Pass: it builds. In a run, P3 and P4. With `sb_iconscale=0` the old `56x56@3` comes back.

**T2: scale summary line (C3).** inject_sb.c, under `scalechk=1`, from the existing 40-s `say_icon_lists` timer.
- One `keep_line`: `redwood: scale: UIScreen %g native %g, MG main-screen-scale %s ArtworkDeviceScaleFactor %s, screen type %ld, icon %gx%g@%g`.
- MobileGestalt through `dlsym(RTLD_DEFAULT, "MGCopyAnswer")`. The screen type from `+[SBHDefaultIconListLayoutProvider currentDeviceScreenType]`.
  The icon info from the root `SBIconListView`'s `layout` `iconImageInfo` (same ABI as T1).
- Pass: P5.

**T3: scenario.** userland/rc.jobs.scale exactly as in section 5.
- Pass: `wc -c` < 8192; `awk 'length > 399' rc.jobs.scale` prints nothing; build.sh copies psgrep and tolog (`ls root/bin`).

**T4: after the run, document.**
- docs/STATUS.md: the scale state, and 1x vs 2x numbers next to lc2/wc5.
- If P7 and P8 pass: replace `fbscale=1` in the documented BASE (HANDOFF NOTE, swipe-profile BASE) with nothing.
- Else: keep `fbscale=1` and note C2 makes 1x consistent.
- Once C2 has run green, delete `cal_fix_start` and the `calfix` parse (C4).
- Pass: the doc diff only.

**T5 (optional, question 6, separate run): chronod log keywords.**
- inject_sb.c `poster_log_reader`: when the program is chronod and `chrono_log=1` is set, use the keyword set of section 6.
- Add chronod to the libredwood-sb INJECT line of a test build only.
- Pass: kept `chronod: ...` lines between 120 and 150 s.

**T6 (optional, only if the P10 shot shows aliasing): `fb_filter=2`.** MiniIOMobileFramebuffer.cpp `drawRows`: a box (area)
average for downscales.
- Pass: the same run with `fb_filter=2` shows D ≤ 5000 µs and a visibly smoother shot.
