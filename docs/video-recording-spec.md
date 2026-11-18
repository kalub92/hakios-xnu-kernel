# Video recording on the r1: spec

Status: analysis only, nothing implemented. Written 2026-11-18 from the iOS 16.5 (20F66) dyld cache in
`ios165/20F66__iPhone10,3_6`, `logs/mv2.log` and the userland sources. Addresses are unslid cache
addresses; `tools/dsc/*.py` reads them. **(guess)** marks anything not read from a binary or a log.

The rules in HANDOFF-2026-11-16.md still apply: no stay images in unattended runs, no `emmc_write=` or
`var_store=`, the system image on eMMC is not rebuilt, and everything new goes into the boot image's
`root.apfs` (1,435,648 B of headroom, zlib).

## 0. Summary

- **There is no registered H.264 encoder. The software encoders exist in the cache, though: their
  registration is switched off on iOS.**
  - `H264SW.videocodec` exports `_VCPAVCRegisterEncoder` (0x1f0353110), but the function is a bare `ret`.
    The image holds only the H.264 *decoder* (every string in it belongs to the decoder).
  - `VCPHEVC.videocodec` exports `_VCPHEVCRegisterEncoder` (0x1beb8fd80), also a bare `ret`. It does still
    export `_HEVCVideoEncoder_CreateInstance` (0x1beb967a0), a full multithreaded **software HEVC**
    encoder (`<LRPVideoEncoder %p>`, wavefront/threads/rate-control strings). Its decoder half already
    decodes HEIC for us in mediaserverd.
  - `VideoProcessing.framework` exports `_H264VideoEncoder_CreateInstance` (0x1b053d054), FaceTime's
    "H.264-legacy" JVTLib **software/hybrid H.264** encoder. Its own callers create it with codec type
    `'ftvc'`. It has `VPHardwareEncoder*` hybrid paths and a "usage" (FaceTime / ScreenSharing).
    `VPModuleInitialize` registers only a hidden `'avc1'` passthrough wrapper
    (`com.apple.videotoolbox.videoencoder.h264.rtvc`, rating -1, Hide, IsHardwareAccelerated).
  - The hardware ones are `H264H9.videoencoder` and `H9.videoencoder` (AVE, iPhone X) in the cache, and
    `AppleVideoEncoder.bundle` on rootfs (arm64e only). None of them can work here.
- **VideoToolbox lets anyone register an encoder.** `VTRegisterVideoEncoderWithInfo(FourCharCode codec,
  CFDictionaryRef info /*may be NULL*/, factory)` is at 0x194eb3870. Its first step is VT's own built-in
  registration (a once-block), so registering later is safe. `VTRegisterVideoEncoder(codec, factory)`
  (0x194ca9744) is the same call with NULL info.
  - The info keys are exported: `kVTVideoEncoder_Rating`, `_Hide`, `_IsHardwareAccelerated`,
    `_EncoderNameKey`, `_CodecNameKey`, and `CMClassImplementationID`.
  - VideoProcessing builds its own info this way, so `IsHardwareAccelerated` is simply a value we
    choose. That defuses any `RequireHardwareAcceleratedVideoEncoder` the capture path might set.
- The plug-in API for a home-made encoder is also exported: `VTEncoderSessionEmitEncodedFrame`,
  `VTEncoderSessionCreateVideoFormatDescription`, `VTEncoderSessionSetPixelBufferAttributes`,
  `VTEncoderSessionCreateCMBlockBuffer`, and `VTVideoEncoderGetClassID` for the CMBaseClass vtable. The
  repo already builds CM derived objects (`CMDerivedObjectCreate`) in camera_device.c.
