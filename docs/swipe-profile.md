# Where home-screen swipe time goes (r1, Zink over the PowerVR Vulkan driver)

Measurement only, 2026-11-18; nothing was optimised. Five device runs, all with `fbscale=1 r1_clk=1 r1_cpufreq=2200 r1_gov=1 r1_gpu=680
glyuv=1 cam_fps=0 nopill=1` (governor on, 4 cores), image built with `RC_JOBS=rc.jobs.swprof` and the full INJECT list.
Logs: `logs/pb1` (pass benchmark), `sp1` (swipes started too early, unusable), `sp2`, `sp3` (swipes, default), `sp4` (swipes, `glfetch=0`).
Read with `python3 tools/swprof.py NAME`.

## What was built

| Tool | Where | What it does |
|---|---|---|
| Pass benchmark | `userland/src/pass_bench.c` (`pass_bench ifarg passbench=` in `rc.jobs.swprof`) | N=200 passes per case on a 375x500 RGBA8 FBO: clear / load / dontcare x store / discard, with 1 and 8 draws; app-thread CPU, rest-of-process CPU (Zink driver thread + Vulkan driver), issue wall, all-done wall; fetch cases (see 5) |
| Render-thread profiler | `userland/src/inject.c`, boot-args `bb_prof=SECS bb_prof_all=1` | samples the two `com.apple.coreanimation` threads of backboardd in every run state (running / waiting) at about 2.3 kHz (24000-sample cap = 5 s), then prints, as kept lines, run state counts, top leaf and inclusive symbols (dladdr) and the GL stack's own frames by exact address; `tools/swprof.py` names those from the unstripped Mesa libs with `nm` |
| Counters | `userland/src/eagl.c`, boot-arg `swprof=1` | per eagl window: frames (GL flush chunks), display frames, glFlush, glFinish, draws, clears, fetch draws, fetch programs made |
| Kernel counters | existing `pvr_stats=1` | jobs and per-request counts and microseconds per ten seconds |
| Scenario | `userland/rc.jobs.swprof` | unlock, 40 home-screen page swipes both ways (wait 0.35); `r1-auto.sh` knows `swprof=1` and `passbench=` |

Old `bb_prof` only sampled RUNNING threads: in `prof2` that gave 59 samples. Sampling every state is what shows that the render
thread is idle most of the time.

## 1. Cost of one render pass (pb1, no swipes, 375x500)

