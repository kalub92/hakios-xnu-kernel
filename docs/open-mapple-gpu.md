# Open Mapple: graphics and display, measured against the r1

Research only, 2026-11-18. Sources: the shallow checkouts of `mapple_gpu_mali`, `mapple_gpu_adreno`,
`xnu_kernel_mediatek_common` and `xnu_kernel_mediatek_mt6781` (OM below). Our side: `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`,
`MiniIOSurface.cpp`, `bsd/dev/redwood_pvr*.c`, `userland/src/eagl.c`, `tools/mk-mesa-pvr.sh`, `userland/build.sh`,
HANDOFF-2026-11-16.md (Updates 7, 8, 34) and HANDOFF-2026-11-17-night.md. Nothing was built or run.

Paths: `K/` = `xnu_kernel_mediatek_common/iokit/Kernel/arm/`, `K81/` = `xnu_kernel_mediatek_mt6781/iokit/Kernel/arm/`,
`G/` = `mapple_gpu_mali/`.

## Top 10 (ranked by value to us per unit of effort)

| # | What | OM source | Ours | Port | Effort | Risk |
|---|---|---|---|---|---|---|
| 1 | Brightness through a kernel `AppleARMBacklight` nub + `IOMFBBrightnessLevel`, no injection | `K/MappleFramebuffer.cpp` (backlightStart, AppleARMBacklight::setProperties, AppleCLCD::setProperties) | inject.c hook on BKDisplayBrightnessController; the user found the slider does nothing | direct (call our MT6370 `r1_bl` setter instead of their DCS 0x51) | S | low |
| 2 | Measure what a render pass costs, then profile the render thread: `passbench` + `VMG_CA_PROFILE`/`prof.py` | `G/driver/tests/passbench.m`, `G/driver/tools/prof.py` | none (prof1 was stopped) | idea, rewritten as a small Vulkan test on pvr | S | none |
| 3 | `AppleM2ScalerCSCDriver` (the service behind IOSurfaceAccelerator): CPU backend, then the MDP (RDMA→RSZ→WROT) | `K81/MTKScaler.cpp/.h`, `K81/MTKMDP.cpp` | none | CPU backend direct; MDP needs MT6765 registers found by hand | M (CPU) / L (MDP) | medium |
| 4 | Fences in one call (pooled syncobj + `SYNCOBJ_TRANSFER`) and skipping `WAIT_BO` when the BO's last submit is known done | `G/mesa/patches/0005-panfrost-xnu-fences-idle-seq.patch`, `G/docs/uapi.md` | `redwood_pvr_drm.c`, pvr winsys in `tools/mesa/pvr/pvr-darwin.patch` | idea (pvr's winsys, not panfrost's) | M | low |
| 5 | Driver outside the boot image (system image) | `mapple_build/core/mka.sh` step_gpu | `build.sh` already has it: `PVR_SYS=1` without `PVR_VK_RD=1` | direct, a build flag | S | low |
| 6 | Flip at the panel's frame end, vsync timer phase-locked to it, swaps queued until their GPU work is done, early present | `K/MappleFramebuffer.cpp` (K0c, K0d, K0g sections) | `fb_flip`, `fb_rdma`, `glwait=3` (MiniIOSurface lockCommon) | needs register checks (OVL shadow behaviour on MT6765) | M | medium |
| 7 | Direct scan-out: OVL layer pointed at the IOSurface, contiguous display-sized surfaces; the display RSZ for scaling | `K/MappleFramebuffer.cpp` scanout(), rszPrepare(); `K/MappleSurface.cpp` contig | none (every frame is a CPU copy + scale to 480x640) | needs register changes; MT6765 RSZ unverified | L | medium-high (panel can go dark) |
| 8 | Night Shift / colour matrices: selector 55 → DISP_CCORR | `K/MappleFramebuffer.cpp` setMatrix | none | needs MT6765 CCORR address/format | M | low-medium |
| 9 | GPU-side pass hygiene: discard dontCare attachments, first full-screen constant draw → clear, scissor from draw bounds | `G/driver/vmg_exec.c` (vmg_cmd_end_pass, draw_as_clear P5, P4 bounds) | eagl.c (none of the three) | idea, in eagl.c over GL (glInvalidateFramebuffer / scissor) | S-M | low |
| 10 | A Metal driver (their whole architecture) over Gallium, retargeted at Zink→pvr | `G/driver/metal/*.m`, `G/driver/vmg_*.c`, `G/vgpu`, `G/libdrm-xnu` | `userland/src/metal`, `redwoodmetal.m`, `tools/mesa/air-*.patch` (our older lineage, X6826B) | needs a new backend; ISA path not portable | L (months) | high |