- **Today's failure in Camera is upstream of the movie file.** In mv2, Video mode restarted the stream
  (format 1 at 141.93 s) and the preview ran: 22 PrimaryScaler pictures before 149 s.
  - After the record tap (`!auto: rec1` at 147.05 s) no picture is delivered again ("PrimaryScaler 0",
    "delivering 0"). The graph then loops, requesting and giving back the streams: "streams given back
    1000 times" by 163 s and 8000 by ~230 s (~70 a second, mediaserverd at ~104 % of a core).
  - The microphone was never opened: no "the microphone is listened to" line between 141 s and 163 s.
  - Inference (not proven): starting the recording created the movie sink's compressor, the
    VTCompressionSession failed (-12908 or similar), and the graph tore down and keeps retrying. HANDOFF
    also lists a Video-mode "Storage Full" in CameraUI, which can refuse a recording before it starts.
- **Recommendation:**
  1. Run one diagnostic first (§3.1). It costs a few KB and decides everything below.
  2. Then register an Apple in-cache encoder from libredwood-media (option a). That is ~2 KB of code and no
     codec bytes.
  3. If the diagnostic shows neither Apple encoder is usable or fast enough, plug a small baseline
     encoder (minih264e) into the **same** registration point (option b, ~40-60 KB compressed).
  4. The hardware VENC (option d) is out of reach.

## 1. The pipeline end to end, on this port

### 1.1 What Camera asks for

1. CameraUI checks space before it starts: `-[CAMStorageController
   hasDiskSpaceToAllowCaptureWithConfiguration:allowPurging:verbose:]` and
   `minAvailableBytesNeededForCaptureForWidth:height:useHEVC:`.
   - /private/var is a ramfs: about 248 MB with about 77 MB free (inject_media.c `has_space_to_copy`).
     MobileGestalt's AmountDataAvailable is therefore 0.
   - This is the likely cause of the "Storage Full" listed in the handoff **(guess)**.
   - Our existing hook `+[PLFileUtilities hasDiskSpaceToCopyFileAtURL:]` covers PhotoKit's import, not
     this check.
