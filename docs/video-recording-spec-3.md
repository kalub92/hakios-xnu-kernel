# Video recording on the r1: spec 3 (after vd2-vd6)

Status: analysis only, nothing implemented. Written 2026-11-18 from `logs/vd5.log`, `logs/vd6.log`, the "Video diag 2" section
of HANDOFF-2026-11-17-night.md, `userland/src/{inject_media.c,video_test.m}`, and the iOS 16.5 (20F66) cache, read with
`tools/dsc` plus capstone. Addresses are unslid cache addresses. On the device the slide was 0xbec0000 in vd6
(`VTEncoderSessionSetPixelBufferAttributes` 0x194cadbc0 + 0xbec0000 = 0x1a0b6dbc0, as logged). **(guess)** marks anything not
read from a binary or a log. **(inference)** marks a conclusion from both, with how sure it is.
The rules of HANDOFF-2026-11-16.md and of specs 1 and 2 still apply.

## 0. Summary

- **Why no factory was ever called: video_test's sessions never ran in video_test.**
  - On iOS, `VTCompressionSessionCreate` in an ordinary process creates a **remote** session in **mediaserverd**
    (Mach service `com.apple.coremedia.compressionsession`). Encoder selection and the factory call happen in mediaserverd.
  - The registrations made inside video_test go into video_test's own registry, which nothing reads. So mediaserverd's
    registry, which holds none of them, answers:
    - EncoderID given: -12908 at create;
    - no spec, avc1: create returns 0 (mediaserverd has `h264.rtvc`), then the callback gets -12908;
    - no spec, hvc1: -12908 at create (mediaserverd has no hvc1 encoder).
  - This explains every vd2-vd6 B-part line. Nothing in VT's matching rejects our items: no sandbox gate, no entitlement,
    no bundle-only rule, no required key.
- **Fix: register the encoder inside mediaserverd** (libredwood-media.dylib, `redwood_media_init`). No bundle is needed, no
  bind mount, and no system-image change.
  - mediaserverd is the process that selects encoders for the Camera (CMCapture runs there, and selects locally).
  - It also selects for every app's or tool's remote session, including AVAssetWriter in video_test.
- **For in-process tests,** video_test can force a local session with `VTCompressionSessionCreateWithOptions` and the option
  `AllowClientProcessEncode = true`.
- **Bug to fix first:** the vd2 null encoder's class table is in the wrong slot order. The real order (read from VT's call
  sites and VCPHEVC's StartSession) is `+0x08 StartSession, +0x10 EncodeFrame, +0x18 CompleteFrames`. The probe's
  `{start, prepare, encode, complete}` would route EncodeFrame into `n_prepare`, and CompleteFrames into `n_encode`.
- **The movie path.** The four vd_wrap_sinks selectors do not exist on 16.5, so nothing was wrapped. The real names are in
  §B.1.
  - CMCapture creates its session lazily in `-[BWVideoCompressorNode renderSampleBuffer:forInput:]`. It calls VT through a
    direct-branch stub island (0x1a258d958: `adrp/add/br`, no GOT). A `__DATA,__interpose` entry therefore cannot see it.
  - So "no VTCompressionSessionCreate line" in vd5/vd6 says nothing.
  - Failures there are logged through `-[BWLimitedGMErrorLogger logErrorNumber:errorString:]` with "creating compression
    session". That is the one hook that answers it.
- **video_test scan output was lost** because the kept-log channel (pmsg, 64 KB) was full: vd6 shows "65270 bytes (332
  dropped)". The probe's ~400 lines filled it before `scan` ran. Fix: drop the probe from the run and run `scan` right after
  the Camera scenario.

## A. VideoToolbox encoder selection (16.5)

### A.1 Registration: `VTRegisterVideoEncoderWithInfo` 0x194eb3870

- `FigThreadRunOnce(0x1dd4012c0, 0x194eb3ab0)` → `FigRegistryCreate(kCFAllocatorDefault, kCMObjectLibraryClassID_VideoEncoder,
  &registry@0x1dd401628)`. The registry is per process, held in VideoToolbox's `__DATA`.
- **MatchingInfo** = a mutable copy of `info` (or an empty dictionary), plus:
  - `VTCodecType` = the 4cc as a CFString (0x194eb392c);
  - `VTRating` = 0, added only if absent (`CFDictionaryAddValue`);
  - `VTAllowSandboxedEncode` = false (forced).