Items 1, 2, 5 are cheap and sure. Item 10 is the only one that changes the architecture: it would remove the GL
injection, the CoreImage guard and possibly most of the per-pass cost, and it is months of work with a compiler we have not tried.

---

## 1. Their GPU architecture, end to end

**They run a Metal driver. There is no OpenGL ES, Vulkan or Zink in their stack.** (`G/README.md`, `G/docs/uapi.md`,
`G/ios/build.sh`, `G/mesa/build.sh`: Mesa is built with `-Dgallium-drivers=panfrost -Dvulkan-drivers= -Dopengl=false
-Dgles2=disabled -Degl=disabled`.)

```
backboardd / apps: QuartzCore's Metal renderer, CoreImage, AppleMetalGLRenderer (Apple's GLES-on-Metal), MPS
  -> Metal.framework: IOServiceMatching("IOAcceleratorES") -> MetalPluginName / MetalPluginClassName
  -> /System/Library/Extensions/MappleMTLDriver.bundle, class VMDevice (a _MTLDevice subclass)      G/driver/metal/*.m
  -> vmg: Gallium back-end (pipe_screen/pipe_context), AIR -> NIR translator, AIR -> Valhall ISA    G/driver/vmg_*.c, G/driver/isa
  -> Mesa panfrost (patched)                                                                         G/mesa/patches/0001..0009
  -> libdrm-xnu (Panfrost DRM calls) -> vgpu (IOConnectCallStructMethod)                             G/libdrm-xnu, G/vgpu
  -> kernel MappleGPUAccelerator : IOAcceleratorES, MappleGPUUserClient                              K/MappleGPU.cpp, K/MappleGPUUserClient.cpp
frames: IOSurface (K/MappleSurface.cpp, their IOCoreSurfaceRoot) -> AppleCLCD : IOMobileFramebuffer (K/MappleFramebuffer.cpp)
```

- **How it loads without injection.** The kernel service `MappleGPUAccelerator` (subclass of an `IOAcceleratorES` the kernel
  defines) carries `MetalPluginName = MappleMTLDriver` and `MetalPluginClassName = VMDevice`. Metal's own registration finds it
  and loads the bundle from `/System/Library/Extensions`. Boot-arg `mp_gpu=0` leaves the service out and Metal sees no
  device. This is the same discovery path our `docs/metal.md` section 1 documents. We already found it; they shipped it.
- **`ios/`** has no runtime code. It holds `build.sh` (builds Mesa for iOS, the driver objects and the bundle; Info.plist
  `NSPrincipalClass VMDevice`), `vctl.m` (a test tool that injects HID touches, takes framebuffer shots, kills
  backboardd), `vmprobe.m`, `compat/IOSurface/IOSurface.h` and the entitlements for vctl. Their release build is `mka.sh step_gpu`.
- **`vgpu/`** is the user-space side of the IOKit transport: Panfrost-shaped calls (`CREATE_BO`, `MMAP_BO` through
  `IOConnectMapMemory64`, `SUBMIT`, syncobjs) plus three of their own: `SYNCOBJ_TRANSFER`, `IMPORT_IOSURFACE` (zero copy) and
  `IMPORT_USERPTR` (`newBufferWithBytesNoCopy`). It is not an AGX/IOGPU-compatible interface. Apple's IOGPU is not
  emulated; Metal only needs the `IOAcceleratorES` property pair.
- **CoreImage, GLES, Photos** work because each sits on Metal. GLES apps run through Apple's `AppleMetalGLRenderer`. Our
  CoreImage crash comes from a default CIContext finding no Metal and falling back to Apple's GLES (Update 34). That path
  cannot fail for them.
- **Shaders.** System metallibs arrive as AIR (LLVM bitcode). `vmg_air.c` (164 KB) translates AIR to NIR, and panfrost's
  Valhall compiler takes it from there. `driver/isa` (about 1 MB, `va_v9_gen.c` alone 470 KB) compiles AIR straight to Valhall
  ISA and plugs in through patch 0006. Translations are cached on disk per key (`vmg_screen.c` lines 82-130:
  `/private/var/tmp/vmg-cache-<uid>`, blake3 of driver build + inputs). Mesa's own disk cache is off.
