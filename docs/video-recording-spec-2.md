# Video recording on the r1: spec 2 (after vd0/vd1)

Status: analysis only, nothing implemented. Written 2026-11-18 from `logs/vd0.log`, `logs/vd1.log`, `logs/mv2.log`,
`userland/src/camera_device.c`, the iOS 16.5 (20F66) cache in `ios165/20F66__iPhone10,3_6` (read with `tools/dsc` plus
capstone), and `ios165/rootfs/System/Library/PrivateFrameworks/CMCapture.framework/D22/AVCaptureSession.plist`.
Addresses are unslid cache addresses. **(guess)** marks anything that was not read from a binary or a log.
The rules of HANDOFF-2026-11-16.md and of `docs/video-recording-spec.md` (spec 1) still apply.

## 0. Summary

- **Two corrections to what vd0/vd1 were read as showing:**
  - "VideoOutputHandlers never set again" is an artifact of the logging. `trace_key()` (camera_device.c:95) stops logging a key
    after 8 lines. The Photo session already used all 8 for `stream set: VideoOutputHandlers` (vd0:1585-1597).
    mv2's *second* Photo session (130.39 s) worked and shows no handler line at all, so a missing line does not mean a missing set.
  - Spec 1 said that in mv2 "Video mode restarted the stream and the preview ran: 22 pictures". That was wrong. The 22 were
    Photo-mode pictures from the 10-second window 139-149 s. From 143.6 s on, every frame line says `delivering 0`. In mv2,
    as in vd0/vd1, the loop starts the moment Video mode is selected (mv2:2194-2196).
- **Part A (capture graph).** The graph fails inside one function, `-[BWMultiStreamCameraSourceNode configure:]`. The window is
  between `setActiveFormatIndex:` (our `stream set: FormatIndex` / `format 1`) and the `getProperty: VideoOutputConfigurations`
  that comes right after it in Photo mode. Our stream is not called inside that window, so the refusal is CMCapture's own
  decision, based on what our format and outputs advertise compared with what the Video configuration needs. The only code that
  can fail there:
  1. the scaler assignment (0x1a1f33f64, returns -12780);
  2. a GDC check that throws `"Cannot have two streaming outputs AND GDC enabled without scaler chaining."`;
  3. (unlikely) a nondisruptive-switching `setPropertyIfSupported:`.

  All three are Video-only because Video enables a **second streaming output** (`videoCaptureOutputEnabled`) next to the preview.
- **Part B (encoder).** `-12908` is **kVTCouldNotFindVideoEncoderErr**, not "not available now" (that is -12915).
  - In the cache, only VideoToolbox emits it: its encoder-selection routine 0x194eb3d2c, plus `VTCopyVideoEncoderList` and
    `VTCopySupportedPropertyForVideoEncoderID`. Neither VideoProcessing nor VCPHEVC contains the constant.
  - In video_test, ours was the **only** avc1 encoder in the process: the list at start has only h263/jpeg/sjpg. Even so, the
    no-spec session failed asynchronously with -12908.
  - So the refusal is most likely **on VT's side**: selection filters out our registry item, or finds no factory pointer in it.
    The encoders' StartSession is probably never reached.
  - That means option (b) (our own minih264e plug-in) registered the same way would **also** get -12908. The next run has to
    prove the registration path with a null encoder before any codec work.
- **Recommendation:** one combined run, `vd2` (§3), at about 2 KB of logging/probe code. Then:
  - fix the graph (most likely a VIS/GDC/pixel-format answer, a few lines);
  - fix the registration (an info-dict variant);
  - then build (b) on the variant that works. Apple's encoders get only the free side-probe in vd2.

## A. Capture graph: why Video mode loops

### A.1 What the logs show (vd0; vd1 and mv2 are the same)