- The **ImplementationID** is taken from `info["CMClassImplementationID"]`.
- Then 0x194eb3ad4 builds the item description:
  - `CMClassID` = VideoEncoder; `CMClassImplementationName` = "Dynamically Registered %@ Video Encoder"; the ID;
  - `VTCodecName` / `VTEncoderName`; `CMClassImplementationVersion` = 1; `CMMatchingInfo`;
  - (`CMDependencies` = {IORegistryRequiredKey: IOGVACodec} only for ID `...h264.gva`);
  - `VTFactoryFunctionHasSpecificationArgument` = **false** (0x194eb3c34, always on this path).
- Finally `FigRegistryAddItem(registry, desc, factory)` (0x194eb3ca4).
- `VTRegisterVideoEncoder` (0x194ca9744) is the same call with `info = NULL`: no ImplementationID, so it cannot be reached by
  EncoderID.
- **Consequence:** the public path always calls the factory with **3 arguments** `(codec, allocator, &encoder)`. vd2's R4
  (an info key, plus a 5-argument factory) would have been called with 3 arguments and read garbage. Delete R4. The 5-argument
  form exists only for the built-in table (0x194eb683c, from 0x194eb6808).

### A.2 Selection: 0x194eb3d2c (called from `VTCompressionSessionCreateWithOptions` at 0x194cb2004)

1. Reads `EncoderID` from the spec. When `RequiredLowLatency` / `EnableLowLatencyRateControl` is set, it also reads
   `RequireHardwareAcceleratedVideoEncoder`.
2. `FigThreadRunOnce(0x1dd4012d0, 0x194eb45f4)` registers the built-ins: hevc-with-alpha, disparity-hevc, depth-hevc;
   testipb only on internal builds; the paravirtualization guest query.
   - The registry pointer at 0x1dd401628 is NULL → -12908 (0x194eb3e54).
3. `FigRegistryCopyFilteredItemList(registry, matcher 0x194eb4a10, ctx)`. ctx is `{codecStr, spec, requireHW@+0x10, pass1@+0x11 = 1}`.
4. `pass1 = 0`. Then 0x194eb4e0c expands **wrapper encoders** (`VTIsWrapperEncoder` / `VTWrapsCodecType`) into "%@:%@"
   items.
5. If `EncoderID` is set: keep only the items whose `CMClassImplementationID` equals it (0x194eb3fec..0x194eb4044).
   **An empty list gives -12908.**
6. Sort with 0x194eb546c: higher `VTRating` first; on a tie, `VTIsHardwareAccelerated` first; then by ID string.
7. Starting at the index argument, for each candidate:
   - `FigRegistryItemGetFactory(item, 1, &f)`;
   - with `HasSpecificationArgument`: `f(codec, implID, hostSpec, alloc, &enc)`; otherwise `f(codec, alloc, &enc)`;
   - no factory → -12908 (0x194eb4340); success with NULL → -12912 (0x194eb4378);
   - avc1 / mp4v with a CMSession: first `CMSessionAddResource(VideoEncoder)` (-12983 / -12985 → -12915).

Matcher 0x194eb4a10 (returns 1 = keep):

| check | address | effect on a runtime item |
|---|---|---|
| `VTCodecType` present, a CFString, equal to the requested 4cc | 0x194eb4aac..0x194eb4aec | ours pass |
| spec `AllowParavirtualizedEncoder` = false and item `VTIsParavirtualized` = true → drop | 0x194eb4af0 | n/a |
| spec has EncoderID → keep (the ID filter is step 5) | 0x194eb4b44 | |
| pass 1 and item is a wrapper → keep | 0x194eb4b94 | |
| byte **0x1dd401655** ≠ 0 and item `VTIsHardwareAccelerated` → drop | 0x194eb4ba4 (0x194ed046c reads the byte) | byte is 0 on the device (vd2..vd6) |
| spec `RequiredPropertySupport`: ProfileLevel against `VTSupportedProfileArray` (HEVC only) | 0x194eb4c44 | n/a without that key |
| item `VTRequiredSpecificationEntries` must be in the spec | 0x194eb4d68 | absent → keep |

- `requireHW` (ctx+0x10) is not read by the matcher. After creation VT only records `UsingHardwareAcceleratedVideoEncoder`
  in the session (+0x300, 0x194cb215c..0x194cb2200), with no failure **(inference, medium: the wrapper step was read only in
  part)**.
