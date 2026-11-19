# What the home-screen widget tiles cost during swipes, and how to get it back

Analysis and specification only (2026-11-19). Nothing in this file has been built or run. It uses the kept logs
`logs/lc2`, `swr1`..`swr5`, `sc2` and `sc3`, the sources, and strings from the iOS 16.5 shared cache in `ios165/`.

Tags used below:
- **[V]** checked in a log line, a source line or a shared-cache string (the place is given).
- **[I]** inferred from [V] facts.
- **[G]** general iOS or CoreAnimation knowledge that this tree cannot confirm.

## 0. Short answer

1. **The earlier finding that "the render thread is idle, so the limit is upstream" does not hold for the swipes [V].**
   - Every `bb_prof` window started after, or near the end of, the motion. The windows were lc2 150.7-155.7 s, swr1
     153.0-158.0, swr2 152.7-157.7, swr3 187.8-192.8 and swr5 187.9-192.9.
   - The GPU work of the swipes had already stopped by then. The kernel's `jobs so far` count stops growing at 153.6 s
     (lc2), 153.7 (swr1) and 154.1 (swr2). After 188.66 s, swr3 and swr5 show only 12 display swaps in 12 s.
   - The swipes ended that early because `rc.jobs.swprof` runs **6 swipes, not 40** [V]. `init` cuts every line at 63
     words (`userland/src/init.c:1125`, `argc < 63`). The swipe line is 1734 characters and 40 swipes long; after the
     18 words of its prefix, only 5 whole swipes fit, plus a sixth that loses its `250` and runs at 300 ms.
2. **During the motion, the GL path in backboardd is the limit [I, strongly supported].**
   - In the eagl windows that hold the swipes, GL frames times wall time per frame cover 65-80% of the motion.
   - That wall time equals the CPU of two backboardd threads added together, because one waits for the other. The two
     are the CoreAnimation render thread and Zink's flush thread `backboard:zf` (table in §1.3).
   - With the tiles drawn, GL cost per displayed swap is 7.3 frames x 6.5 ms = 47 ms. Without them it is 7.4 x 5.0 =
     37 ms. The ratio 0.79 predicts 18 x 0.79 = 14 swaps/s, and 13 were measured.
3. **What the tiles add is Vulkan render passes [V]:** 3.21-3.27 passes per GL frame with the tiles against 2.72-2.75
   without (`!MESA: pvr: 10 s: N render passes begun` divided by `!MESA: zink: 10 s: N batches`, same 10-s window).
   - Each extra pass also brings more render-target-dataset churn in the kernel: CREATE_HWRT per pass 0.22 against
     0.155.
   - CPU per GL frame goes up by about 1.2 ms (6.67 against 5.45 ms), split between both threads.
   - The best fit is **one offscreen render target per tile per CoreAnimation frame [I].** In Zink each one costs two
     passes: the offscreen pass, and resuming the main target with a LOAD.
4. **Ruled out [V]: texture re-upload of IOSurfaces.** There are 17-24 uploads (157-256 KiB) per whole swipe phase,
   with or without the tiles.
5. **Unlikely [I]: a per-frame blur or material.** Transfer jobs rise by only about 30 per phase and there are 0 fetch
   draws.
6. **Not shown either way: SpringBoard main-thread stalls at page changes.** There is a hint: more pauses of 90 ms or
   longer (26-29 against 13-20 in a 5-s window), and 10-19 more glFinish calls per phase. The diagnostic run in §4
   separates this from (3).
7. **Cheapest fixes, ranked:**
   - **F1:** rasterize each tile's container while the icon list scrolls (`shouldRasterize`, in SpringBoard).
   - **F2:** enlarge the PowerVR driver's render-target-dataset pool from 12 to 48 entries. This helps every offscreen
     pass, with or without widgets.
   - **F4** (only if the run shows stalls): defer CHUIS visibility and snapshot updates until the scroll ends.

## 1. Question 1: where backboardd's time goes, with the tiles and without

### 1.1 Which runs can be compared [V]

| run | build | swipes start | widget extensions during the swipes |
|---|---|---|---|
| lc2 | before widgets (18 Nov) | 146.5 s | none |
| swr1 | default (widgets) | 148.1 s | **inside the render burst**: `programs ... CalendarWidgetEx[44] 180 ... chronod 11` in 148.3-189.5 s |
| swr2 | `WIDGETS=0` | 148.1 s | none (white placeholder tiles) |
| swr3, swr4 | default | 183.2 s | burst over: no extension in the top 5 (`backboardd 92 SpringBoard 28 chronod 6`) |
| swr5 | `WIDGETS=0` | 183.2 s | none |

- The clean A/B pair is **swr3 against swr5**, both late.
- swr1 against swr2 also carries CPU contention from the extensions' burst. That is why swr1 gives 11 swaps/s and
  swr3 gives 13.