Photo, a working session (vd0):
```
1601 stream set: DeferAdditionOfAttachments
1603 stream set: FormatIndex
1604 stream: format 0
1605 stream get: VideoOutputConfigurations        <- configure: reached function C (0x1a21078a0)
1606 stream get: SupportedColorSpaces
1611 processing_session_create: configuration { SessionType = 4; }
1613.. MaximumAllowedFrameRate, Min/MaxFrameRate, ..., VideoOutputConfigurations set, VideoOutputsEnabled, START
```
Video, every iteration (~70 a second):
```
2082 stream set: DeferAdditionOfAttachments
2083 stream set: FormatIndex
2084 stream: format 1
2085 the StillImage output: not rendered (the preview only)
2086 device set: ClientIsActive
2087 device: streams given back (1)                 <- graph torn down; next line: Clock, request_streams again
865/921/... streams given back 2000/3000/... times
```
mv2:2194-2196 (no vdiag code at all) shows exactly this. So the vdiag hooks are not the cause.

Other facts:
- No `stream: no answer for`, `device: no answer for`, or new `set (not used)` key appears in Video mode (all three logs).
  So the graph asks our objects for nothing new and refuses on its own.
- No widened os_log line appears either. The error path in question logs through FigDebugAssert, not through the formats we
  filter.
- CameraUI's storage check answered YES (vd0:2049-2050, 2119-2120): it is not the blocker.

### A.2 Where in CMCapture (read from the cache)

- `-[BWMultiStreamCameraSourceNode configure:]` (0x1a1f631b0) calls the configure body 0x1a1f330c8 (at 0x1a1f632c0) and keeps its
  status. A nonzero status fails the graph.