- **There is no sandbox, entitlement, platform, bundle-only, rating-threshold, or `QualityRating` /
  `SupportsFrameReordering` gate.** A runtime item with only `VTCodecType` matches.

### A.3 Local or remote: `VTCompressionSessionCreateWithOptions` 0x194cb19e4

Signature, read from the `VTCompressionSessionCreate` wrapper 0x194cadadc:
`(alloc, w, h, codec, spec, sourceAttrs, compressedAlloc, callback, refcon, options, &session)`. `VTCompressionSessionCreate`
passes `options = NULL`.

```
0x194cb1b84  local = options["AllowClientProcessEncode"] == kCFBooleanTrue
0x194cb1bac  if (FigServer_IsServerProcess() || local)  -> local (0x194cb1bc0)
0x194cb1bb8  else if (byte 0x1dd401654 != 0)           -> local
0x194cb1c4c  else VTCompressionSessionRemote_Create      (0x194eaebe4: FigRPCCreateServerConnectionForObject)
```

- Server: `VTCompressionSessionServerStart` 0x194cbc1a0 → `FigRPCStartServer("com.apple.coremedia.compressionsession")`.
  `com.apple.mediaserverd.plist` lists `com.apple.coremedia.compressionsession` and `....xpc`, so the server is mediaserverd,
  which is in our job set: the Camera works, and the no-spec avc1 create returned 0, which needed a server.
- Byte 0x1dd401654 was read as 00 (vd6:3731). In video_test, `FigServer_IsServerProcess()` is false **(inference, high)**, so
  every probe create was remote.
- The no-spec results match mediaserverd's encoder list, not video_test's:
  - hvc1 fails synchronously: mediaserverd has no hvc1 encoder, while video_test had three registered;
  - avc1 succeeds: mediaserverd's `h264.rtvc` exists, and then fails at encode **(inference, high)**.
- mediaserverd is a FigServer process, so CMCapture's compressor selects **locally in mediaserverd**.

### A.4 What satisfies it

| goal | where | how |
|---|---|---|
| Camera (CMCapture, BWVideoCompressorNode) | mediaserverd | `VTRegisterVideoEncoderWithInfo('avc1', info, factory3)` in `redwood_media_init` (libredwood-media.dylib), behind `venc=1` |
| apps / AVAssetWriter / `video_test encode` | mediaserverd (remote session) | the same registration |
| quick in-process tests in video_test | video_test | register in video_test, **and** create with `VTCompressionSessionCreateWithOptions(..., options = {AllowClientProcessEncode: true}, &s)` (`dlsym` both `VTCompressionSessionCreateWithOptions` and `kVTCompressionSessionOption_AllowClientProcessEncode`) |

Recommended `info`:
- `CMClassImplementationID` = "com.redwood.videoencoder.h264";
- `VTRating` (the value of `kVTVideoEncoder_Rating`) = 1000 (a CFNumber int). Anything above -1 beats `h264.rtvc`.
- Leave out `VTIsHardwareAccelerated`. It is only a tie-breaker, and it is dropped if byte 0x655 is ever set.

**Bundle route: not needed.** VT names its encoder bundles with fixed paths (`/System/Library/VideoEncoders/H264H2.videoencoder`,
... a table in `__cstring`), while the rootfs only has `AppleVideoEncoder.bundle` (AVE, IOGVACodec-dependent). Whether and how
VT or CoreMedia scans that directory was **not** traced (not needed). The init.c bind-mount and the 1.4 MB of boot-image
headroom are not used by the registration. The encoder code itself goes into libredwood-media.dylib.

### A.5 The encoder class layout (read)

`CMDerivedObjectCreate(alloc, &vtable, VTVideoEncoderGetClassID(), &enc)` creates the object, with
`vtable = {NULL, &baseClass, &encoderClass}`. Storage comes from `CMBaseObjectGetDerivedStorage(VTVideoEncoderGetCMBaseObject(enc))`.

- baseClass: `{version 1, derivedStorageSize, equal, invalidate, finalize, copyDebugDesc, copyProperty, setProperty, ...}`.
  VCPHEVC uses `{1, 0x5b0, NULL, ...}`.
- encoderClass: `{version, slots...}`. VCPHEVC 0x1e246ee60 = `{3, 0x1beb968b0, 0x1beb96988, 0x1beb973e8, 0x1beb97cd0, 0, 0, 0}`.