CPU ms per pass. "driver" = process CPU minus the app thread (Zink's driver thread, the Vulkan driver, kernel calls). Wall is per pass for N=200.

| case | app CPU | driver CPU | issue wall | all done wall | geometry / fragment jobs per pass |
|---|---|---|---|---|---|
| clear / store, 1 draw | 0.05 | 3.27 | 1.75 | 2.02 | 1 / 2 |
| clear / discard, 1 draw | 0.02 | 2.78 | 1.30 | 1.41 | 1 / 2 |
| load / store, 1 draw | 0.02 | 3.40 | 1.87 | 2.01 | 1 / 2 |
| load / discard, 1 draw | 0.01 | 1.38 | 0.97 | 1.08 | 1 / 2 |
| dontcare / store, 1 draw | 0.01 | 1.28 | 0.96 | 1.07 | 1 / 2 |
| dontcare / discard, 1 draw | 0.02 | 1.61 | 1.20 | 1.31 | 1 / 2 |
| clear / store, 8 draws | 0.02 | 2.88 | 1.38 | 1.49 | 1 / 2 |
| clear / discard, 8 draws | 0.02 | 2.89 | 1.34 | 1.45 | 1 / 2 |
| load / store, 8 draws | 0.02 | 3.45 | 1.89 | 2.05 | 1 / 2 |
| load / discard, 8 draws | 0.02 | 1.53 | 1.07 | 1.19 | 1 / 2 |
| dontcare / store, 8 draws | 0.02 | 1.51 | 1.16 | 1.27 | 1 / 2 |
| dontcare / discard, 8 draws | 0.02 | 1.66 | 1.28 | 1.38 | 1 / 2 |
| load / store, 2 draws and 8 draws (plain) | 0.02 | 3.38 / 3.39 | 1.89 / 1.84 | 2.03 / 2.00 | (job lines lost from the log tail) |

Syncobj requests per pass, every case: SYNCOBJ_TRANSFER 25, SYNCOBJ_RESET 12, SYNCOBJ_WAIT 2-3 (kernel time in them 14-26 us per pass in all).
GPU time: the all-done wall (1.1-2.0 ms/pass) is the ceiling; the app thread is never the limit (0.02 ms).

Readings:
- A pass costs 1.3-3.4 ms of driver CPU and almost nothing else. Draws do not matter (1 vs 8: no difference).
- **LOAD and CLEAR cost about 2 ms more CPU than DONT_CARE, independent of what is stored.** load/store 3.4 vs dontcare/store 1.28; clear/store 3.27.
  Discarding the store alone saves 0 (clear 3.27 -> 2.78) to 2 ms (load 3.40 -> 1.38; with clear the load-op shader is still made).
  The profile in 3 shows why: the driver builds a load-op shader at the start of the pass.
- One Vulkan pass is always 1 geometry + 2 fragment jobs, one SUBMIT_JOBS.

## 2. Swipe phase numbers (sp2, sp3 default; sp4 `glfetch=0`)

The swipes run about 9 s of wall time (log 146.5 s to about 155 s); all job counts are inside one kernel stats window (143.6-153.6 s).

| | sp2 (default) | sp3 (default) | sp4 (`glfetch=0`) |
|---|---|---|---|
| GL frames ("chunks", glFlush ends) in the phase | 462 | (line not printed) | 472 |
| display frames of them | 405 | - | 415 |
| glFlush calls / glFinish calls | 462 / 12 | - | 472 / 18 |
| draws per frame / clears per frame | 5.3 / 0.85 | - | 5.3 / 0.85 |
| **fetch draws, fetch programs made** | **0, 0** | 0 | **0, 0** |
| geometry jobs = render passes in the window | 1273 | 1290 | 1299 |
| **render passes per GL frame** | **2.76** | - | **2.75** |
| fragment jobs per frame | 5.5 | - | 5.5 |
| SUBMIT_JOBS per frame (per pass) | 2.9 (1.06) | - | 2.9 (1.07) |
| SYNCOBJ_TRANSFER per frame (per pass) | 45.0 (16.3) | - | 45.2 (16.4) |
| SYNCOBJ_RESET per frame (per pass) | 22.6 (8.2) | - | 22.7 (8.2) |
| SYNCOBJ_WAIT per frame | 1.9 | - | 1.9 |
| GL frame time in eagl (ms) / of it in glFlush | 11.0 / 9.6 | - | 10.8 / 9.3 |
| display swaps per second (5 s window) | 9 | - | 10 |

Display swaps are about 5.3 GL frames each, so **a swipe swap is about 14.6 passes, about 58 ms of render thread time, every 100 ms**.
Kernel time in the window: SUBMIT_JOBS 56 ms (41 us a call), CREATE_BO 58 ms (645 calls past the BO cache), DESTROY_HWRT 40 ms (111),
DESTROY_FREE_LIST 28 ms, CREATE_FREE_LIST 20 ms, SYNCOBJ_WAIT 32 ms (it blocks), SYNCOBJ_TRANSFER 3.0 ms and SYNCOBJ_RESET 1.6 ms for about 31000 calls.
Idle governor: 85-90% of the time at the high clock; the GPU's fragment work is about 1.6 ms (earlier figure), not the limit.

glfetch A/B (sp2/sp3 vs sp4): identical to within noise (2.76 vs 2.75 passes per frame, 11.0 vs 10.8 ms per frame). The hypothesis is not what happens in
the swipes: **no draw in the home-screen swipe uses framebuffer fetch** (counted per program: both CoreAnimation's `gl_LastFragData` shaders and our
vibrant program), and Mesa does not even list `GL_EXT_shader_framebuffer_fetch` for the ES 3 context in pass_bench ("NOT in the extension list" in pb1; the fetch
program's own compile was not tried in that run). The cases that would test Zink splitting a pass around a fetch draw exist in `pass_bench`
(`passbench=2` runs only those) but were not run: the five-run limit went to the swipe runs.

## 3. Top 10 CPU frames of the render thread (sp3; sp4 within 5%)

The thread (`com.apple.coreanimation` #2, the GL thread; the first one is the message loop and is 99% in `mach_msg`), 5.2 s of samples taken while swiping:

- samples 12000: **running 2412 (20%)**, waiting 9588 (80%). Of the waiting 9090 are `mach_msg2_trap` (the run loop idle between frames), 582 are
  `util_queue_fence_wait` (waiting for Zink's driver/queue thread). sp4: running 2589, waiting 9402.
- The percentages below are of the 2412 running samples (sp4 in brackets, of 2589). Inclusive counts, exact return addresses summed per function (frame-pointer
  stacks can lose a frame, so a child can exceed its parent by a few percent).

| # | function | share of running | what it is |
|---|---|---|---|
| 1 | `my_glFlush` (libredwood-gl) -> `_mesa_glthread_finish`, `_tc_flush`, `_glthread_unmarshal_batch` | 82% (84%) | our hook; the flush runs the queued GL calls on this thread |
| 2 | `zink_draw` (two template variants) -> `zink_batch_rp` -> `begin_rendering` | 58% (52%) | Zink starts the render pass at the first draw |
| 3 | `pvr_rogue_CmdBeginRendering` | 66% (60%) | the PowerVR driver's begin of a pass |
| 4 | `pvr_rogue_mrt_load_ops_setup` -> `pvr_rogue_load_op_shader_generate` -> `pvr_uscgen_loadop` | 46% (42%) | **builds a load-op (clear/load) shader for every pass** |
| 5 | `build_shader` (NIR builder under it) | 47% (47%) | compiles that shader's NIR |
| 6 | `ralloc_size`, `gc_alloc_size`, `gc_zalloc_size`, `unsafe_free` (the compiler's allocator) | 21%, 9%, 9%, 7% | compiler memory churn |
| 7 | `pco_nir_opt`, `pco_preprocess_nir`, `pco_lower_nir`, `pco_process_ir` | 14%, 13%, 6%, 7% | the compiler passes of that shader |
| 8 | `pvr_rogue_render_state_setup`, `pvr_rogue_spm_init_eot_state`, `pvr_usc_eot` | 18%, 18%, 16% | **end-of-tile state and shader, also made per pass** |
| 9 | `my_finish_update` (libredwood) | 32% | CoreAnimation's end of update (calls glFlush) |
| 10 | `zink_flush_clears`, `pvr_rogue_CmdEndRendering` | 14%, 12% | clears and the end of the pass |

So **at least 46% and probably 60% or more of the GL thread's running time is the PowerVR driver generating load-op and end-of-tile
shaders per pass**, matching pb1 (LOAD or CLEAR: +2 ms of 3.4 ms per pass; DONT_CARE: 1.3 ms). Draw recording itself (`zink_draw` minus begin) is the small part.
Not sampled: Zink's own queue thread (where `vkQueueSubmit` runs; 582 samples of fence wait show it is waited for sometimes); a third thread's CPU is in the 20% share
of "rest of process" in pb1.

Where the time goes, per swap (100 ms): render thread busy about 58 ms (11 ms x 5.3 frames), of that about 9.3 of every 11 ms in glFlush; the thread's other 42%
of the time is `mach_msg` wait for the next CoreAnimation update. SpringBoard's main loop (earlier `tl: a` runs, 7.7/25.6 ms) is the likely source.

## 4. Ranked: what would help most

Everything below is an estimate from these numbers; none was tried. "Frame" = one GL chunk (11 ms now, 9.6 of it glFlush); a swap is 5.3 of them.

| Rank | Change | Expected gain | Why |
|---|---|---|---|
| 1 | **Cache the load-op and end-of-tile shaders in the pvr driver** (key by format/op/sample count; `pvr_rogue_load_op_shader_generate`, `pvr_uscgen_loadop`, `pvr_usc_eot`, render-state/SPM-EOT setup), instead of regenerating them in `CmdBeginRendering` | **-45% to -65% of render-thread CPU**: a pass 3.3 -> about 1.3 ms; a frame's glFlush 9.6 -> about 4 ms; per swap 58 -> about 25-30 ms. The swap interval (100 ms) will shrink only as far as the render thread is the limit; say 10 -> 13-16 fps | profile 3 (46-47% in the load-op path, 18% in EOT state) and pb1 (+2 ms for LOAD/CLEAR). Same key every pass: the cache hit rate should be ~100% |
| 2 | Fewer passes per frame: where CoreAnimation's 2.75 passes per chunk (14.6 a swap) can merge (flush less often between layers, `glflush_mid` told us it does not help alone: cost is per pass, not per flush), or fewer layers (Reduce Transparency: -30..40% measured earlier) | -1 pass is -1.3..3.4 ms; Reduce Transparency's 30-40% is the known size | cost follows passes (pb1: draws free, passes not) |
| 3 | `glInvalidateFramebuffer` where CoreAnimation leaves content dead (dontcare load, discard store) | up to -2 ms per pass where it applies (pb1), but it needs the content to really be dead; only a share of the passes | pb1 dontcare/discard 1.3-1.6 vs load/store 3.4 |
| 4 | Remove the Vulkan-side idle: the 582 fence-wait samples (5% of the thread's samples) and `CREATE_BO` 58 ms / `DESTROY_HWRT` 40 ms / free-list churn (about 170 ms of kernel time per swipe phase of 9 s) | -3% to -8% | BO cache already recycles flags-0 buffers; the HWRT/free-list create-destroy per pass could be pooled |
| 5 | **Fence batching (open-mapple 1.6)** | **under 2%** | about 31000 SYNCOBJ_TRANSFER/RESET calls took 4.6 ms of kernel time in the phase (0.15 us each) and about 70 calls a frame at a few us of trap cost is under 1% of an 11 ms frame; the earlier "6% of a core" is not borne out. SYNCOBJ_WAIT (32 ms) is real blocking, not call overhead |
| 6 | `glfetch=0` / avoiding framebuffer fetch | **none** | 0 fetch draws in the swipes, same passes per frame with it hidden |
| 7 | Upstream: why the render thread idles 42% of each swap (SpringBoard / CA commit pacing at 30 Hz) | unknown; only matters after 1 | `mach_msg` wait 76% of samples; do 1 first and re-measure, then sample SpringBoard's main thread |

Caveats: the profiler's 5.2 s window is the 24000-sample cap, so it covers about the second half of the swipes; the sampling itself suspends the thread
2300 times a second (its effect on frame times was not measured separately; all swipe runs carried it). Names come from `nm` of the unstripped build in
`xnu-deps/build/pvr-mesa`, assumed to match the libraries flashed in /fw/pvr (symbol names agree with the code path, e.g. `pvr_rogue_CmdBeginRendering`).
`dladdr` itself names only exported symbols, so the in-log symbol lines (`bbprof leaf/incl`) are rough: use the "GL stack frames" table of `tools/swprof.py`.
Swipe runs: only the first of two 10 s eagl windows is kept in sp3, so its frame count is missing; sp2 and sp4 carry the counters.

## 5. Reproducing

```
cd userland && RC_JOBS=rc.jobs.swprof <the full build command of HANDOFF-2026-11-16.md NOTE> ./build.sh   # and the payload step
tools/r1-auto.sh pb1 "<BASE> passbench=1" 400            # pass benchmark (about 11 minutes with the 100 s settle and 11 s gaps)
SHOTS= tools/r1-auto.sh sp2 "<BASE> swprof=1 bb_prof=115 bb_prof_all=1" 180          # add glfetch=0 for the A/B
python3 tools/swprof.py pb1 sp2 sp3 sp4
```
BASE = `fbscale=1 r1_clk=1 r1_cpufreq=2200 r1_gov=1 r1_gpu=680 glyuv=1 cam_fps=0 nopill=1` (r1-auto adds its own base). `bb_prof` counts from the window server's start
(about log second 35.6): 115 puts the 5 s of samples at log 150-156, inside the swipes that begin at 146.5. The swipes were over after about 9 s
in every run (the 40-swipe line is more than the phase needs).

## 6. Load-op cache (2026-11-18)

Change 1 of the ranking in 4, done: the PowerVR Vulkan driver keeps what it generates for a pass. Patch `tools/mesa/pvr/pvr-zz-loadop-cache.patch` (applied last by
`tools/mk-mesa-pvr.sh`; the changed files are `pvr_arch_mrt.c`, `pvr_mrt.h`, `pvr_device.h`, `pvr_arch_device.c`, `pvr_usc.c`). No upstream design to backport: Mesa main has no load-op
cache for dynamic rendering (checked the same files upstream, nothing; the load op is made in `pvr_mrt_load_op_init` and freed with the command buffer).

- **Load-op shader.** `pvr_mrt_load_op_init` asks `pvr_load_op_shader_get_cached` instead of `pvr_arch_load_op_shader_generate`. Device-level cache, `simple_mtx` around lookup and
  generate, at most 256 entries (past it: made the old way and one "cache full" count in the 10 s line), freed at `vkDestroyDevice` (`pvr_arch_loadop_cache_finish`).
  What is kept is everything the generator produces: the USC program BO, the two PDS programs (they depend on the shader's entry offset, temps and msaa) and the four counts (`temps`, `const_shareds`,
  `shareds`, `num_tile_buffers`). A load op that uses cached programs has `progs_cached` set, so `pvr_load_op_fini` does not free them.
  **Key = what `pvr_uscgen_loadop` reads**: rt_clear_mask, rt_load_mask, rt_2d_view_3d_mask, unresolved_msaa_mask, depth_clear_to_reg (a load op that clears depth to a register is not cached), and per
  render target of clear|load the destination VkFormat plus its MRT resource (type, output register or tile buffer index, tile buffer offset). The per-pass data (clear colours, texture states, tile buffer
  addresses) are not in the shader: they go in the shared registers at submit time as before.
- **End-of-tile shader.** Already kept by an earlier patch (`pvr_usc_eot_kept`, key = the PBE state words, which hold surface addresses) but at 32 entries for good: in the swipes 38% of the lookups missed
  (1478 of 3870 in lc1) and the compiler ran for each. Limit now 1024 with a hash in front of the compare; the swipes make about 95 kinds (417 in the whole autotest + Settings scenario). The "per pass EOT state" is not
  cachable beyond the shader: the PDS data segment, the PBE words and the BO uploads depend on the framebuffer's addresses and are cheap (the compiler was the cost).
- **Kill switch.** Boot-arg `pvr_loadop_cache=0` (userland/src/eagl.c sets env `PVR_LOADOP_CACHE=0` in the Zink process; `glenv=PVR_LOADOP_CACHE=0` does the same): load-op cache off AND the EOT limit back to 32, i.e. the old behaviour, for A/B.
- **Log line** every 10 s per process: `!MESA: pvr: load-op cache: N entries, H hits, M misses, F uncached (cache full)` and `!MESA: pvr: eot kept: N programs, H hits, M misses, P past the limit`.

### Numbers (same BASE as before: `fbscale=1 r1_clk=1 r1_cpufreq=2200 r1_gov=1 r1_gpu=680 glyuv=1 cam_fps=0 nopill=1`, governor on, 4 cores)

Swipe scenario, `swprof=1 bb_prof=115 bb_prof_all=1`. lc3 = cache off (`pvr_loadop_cache=0`), lc1 = load-op cache on with the EOT limit still 32, lc2 = both on (final).
Logs `logs/lc1`, `lc2`, `lc3` (and sp2/sp3 of section 2 for the old build: same as lc3).

| | lc3 off (= sp2/sp3) | lc1 load-op only | lc2 both |
|---|---|---|---|
| GL frame time in eagl, ms | 10.8 | 6.4 | **4.4** |
| of it in glFlush, ms | 9.4 | 5.2 | **3.3** |
| display swaps per second (5 s window) | 9 | 15 | **21** |
| GL frames in the log window | 471 | 705 | 952 |
| render thread running samples (of 12000) | 2458 (20%) | 2016 | **1061 (9%)** |
| `pvr_rogue_CmdBeginRendering` inclusive samples | 1509 (61% of running) | 865 | 65 (6%) |
| `pvr_rogue_load_op_shader_generate` | 1001 (41%) | 0 | 0 |
| `pvr_usc_eot` (compiler) | 532 (22%) | 741 | 0 |
| cache lines | - | load-op 3 entries, 1935 hits, 0 misses; EOT 1478 of 3870 missed | load-op 3 entries, 2593 hits (miss only at first use); EOT 95 programs, 5134 hits, 52 misses |

The swap rate is now probably limited by the scenario (a swipe of 250 ms and a 0.35 s wait, 0.6 s per swipe) and by CoreAnimation's own pacing, not by the render thread: 21 swaps a second is the peak of the 5 s
window, the 6 swipes themselves last about 5 s (CORRECTED 2026-11-19: this said 24 s and 40 swipes; `init` cuts every rc line at 63 words, `init.c:1125`, so only 6 swipes ever ran: see docs/widget-swipe-cost.md section 0). The render thread is idle 91% over the 12 s profile window, which started after the 5 s of swipes had ended (CORRECTED: this does not show that the limit is upstream). What is left in `glFlush`: `_tc_sync` / fence waits for Zink's queue thread, `zink_draw`, `glBindFramebuffer` and
`glGetFramebufferAttachmentParameteriv` marshalling.

Pass benchmark (pb1 before, pb2 after; CPU ms per pass, rest of process, 375x500, 200 passes a case; noise about 0.2):

| case | pb1 before | pb2 after |
|---|---|---|
| clear / store, 1 draw | 3.27 | 1.78 |
| clear / discard, 1 draw | 2.78 | 1.30 |
| load / store, 1 draw | 3.40 | 1.64 |
| load / discard, 1 draw | 1.38 | 1.40 |
| dontcare / store, 1 draw | 1.28 | 1.50 |
| dontcare / discard, 1 draw | 1.61 | 1.52 |
| clear / store, 8 draws | 2.88 | 1.31 |
| clear / discard, 8 draws | 2.89 | 1.56 |
| load / store, 8 draws | 3.45 | 1.73 |
| load / discard, 8 draws | 1.53 | 1.70 |
| dontcare / store, 8 draws | 1.51 | 1.26 |
| dontcare / discard, 8 draws | 1.66 | 1.46 |
| load / store plain, 2 / 8 draws | 3.38 / 3.39 | 1.54 / 1.57 |

LOAD and CLEAR now cost what DONT_CARE costs (1.3-1.8 ms of driver CPU, the rest is kernel calls and Zink); the 2 ms difference is gone.

Regression (`reg1`, default build with the final lib, `autotest=1 settings=1`, 440 s): camera: taps 0, 1, 2 written (408, 157, 148 ms) and taps 1, 2 "THE PHOTO IS IN THE LIBRARY"; home phase 5.8 ms a frame (4.2 in glFlush), 12.4 swaps/s; Settings
launched (`!auto: settings`, no crash line). The only fbshot picture that survived the log's tail is the home screen (`logs/reg1-shot2.png`): icons, dock, wallpaper correct, no corruption. The Camera and Settings pictures were
NOT captured (shots every 40 s from 100 s; the pstore tail kept two shots and one of them did not decode), so rendering of Camera and Settings is not confirmed by a picture in this run: run `settings=1` with `SHOTS="fbshot=5 fbshot_every=5 fbshot_w=70"` (as btS1) to see it.
No GPU fault, no panic, run ended `reason 0 (end)`.

### Reproducing

```
tools/mk-mesa-pvr.sh   # ZINK=1: applies tools/mesa/pvr/*.patch when they change (a new source tree and a full rebuild), else ninja only
userland build (NOTE of HANDOFF-2026-11-16.md, RC_JOBS=rc.jobs.swprof) + payload
SHOTS= tools/r1-auto.sh lc2 "<BASE> swprof=1 bb_prof=115 bb_prof_all=1" 180                       # cache on
SHOTS= tools/r1-auto.sh lc3 "<BASE> swprof=1 bb_prof=115 bb_prof_all=1 pvr_loadop_cache=0" 180   # off
```