- **Software fallback.** `vmg_device_create("llvmpipe")` exists for the Mac only (`VMG_HAVE_LLVMPIPE=0` on iOS). On the phone,
  "no GPU" means CoreAnimation's own CPU renderer (`mp_gpu=0`; device.conf: "the UI drawn through Metal on the GPU
  (mp_gpu=2) or in software").
- **Adreno** (`mapple_gpu_adreno`) is an overlay that swaps panfrost for freedreno and adds a msm libdrm. The Metal front end
  stays. They state the aim is "one generic mapple_gpu with Mali and Adreno backends". This shows the front end is
  meant to be backend-independent.
- **Maturity (their own words, device.conf `status gpu partial`):** Mali-G57 at LK's 300 MHz, no DVFS; "heavy wallpapers
  (photo wallpapers, most Collections) may not work yet, release 2"; Photos' YCbCr thumbnails converted on the CPU; "Safari
  plays video ... each frame is converted to RGB on the CPU, most of one core"; **camera: missing**. `G/ios/build.sh` still
  says "Not run yet: this Mac has only the Command Line Tools". That comment is stale, because `mka.sh` builds it with Xcode
  (`X15`). No home-screen fps figure was found in the repos.

## 2. What is vendor-independent, and what a port onto PowerVR would cost

| Piece | OM files | Vendor-specific? | On our pvr stack | Effort / risk |
|---|---|---|---|---|
| Metal API front end (VMDevice, encoders, resources, layers) | `G/driver/metal/*.m` (~170 KB) | no; talks only to `vmg.h` | reusable as is | M / medium (already debugged by them against CA) |
| AIR → NIR | `G/driver/vmg_air.c` | no | reusable; the NIR then has to pass **pvr's rogue compiler**, which so far has only seen Zink's NIR | L / **high**: 16-bit ops, subgroup and image ops CA uses are untested on rogue |
| Gallium back end | `G/driver/vmg_screen.c`, `vmg_exec.c`, `vmg_bounds.c` | mostly no; `vmg_screen.c` has 34 panfrost references (screen creation, BO-cache hooks, ISA hooks) | pvr has **no Gallium driver**, so the target would be **Zink over our pvr Vulkan** (`zink_create_screen`), which we already build (`libgallium-26.3.0.dylib`) | M for the swap; but Zink+pvr keeps its per-pass CPU cost (see 3) |
| AIR → Valhall ISA | `G/driver/isa/*` | Mali only | not portable; drop | n/a |
| libdrm/vgpu transport | `G/libdrm-xnu`, `G/vgpu`, `G/uapi` | Panfrost-shaped | we already have our own (`redwood_pvr_drm.c` + pvr-darwin.patch) | n/a; borrow ideas only (item 4) |
| Kernel accelerator service | `K/MappleGPU.cpp` (`IOAcceleratorES` + properties) | Mali job manager | our `RedwoodPVR.cpp` would gain the two properties | S once a bundle exists |
| IOSurface kernel side | `K/MappleSurface.cpp` | no | we have `MiniIOSurface.cpp` (same lineage). Their extras: contiguous scan-out allocation, `mapple_gpu_surface_idle` | S-M per feature |
| Framebuffer | `K/MappleFramebuffer.cpp` (AppleCLCD) | MT6781 OVL/DSI/RSZ/CCORR offsets | `MiniIOMobileFramebuffer.cpp`; same selector map | see 5 |
| IOSurfaceAccelerator on MDP | `K81/MTKScaler.*`, `K81/MTKMDP.cpp` | MT6781 MDP values found by hand | none | CPU backend M; MDP L |

**Porting the Metal driver (item 10) means** building `driver/metal` + `vmg_air.c` + `vmg_screen/exec/bounds` against Zink
instead of panfrost, keeping our pvr Vulkan driver underneath, and publishing `MetalPluginName` on our accelerator service.
Gains: CoreImage, GLES apps, Photos filters and MPS without injection; eagl.c (205 KB of interposition) and the CoreImage guard
go away; CA's Metal renderer gives explicit load/store actions and one command buffer per frame instead of 6-17 `glFlush`
chunks. Costs: months; the rogue compiler must survive CA's shaders (our `docs/air-report.md` reached ir3 on 313 of 456
modules, and we have never pointed it at rogue); the 11 MB driver grows by the Metal front end and the AIR translator
(Zink is already present). Our own X6826B attempt (`docs/metal-x6826b-plan.md`) found "breakcore" corruption in the
asynchronous path. OM's code is the later, fixed generation of that idea, so the port would start from theirs, not ours.