| offset | slot | arguments (read from the caller in VT / the callee in VCPHEVC) |
|---|---|---|
| +0x08 | StartSession | `(enc, VTEncoderSessionRef session, uint64 dims, CFTypeRef x3)`. dims = width in the low 32 bits, height in the high (0x194cb2068: `ldr x20,[x19,#0x28]`, stored `stp w28(w), w27(h)`; VCPHEVC: `lsr x22,x2,#32`). x3 is a CF object that VCPHEVC retains **(meaning unknown: the compressed allocator or attributes; ignore)** |
| +0x10 | EncodeFrame | `(enc, token, CVImageBufferRef, const CMTime *pts, const CMTime *dur, CFDictionaryRef frameProps, VTEncodeInfoFlags *infoOut)` (0x194cb2654..0x194cb26c0) |
| +0x18 | CompleteFrames | `(enc, const CMTime *until)` (0x194cb27d4) |
| +0x20 | ? (multi-pass / time ranges) | leave NULL. Every VT call site seen tests it with `cbz` first |
| +0x28 | ? (called from the PrepareToEncodeFrames path) **(guess)** | leave NULL |

Session helpers exported by VT: `VTEncoderSessionSetPixelBufferAttributes` 0x194cadbc0, `VTEncoderSessionEmitEncodedFrame`
0x194caecdc `(session, token, status, infoFlags, sbuf)`, `VTEncoderSessionCreateCMBlockBuffer` 0x194cb9a14,
`VTEncoderSessionCreateVideoFormatDescription` 0x194caea20.

## B. The movie path in CMCapture 16.5

### B.1 Real classes and selectors (`vr2/methimp.py`; all instance methods, all in CMCapture)

The vd_wrap_sinks names `_doStartRecordingAtTime:withSettings:sensorVideoPort:`, `_writeBuffer:forInputIndex:`,
`_doEndRecordingAtTime:earlyTerminationErrCode:` and `_doStartingToRecordWithSBuf:inputIndex:sbufPTS:` do not exist on 16.5.
That is why no "wrapped" line appeared, and why no "never appeared" line did either: that line only comes after 600 s.