2. Camera adds an `AVCaptureMovieFileOutput` (plus the microphone's `AVCaptureDeviceInput`) and calls
   `startRecordingToOutputFileURL:recordingDelegate:`. The codec comes from CameraUI's HEVC preference.
   - `no_hevc_in_camera_ui()` (inject_media.c) answers NO to `isHEVCEncodingSupported` and
     `_preferHEVCWhenAvailable`, so Camera asks for **H.264 ('avc1')** for video too **(guess, but
     consistent with stills going to JPEG)**.
3. The resolution comes from Camera's video configuration (default 1080p30 on an iPhone X). See §1.5.

### 1.2 What mediaserverd builds (CMCapture, all in mediaserverd)

- `FigCaptureMovieFileSinkPipeline` / `FigCaptureMovieFileSinkHeadPipeline _buildMovieFileSinkHeadPipeline:
  videoSourceCaptureOutput:audioSourceCaptureOutput:...` builds this chain:
  `BWFileCoordinatorNode` (video + audio inputs) -> `BWVideoCompressorNode` (VTCompressionSession,
  "compression session queue", failure string "VideoCompressorFailure") -> `BWQuickTimeMovieFileSinkNode`
  (`_doStartRecordingAtTime:withSettings:sensorVideoPort:`, `_writeBuffer:forInputIndex:`,
  `_doEndRecordingAtTime:earlyTerminationErrCode:`).
- **The video source is the same capture output that feeds the preview:** the stream's `PrimaryScaler`
  output, fanned out inside the graph. The head pipeline takes `videoSourceCaptureOutput`, and the
  iPhone's ISP has no separate "record" output. So "camera_device.c delivers only the preview stream" is
  **not by itself a blocker** **(guess, high confidence; the diagnostic's `_writeBuffer` count confirms
  it)**.
  - The `StillImage` output is not needed for video.
  - Pixel formats are what the graph puts in `VideoOutputConfigurations` / the BufferPool: '420f' or
    '420v' biplanar (camera_device.c already serves both, and BGRA). The encoder must accept 420v/420f
    (NV12).
  - The frame rate is whatever the stream delivers: `cam_fps=10` gives a 10 fps movie.
    `BWFileCoordinatorNode` has a variable-frame-rate path ("Recording with variable frame rate video
    enabled").
- The compressed file is written by mediaserverd, then Camera / assetsd import it into DCIM. The import
  runs `hasDiskSpaceToCopyFileAtURL:`, which is already hooked. Thumbnails need an H.264 decode, and the
  H264SW decoder is registered (in VT's decoder table: `H264SW.videocodec` / `VCPAVCRegisterDecoder`).

### 1.3 Audio

- The microphone does not work (no uplink driver), but the audio plug-in **does** deliver a running
  input of zeros. `record_test 4` got 192000 samples at peak 0 through the whole AVAudioSession stack.
  A Camera recording should therefore get a **silent** AAC track.
  - AAC encoding is software (AudioToolbox AudioCodecs) **(guess, high confidence)**. CMCapture sets
    `AVAudioEncoderSpecificationUseHardwareEncoderIfAvailableKey`, which falls back to software.
- A video-only .MOV is valid. If the audio input stalls the file coordinator (it waits for both inputs
  to start), the fallback is to keep Camera from adding the audio input (§3.3 step 7). Not expected to
  be needed **(guess)**.

### 1.4 Where the encoder is looked up

1. `BWVideoCompressorNode` calls `VTCompressionSessionCreate(w, h, codec, encoderSpecification, ...)`.
2. VT walks its registry: built-ins from the table at 0x194fbdd90..0x194fbe180 (H264H9, H9, HEVCH9, JPEGH1,
   VCH263, VideoProcessing `VPModuleInitialize`, ProRes...), the bundles under
   /System/Library/VideoEncoders (`CMClassImplementations` in Info.plist), and anything registered at
   run time.
3. Whether CMCapture passes `RequireHardwareAcceleratedVideoEncoder` is **not determinable statically
   here**. The cache's GOT slots read back as zeros with tools/dsc, so the reference cannot be traced.
   The diagnostic logs the actual specification dictionary. It does not matter if our registration says
   `IsHardwareAccelerated = true`.
4. `AVAssetWriter` (video_test encode) goes through the same lookup. Whether an app's
   VTCompressionSession runs in-process or remotely in mediaserverd is unknown: VT contains
   `VTCompressionSessionRemote` and `kVTVideoEncoder_AllowSandboxedEncode`. The diagnostic answers it.

### 1.5 Size and CPU budget

- Free CPU: mediaserverd already uses 100-130 % of one core for the 10 fps preview (ISP ~28 ms a
  picture). That leaves roughly 2 of 4 A53 cores for encoding while Camera runs.
- Resolution: 1080p30 from a 1632x1224 sensor means up-scaling plus a 1080p encode. Neither fits.
  **Force 720p (or lower) in Camera** (§3.3 step 5).

## 2. Options, ranked by effort and realism

| | Option | Code added to root.apfs | Effort | Realism / speed on 4x A53 @ 2.2 GHz |
|---|---|---|---|---|
| a1 | Register VideoProcessing's `H264VideoEncoder_CreateInstance` as 'avc1' | ~1-2 KB (registration + wrapper) | 0.5-2 days | Unknown. Real-time FaceTime-class H.264 if its software path runs without AVE: **guess** 720p 10-20 fps, 480p 20-30 fps. Risks: wants a usage (`iChatUsageString`, "InitEncoder failed on Unknown Usage"), may try `hwEncoder` / IOSurfaceAccelerator, may emit an 'ftvc' format description |
| a2 | Register VCPHEVC's `HEVCVideoEncoder_CreateInstance` as 'hvc1' | ~1-2 KB | 0.5-1 day, plus letting Camera ask HEVC for video | Likely works (pure software, threaded). **Guess** 720p 2-6 fps, 480p 5-12 fps on 4 threads: probably too slow for real time, so the movie drops frames. Bonus: HEIC stills (the -12908 in inject_media.c) would encode |
| b | Our own VT encoder plug-in (CMBaseClass VTVideoEncoder) backed by **minih264e** (single-header baseline H.264, NEON) | **guess** 60-120 KB raw, ~30-60 KB compressed (2-4 % of headroom), plus ~10 KB glue | 3-5 days | Predictable. **Guess** 1 core: 480p 25-40 fps, 720p 10-15 fps; slice threads on 2 cores: 720p 15-25 fps. CAVLC baseline only, larger files (~2-4 Mbps at 720p). openh264's encoder: **guess** 0.7-1.1 MB raw / ~400-500 KB compressed (C++), fits but costs a third of the headroom. x264 (1.5-3 MB): no |
| c | MJPEG ('jpeg') in a .MOV | glue + our JPEG encoder | 3-5 days | Poor. VT's JPEG encoder here is hardware (`JPEGH1.videoencoder`), so we would still write an encoder. Camera / AVCaptureMovieFileOutput only ask for avc1/hvc1 (ProRes on Pro models), so the codec would have to be rewritten inside BWVideoCompressorNode's settings. Our ImageIO JPEG costs 280 ms per 1632x1224 still. Strictly worse than (b) |
| d | MT6765 hardware VENC | kernel driver + VT plug-in | weeks to months | Not realistic. See below |

Why (d) is out:
- Haretic's `mt6765.dtsi` has `vcodec: video-codec@17020000` ("venc" at 0x17020000, "vdec" at 0x17040000,
  SPI 230/233, `CLK_VENC_SET1_VENC`, larb1 / M4U VENC ports, power domain VCODEC), status disabled. The
  r1 dts sets it "okay".
- Its own comment: "The stock userspace libraries program it; the kernel side is the staging /dev/Vcodec
  driver". So there is **no open register-level encoder programming**. Mainline mtk-vcodec has no
  MT6765 support (its venc paths need VPU/SCP firmware on other SoCs).
- A port would need all of this:
  - Reverse engineering MediaTek's closed vendor libraries from the r1's Android image (libvcodec /
    h264 encoder blobs).
  - A kernel driver: power domain, vcodecsys clocks, larb1 with MMU_EN off as camera did for larb3,
    IRQ, physically contiguous buffers.
  - A VT plug-in on top.

## 3. Recommended path

The registration point is shared by a1, a2 and b: only the factory behind it differs. So the order is:
diagnose, then try Apple's factory, then fall back to minih264e.

### 3.1 First unattended diagnostic run ("vd0")

Everything is behind boot-arg `vdiag=1` and logs only, except that video_test registers encoders **in
its own process**.

**A. mediaserverd** (inject_media.c, `redwood_media_init` when `in_mediaserverd`). Lines go to
`kern.redwood.say` as camera_device.c's `say` does, so they survive in the pstore tail.

1. **Encoder list at start:** `VTCopyVideoEncoderList` with `kVTVideoEncoderListOption_RevealHiddenEncoders`
   and `..._RevealHardwareAcceleratedEncoders` = true. One line per entry: CodecType (4cc), EncoderID,
   IsHardwareAccelerated.
   - Expected: no visible 'avc1'; maybe the hidden `h264.rtvc` passthrough.
2. **Interpose `VTCompressionSessionCreate`** (same `__DATA,__interpose` mechanism as `os_log_media`,
   which already catches cache code in mediaserverd). Log:
   - width x height and codec 4cc;
   - every key/value of `encoderSpecification` (short), especially `RequireHardwareAcceleratedVideoEncoder`
     and `EncoderID`;
   - the source pixel format from `sourceImageBufferAttributes`;
   - the **return status**;
   - the unslid return address, so tools/dsc names the CMCapture caller.
3. **Interpose `VTSessionSetProperty`:** only for sessions created above, first 25 calls (key and short
   value). This shows RealTime, profile, bitrate and whatever else the compressor node sets.
4. **Movie sink hooks** (method_setImplementation wrappers, the pattern at inject_media.c:492). Log entry
   and return, first 3 calls, then one line every 30:
   - `-[BWQuickTimeMovieFileSinkNode _doStartRecordingAtTime:withSettings:sensorVideoPort:]` (sink
     reached);
   - `-[BWQuickTimeMovieFileSinkNode _writeBuffer:forInputIndex:]` (the **first sample buffer**, per
     input: 0 = video?);
   - `-[BWQuickTimeMovieFileSinkNode _doEndRecordingAtTime:earlyTerminationErrCode:]` (log the error
     code);
   - `-[BWFileCoordinatorNode _doStartingToRecordWithSBuf:inputIndex:sbufPTS:]`.
5. **Widen the `os_log_media` filter.** Add the format substrings "VideoCompressor", "MovieFileSink",
   "FileCoordinator", "ecording", "ncoder", "OutOfDiskSpace" with their own budget of 120 lines. CMCapture
   logs `<<<< BWVideoCompressorNode >>>> ... emitting first video compressed buffer` and the sink's
   "stopped recording" and "early termination err code" lines.
6. **`-[BWGraph stop:]` timing** (only if not already logged): a 10 s wait followed by SIGTRAP means a
   sink node stayed busy.

**B. Camera app** (inject_media.c Camera branch):

7. Wrap `-[CAMStorageController hasDiskSpaceToAllowCaptureWithConfiguration:allowPurging:verbose:]` and
   `minAvailableBytesNeededForCaptureForWidth:height:useHEVC:`. Log the arguments and the answers, but
   **do not change them in vd0**.
8. Wrap `-[AVCaptureMovieFileOutput startRecordingToOutputFileURL:recordingDelegate:]`. Log the URL and
   `outputSettingsForConnection:` (codec, size). Also wrap the delegate's
   `captureOutput:didStartRecordingToOutputFileAtURL:fromConnections:` and
   `captureOutput:didFinishRecordingToOutputFileAtURL:fromConnections:error:` and log the error. Reuse
   `wrap_delegate`'s approach.
9. Log the Camera video configuration once (`-[CAMUserPreferences videoConfiguration]` or the selector
   that exists: find it with tools/dsc in CameraUI).

**C. video_test probe** (new mode in userland/src/video_test.m, first line of rc.jobs.mv):

10. Repeat (1) in-process.
11. For each candidate, in this order:
    - (i) `dlopen("/System/Library/PrivateFrameworks/VideoProcessing.framework/VideoProcessing")`,
      `dlsym("H264VideoEncoder_CreateInstance")`, registered as 'avc1';
    - (ii) `dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec")`,
      `dlsym("HEVCVideoEncoder_CreateInstance")`, registered as 'hvc1'.

    Register each with `VTRegisterVideoEncoderWithInfo(codec, info{CMClassImplementationID =
    "com.redwood.probe.<n>", Rating = 100, IsHardwareAccelerated = true}, factory)`. Then:
    - `VTCompressionSessionCreate` 640x480 and 1280x720, source '420v', EncoderID = our ID; log the
      status;
    - set RealTime = true, AverageBitRate = 2e6, ProfileLevel = Baseline/Main AutoLevel; log each
      status;
    - encode 30 synthetic NV12 frames (moving gradient); log ms per frame (wall clock and
      `getrusage`), the output format description's 4cc and dimensions, the first sample's size, and the
      number of samples emitted.
12. Then `AVAssetWriter` H.264 and HEVC in the same process (the existing `write_one`). If they now
    succeed, app sessions are in-process. If they still give -11834, they are remote and the mediaserverd
    registration is what counts.

**Run:** build with `RC_JOBS=rc.jobs.mv` (put `video_test probe` first and keep `scan` / `encode`), then
`SHOTS=... tools/r1-auto.sh vd0 "fbscale=1 r1_clk=1 r1_cpufreq=2200 r1_gpu=680 glyuv=1 cam_fps=10 vdiag=1" 300`.

**Read the result as:**
- The status and caller of `VTCompressionSessionCreate` after rec1 say whether the encoder lookup is
  where it stops.
- No `_doStartRecordingAtTime` means it stops before the sink. A false storage answer in Camera means
  CameraUI refused.
- `_writeBuffer` counts per input say which streams reach the file.
- The probe lines choose between a1, a2 and b.

### 3.2 Decision after vd0

- **a1** if candidate (i) creates a session for 'avc1' at 1280x720 '420v', emits samples whose format
  description is 'avc1' (or 'ftvc' with an avcC atom, see U3), and runs at ≤ 60 ms a frame.
- Else **a2** if (ii) works at ≤ 100 ms a frame at 720p or ≤ 60 ms at 480p. Also force 480p.
- Else **b**.

### 3.3 Implementation steps (in order)

1. **userland/src/venc.c** (new, ~100 lines; shared by libredwood-media and video_test):
   `int redwood_venc_register(void)`.
   - `dlopen` the chosen image and `dlsym` the factory (a1/a2), or use our own factory (b).
   - Call `VTCopyVideoEncoderList` first; it is harmless, and VT's register also runs its own
     once-block.
   - `VTRegisterVideoEncoderWithInfo(codec, info, factory)`. Info: `CMClassImplementationID =
     "com.redwood.videoencoder.<codec>"`, `kVTVideoEncoder_EncoderNameKey`,
     `kVTVideoEncoder_CodecNameKey`, `Rating 100`, `IsHardwareAccelerated true`, no Hide.
   - Log one line.
   - For a1, if vd0 shows it needs a usage or rejects 'avc1': a wrapper factory that calls the real one
     with 'avc1' (or 'ftvc') and then sets `iChatUsageString = "FaceTime"` / `EncoderMode` through the
     instance's CMBaseObject SetProperty (`VTVideoEncoderGetCMBaseObject`).
   - Add `src/venc.c` and `-framework VideoToolbox` to the libredwood-media line in build.sh (~line
     296), and to video_test.
2. **inject_media.c:** call `redwood_venc_register()` in `redwood_media_init` when `in_mediaserverd`,
   gated by boot-arg `venc=1` (default 0 until passed, then default on). Keep the vd0 logging behind
   `vdiag=1`.
3. **inject_media.c, Camera branch:** `hasDiskSpaceToAllowCaptureWithConfiguration:allowPurging:verbose:`
   answers from statfs, as `has_space_to_copy` does: free bytes ≥ 32nd of the volume + 20 MB for a
   video. `ponytail:` comment: the ramfs can still fill. A 720p movie at 2-4 Mbps is ~15-30 MB a minute,
   so ~2-4 minutes fit in ~77 MB.
4. If a2 was chosen: in `no_hevc_in_camera_ui()`, leave `isHEVCEncodingSupported` / `preferHEVC` YES
   for video only. Stills stay on our JPEG path. If that cannot be separated, keep stills as they are
   and let HEIC encode with the now-registered hvc1. Check that the still path still writes our JPEG.
5. **Camera resolution:** force 720p30 (a1/b) or 480p (a2). Either hook the CameraUI video-configuration
   getter found in vd0 (step 9), or set `com.apple.camera` `CAMUserPreferenceVideoConfiguration` before
   launch, as `CAMUserPreferenceEnableResponsiveShutter` is set now.
   - The enum value is unknown (U6); read it from CameraUI with tools/dsc.
   - Alternative: camera_device.c offers only a ≤ 1280x720 "Video" format **(guess: more invasive)**.
6. **Option b only:** userland/src/venc_h264.c plus third_party/minih264e.h (vendored, ~10k lines).
   - A CMBaseClass/VTVideoEncoderClass vtable: CopySupportedPropertyDictionary, SetProperty
     (RealTime, AverageBitRate, MaxKeyFrameInterval, ProfileLevel: accept and remember),
     StartSession(session, codec, w, h), EncodeFrame(token, pixelBuffer, pts, dur, props),
     CompleteFrames, Invalidate.
   - StartSession calls `VTEncoderSessionSetPixelBufferAttributes` with 420v/420f, and builds the format
     description from the SPS/PPS (`CMVideoFormatDescriptionCreateFromH264ParameterSets`, or
     `VTEncoderSessionCreateVideoFormatDescription` with an avcC dictionary).
   - EncodeFrame: NV12 -> I420 (deinterleave chroma into a reused buffer), `H264E_encode`, Annex-B ->
     AVCC length prefixes, `VTEncoderSessionCreateCMBlockBuffer` + `CMSampleBufferCreate`,
     `VTEncoderSessionEmitEncodedFrame`.
   - Settings: IDR every 30 frames, no B-frames, so DTS = PTS.
   - The vtable layout comes from the H264SW decoder / VCPHEVC encoder `FigDerivedObjectCreate` calls
     (VCPHEVC 0x1beb967a0 is a ready-made example of the encoder class to copy the slot order from).
     Build with `-Os` or `minsize` like camera_device.c.
7. **Only if vd1 shows the file coordinator waiting on audio:** in the Camera branch, keep
   `AVCaptureSession` from adding the audio `AVCaptureDeviceInput` (hook `-[AVCaptureSession
   addInput:]` and drop audio inputs while `venc_noaudio=1`).
8. **video_test scan:** add the frame count and codec per video track. Use an `AVAssetReader` with
   `outputSettings:nil` and count samples; the 4cc comes from the track's format description.
9. Run **vd1**: `venc=1 vdiag=1`, same scenario (rec1 ... 6 s ... rec2). Then **vd2**: a 60 s recording
   for drops and heat.

### 3.4 Pass criteria

- **P1 (encoder):** `video_test encode` gives status 2 (completed), 30 frames appended, a file > 0
  bytes. Valid in mediaserverd if app sessions are remote, otherwise with the in-process registration.
- **P2 (Camera):** after a 6 s recording at cam_fps=10, `video_test scan` lists one .MOV under
  /private/var/mobile/Media/DCIM with:
  - 1 video track, codec avc1 (hvc1 for a2), 1280x720 (or the forced size);
  - duration 5-7 s;
  - **N ≥ 40 video frames** (≥ 0.65 x 10 fps x 6 s);
  - 0 or 1 audio tracks (silent).
- **P3 (graph health):** no "streams given back" loop after rec2, the preview resumes (PrimaryScaler
  pictures > 0 in the next 10 s window), and mediaserverd is not restarted (no `BWGraph stop` 10 s
  SIGTRAP).
- **P4 (cost):** mediaserverd ≤ 250 % of a core while recording, thermal below the 58 C guard over vd2's
  60 s.

### 3.5 Unknowns (11)

- U1. Does CMCapture pass `RequireHardwareAcceleratedVideoEncoder`? Moot if we register with
  IsHardwareAccelerated=true. vd0 A2 answers it.
- U2. Does VideoProcessing's legacy H.264 encoder run its software path without AVE or
  IOSurfaceAccelerator ("Failed to allocate hwEncoder", "VPHardwareEncoder*")? What usage or
  EncoderMode does it need?
- U3. Does that encoder emit 'avc1' or 'ftvc' format descriptions when created with 'avc1'? (Its
  create function only stores the 4cc at +0x1384 and does not check it.)
- U4. Real speed of a1 / a2 / b on the A53 (every fps above is a guess).
- U5. Are an app's VTCompressionSessions in-process or remote (in mediaserverd)?
- U6. CameraUI's video-configuration selector and the value for 720p30 / 480p.
- U7. Is CameraUI's "Storage Full" really `CAMStorageController`'s check, and does mediaserverd's
  sink / `minFreeDiskSpaceLimit` refuse too on a ~77 MB-free ramfs?
- U8. Does the movie sink take PrimaryScaler (same output as the preview), and do video frames need
  attachments (`MetadataDictionary` / PortType) that camera_device.c only puts on stills today?
- U9. Does the file coordinator start with a silent (all-zero) audio input, or does it need the audio
  input removed?
- U10. Does a variable or low (10 fps) frame rate pass `_verifyMovieTiming:`?
- U11. Does the format-1 ("Video-60") restart loop have a cause other than the encoder (e.g. a
  property our stream refuses in Video mode)? vd0's `VTCompressionSessionCreate` status after rec1
  separates the two.