## 3. Performance techniques, ranked for our swipe fps

Our measured bottleneck (HANDOFF Update 7/8): during a home swipe the render thread issues 6-17 GL chunks per frame at about
3.3 render passes each. That costs about 10 ms of CPU per chunk in Zink/pvr submission, about 3 ms per render pass. GPU fragment time is
about 1.6 ms and the present path is not the limiter. Reduce Transparency cut 30-40%. So anything that only saves GPU bandwidth is
worth little here. What counts is fewer passes, or cheaper ones on the CPU.

| Rank | Technique | OM source | Applies to us? | Expected gain on swipes |
|---|---|---|---|---|
| 1 | **Fewer render passes by construction**: CA's Metal renderer ("4+ passes per home screen frame", `vmg_exec.c` end-pass comment) vs our GL path's ~20-56 passes per frame | the Metal driver as a whole | only via item 10 | largest, if per-pass cost stays about 3 ms (estimate, not measured) |
| 2 | **Find where the 3 ms per pass goes before changing anything**: `passbench.m` times full-screen passes per load/store combination on a linear IOSurface vs a private texture; `prof.py` aggregates in-process stack samples of the CA render thread | `G/driver/tests/passbench.m`, `G/driver/tools/prof.py`, `VMShmStats.m` | yes, rewritten as a pvr Vulkan test plus a sampler in backboardd | none by itself; it decides 3-6 |
| 3 | **Offload CA's whole-screen reductions** (backdrop/blur downsampling) to `AppleM2ScalerCSCDriver`. Without the service QuartzCore does them on the GPU, which for us means more GL passes | `K81/MTKScaler.h` comment (mp_scaler_cpumax) | yes; at r1's 375x500 frame a CPU reduction is about 14x cheaper than their 1125x2436 | possibly large on blurred UI (Reduce Transparency already showed 30-40% lives there); **verify** CA calls it during a swipe |
| 4 | Fence calls batched (syncobj pool + transfer in one IOKit call), `WAIT_BO` skipped via a done-sequence number | patch 0005 | yes (we measured about 600 ms of syncobj ioctls per 10 s, about 6% of a core) | about 5% CPU |
| 5 | BO cache trimmed at fetch and submit, sized by sysctl | patch 0007 | we already recycle flags-0 BOs (25.3→21.7 ms/frame); our broader variant crashed | done-equivalent |
| 6 | Store dontCare = discard (CA's second full-screen colour attachment was written back about 11 MB per pass), first full-screen constant draw turned into a clear (P5), scissor from draw vertices (P4) | `vmg_exec.c` vmg_cmd_end_pass, draw_as_clear, vmg_cmd_draw | partly, from eagl.c: glInvalidateFramebuffer/glDiscardFramebufferEXT where CA leaves content dead; per-draw scissor | GPU bandwidth only; small for us (GPU 1.6 ms) |
| 7 | Framebuffer fetch kept correct across split passes (preload) | patch 0008 | related **hypothesis to test**: Zink may end a render pass around each fbfetch draw (our vibrant program uses `GL_EXT_shader_framebuffer_fetch`, eagl.c `fetch_offered=1`) when pvr lacks rasterization-order attachment access. Compare passes per frame with `glfetch=0` | unknown; cheap to test |
| 8 | Persistent shader cache (their AIR translation cache on disk) | `vmg_screen.c` 82-130 | we build Mesa with `-Dshader-cache=disabled`; a Zink/pvr pipeline cache under /var would remove first-swipe compile hitches only | hitches, not steady fps |
| 9 | Skipping texture re-uploads when an IOSurface's seed has not moved; hashing buffer contents (XXH3) | `vmg_exec.c` 320-380 | eagl.c already refills by seed | done-equivalent |
| 10 | Direct scan-out / no copy | `K/MappleFramebuffer.cpp` scanout() | yes (section 5) | frees CPU outside the render thread; small for fps |
| - | `CA_DISABLE_GENERIC_SHADERS`, min 3 drawables per CAMetalLayer | `G/driver/metal/VMLayer.m` | Metal-only | n/a now |
| - | Render-pass merging, tile memory / subpass merging | **not found** | | |
| - | YUV overlay planes for camera/video | **not found**. OM converts video YUV→RGB on the CPU ("most of one core"), camera is missing, and their MDP converts NV12→BGRA for IOSurfaceAccelerator. Our `glyuv=1` zero-copy sampling is ahead of theirs | | |
| - | Double-painted camera frames after a swipe | **not found** (no counterpart issue or fix) | | |

## 4. How they deliver the driver

- `MappleMTLDriver.bundle` goes into `/System/Library/Extensions` of the **system image** (`mka.sh step_gpu`: copied into
  `$OUT/root`, `strip -S`, ad-hoc signed; `step_rootimage` builds `system.img` from the user's IPSW). On the realme that image
  is a 16 GB volume on UFS LUN 2 (`device.conf`: `root_size 16g`, `mp_ufsroot=xnusys`). Their boot payload limit is 64 MB
  (`payload_max 0x3fe0000`). **Nothing GPU-related is in the boot image.**
- Mesa is linked statically into the bundle (`libmesa_mapple.a`, panfrost + NIR + Gallium util, no LLVM, no shader cache, no
  zstd). Its size is **not stated** anywhere in the repos, and nothing was built here to measure it.
- Ours: `libvulkan_powervr_mesa.dylib` 11.2 MB raw = 1.67 MB zlib, 71% of root.apfs. We already strip with `-x` and build with
  `-Doptimization=s -Db_ndebug=true`. **`userland/build.sh` already supports OM's approach**: `PVR_SYS=1` without
  `PVR_VK_RD=1` makes `root/lib/libvulkan_powervr_mesa.dylib` a symlink to `/fw/pvr/` on the iOS system volume, where
  libgallium already lives. The HANDOFF build line sets both, so the driver lands in the ramdisk. Dropping `PVR_VK_RD=1`
  for anything but driver-iteration builds returns about 1.7 MB of the 32 MB boot partition. That is the whole headroom
  problem of the BT payload (32 KB left) and the ringtones (276 KB over). Cost: a driver change needs a system-image write.
  Risk: if `/fw` is not bound before backboardd starts, CA falls back to software. Check the `init.c` flash_root order.

## 5. Display driver: theirs (MT6781 AppleCLCD) vs ours (MiniIOMobileFramebuffer)

Both implement the same iOS 16 IOMobileFramebuffer user client (selectors 3/4/5/6/7/8/12/18/28/72, swap record 0x46c) over
LK's continuous-splash framebuffer, from the same lineage. Their additions:

| Feature | OM (`K/MappleFramebuffer.cpp`) | Ours | Port to MT6765 | Effort / risk |
|---|---|---|---|---|
| **Brightness** | `AppleARMBacklight` nub with `backlight-control = true` and `IODisplayParameters.brightness {min,max,value}`. CoreBrightness finds it (`CBDisplayContaineriOS findBacklight`) and writes `IOMFBBrightnessLevel` (nits 16.16) on the framebuffer, mapped to panel level and sent at the next vsync tick. Off with the display (`mp_fb_bloff`), min floor (`mp_fb_blmin`) | inject.c hook on `-[BKDisplayBrightnessController setBrightnessLevel:...]` → sysctl `kern.redwood.backlight`; inject.c:1368 notes CoreBrightness finds no AppleARMBacklight. **User: slider did nothing.** | direct: same two classes; replace `dsiSendDcs(0x51..)` with our MT6370 I2C write (r1_bl, 0..512 scale); `mp_panel_nits` calibration knob kept | S / low |
| **Night Shift, colour filters** | selector 55 `IOMobileFramebufferSetMatrix` (9 x s32.32) → product of matrices → DISP_CCORR (MT6781 0x1400b000 per their comment, 1.0 = 0x800, relay for identity) | none | needs MT6765 DISP_CCORR0 address and coefficient format (Android 4.9 `ddp_ccorr.c`, haretic DT) | M / low-medium |
| **Vsync** | 60 Hz timer **phase-locked** to OVL frame-complete (INTSTA bit 1, polled). The tick fires `fLead` (1 ms) before the predicted frame end, re-calibrated after misses | `fb_rdma=1`: RDMA0 INT_STATUS bit 2, but `present()` busy-polls up to a full period after a 3 ms margin (a CPU spinning per frame) | idea; reuse our RDMA0 bit with their predict-and-short-poll scheme | M / low |
| **Tearing / double buffer** | flip of LK's OVL layer address **at the frame end**, with interrupts off, within a few µs of the bit rising. On MT6781 the OVL shadow registers are bypassed, so a write applies immediately | `fb_flip=1` writes `OVL0 L0_ADDR` right after the copy, at any point in the frame (not in BASE args) | **check** whether MT6765 OVL0 latches L0_ADDR through shadow registers at frame start. If not, our fb_flip can tear and should take their timing | S to check, M to fix |
| **GPU-aware present** | swaps queued (4 deep), a tick shows the newest whose surfaces have no GPU job pending (`mapple_gpu_surface_idle`), forced after 12 ticks. Early present on swap arrival and on GPU job completion. Real `SwapWait` semantics (retired/displayed, busy bit 31) with per-reason stats | `glwait=3`: the surface lock waits for `pvrFence` (MiniIOSurface lockCommon). SwapWait answers "done" at once (fine while the copy is synchronous in SwapEnd) | idea; only worth it together with scan-out | M / medium |
| **Direct scan-out** | one full-screen contiguous BGRA surface below 4 GiB → OVL layer address and pitch point at it, no copy. MappleSurface allocates display-sized surfaces contiguously when scan-out is on. Falls back to the copy with a logged reason | none: every frame copied and scaled (375x500 → 480x640 with `fbscale=1 fbaspect=1`) | needs **scaling**, so their `mp_fb_rsz` path (OVL0 → RSZ0 → RDMA0, switched in the 60 Hz blanking) is the relevant one. MT6765 display RSZ presence and address **unverified**. Contiguous allocation in MiniIOSurface + pvr able to render to it (it can, via its MMU) | L / medium-high (pipe misconfig = black panel; their fallback logic is the model) |
| Refresh 60/120/auto, `IOMFBDisplayRefresh` | DSI VFP switch via VM_CMD | n/a | r1 panel is 60 Hz | skip |
| Extended-range formats (w30r, w40a, l10r) converted while copying | yes | **check** whether CA ever hands us these | idea | S |
| Letterbox / native size | iPhone X logical size, centred crop, or scaled letterbox | `fbscale`, `fbaspect`, `fbfit` (ours is broader) | n/a | |
| Rotation | in the Metal driver (private texture rotation of IOSurface render targets, `vmg_exec.c` rotated blit), not in the display | user verdict: rotation good | nothing to take | |
| Panel power | RequestPowerChange(0) fades out over a few vsyncs, backlight to 0 | our off-when-blanked path is unverified (HANDOFF) | direct, with the backlight nub | S |

### IOSurfaceAccelerator on the MDP (`K81/MTKScaler.*`, `K81/MTKMDP.cpp`)

A kernel `AppleM2ScalerCSCDriver` (user-client ABI reverse-engineered from iOS 16.7.16) with a CPU backend (`mp_scaler=1`) and
an MDP backend (`mp_scaler=2`, default): NV12 (420v/420f) → BGRA through MDP RDMA0 → WROT0 (BT.601/709 matrices in WROT), and
BGRA scaling through RDMA0 → RSZ0 → WROT0 in 256-column strips. The values were found by hand on the phone, since nothing public
describes them. Their notes: without the service, VideoToolbox transfers between IOSurfaces **return zeros** (Photos, anything
scaling through VideoToolbox), and QuartzCore falls back to the GPU for its whole-screen reductions.

For us: (a) it may explain empty thumbnails or Photos problems if we see any; (b) it could take CA's blur reductions off the
GL path (rank 3 above); (c) the MDP could do camera NV12 → BGRA for AVFoundation consumers that ask for BGRA. MT6765 also has
MDP RDMA/RSZ/WROT in MMSYS, but none of their register values carry over: the same hand discovery would be needed. Start with
the CPU backend (small frames on the r1) and log which requests CA and VideoToolbox send.

## 6. What to measure first (cheap, before porting anything)

1. Per-pass CPU cost on pvr in isolation (a Vulkan `passbench`): an empty pass, clear-only, load+store on a linear IOSurface-backed
   image vs an optimal-tiling image, with 1 and 10 draws. This tells whether the 3 ms is fixed per `vkQueueSubmit`/job
   (firmware command build, render-target dataset) or grows with draws.
2. Passes per frame with `glfetch=0` vs default (rank 7 hypothesis), and with Reduce Transparency, using the existing `tl: g` counters.
3. Register a stub `AppleM2ScalerCSCDriver` that only logs requests (refusing with `kIOReturnUnsupported`, which every client
   handles), then swipe: does QuartzCore ask it for reductions?
4. Read MT6765 OVL0 shadow control before trusting `fb_flip` timing.

## Not found in Open Mapple

PowerVR anything; a Vulkan or GLES driver of theirs; render-pass merging; YUV overlay planes on OVL; a camera pipeline on
spaced (status "missing"); H.264 encode (not in the GPU repos); a measured home-swipe fps; the installed size of the Metal bundle.