| purpose | selector | IMP | safe for vd_wrap? |
|---|---|---|---|
| compressor gets a frame (creates the session lazily) | `-[BWVideoCompressorNode renderSampleBuffer:forInput:]` | 0x1a20edb8c | yes (2 pointers) |
| the session it has (the typo is Apple's) | `-[BWVideoCompressorNode _compresessionSession]` | 0x1a20ef71c | yes |
| compressor drops | `-[BWVideoCompressorNode handleDroppedSample:forInput:]` | 0x1a20ef0bc | yes |
| **every node error**, including "creating compression session" | `-[BWLimitedGMErrorLogger logErrorNumber:errorString:]` | 0x1a2014cb8 | yes (int, NSString) |
| coordinator start | `-[BWFileCoordinatorNode startRecordingWithSettings:videoSettings:maxFrameRate:stopAtPTS:startAfterPTS:]` | 0x1a1fca660 | **no**: an FP argument (maxFrameRate) and CMTimes; vd_wrap's `host_line` before the call clobbers v0. Skip it |
| coordinator frames | `-[BWFileCoordinatorNode renderSampleBuffer:forInput:]` | 0x1a1eb0c38 | yes |
| coordinator stop / end | `-[BWFileCoordinatorNode stopRecordingWithErrorCode:]` 0x1a1fcad54, `-[BWFileCoordinatorNode recordingTerminated:]` 0x1a1fcb090 | | yes |
| file sink frames (written samples) | `-[BWQuickTimeMovieFileSinkNode renderSampleBuffer:forInput:]` | 0x1a1fe75e8 | yes |
| file sink drops | `-[BWQuickTimeMovieFileSinkNode handleDroppedSample:forInput:]` | 0x1a1fe9b0c | yes |
| file size at the end | `-[BWQuickTimeMovieFileSinkNode lastFileSize]` | 0x1a1f7cb5c | yes, if its type is `q16@0:8` (log the type encoding). **Not** `lastFileDuration`: it returns a CMTime through x8, which vd_wrap does not pass on |

Where the session is created:
1. `renderSampleBuffer:forInput:` (0x1a20eea84) does `dispatch_sync` of the block 0x1a20ef14c.
2. That block calls helper 0x1a20ef228, which reads `AVVideoCodecKey` / `AVVideoWidthKey` / `AVVideoHeightKey` /
   `AVVideoEncoderSpecificationKey` / `AVVideoCompressionPropertiesKey`.
3. Helper 0x1a20ef7d8 calls `VTCompressionSessionCreate` (0x1a20ef83c, via stub 0x1a258d958, a direct `br` to 0x194cadadc),
   then `VTSessionCopySupportedPropertyDictionary`, then the property setter 0x1a20efa64.
4. Any nonzero status goes to `[logger logErrorNumber:status errorString:@"creating compression session"]` (0x1a20ef890).

So hooking `logErrorNumber:errorString:` gives the create status, and our own factory's log line gives the success case.

### B.2 Why the interpose saw nothing

The `vdiag_interpose` entry for `VTCompressionSessionCreate` only rebinds GOT-based imports. CMCapture's call goes through a
cache stub island that branches directly. VT's own server-side create is an intra-image call. Neither can be interposed
**(inference, high: the stub bytes were read)**. Keep the entry for non-cache callers, but do not read anything from its
absence.

### B.3 The smallest check that a .MOV was written

- Path: vd5 shows `file:///var/mobile/Media/DCIM/.MISC/Incoming/<n>__<UUID>.MOV`. After `didFinish...error:nil` the Camera
  hands it to the photo library: probably `DCIM/100APPLE/IMG_xxxx.MOV` **(guess)**. `video_test scan` enumerates all of
  `/private/var/mobile/Media`, hidden `.MISC` included, so it covers both.
- Its output was lost to the 64 KB pmsg cap. vd6's kept block header reads "65270 bytes (332 dropped)". `say()` in video_test
  already writes through `kern.redwood.host_keep`. Nothing needs to change in video_test for this, only the order of the run:
  - remove `video_test probe` (its question is answered; ~25 KB of kept lines);
  - put `video_test scan` **directly after** the rwtouch scenario, before `encode` and `record_test`.
- In-mediaserverd confirmation in the same run: the `-[BWQuickTimeMovieFileSinkNode renderSampleBuffer:forInput:]` count, and
  `lastFileSize` "out:" > 0.

## C. Spec for the next run (vd7) and the encoder (Sonnet-ready)

### C.1 vd7: prove the mediaserverd registration, with the null encoder (~150 lines, all behind `vdiag=1` / `venc=1`)

1. **New `userland/src/venc.c`**, linked into libredwood-media.dylib next to inject_media.c (build.sh line 298):
   - Move the null encoder out of video_test.m (`n_*`, `null_factory`), with the **fixed class table**:
     ```c
     static const struct vt_encoder_class null_class = { 3, { n_start, n_encode, n_complete, NULL, NULL, NULL, NULL, NULL } };
     ```
     The slots are: n_start `(enc, session, dims, x3)`; n_encode `(enc, token, ib, *pts, *dur, props, *flags)` (emits a dropped
     frame); n_complete `(enc, *until)`. Drop `n_prepare`.
   - Log `venc: factory called codec=%4.4s pid=%d` (getpid tells which process), `StartSession %ux%u`, the first 3 EncodeFrame,
     and CompleteFrames.
   - Log every copyProperty / setProperty key, up to 40 lines (they show what CMCapture asks for).
   - copyProperty answers:
     - `SupportedPropertyDictionary`: `{}`;
     - `UsingHardwareAcceleratedVideoEncoder`: false;
     - everything else: -12900.
   - setProperty: accepts everything (returns 0).
   - `venc_register(void)`: `VTRegisterVideoEncoderWithInfo('avc1', {CMClassImplementationID: "com.redwood.videoencoder.null",
     VTRating: 1000}, null_factory)`. Log the status and the `VTCopyVideoEncoderList` count after.
2. **inject_media.c:** in `redwood_media_init`, when `boot_arg_has("venc=1")`, call `venc_register()` once.
3. **inject_media.c, `vd_wrap_sinks`:**
   - replace the four dead entries with the "yes" rows of §B.1: compressor render / `_compresessionSession` / drop, logger,
     coordinator render / stop / terminated, sink render / drop / `lastFileSize`;
   - for the logger, an `extra` that prints `args[0]` as int and `text_of_object(args[1])` (all calls, not only the first 3:
     errors are rare);
   - also print `method_getTypeEncoding(m)` in the "wrapped" line.
4. **video_test.m:**
   - `probe` is no longer run.
   - Add `video_test inproc`: register the null encoder in-process (the same venc.c compiled in, or a copy), then
     `VTCompressionSessionCreateWithOptions` with `AllowClientProcessEncode = true`, 640x480 '420v', 10 frames, CompleteFrames.
     Log the create status and "factory called N" (this re-tests §A.3 in one line).
   - Delete R1-R4 and the shim.
5. **`userland/rc.jobs.mv2`:**
   ```
   /redwood/bin/rwtouch say start wait 85 notify com.redwood.unlock wait 5 say camera notify com.redwood.camera wait 30 say video swipe 0.3 0.80 0.7 0.80 300 wait 5 say rec1 tap 0.5 0.887 wait 6 say rec2 tap 0.5 0.887 wait 10 say end
   /redwood/bin/video_test scan
   /redwood/bin/video_test inproc
   /redwood/bin/video_test encode
   ```
6. **Run:** `SHOTS=... tools/r1-auto.sh vd7 "fbscale=1 r1_clk=1 r1_cpufreq=2200 r1_gpu=680 glyuv=1 cam_fps=10 vdiag=1 vfix=1 venc=1" 300`,
   built with `RC_JOBS=rc.jobs.mv2`.

**vd7 pass criteria:**
- **PV1 (analysis check):** `video_test inproc` shows "factory called 1", StartSession 640x480, 10 EncodeFrame, and
  CompleteFrames. If not, §A.3 is wrong: stop and re-read 0x194cb1b80.
- **PV2 (mediaserverd registration):** "venc: factory called codec=avc1 pid=<mediaserverd>" during `video_test encode`. That
  session is remote; AVAssetWriter should get past -11834 (it may still fail on 0 samples: acceptable).
- **PV3 (Camera):** after rec1:
  - `BWVideoCompressorNode renderSampleBuffer` count > 0;
  - "venc: factory called" from mediaserverd with the Camera's size (gives U4), or a `logErrorNumber` line naming the status;
  - FileCoordinator / sink render counts;
  - a `scan:` line (0 or 1 movie files; either way it is now known).

### C.2 The encoder: `venc_h264.c` on minih264e (spec 1 option b)

Starts after PV2 and PV3 pass. It replaces the null encoder's three slots; the registration stays the same (ID
"com.redwood.videoencoder.h264").