- The body 0x1a1f330c8 runs, in order:
  1. 0x1a1f4e8ac: `-[BWFigVideoCaptureStream setActiveFormatIndex:]` (0x1a1f40c3c, the **only** caller). This is our
     `stream set: FormatIndex`. It returns 0 here because our setter accepts index 1.
  2. **A** 0x1a21077e0: `activeStreamingNondisruptiveSwitchingFormatIndices` /
     `activeUltraHighResStillImageNondisruptiveSwitchingFormatIndices`. Only if either is non-empty, it calls
     `setPropertyIfSupported:` on the stream. That key is not in our SupportedPropertiesDictionary.
  3. **B** 0x1a1f374f8 = **B1** 0x1a1f33f64 (scaler assignment, `-12780` at 0x1a1f342c8 / 0x1a1f342d0; throws "Cannot enable
     two streaming outputs on the IR camera" for IR) followed by B2 0x1a210a044 (still-image output names, always 0).
  4. If `previewOutputEnabled && videoCaptureOutputEnabled && geometricDistortionCorrectionOnStreamingOutputsEnabled`, it calls
     0x1a1f86504. If that returns false, it **throws** NSInternalInconsistencyException "Cannot have two streaming outputs AND GDC
     enabled without scaler chaining." (0x1a1f33f1c).
  5. **C** 0x1a21078a0: `visOverscan`, `sensorOverscan`, then `getProperty:` **VideoOutputConfigurations** (0x1a210793c). That
     is the first get we see in Photo mode.
- In Video mode, 5 is never reached, so the failure is in 2, 3 or 4.

B1 (0x1a1f33f64) decides which stream output (`PrimaryScaler` / `SecondaryScaler` / `IntermediateTap`) feeds the preview and which
feeds the video capture output. Its inputs:

| reg | from | meaning (read from the code; names partly **guess**) |
|---|---|---|
| w24 | `[cfg previewOutputEnabled]` | |
| w26 | 0x1a1f3e238 | another streaming output is on: videoCapture / depth / still checks |
| w27 | 0x1a1f856ac | PrimaryScaler is in the active format's `SupportedOutputs` (plus depth / `secondaryScalerIsNotAvailable` / bravo / `primaryScalerDecoupled` / GDC conditions) |
| w25 | 0x1a1f422e4 | SecondaryScaler is in `SupportedOutputs` and `!secondaryScalerIsNotAvailable` |
| w21, w22, w23 | 0x1a21097d4(name) for Primary, Secondary, IntermediateTap | "this output can serve the client" |

0x1a21097d4(name) returns NO when any of the following holds:
- name ≠ PrimaryScaler and `[cfg visionDataConfiguration]` is set;
- name ≠ PrimaryScaler and **`[cfg videoStabilizationEnabled]`** is YES;
- name = SecondaryScaler and 0x1a1f3d61c fails. That check compares the required dimensions with `NativeWidth/Height ×
  MaxScaleFactor` of the output;
- GDC on streaming is on, and the output's dictionary lacks **`GeometricDistortionCorrectionSupported`** = true;
- the output's **`SupportedPixelFormats`** has no entry whose three attributes (0x1a21773e0 / 0x1a21775e4 / 0x1a217797c) equal
  those of **`[cfg clientVideoFormat]`**.

With preview and video capture both on, B1 returns -12780 when:
- Primary is unusable (w21 = 0) and so are Secondary and IntermediateTap; or
- GDC is on with some combinations; or
- neither scaler is "available" (w27 = w25 = 0).

### A.3 The three most likely causes

**Cause 1: B1 rejects PrimaryScaler for the Video client format (pixel-format family), and Secondary / IntermediateTap are
excluded by VIS.**
- Every Video format of D22 has `VideoStabilizationSupported = true` (1080p30, 720p30, 4k30 ...; 1080p30 is the
  `AVCaptureSessionPresetHigh` default). Camera turns stabilization on in Video mode, so only PrimaryScaler can qualify.
- PrimaryScaler then qualifies only if `clientVideoFormat` matches one of our `SupportedPixelFormats` ('420f', '420v', 'BGRA'
  in `output_info()`).
- The Video client format may be a 10-bit or packed format (D22 has `displayPipeRequiresPacked10BitPixelFormatForP3Input =
  true`) **(guess)**.
- Supporting lines: vd0:2083-2087 (fails right after FormatIndex, before function C); Photo vd0:1603-1606 (only one streaming
  output, so B1 takes the single-output path).
- Candidate fixes (camera_device.c, `output_info()`), chosen by the vd2 values:
  - (a) If `clientVideoFormat` is a 4cc we lack, add it to `SupportedPixelFormats` of PrimaryScaler and SecondaryScaler, and
    make `output_buffer()` / `render_piece()` render it. For 'x420' (10-bit biplanar video range), render 420v data into 16-bit
    samples (<< 8), about 20 lines in camera_isp.c. This is the expensive variant. Prefer (b).
  - (b) In the Camera app, keep the client format 8-bit. Hook the CameraUI / AVCapture setter that picks the format: an
    `activeFormat` with 420v. This needs the vd2 value first.
  - (c) Turn VIS off: in the Camera branch of inject_media.c, wrap `-[AVCaptureConnection setPreferredVideoStabilizationMode:]`
    and pass 0 (Off). This frees Secondary and IntermediateTap and removes BWVISNode. That node needs gyro motion data and Metal,
    which we do not have, so it would fail later anyway **(guess, high)**. Do (c) whatever vd2 shows.

**Cause 2: the GDC exception (step 4) or the GDC branches of B1.**
- GDC on streaming is set in 0x1a1f639d0 (`setGeometricDistortionCorrectionOnStreamingOutputsEnabled:` at 0x1a1f643f0) as
  `GDCsupported && wantedForStreaming`.
- `GDCsupported` comes from the FigCaptureSource's `copyProperty` (key `GeometricDistortionCorrection`, a dictionary of booleans),
  which Apple's tuning for sensor 0x0343 may answer. It does not come from our stream, which explains why no new key shows in
  our log.
- If it is on, PrimaryScaler needs `GeometricDistortionCorrectionSupported` in its output dictionary, which we do not provide.
  With two streaming outputs, the "no scaler chaining" exception fires.
- Supporting lines: same window, plus no crash. mediaserverd keeps the same `relinquishes` counter, so if this is the path, the
  exception is caught upstream **(guess)**.
- Candidate fixes:
  - (a) In camera_device.c `output_info()`: `put(o, "GeometricDistortionCorrectionSupported", CFRetain(kCFBooleanTrue));`
    for each output. It is harmless when GDC is off, and our pictures are scaled, not distortion-corrected, so a small geometry
    error is acceptable.
  - (b) If vd2 still shows the exception: in mediaserverd, wrap
    `-[BWMultiStreamCameraSourceNodeConfiguration geometricDistortionCorrectionOnStreamingOutputsEnabled]` (IMP 0x1a1f41dec) to
    return NO.

**Cause 3: the scaler-availability flags (w27 = w25 = 0, or Secondary failing the size check).**
- 0x1a1f3d61c requires `NativeWidth/Height × MaxScaleFactor` ≥ the required output size. We give MaxScaleFactor 1 and Native =
  sensor (4224x2376 for format 1), so this should pass. A required size above the sensor's (VIS overscan on 4k) would fail it
  **(guess, low)**.
- The other flag is `secondaryScalerIsNotAvailable`, set at 0x1a1f6443c from the session's own state.
- Candidate fixes: `MaxScaleFactor` 2 in `output_info()`; or answer for format 1 an output list without SecondaryScaler, so that
  w25 = 0 and B1 takes the Primary-only branch.

(The nondisruptive-switching path A is listed for completeness only: D22's back camera has no switching formats, so the arrays
are empty **(guess)**.)

### A.4 The smallest logging that settles it in one run

All of it goes in the existing vdiag block of inject_media.c (mediaserverd), reusing the generic method wrapper:

1. `-[BWMultiStreamCameraSourceNode configure:]`: log the return value (an int status; log the type encoding once). For the
   first 4 calls, also log `[[arg description] substringToIndex:300]`.
   - Nonzero in Video mode confirms §A.2. -12780 points to B1.
2. Getters of `BWMultiStreamCameraSourceNodeConfiguration`, each distinct `(selector, value)` logged once:
   - `previewOutputEnabled`, `videoCaptureOutputEnabled`, `videoStabilizationEnabled`, `visionDataConfiguration` (nil or not);
   - `geometricDistortionCorrectionSupported`, `geometricDistortionCorrectionOnStreamingOutputsEnabled`;
   - `secondaryScalerIsNotAvailable`, `primaryScalerDecoupled`, `optimizeBandwidthForPreviewOutput`, `depthType`;
   - `clientVideoFormat`: log the raw return value as a 4cc **and** as a pointer; the type encoding says which it is.

   About 15 lines. With them, B1's branch can be worked out by hand from the table in §A.2.
3. `+[NSException exceptionWithName:reason:userInfo:]` in mediaserverd: log name and reason, first 10 calls. This catches
   Cause 2's throw.
4. camera_device.c `trace_key()`: raise the cap from 8 to 24 when `vdiag=1` (one condition), so that the Video iteration's own
   property traffic is visible.

## B. Encoder: what -12908 means here

### B.1 What VT does (VideoToolbox, read from the cache)

- `VTRegisterVideoEncoderWithInfo` (0x194eb3870) builds a FigRegistry item and adds it to the registry at `0x1dd401628`. The item
  holds: CMClassID, ImplementationName, ID (CMClassImplementationID), Version, CMMatchingInfo with VTCodecType, the factory, and
  for `h264.gva` an `IORegistryRequiredKey = IOGVACodec`. `VTCopyVideoEncoderList` reads the same registry, which is why our
  entries are listed.
- Selection (0x194eb3d2c, the only VT routine besides the two above that emits -12908):
  - Reads the spec: `EncoderID`, `RequiredLowLatency`, `EnableLowLatencyRateControl`, `RequireHardwareAcceleratedVideoEncoder`.
  - Queries the registry with the matching callback 0x194eb4a10. That callback compares `VTCodecType` and checks
    `VTIsParavirtualized` / `AllowParavirtualizedEncoder` and `VTIsHardwareAccelerated` / `VTIsWrapperEncoder`. HW-flagged items
    are dropped only if a `VTApplyRestrictions` bit set byte `0x1dd401655`. It then checks properties (`RequiredPropertySupport`).
  - No match: **-12908** (0x194eb40fc / 0x194eb4138).
  - For each candidate, the factory pointer is fetched from the item. If `VTFactoryFunctionHasSpecificationArgument` = true it
    is called with 5 arguments; otherwise with 3, (codec, allocator, &encoder). No pointer: **-12908** (0x194eb4340).
  - A factory that returns success with a NULL instance: -12912.
  - For 'avc1'/'mp4v', with a CMSession, VT first takes `kCMSessionResource_VideoEncoder` (errors -12983/-12985 map to
    **-12915**, "not available now").
- StartSession is called later by the session code, not by the selection routine. An error there would surface with the
  encoder's own status: VideoProcessing's StartSession (0x1b053d1d8) returns -108 or -12904. Neither encoder image contains
  -12908 (checked by scanning both `__text`s for the constant).

### B.2 What vd0/vd1 therefore say

- **a1 / a2 with EncoderID: -12908 at create.** Either no item matched the spec, or the matched item gave no factory. Not the
  encoder.
- **a1 with no spec: create 0, frames accepted, `cb status -12908`, `CompleteFrames -12782` (the CMBaseObject
  "unsupported operation"), 0 samples.**
  - Encoder creation was deferred, and the deferred selection also failed.
  - ~6 ms of CPU a frame including the gradient shows nothing encoded.
  - Our item was the *only* avc1 entry in video_test, since the start-of-probe list had none.
- **AVAssetWriter -11834 / -12908**: the same selection failure, in-process (no rtvc in video_test).
- Conclusion **(inference, medium-high)**: VT's selection never called our factory.
  - Suspects: the info dictionary our probe passed (`IsHardwareAccelerated = 1` steers the item through the HW / wrapper branch
    of the matcher), the missing `VTFactoryFunctionHasSpecificationArgument`, or how VT stores the factory for a runtime
    registration.
  - The cause sits in VT's handling of our registration, not in the Apple encoders. **Option (b) through the same call with
    the same info would fail identically.**
  - The test that separates the two is "is our factory called?" (§3 B2-B4).

### B.3 Forcing VideoProcessing's H.264 into software; calling StartSession directly

- Forcing software through keys (`Usage`, `iChatUsageString`, FaceTime / ScreenSharing usage, an 'ftvc' codec, a
  "UseSoftwareEncoder"-style key): none of these matters until VT calls the factory. Its create stores the 4cc unchecked
  (+0x1384), and StartSession only builds pixel-buffer attributes.
  - Its hybrid `VPHardwareEncoder*` path may itself create an inner VT session on AVE, which would fail with -12908 the same way
    **(guess)**. If so, a1 is dead on this hardware.
  - No key names are verified here. Do not spend a run on them.
- Calling StartSession directly through the vtable (`CMBaseObjectGetVTable(instance)->derived->StartSession`) without VT:
  - it needs a `VTEncoderSessionRef`. With NULL, the encoder's `VTEncoderSessionSetPixelBufferAttributes` returns a parameter
    error (or the encoder crashes);
  - so it would tell only whether the encoder's own setup passes, which is already likely. Low value.
  - The **vtable shim** in §3 B2 tells more: it sees VT's real StartSession, PrepareToEncodeFrames, EncodeFrame and
    CompleteFrames calls with VT's real session.
- Vtables, for the shim and for option (b): `{reserved, base_class*, encoder_class*}`.
  - VCPHEVC 0x1e246ed40: base `{1, 0x5b0, NULL, invalidate, finalize, copyDebugDesc, copyProperty, setProperty, ...}`, encoder
    class `{3, 0x1beb968b0, 0x1beb96988, 0x1beb973e8, 0x1beb97cd0, 0...}`.
  - VideoProcessing H.264 0x1d898cf60: class version 1, same four slots.
  - The four slots are StartSession, PrepareToEncodeFrames, EncodeFrame, CompleteFrames **(order a guess from the public
    VTVideoEncoder layout; confirm with the shim: StartSession is the call with (session, codec, w, h))**.
  - VCPHEVC also has a stderr log level at `0x1e246fef8` (+slide, currently 3). Writing 7 there in video_test makes it verbose
    for free.

### B.4 Go / no-go

- **No-go on further work for Apple's encoders as the main path.**
  - The blocker sits before them. a1 likely ends on AVE anyway **(guess)**. a2 (HEVC software) is too slow on A53 for real time
    (spec 1 estimate, unmeasured).
  - They stay as a free side-probe in vd2 (same shim, two more registrations).
- **Go for option (b), with a gate.**
  - vd2 must show a **null encoder** registered by one of the variants R1-R3 below that gets its factory and StartSession
    called and emits frames.
  - The null encoder is (b)'s skeleton (vtable, StartSession, pixel-buffer attributes, emit), so nothing is thrown away.
  - If no variant reaches the factory: the problem is VT's runtime registration. Next options are (i) the bundle route: an
    encoder bundle with `CMClassImplementations` in its Info.plist where VT scans `/System/Library/VideoEncoders`; whether
    root.apfs can carry that path is unknown. Or (ii) a direct FigRegistry item copying the fields of a working built-in one.
    Either costs a run **(guess)**.
- What separates "VT session creation" from "the encoder": whether the **factory log line** appears.
  - No line: VT-side. (b) blocked the same way.
  - Line, then a failing StartSession, PrepareToEncodeFrames or EncodeFrame in the shim: encoder-side. (b) unaffected.

## C. Next steps for a Sonnet agent

### C.1 One combined diagnostic run "vd2"

Everything goes behind `vdiag=1`. Logs only, except video_test's in-process registrations.

**A-part (mediaserverd, inject_media.c vdiag block; camera_device.c one line):**
- A1-A3 as in §A.4: wrap `configure:`, the getters, and `+[NSException exceptionWithName:reason:userInfo:]`.
- A4: the `trace_key` cap goes to 24 under vdiag.
- Also apply candidate fix **2a** in the same build (add `GeometricDistortionCorrectionSupported = true` to `output_info()`).
  It is harmless and saves a run if GDC is the cause. Mark it in the log ("vd2: outputs say GDC supported") so that the reading
  stays unambiguous.

**B-part (video_test `probe`):**
- B1. Read and log the VT restriction bytes at 0x1dd401650..0x1dd401655 (+cache slide, via `_dyld_get_shared_cache_range` or
  the slide helper the repo already has).
- B2. A **wrapper factory** for each candidate:
  - log `factory called: codec=%4.4s` first;
  - call the real factory;
  - find the vtable pointer inside the instance (scan its first 64 bytes for the value of `CMBaseObjectGetVTable(instance)`);
  - swap in a copy whose encoder-class slots 1-4 and base `copyProperty` / `setProperty` log (slot, key or arguments, status)
    and forward. First 3 calls per slot, then every 30th.
- B3. A **null encoder** (~120 lines, the start of venc.c):
  - base: `copyProperty` answers `SupportedPropertyDictionary` with `{}` and everything else with -12782; `setProperty` takes
    all;
  - class version 3;
  - StartSession: `VTEncoderSessionSetPixelBufferAttributes(session, {PixelFormatType: [420v, 420f]})` and return 0;
  - PrepareToEncodeFrames: 0;
  - EncodeFrame: `VTEncoderSessionEmitEncodedFrame(session, token, 0, kVTEncodeInfo_FrameDropped, NULL)` and return 0;
  - CompleteFrames: 0.
- B4. **Registration matrix**, each variant with its own EncoderID:
  - R1 `VTRegisterVideoEncoder(codec, factory)` (no info);
  - R2 info `{CMClassImplementationID, Rating 100}`, **no** IsHardwareAccelerated;
  - R3 = R2 + `IsHardwareAccelerated = true` (the vd1 control);
  - R4 = R2 + `VTFactoryFunctionHasSpecificationArgument = true` with a 5-argument wrapper `(codec, implID, spec, allocator,
    &out)` **(argument order a guess from the 0x194eb4338 call: x0 codec, x1 implementation ID, x2 dict, x3 allocator, x4 out)**.

  Run it for the null encoder (codec 'avc1'), then for a1 ('avc1') and a2 ('hvc1'). For each: create 640x480 '420v' with the
  EncoderID spec, then without a spec. Log the create status, whether the factory was called, the shim lines, 10 frames,
  CompleteFrames, and the number of samples.
- B5. Then `write_one` avc1 (AVAssetWriter) with whatever variant worked. Status 2 means app sessions are in-process here.
- B6. In video_test: write 7 to VCPHEVC's log level (0x1e246fef8 + slide) before a2, so its stderr explains itself.

**Run:** `SHOTS=... tools/r1-auto.sh vd2 "fbscale=1 r1_clk=1 r1_cpufreq=2200 r1_gpu=680 glyuv=1 cam_fps=10 vdiag=1" 400`.
Same Camera scenario (camera, video, rec1, rec2).

### C.2 Pass criteria for vd2 (that is, "the run decided it")

- **PA1:** a `configure:` status line for the Video configuration, plus the getter values. Either the status is nonzero with a
  value, so the B1 branch can be worked out from §A.2, or an NSException reason is logged.
- **PA2 (bonus):** if fix 2a alone fixed it, there is no "given back" loop after `!auto: video`, there are PrimaryScaler pictures
  in the next 10 s window, and `delivering 1`.
- **PB1:** for each of R1-R4 with the null encoder, one of three lines: "factory called" yes/no, then StartSession status, then
  samples or dropped-frame callbacks for 10 frames.
- **PB2:** the a1/a2 shim lines say which slot fails, if the factory is reached at all.

### C.3 Then, in this order

1. **Graph fix** (depends on PA1):
   - Always: Camera branch wrap `setPreferredVideoStabilizationMode:` → Off (fix 1c).
   - Plus the fix of the cause vd2 named: 1a/1b (pixel format), 2a/2b (GDC), or 3 (MaxScaleFactor / Primary-only outputs for
     format 1).
   - Pass: Video-mode preview pictures > 0 and no relinquish loop for 30 s.
2. **Movie path up to the encoder:**
   - Run once with `vdiag=1` and the fix. Expect `-[AVCaptureMovieFileOutput startRecording...]` and mediaserverd's
     `VTCompressionSessionCreate` lines (spec 1 §3.1 A2), which never appeared because the graph never got there.
   - This gives the real codec, size and spec dictionary (U1 of spec 1).
3. **Encoder registration in mediaserverd:** `venc.c` with the variant that passed PB1, behind `venc=1`, called from
   `redwood_media_init` (spec 1 §3.3 step 1). At first with the null encoder, to watch the sink path take frames: expect
   `_writeBuffer` counts and a movie with no video samples.
4. **Option (b):** minih264e behind the null encoder's EncodeFrame (spec 1 §3.3 step 6): NV12 → I420, Annex-B → AVCC,
   `CMVideoFormatDescriptionCreateFromH264ParameterSets`, IDR every 30 frames.
5. **Size:** force 720p30 or 480p through `-[CAMUserPreferences videoConfiguration]` (`q16@0:8`, logged in vd0). The enum value
   for 720p30 has to be read from CameraUI first.
6. Runs vd3 (6 s recording, spec 1 P2) and vd4 (60 s, P3/P4). The spec 1 pass criteria are unchanged. Storage needs no override
   (YES both times).

### C.4 Unknowns (9)

- U1. Which branch of the configure body fails, and with what status (B1 -12780, the GDC throw, or path A)? vd2 PA1.
- U2. What `clientVideoFormat` Video mode asks for (8-bit 420v or a 10-bit / packed one)? vd2 A2.
- U3. Whether GDC on streaming is on for sensor 0x0343 in Video mode, and whether its exception is caught upstream. vd2 A2/A3.
- U4. Whether turning VIS off is enough by itself, and whether BWVISNode would otherwise fail later (needs gyro and Metal)
   **(guess: yes, it would)**.
- U5. Why VT's selection does not reach our factory: info keys (HW flag / spec-argument flag) versus the runtime-registration
   storage. vd2 PB1 R1-R4.
- U6. If no R variant works: whether a bundle in a VideoEncoders path or a hand-made FigRegistry item is reachable from
   root.apfs.
- U7. The real encoder-class slot order and argument layout (shim confirms).
- U8. Whether VideoProcessing's H.264 needs AVE internally (decides a1 for good). vd2 PB2.
- U9. Carried over from spec 1: the movie sink's timing with 10 fps (U10), silent audio (U9), and the 720p enum value (U6).
   Not reachable until U1 is fixed.

Tools left in the scratchpad: `vr2/dis2.py` (CMCapture disassembly naming objc selectors and doubly-indirect CFString keys),
`vr2/selcallers.py` (callers of a selector), `vr2/blto.py` (direct callers), `vr2/methimp.py` (method → IMP),
`vr2/vt12908.py`. They are not in the repo. Promote them to tools/dsc if useful.