### 1.2 Counters in the window that holds the swipes [V]

Sources:
- eagl lines: `redwood-gl: swprof`, `pvr: shown surfaces`, `threads`, `programs`.
- MESA lines: `render passes begun`, `zink: N batches`, `buffers made past the cache ... xN`.
- Kernel lines: `pvr: stats` and `MiniIOMobileFramebuffer`.

| | lc2 (none) | swr1 (tiles + burst) | swr3 (tiles) | swr5 (none) |
|---|---|---|---|---|
| swaps/s, first 5-s window | 21 | 11 | 13 | 18 |
| intervals: <18 / <26 / <35 / <90 ms, pauses (90 ms or more) | 36/0/24/35, 14 | 10/0/4/19, **26** | 12/1/14/15, **28** | 27/3/18/26, 20 |
| eagl window | 146.6-192 | 148.3-189.5 | 183.3-200.5 | 178.6-188.6 (holds the unlock's tail) |
| GL frames (= zink batches) | 952 | 611 | 669 | 1149 |
| Vulkan passes begun | 2593 | 1962 | 2187 | 3158 |
| **passes per GL frame** | **2.72** | **3.21** | **3.27** | **2.75** |
| draws per GL frame | 5.3 | 6.2 | 6.3 | 5.1 |
| GL ms per frame (wall, first draw to the end of the flush) | 4.4 | 7.3 | 6.5 | 5.0 |
| render thread CPU, ms (`com.apple.co*`) | 2571 | 2487 | 2443 | 3363 |
| Zink flush thread CPU, ms (`backboard:zf`) | 2119 | 1996 | 2018 | 2893 |
| **CPU of both per GL frame, ms** | 4.9 | 7.3 | **6.7** | **5.4** |
| CoreAnimation message thread, ms | 503 | 443 | 431 | 422 |
| render-target datasets made (`buffers ... x`) per GL frame | 0.08 | - | 0.77 | 0.42 |
| kernel CREATE_HWRT / geometry jobs (10-s block) | 78 / 2593 | 294 / 1942 | 442 / 1979 | 304 / 1961 |
| IOSurface texture uploads (whole window) | 18, 1527 KiB | 17, 157 KiB | 18, 171 KiB | 24, 256 KiB |
| glFinish calls | 8 | 29 | 20 | 10 |
| transfer jobs, unlock + swipes (174-204 s) | - | - | 150 | 120 |

Totals for unlock plus the 6 swipes in the late runs (geometry jobs 174-204 s; swaps from the reports at 183.5, 188.6
and 200.6):
- with the tiles (swr3 / swr4): 3223 / 3294 passes, 131 swaps, 958 GL frames;
- without (swr5): 3347 passes, 165 swaps, 1223 GL frames.

So in the same scenario the GPU path does about the same number of passes either way. With the tiles it delivers
them in fewer, heavier frames. That is how a throughput-limited pipeline behaves [I].

### 1.3 Threads [V for the numbers, I for the reading]

- GL wall time per frame comes to about the sum of the two threads' CPU per frame:
  - 4.46 s of CPU against 4.35 s of wall (swr3);
  - 6.26 against 5.75 (swr5);
  - 4.69 against 4.2 (lc2).
- The profile explains why. In every run, 58-66% of the render thread's samples under `my_glFlush` sit in
  `zink_flush -> util_queue_fence_wait`, that is, waiting for the Zink flush thread.
- So a GL frame is the render thread recording, then the flush thread submitting, one after the other.
- The CoreAnimation message thread (commit decoding) is 0.42-0.50 s per window in every run. It is not the difference.
- **Which functions:** the profile of the render thread looks the same with and without the tiles.
  - Top frames, rank for rank: `__mesa_glthread_finish`, `_tc_flush`, `zink_flush`, `util_queue_fence_wait`,
    `tc_sync`, `zink_batch_rp`/`begin_rendering`, `pvr_rogue_CmdBeginRendering`, `pvr_rogue_render_state_setup`,
    `pvr_bo_alloc`, `pvr_usc_eot_kept`.
  - None of the classes of work asked about appears as a new function:
    - no image decode (no ImageIO or CoreGraphics);
    - no upload (`surface_to_texture` / `glTexImage2D` are absent from the top 40 GL frames);
    - no blur shader building (`build_shader` appears only in swr2's profile, a sample of the no-widget run).
  - The extra time is more of the per-pass work that is already there.
- **Caveat:** the profiles cover mostly the end of the motion and the idle after it (§0 point 1), so their absolute
  shares mean little. Use them only for "no new function".

### 1.4 The asked-for items, one by one

| Item | Verdict | Evidence |
|---|---|---|
| CA::Render update/commit decoding | not the difference **[V]** | message thread 0.42-0.50 s per window, with or without the tiles |
| layer-tree traversal | not separable in these logs | QuartzCore is stripped (`(no symbol) +0x51000` pages); the share outside GL is about the same |
| image decode or upload of tile contents every frame | **no [V]** | uploads 17-24 per phase in all runs; `surface_changed` / `surface_to_texture` not in the profiles |
| filter, blur or mask on the tile's material | blur unlikely **[I]**; mask likely **[I]** | blur: 0 fetch draws, and transfer jobs +30 per phase (a backdrop capture each frame would be +1 per tile-frame, hundreds); mask: +0.5 passes per GL frame |
| offscreen passes for rounded-corner masks or shadows | **likely, the main cost [I]** | §1.2 passes per frame; the ~141x141 offscreen is not measured yet (§4) |
| re-upload of IOSurface-backed remote content each frame | **no [V]** | as above; the widget content is a remote layer tree drawn by backboardd itself, not a flattened surface (§2) |
| frame rate limited by SpringBoard's commit rate | **not supported [V/I]** | the "render thread idle" evidence was sampled after the motion (§0); GL path busy 65-80% of the motion; swap ratio matches GL cost ratio |

Evidence that still leaves room for the SpringBoard idea [V]:
- More long gaps (90 ms or more) with the tiles: 26-29 against 13-20.
- 10-19 more glFinish calls per phase. A synchronous snapshot in the render server ends with glFinish [G].
- chronod uses 15-16% of a core during the 18 swipes of sc2 and sc3 (`programs ... chronod[40] 15`), and 0.6 s in
  swr3's 17-s window, against 0.3 s before the swipes.

A slower pipeline also makes more gaps of 90 ms or more, so these do not prove stalls. §4 separates the two.

Unexplained [V, no reading offered]:
- In swr2 (148 s, no widgets) CREATE_HWRT per pass is 0.05. In swr5 (183 s, no widgets) it is 0.155.
- The render-target dataset pool (12 entries, see F2) is the likely place, but which sizes miss is not logged.

## 2. Question 2: how a widget tile is composited on iOS 16.5

**Verified in our logs and the shared cache [V]:**
- The view chain in SpringBoard is `SBHWidgetContainerView` 141x141 holding `CHUISWidgetHostViewControllerView`
  141x141.
  - The host view has no contents of its own and 2 sublayers (`sbw: w2 view ...` lines in wg2, wc4, sc2).
  - The classes `SBHWidgetStackViewController`, `SBHWidgetWrapperViewController` and `SBHWidgetViewController` are in
    SpringBoardHome (`dyld_shared_cache_arm64.21`).
- ChronoUIServices (`dyld_shared_cache_arm64.15`, around offset 18069136) holds the host controller's parts:
  - Scene: `CHUISWidgetScene`, `CHUISWidgetSceneSettings`, `CHUISWidgetSceneClientSettings`,
    `CHUISWidgetVisibilitySettings`, `_FBSScene`, `_scenePresenter` (`<UIScenePresenter>`), `_liveSceneView`.
  - Snapshot views: `_liveSceneSnapshotView`, `_staticCAARSnapshotView`, `_staticImageSnapshotView`,
    `_persistedWidgetSnapshotImageView`.
  - Presentation mode: `_requestedPresentationMode`, `_effectivePresentationMode`, and the messages
    `Effective mode overridden to "static"` and `Live mode can only show a snapshot when the scene does not have content`.
  - Pausing: `contentPaused`, `animationsPaused`.
  - Corners: `_setContinuousCornerRadius:`, `_updateViewsCornerRadius`, `_effectiveCornerRadius`.
  - Material: `_materialView`, `_usesSystemBackgroundMaterial`, `_drawSystemBackgroundMaterialIfNecessary`, and the
    path of MaterialKit.
  - Detaching: `_detachedSceneTimer` ("Detached scene idle timer fired. Invalidating scene.").
- SpringBoardHome's stack controller has the methods `_updatePresentationModeForWidgetViewControllers`,
  `_updatePauseReasonForWidgetViewControllers`, `_updateVisiblySettledForWidgetViewControllers`,
  `_updateShowsSnapshotWhenDeactivatedForWidgetViewControllers` and `_updateWidgetVisibility`.
- There is also `setVisiblySettled:` and `automaticallyUpdatesVisiblySettled`.

**How it works [G, consistent with the names]:**
- Each tile is an FBScene whose client is chronod. chronod renders the widget's archived SwiftUI views
  (`CHKWidgetViewArchive`, `WidgetArchiver`) into the scene's CAContext.
- SpringBoard shows that context through `_UIScenePresenter`, as a **live remote layer tree**: a `CALayerHost` inside
  the host view.
- backboardd draws chronod's layers in place, every frame, like local layers. The backing stores are uploaded once.
  That matches the measured upload counts [V].
- On our port chronod has no Metal device (`redwoodmetal.m:889-898`, widgets-spec §1). So its layers are CPU-drawn
  bitmaps, and they are static while you scroll [I].
- While the page moves, SpringBoardHome changes each tile's visibility, settled state and pause reasons. These go to
  chronod as scene-settings updates.
- The host can switch to a snapshot (live, CAAR or image) in some modes. Making a snapshot of a scene is a
  render-server render into an IOSurface that ends in a glFinish [G].
- Corners: a continuous corner radius with clipping on the host's layers.
- Material: a MaterialKit view drawn only when the widget asks for the system background.
- No shadow names were found in the CHUIS strings. iOS 16 home-screen widgets draw no drop shadow outside edit and drag
  [G].

**What forces extra work in a GL backend like ours [G for CoreAnimation, I for Zink]:**

| Layer feature | What CoreAnimation's GL renderer does | Cost here |
|---|---|---|
| `masksToBounds` + `cornerRadius` (continuous) on a layer **with sublayers**, here the `CALayerHost` | renders the subtree into an offscreen texture, then draws it through a rounded mask. A contents-only layer can be drawn rounded directly | in Zink every mid-frame switch to another framebuffer ends the main pass: offscreen pass + main pass begun again with LOAD = **+2 passes per tile per frame**, each with a render-target dataset (kernel HWRT + free list) unless the pool has one |
| `mask` layer, group opacity < 1 (`allowsGroupOpacity`), shadow without `shadowPath`, filters | offscreen as above | as above |
| backdrop or material blur (`CABackdropLayer`) | copy of the framebuffer (`glCopyTexSubImage2D`, a Vulkan transfer) + 2-3 blur passes | +3 to 5 passes and +1 transfer per tile per frame: **not seen** (transfer +30 per phase) |
| remote content (`CALayerHost`) | no extra pass by itself | its layers' bitmaps are uploaded once: seen as such |
| `shouldRasterize` | one offscreen when the subtree changes, then a cached texture drawn each frame while only its position changes | removes the per-frame offscreen |

**Fit [I]:**
- Measured: +0.5 passes per GL frame x 7.3 GL frames per swap = +3.8 passes per swap.
- That is about 2 per tile per swap, which is exactly "one offscreen per tile per frame".
- A blur would be more than twice that.
- The `WIDGETS=0` placeholder is a plain rounded tile with no live sublayers. It runs at 18-20 swaps/s, so the rounded
  corners alone are not the cost; rounded corners over the remote subtree are.

## 3. Question 3: hypotheses ranked, and the cheapest fixes

### 3.1 Hypotheses

| # | Hypothesis | Status | Predicts: passes per GL frame | upload bytes per frame | busy thread | what recovers it |
|---|---|---|---|---|---|---|
| H1 | **The live tile content is drawn through an offscreen target every frame** (clip or mask over the remote subtree) | top **[I]** | +0.5 (2.75 -> 3.25); offscreen segments about 141x141, about 2 per frame while page 1 shows | about 0 | render + Zink flush threads, kernel HWRT/BO churn | tiles hidden, tiles rasterized, clip removed |
| H1b | the render-target dataset pool (12, `pvr-darwin.patch:904`) misses more often with more offscreen sizes, so each extra pass also creates and destroys an HWRT and a free list (about 1 ms of kernel time) | amplifier **[V numbers / I cause]** | as H1; CREATE_HWRT per pass 0.22 against 0.155 | 0 | Zink flush thread (ioctls) | bigger pool: about -3 to -6 ms per swap, with or without widgets |
| H2 | SpringBoard's main thread stalls at each page change: CHUIS visibility, settled or pause updates to chronod, maybe a synchronous scene snapshot | possible **[I]** | unchanged | 0 | SpringBoard main thread; render thread in glFinish; chronod | not tile hiding; deferring the updates until the scroll ends |
| H3 | CPU contention from the extensions' render burst | **[V] for swr1 only** | unchanged | 0 | the extensions | swipe after about 172 s (swr3/swr4 do) |
| H5 | per-frame material blur behind the tiles | unlikely **[I]** | +3 to 5 per tile-frame | 0 | render thread | - |
| H4 | per-frame re-upload of the tiles' IOSurfaces | **refuted [V]** | - | would be about 80 KB per tile-frame; measured 157-256 KiB per **phase** | - | - |

About H4 and our texture cache [V]:
- `eagl.c` keeps one `struct image` per GL texture name (`image_of`, `eagl.c:730`).
- It refills the texture from its IOSurface only when the seed changed, or, for a texture that is not a target, when a
  content checksum taken once per frame changed (`my_glBindTexture`, `eagl.c:1793-1796`; `surface_changed`,
  `eagl.c:884`).
- The measured upload counts show this works for the tiles. There is nothing to change there.

### 3.2 Candidate fixes

| Fix | For | Where | Size | Risk |
|---|---|---|---|---|
| **F1. Rasterize each tile while the icon list scrolls** | H1 (and H1b, since fewer passes means fewer datasets) | `userland/src/inject_sb.c`; details below | about 80 lines | see below |
| **F2. Render-target dataset pool 12 -> 48, and count its hits** | H1b; also every offscreen without widgets | `tools/mesa/pvr/pvr-darwin.patch:904` and `:1051`; details below | 1 + about 12 lines; Mesa rebuild (`tools/mk-mesa-pvr.sh`) | low: up to 36 more pooled datasets, a free list (about 250 KiB, `free lists 8192 KiB` for 33) and 4 buffers each, about +10 MB of GPU memory at worst; the 10-s expiry stays |
| F3. Clip off (`cornerRadius 0`, `masksToBounds NO`) while scrolling | H1, diagnosis only | same place as F1 | about 20 lines | square corners visible during the scroll on the Weather tile. **Not for the default** |
| F4. Defer CHUIS visibility, settled and pause updates until the scroll ends | H2, **only if §4 shows the stalls** | `inject_sb.c`: wrap `-[SBHWidgetStackViewController _updateVisiblySettledForWidgetViewControllers]` and `_updatePauseReasonForWidgetViewControllers` (names [V], class [G]); while scrolling, mark them pending and run them once at the scroll end | about 50 lines | medium: a widget can stay paused if the end is missed. Needs a timeout (1 s) |
| F5. Pause the remote context while scrolling (`contentPaused`/`animationsPaused`) | H2 only (chronod commits) | - | - | does nothing for H1: the content is static. **Not proposed** |
| F6. Texture caching in eagl | H4 | - | - | not needed (refuted) |

F1 in detail:
- Code goes after `sbw_start` (about `inject_sb.c:1485`). It is started from the boot-args block at
  `inject_sb.c:2645-2657`.
- Wrap these with `method_setImplementation`, calling the original:
  - `-[SBFolderView scrollViewWillBeginDragging:]`: set `shouldRasterize YES` and `rasterizationScale` equal to the
    screen's scale on every `SBHWidgetContainerView` layer.
  - `scrollViewDidEndDecelerating:` and `scrollViewDidEndScrollingAnimation:`: set it back to `NO`.
- The three selectors are in SpringBoardHome [V]. That `SBFolderView` implements them is [G]: check with
  `class_getInstanceMethod` and log it.
- Default on; `wraster=0` turns it off.

F1 risks:
- **[G]** CoreAnimation may not notice changes inside a hosted remote context under a rasterized layer, which would
  show stale content. Gating on scrolling limits this to the scroll itself.
- **[G]** CoreAnimation's GL renderer may not cache when the subtree holds a `CALayerHost`. Set C of §4 tests that.
- Cost: about +80 KB of texture per tile.

F2 in detail:
- Change `#define PVR_RT_POOL 12` to 48.
- Add pool takes and misses to the existing 10-s line in `pvr_rstates_say`.

**Order:**
1. F2 first. It is independent, cheap, and helps everything; on its own, expect +5-10%.
2. Then F1, which removes most of the remaining widget cost if set C passes.
3. F4 only on the evidence of §4.

## 4. Question 4: one silent diagnostic run that separates the hypotheses ("wt1")

### 4.1 Idea

One boot, after the extensions' burst (it ends about 171 s, swr3/wc runs). Five sets of the same 6 swipes, about 7 s
apart. Before each set, SpringBoard changes the tiles on a notification:

| set | mode | what it isolates |
|---|---|---|
| A | `base`: live, as shipped | reference |
| B | `hide`: `opacity 0` on each `SBHWidgetContainerView` layer (CoreAnimation skips it; UIKit and CHUIS do not know, so visibility updates and chronod go on) | rendering cost (H1) against everything else (H2) |
| C | `raster`: `shouldRasterize` on the same layers | the F1 candidate |
| D | `noclip`: `cornerRadius 0`, `masksToBounds NO` on the host view and its local sublayers | whether the clip is the offscreen trigger |
| E | `base` again | drift; the burst's tail |

**Measuring swaps per set without the kernel's 5-s windows:**
- The kernel's report fires at the first swap at least 5 s after the last one, so its windows cannot be lined up with
  7-s sets.
- So backboardd counts **display updates**: a draw into a display-sized surface whose surface differs from the last
  one. CoreAnimation alternates the display buffers (`swap 5: surface 20`, `swap 6: surface 4`, swr2 [V]), so each
  change is one update.
- Check: over 178-220 s, the per-set totals must match the sum of the kernel's `N frames in` lines within 5%.

### 4.2 Build (userland and payload only; main session runs the device)

```
WIDGETS=1 scratchpad/ubuild-d.sh "" rc.jobs.wtile
XL_RAMDISK=userland/root.apfs ./xl_x6826b build
```

Before the run, check two things:
- `root/LaunchDaemons/com.apple.chronod.plist` exists;
- `root/etc/rc.jobs` is the wtile file.

### 4.3 Boot-args and run

`tools/r1-run.sh wt1 230 "<args>"`. The test switches come first. In sc2 the runner's prefix took bytes 1-96, so
`wtile=1` lands at about byte 97 and `swprof=1` at about 105, well inside the first 256 bytes.

```
wtile=1 swprof=1 nopill=1 fbscale=1 mtkaudio=2 r1cam=4 pvr=6 pvr_gl=1 fbaspect=1 glwait=3 glthreads=1 cam_fps=0 r1_clk=1 r1_cpufreq=2200 r1_gov=1 r1_gpu=680 glyuv=1 pvr_stats=1 loader_return_after=228
```

Leave these out:
- `bb_prof`: it suspends the render thread 2,800 times a second.
- `fbshot*`, `proc_sample`, `lsdiag`: they fill the console ring that holds the kernel's frame reports.

### 4.4 Scenario `userland/rc.jobs.wtile`

About 2.4 KB. Every line is under 400 characters and at most 53 words, against the cap of 63 (`init.c:1125`). The
timings assume rwtouch starts at about 22 s (swr3: `wait 155` gave the unlock at 177 s). Silent: one status-bar tap,
the unlock notification, page swipes; no app, no camera, no sound.

```
# Widget swipe cost (docs/widget-swipe-cost.md section 4). wtile=1 FIRST in the boot-args. Silent: one tap, page swipes, no sound.
/redwood/bin/rwtouch ifarg wtile=1 say wstart wait 150 tap 0.5 0.06 wait 1 notify com.redwood.unlock wait 4 notify com.redwood.wtile.base wait 1
/redwood/bin/rwtouch ifarg wtile=1 say setA swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250
/redwood/bin/rwtouch ifarg wtile=1 wait 2.5 notify com.redwood.wtile.hide wait 0.8
/redwood/bin/rwtouch ifarg wtile=1 say setB swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250
/redwood/bin/rwtouch ifarg wtile=1 wait 2.5 notify com.redwood.wtile.raster wait 0.8
/redwood/bin/rwtouch ifarg wtile=1 say setC swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250
/redwood/bin/rwtouch ifarg wtile=1 wait 2.5 notify com.redwood.wtile.noclip wait 0.8
/redwood/bin/rwtouch ifarg wtile=1 say setD swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250
/redwood/bin/rwtouch ifarg wtile=1 wait 2.5 notify com.redwood.wtile.base wait 0.8
/redwood/bin/rwtouch ifarg wtile=1 say setE swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250 wait 0.35 swipe 0.85 0.45 0.15 0.45 250 wait 0.35 swipe 0.15 0.45 0.85 0.45 250
/redwood/bin/rwtouch ifarg wtile=1 wait 2.5 notify com.redwood.wtile.end say end
```

Timeline: unlock at about 173 s; sets at about 178, 185, 192, 199 and 206; `end` at about 213 s. Every set ends on
page 1, where it started.

### 4.5 Log lines to add (all kept, under 200 characters, only with `wtile=1`)

**backboardd** (`eagl.c`). On each `com.redwood.wtile.*` notification, print what happened since the previous one.
"Since" means the toggle; the first update at least 400 ms after the toggle opens a 5.0-s window, which matches the
kernel's figure.

```
redwood-gl: wset <prev> u5 U5 (U5/5 a second); updates U, intervals <18 a <26 b <35 c <90 d, pauses e, longest L ms
redwood-gl: wset <prev> gl F frames P passes (P/F) O offscreen; top WxH:n WxH:n WxH:n; draws D, glFinish K, uploads N KiB
redwood-gl: wset <prev> ms: frames M (M/F each), render R, zf Z, ca-msg C
redwood-gl: wset <prev> programs: <top 5 processes, ms of CPU since the last set>
```

Definitions:
- Passes are framebuffer segments with at least one draw or clear: a new segment at each `glBindFramebuffer` to
  another framebuffer.
- Offscreen means a segment whose target is not an IOSurface image. Its size comes from a new hand-written
  `my_glViewport` (no `glGet`, so no glthread sync; `mkeagl.py` picks it up by its name).
- Calibrate P against the MESA `render passes begun` lines over the whole run.

**SpringBoard** (`inject_sb.c`):

```
sbw: wtile <mode> on N containers at T ms (raster r, opacity o, radius x)                 (each notification)
sbw: wset <prev> main loop: T turns, >16 a, >33 b, >90 c, longest L ms, busy B ms           (since the previous)
sbw: wset <prev> chuis: vis V (Vus), settled S, pause P, mode M, snap N (Nus)               (calls and time)
sbw: tile L<i> <class> WxH r<cornerRadius> <curve> clip<0|1> mask<0|1> sh<opacity> op<o> grp<0|1> f<n> bf<n> cf<0|1> ras<0|1> host<ctx> backdrop<0|1>   (once, at the first notification, at most 24 lines: the subtree of each SBHWidgetContainerView)
```

The `chuis:` counters come from wrappers that call the original:
- On `CHUISWidgetHostViewController`: `_modifyVisibilitySettings:`, `_setEffectivePresentationMode:`,
  `_createWidgetSnapshotViews`, `_applyLiveSnapshotContentsFromSnapshot:`.
- On `SBHWidgetStackViewController`: `_updateVisiblySettledForWidgetViewControllers`,
  `_updatePauseReasonForWidgetViewControllers`, `_updatePresentationModeForWidgetViewControllers`.
- Wrap a method only if its type encoding is one of `v16@0:8`, `v24@0:8@16` or `v24@0:8q16`. Otherwise print
  `sbw: chuis <sel> not wrapped (<encoding>)`.

**Already there:**
- kernel `MiniIOMobileFramebuffer: N frames in`, `from one frame to the next`;
- `pvr: stats: requests ... CREATE_HWRT` (10-s blocks);
- `!MESA: pvr: 10 s: N render passes begun`, `!MESA: zink: 10 s: N batches`, `!rwpvr: 10 s`.

### 4.6 How to read wt1 (the decision table)

"Swaps" below is `U5/5` from the `wset ... u5` line.

| Result | Meaning | Next |
|---|---|---|
| A and E within 1 swap/s of each other, and A ≤ 14 | the reference holds | - |
| B ≥ 17 and B's P/F ≤ 2.85 | **H1: rendering of the tiles is the cost** | look at C and D |
| B ≤ 14 and B's `>90` count ≥ 1 per swipe (≥ 6 per set), with `chuis` vis/settled/snap calls in the same sets | **H2** | F4 |
| A's top offscreen sizes include about 141x141 with n ≥ 1.5 per frame, gone in B | H1's offscreen is the tile | - |
| also gone in D | the clip is the trigger | - |
| C ≥ 17 and C's 141x141 n about 0 per frame | **F1 works** | build F1 (T5) |
| C ≈ A | CoreAnimation does not cache this subtree | F2, then think about F3-like trades |
| `sbw: tile` shows `backdrop1` or `bf>0` on a tile layer | H5 is real | its passes appear as 3-5 offscreen segments per frame |
| the per-set totals of U against the kernel's `N frames` over 178-220 s differ by more than 5% | the update count is not a swap count | use the kernel's lines alone |

**Pass for the diagnostic itself:**
- five `wset` triplets (A-E) and the `sbw: tile` dump;
- `XNU returned ... reason 0`;
- 0 x `AT> AT+EFUN=1`;
- the device back in Android.

## 5. Pass criteria for the fix

Target: at least 18 swaps per second with both tiles drawn.

Run the reference swipe scenario at the late timing (swr3's: unlock at about 177 s, 6 swipes from 183 s) with the
default build plus the fix, and `bb_prof` off. It passes when the kept log shows:

1. `MiniIOMobileFramebuffer: N frames in ~5000 ms (≥18 a second)` for the window opened by the first swipe swap
   (about 188.6 s), in two runs.
2. Tiles drawn and live:
   - `lsd: widgets: 27 of 27 widget extensions hidden`;
   - `programs ...` lists `chronod`;
   - a picture after the scroll shows the Weather and Calendar content, not white.
3. In the 10-s window that holds the swipes, `!MESA: pvr: 10 s: P render passes begun` divided by
   `!MESA: zink: 10 s: B batches submitted` is ≤ 2.9.
4. `redwood-gl: swprof: ... X ms a frame` is ≤ 5.3.
5. With F2: kernel `CREATE_HWRT` in the block that holds the swipes is ≤ 150 (now 442), and the new pool line shows
   takes greater than misses.
6. Regression (`rc.jobs.homechk`):
   - `swap #2 ... at` 111 ± 3 s;
   - `settings launched by bundle id: 1`;
   - `camera launched by bundle id: 1`;
   - `reason 0`.

## 6. Implementation tasks (Sonnet-sized; device runs are the main session's)

1. **T1. Scenario.**
   - Create `userland/rc.jobs.wtile` exactly as in §4.4.
   - Pass: `wc -c` < 8192; `awk '{print NF}'` ≤ 62 on every line; longest line < 400 characters; `build.sh:520` copies
     it as `root/etc/rc.jobs` with `RC_JOBS=rc.jobs.wtile`.
2. **T2. backboardd counters** (`userland/src/eagl.c`).
   - Parse `wtile=1` next to `swprof_on` (`eagl.c:4849`).
   - Add the hand-written `my_glViewport` (stores w and h in `struct eagl`, passes the call on).
   - In `my_glBindFramebuffer` (`eagl.c:4182`): count segments.
   - In `drawing()` (`eagl.c:1715`):
     - mark the segment drawn;
     - for an offscreen target, add the viewport size to a 16-entry size table;
     - for a display-sized target whose `image->surface` differs from the last, count an update and put its interval
       in the buckets (<18, <26, <35, <90, pause, longest).
   - Keep monotonic totals for:
     - frames, `frame_us` and draws (in `frame_end`, `eagl.c:1448`);
     - glFinish (`my_glFinish`, `eagl.c:3771`);
     - uploads (in `surface_to_texture`).
   - Register `com.redwood.wtile.base/.hide/.raster/.noclip/.end` with `notify_register_dispatch` on a serial queue.
     The handler prints the four `wset` lines of §4.5 from the deltas.
   - Thread CPU: give `threads_say`/`programs_say` (`eagl.c:1375`, `:1323`) a caller-owned baseline, so the 10-s lines
     and the set lines do not reset each other.
   - Pass: builds; without `wtile=1` no notification is registered and no `wset` line appears.
3. **T3. SpringBoard modes and lines** (`userland/src/inject_sb.c`).
   - Add `wtile_start()` after `sbw_start` (about `:1485`), called from the boot-args block at `:2645-2657` when
     `wtile=1`.
   - Find containers with the window walk of `sbw_views` (`:1238`), matching the class `SBHWidgetContainerView`.
   - Modes:
     - `base`: restore the saved values;
     - `hide`: `opacity 0`;
     - `raster`: `shouldRasterize YES`, `rasterizationScale` = UIScreen scale;
     - `noclip`: `cornerRadius 0`, `masksToBounds NO` on the CHUIS host view's layer and its local sublayers.
     Save the values first.
   - Print the `sbw: tile` dump once. Reuse `layer_line`'s walk (`:692`); add cornerRadius, cornerCurve,
     masksToBounds, shadowOpacity and allowsGroupOpacity; keep it to 24 lines.
   - Main-loop histogram: observers as in `watch_main_loop` (`:124`), but counting instead of printing.
   - The `chuis` wrappers of §4.5, checked by type encoding.
   - Pass: builds; nothing changes without `wtile=1`.
4. **T4. Diagnostic build.**
   - Build with `WIDGETS=1 scratchpad/ubuild-d.sh "" rc.jobs.wtile`, then the payload.
   - Check the chronod plist, `root/etc/rc.jobs`, and that the chain image fits 32 MB.
   - The main session runs wt1 (§4.3) and reads §4.6.
5. **T5. F1, only if set C passes.**
   - In `inject_sb.c`, wrap `scrollViewWillBeginDragging:`, `scrollViewDidEndDecelerating:` and
     `scrollViewDidEndScrollingAnimation:` on `SBFolderView`. Log `sbw: raster hook <class> <sel> found <0|1>` once
     each.
   - Rasterize the `SBHWidgetContainerView` layers between scroll begin and scroll end, plus a 1-s timeout that turns
     it off.
   - Default on; `wraster=0` turns it off.
   - Pass: the §5 lines 1-4 and 6, and the picture taken after the scroll shows the live tiles.
6. **T6. F2.**
   - In `tools/mesa/pvr/pvr-darwin.patch:904`, change `PVR_RT_POOL` to 48.
   - Count takes and misses in `pvr_rt_dataset_pool_take` and add them to the 10-s line of `pvr_rstates_say`
     (`:1051`): `...; rt pool: H taken, M made`.
   - Rebuild with `tools/mk-mesa-pvr.sh`.
   - Pass: the §5 line 5 and no new `GPU resets`/`faults`. Measure swipes with and without widgets (expect +5-10%).
7. **T7. Docs.**
   - The comment in `rc.jobs.swprof` says 40 swipes; 6 run (`init.c:1125`). Rewrite that line with the 6 swipes it
     really runs, so the file says what happens.
   - Correct `docs/swipe-profile.md` §6 ("the swipes themselves last 24 s"): it is about 5 s.
   - Correct the handoff's "render thread idle, limit upstream" (§0 point 1 of this file).
   - Results go to `docs/STATUS.md` and the newest HANDOFF. This file stays uncommitted.