- **Vendoring:** `userland/third_party/minih264e.h` (single header, ~10k lines, CC0/BSD). Build `-Os`.
  - Plain C, or its NEON path if it compiles for arm64 **(guess: the header's SIMD selection macros need checking;
    `H264E_ENABLE_PLAIN_C` / `H264E_ENABLE_NEON` names unverified)**.
  - No threads at first (`H264E_MAX_THREADS` 0) **(guess name)**.
- **Storage** (derived storage size = `sizeof(struct venc)`): session, width, height, the H264E persist/scratch buffers, reusable
  U/V planes, `CMVideoFormatDescriptionRef fmt`, frame count, bitrate, keyframe interval (default 30).
- **Base class:**
  - `invalidate`: free the H264E buffers;
  - `finalize`: release `fmt`;
  - `copyProperty`: `SupportedPropertyDictionary` returns a dictionary keyed by the property keys below, each mapped to `{}`;
    `UsingHardwareAcceleratedVideoEncoder` returns false; anything else -12900;
  - `setProperty`: store `AverageBitRate`, `MaxKeyFrameInterval`, `ExpectedFrameRate`, `RealTime`, `ProfileLevel`,
    `AllowFrameReordering`. Return 0 for every key (a refusal makes CMCapture log errors but not fail **(guess)**).
- **StartSession(enc, session, dims, x3):**
  - `w = (uint32_t)dims; h = dims >> 32`;
  - keep `session`;
  - `H264E_sizeof` / `H264E_init` with w, h, gop = 30, a constant-QP or bitrate mode (bitrate from the property, else
    w·h·fps·0.1);
  - `VTEncoderSessionSetPixelBufferAttributes(session, {PixelFormatType: [420v, 420f], Width: w, Height: h})`;
  - return 0, or -12902 when init fails.
- **EncodeFrame(enc, token, ib, pts, dur, props, flagsOut):**
  1. Lock the base address. Y = plane 0. Deinterleave plane 1 (CbCr) into U/V: 8-bit NEON-able, ~0.6 ms at 720p
     **(guess)**.
  2. Force an IDR when `props[kVTEncodeFrameOptionKey_ForceKeyFrame]` is true, or every gop frames. Then `H264E_encode` gives an
     Annex-B buffer.
  3. Split the NALs. Take SPS(7) and PPS(8) from the first IDR, and create `fmt` with
     `CMVideoFormatDescriptionCreateFromH264ParameterSets(2 sets, 4-byte NAL headers)`. Drop SPS/PPS/AUD from the sample data.
  4. Rewrite the remaining NALs with 4-byte big-endian lengths (AVCC) into a `CMBlockBuffer` (`CMBlockBufferCreateWithMemoryBlock`
     with kCFAllocatorDefault, copied).
  5. `CMSampleBufferCreateReady(fmt, timing {*dur, *pts, dts = *pts})`. Attachments: non-IDR gets `NotSync = true` and
     `DependsOnOthers = true`.
  6. `VTEncoderSessionEmitEncodedFrame(session, token, 0, 0, sbuf)` synchronously, then `CFRelease(sbuf)`. Return 0.
  7. On any failure, emit with status -12902 and NULL, so that the token is never left pending.
- **CompleteFrames:** return 0. Everything is emitted synchronously.
- **Size:** minih264e at `-Os` arm64 is about 60-120 KB of `__TEXT` **(guess)**, glue about 6-10 KB.
  - Measure `obj/libredwood-media.dylib` before and after.
  - The default boot image has 1,681,408 B of headroom and the BT image 1,439,744 B (HANDOFF line 166), compressed. Expect it
    to cost ≤ 60 KB of it **(guess)**.
- **Speed (guess, measure):**
  - one A53 core at 2.2 GHz: 640x480 about 15-25 ms a frame, 1280x720 about 40-70 ms;
  - at cam_fps=10, 720p fits one core (≤ 70 % of it);
  - 1080p (if CMCapture asks for it, U4) does not; force 720p (spec 1 step 5, U11).

**Encoder pass criteria:**
- **E1:** `video_test inproc` with the real encoder, 30 frames at 640x480:
  - 30 samples, the first one sync with format 'avc1' and an avcC extension, PTS strictly increasing;
  - ms a frame logged.
- **E2:** `video_test encode` (AVAssetWriter, remote): status 2, 30 frames appended, file > 0. `scan` then lists 1 video track
  avc1, 30 frames.
- **E3** (spec 1 P2-P4, unchanged): Camera 6 s at cam_fps=10 gives a .MOV with 1 avc1 track, ≥ 40 frames, 5-7 s. No
  relinquish loop, mediaserverd ≤ 250 % CPU, temperature under the 58 C guard.

### C.3 Unknowns (12)

- U1. The meaning of StartSession's 4th argument (a CF object VCPHEVC retains). Ignored, low impact.
- U2. The meaning of the class slots at +0x20 and +0x28. Left NULL; every call site seen tests for NULL **(inference, medium)**.
- U3. Whether the Camera's graph feeds `BWVideoCompressorNode` at all with our stream (vd7 PV3).
- U4. Codec, size and spec that CMCapture asks for: avc1 assumed because the HEVC switches are off; 1080p by default? (vd7: the
  StartSession dims and the set/copyProperty keys).
- U5. `FigServer_IsServerProcess()` false in video_test **(inference, high; vd7 PV1 confirms)**.
- U6. The exact key string VT uses to ask an encoder for its supported properties (vd7 key log; the heuristic "contains
  Supported" works meanwhile).
- U7. minih264e's arm64 SIMD and thread macros, its real size and speed **(guess)**.
- U8. Whether the QuickTime sink accepts our samples (avcC description, attachments) and whether `_verifyMovieTiming` passes at
  10 fps (spec 1 U10).
- U9. Whether vd5 wrote a .MOV at all (vd7 `scan`).
- U10. Whether the file coordinator needs audio, or tolerates none (spec 1 U9).
- U11. CameraUI's 720p30 configuration value (spec 1 U6).
- U12. Whether a refused setProperty makes CMCapture fail the session or only log it **(guess: only log)**. Avoided by
  accepting every key.

## Tools

The scratchpad dir `vr3/` holds `dis3.py IMAGE START END [-q]`. It is spec 2's `dis2.py` for any image, and it names cache stub
islands (`adrp/add/br`), which dis2 got wrong. It keeps its export table in `exports.pkl` (120 MB): that file was deleted, and
`dis3.py` rebuilds it in about 5 s. `vr3/callsto.py IMAGE ADDR...` lists the direct and stub callers. Neither is in the repo.
