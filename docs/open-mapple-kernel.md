# Open Mapple's MediaTek kernel, compared with ours

Research notes, 2026-11-18. Source: the shallow checkouts of `xnu_kernel_mediatek_common` (Apple's xnu-8796.141.3
with Open Mapple's drivers, one commit `c033748`), `xnu_kernel_mediatek_mt6781` (the SoC layer laid over it at build
time) and `xnu_device_realme_spaced` (the realme 8i's `device.conf`, firmware puller, ILI touch-firmware tool).
Nothing was built or run. Paths below are relative to those repos; "ours" means `xnu/files/` in this repository.

Their target: realme 8i, MT6781 (Helio G96: 6x A55 + 2x A76), MT6366 PMIC (MT6358 register layout, which includes the
codec), Mali-G57, UFS 2.1, MT6631 CONNSYS radio, MT6371 PMU (torch/backlight/charger), iOS **16.7.16** userland
(ours is 16.5: some selector numbers they quote are 16.7's, check before copying).

How they got the list of added files: a file-list diff of their common tree against our `xnu-src/xnu-8796.121.2`
(340 files only on their side; 120 of those are in `iokit/Kernel/arm`, 79 are Apple's open-source HFS sources staged
into `bsd/hfs_staged`, 41 libpthread, 30 BearSSL crypto), plus the build lists `bsd/conf/files`,
`iokit/conf/files.arm64`, `osfmk/conf/files.arm64`.

## Their device status (from `device.conf`, their own words, abridged)

| Area | Their status | Ours |
|---|---|---|
| Display | partial: LK's framebuffer, UI through Metal on Mali (Panfrost) | works (PVR/Zink) |
| Touch | mostly (ILI7807S over bit-banged SPI) | works |
| Buttons | works | works |
| CPU | "missing" in the table, but DVFS + LVTS thermal code exists and is enabled (`mp_cpufreq=2`, `mp_therm=1`) | governor works |
| Storage | partial: root read-write on HFS+ in a UFS partition, polled | RAM-disk root, `/var` store work in progress |
| Sensors | missing (code exists for the SCP sensor hub, default mostly off) | IMU driver written, not run |
| Wi-Fi | works (scan, WPA2, DHCP, Safari, long downloads) | works |
| Bluetooth | mostly: **headphones play music, keyboards type**; headset mic does not | pairing works, A2DP unverified |
| Audio | mostly: speaker, **main mic while an app records**, earpiece route; no wired headset | speaker only |
| Battery | partial: gauge read, charger not driven | gauge read |
| Haptics | works (VIBR LDO driven from iOS's haptic waveform) | works |
| Files | works: On My iPhone, Safari downloads, kept across reboots | open problem |
| Video | works: Safari plays YouTube, frames converted on the CPU | Safari audio spins |
| Camera | missing | works |
| Torch | works | works |
| Cellular | **missing; no modem/CCCI code at all** | LTE data in test images |
| Thermal | partial (LVTS read) | NTC guard |

## Per-area findings

Each item: their files; what it does; our counterpart; portability to MT6765/MT6357/r1; effort (S/M/L); risk.

### 1. Audio capture (microphone): our top gap, and they have it

- **Files:** `xnu_kernel_mediatek_mt6781/osfmk/arm64/mt6781_afe.c` (393 lines: `mt6781_afe_ul_start/position/stop`),
  `bsd/dev/mtk_audio.c` (`mic_codec_up`, `mic_codec_down`, `mic_poll`, `mic_source`, step `mp_audio_steps=5`),
  `bsd/dev/mapple_audio.c` + `bsd/sys/mapple_audio.h` (`/dev/mapple-mic`, `mapple_audio_set_source`,
  `mapple_audio_mic_open`), consumed by `mapple_userland/audio/audio_plugin.c` (their CoreAudio HAL plug-in in
  mediaserverd; that plug-in is outside this note's scope).
- **What it does, exactly:**
  - AFE side (Android's order from `mt6781-dai-adda.c` / `-afe-pcm.c`): write the UL 35 Hz high-pass IIR coefficients
    (`AFE_ADDA_IIR_COEF_02_01..+16` = `0, 0x3fb8, 0x3fb80000, 0x3fb80000, 0xc048`), `AFE_ADDA_UL_SRC_CON0 = 0x00060400`
    (48 kHz, IIR on), `AFE_ADDA_TOP_CON0` bit 0 = 0 (codec ADC, not loopback), `AFE_ADDA_MTKAIF_RX_CFG0` bit 0 = 0
    (AMIC), `AUDIO_TOP_CON0` bit 24 = 0 (ADC clock on), `AFE_AUD_PAD_TOP` 0x30 then 0x31 (RX FIFO on),
    `AFE_ADDA_MTKAIF_CFG0 = 0` (protocol 1), MISO pins 189/190 to audio mode, UL source on, interconnect I03->O21,
    I04->O22, then memif **VUL12** into a 16 KiB ring in AFE SRAM placed right after the DL1 ring
    (`AFE_SRAM + 0x4000`), 48 kHz/16-bit/stereo, `AFE_DAC_CON0` bit 31 on. No interrupt, no DMA mapping: the CPU
    polls `AFE_VUL12_CUR` (16-byte steps) every 5 ms.
  - Codec side (MT6366, `mt6358.c`'s DAPM order with the values Android has while it records): AIN0 into ADC L and R,
    mic bias 0 at 1.9 V (`AUDENC_ANA_CON9` [6:4] = 2), PGA 12 dB (`AUDENC_ANA_CON0/1` [10:8] = 2), the ADC RC-tune
    check at `0x23a0` with a re-power if it did not settle, then the digital UL enables (`0x22b4`, `0x22cc`,
    `0x228c`, `0x228e`, `0x00de`). Power-down in reverse.
  - Plumbing: the mic is powered **only while `/dev/mapple-mic` is open** (the HAL plug-in opens it when an app
    records); left channel taken as mono 48 kHz into an 8192-sample ring; the reader never blocks; overruns counted;
    a once-a-second peak log line proves the mic hears something.
- **Ours:** `bsd/dev/redwood_mtkaudio.c` (MT6765 AFE at `0x11220000`, SRAM `0x11221000`, MT6357 codec, AW87390 amp):
  playback only, the same overall shape (SRAM ring, polled position, staged boot-arg). `redwood_mic.c` is the
  Qualcomm (redwood) DSP path, irrelevant to r1. `sys/redwood_audio.h` has no source/mic node.
- **Portability:** the *design* ports directly (and matches ours almost line for line: both descend from the same
  HakiOS code). The *registers* do not:
  - MT6765's AFE is the older "common_int" generation: its capture memif is **VUL** (and VUL_DATA2/AWB), not VUL12,
    and the offsets of `AFE_VUL_BASE/CUR/END`, `AFE_CONN*`, `AFE_ADDA_UL_SRC_CON0`, `AFE_ADDA_TOP_CON0`, the IIR
    coefficient block and the MTKAIF RX config all differ. Take them from `mt6765/mtk-auddrv-afe.h`,
    `mtk-soc-pcm-capture.c` and `mtk-soc-afe-connection.c` of the dandelion tree we already used for playback.
  - MT6357's codec register map is **not** MT6358's: our playback already uses `0x20xx/0x21xx` codec addresses where
    MT6366 uses `0x22xx/0x24xx`. The UL sequence (mic bias, PGA, ADC enable, RC tune, UL digital enable) must be
    re-derived from `codec/mt6357/mtk-soc-codec-6357.c` (`TurnOnADcPowerACC`/`Audio_ADC1_Set`-style functions) and,
    best, from an Android trace of `tinycap` on the r1 the same way `logs/aud.trace` was made for playback.
  - The r1's mic is analog into MT6357 AIN0 or AIN1 (check Android's mixer paths/trace); the r1 may also use a
    second mic. The `AUD_DAT_MISO` pins on MT6765 have different numbers from 189/190.
  - The SRAM layout trick (UL ring right after the DL ring, inside SRAM Android leaves free) should hold on MT6765
    (AFE SRAM there is also tens of KiB) but check the size.
- **Effort:** M (kernel UL path 1-2 days with an Android trace; `/dev` source node and HAL side S if our audio
  plug-in grows an input stream). **Risk:** low-medium: wrong codec writes can pop or bias the mic; their approach of
  a register whitelist (`afe_write()` refuses anything not in `afe_regs[]`; `MTKPwrap::writeAudio` has ranged
  codec-register limits) is worth copying as-is.

### 2. Audio playback, earpiece route, haptics through the audio node

- **Files:** `bsd/dev/mtk_audio.c`, `mt6781_afe.c` (DL1), `bsd/dev/speaker_guard.c`, `bsd/dev/mtk_audio_level.h`,
  `bsd/dev/mapple_audio.c`.
- **What's new relative to ours:** (a) `MAPPLE_AUDIO_SET_ROUTE` ioctl: the HAL plug-in tells the kernel which port
  iOS chose; the kernel switches line-out/amplifier off and the codec's handset (earpiece) driver on, and back;
  (b) volume split between the codec's line-out gain (1 dB steps) and a digital factor, with Android's level as the
  ceiling (`mtk_audio_lol_code`); (c) `/dev/mapple-haptic`: iOS's haptic waveform arrives as audio frames from the
  plug-in and drives the VIBR LDO (30 ms min, 1 s max, 1 s rest) instead of a dedicated haptics driver;
  (d) the amplifier is powered only while there is sound and dropped after 3 s of silence.
- **Ours:** `redwood_mtkaudio.c`, `redwood_guard.c` (same guard, same lineage), `redwood_audio.c`.
- **Portability:** idea/design direct; r1 has no earpiece, so the route ioctl matters only as "speaker vs nothing".
  The power-on-sound / off-after-3 s amp policy and the per-write whitelist are directly reusable.
- **Effort:** S. **Risk:** low.
- **Relevance to "all audio silent after pairing":** their kernel has nothing Bluetooth-specific for audio. A2DP
  works for them because mediaserverd's own Bluetooth output route is used and their HAL plug-in only presents the
  built-in speaker (and mic). If ours stays silent after pairing, look at our injected Bluetooth audio plug-in and
  route handling in userland, not the kernel.

### 3. In-kernel IOHIDFamily: touch, buttons, keyboards, sensors without injecting backboardd

- **Files:** `iokit/Kernel/arm/hid/` (≈100 files): Apple's open-source IOHIDFamily-1915.140.3 parts compiled into the
  kernel (`IOHIDEventServiceUserClient.cpp`, `IOHIDEventQueue.cpp`, `IOHIDEvent.cpp`, `IOHIDInterface.cpp`,
  `IOHIDResourceUserClient.cpp`, `IOHIDUserDevice.cpp`, HID descriptor parser) plus their services:
  `MappleHID.cpp` (minimal `IOHIDEventService` base, `MappleHIDButtons`, `MappleHIDTouch`),
  `MappleHIDKeyboard.cpp` (keyboards behind an `IOHIDInterface`: what bluetoothd creates via `IOHIDResource`),
  `ILI7807S.cpp` / `GT9896.cpp` (touch chips, subclasses of `MappleHIDTouch`), `MT6358Keys.cpp`, `MTKKeypad.cpp`.
- **What it does:** backboardd finds services with `HIDServiceSupport = true`, opens them with type `'esuc'`, and
  reads serialized `IOHIDEvent`s from a shared queue, exactly as on an iPhone. Touch arrives as digitizer
  collection events; keys as consumer-page events. Bluetooth keyboards work because `IOHIDResource` exists.
- **Ours:** `/dev/redwood-touch` (`bsd/sys/redwood_input.h`, `redwood_hyn.c`, `redwood_buttons.c`) plus a virtual HID
  service inside backboardd made by `userland/src/inject.c`. No `IOHIDResource`: Bluetooth HID devices cannot
  appear.
- **Portability:** direct; nothing SoC-specific in the HID core or `MappleHIDTouch`. Our Hynitron driver would call
  `mapple_hid_touch()`-style entry points instead of filling `/dev/redwood-touch`; side button and wheel call
  `mapple_hid_key()`. Check selector/ABI against 16.5's IOKit.framework (they built from IOHIDFamily-1915.140.3 for
  16.7; 16.5 shipped 1915.120.x, so the user-client ABI is very likely the same but must be verified).
- **Effort:** M-L (large import, mostly mechanical; touch timing tuning). **Risk:** medium: replaces a working input
  path; keep `/dev/redwood-touch` behind a boot-arg until hands-on. Wins: removes the backboardd touch injection,
  enables Bluetooth keyboards/mice, and is the prerequisite for item 5 (sensors).

### 4. Display: IOMobileFramebuffer with direct scanout, GPU-aware swap queue, real backlight personality

- **Files:** `iokit/Kernel/arm/MappleFramebuffer.cpp` (3504 lines), `MappleLetterbox.h`, `MappleSurface.cpp/.h`
  (IOSurface root), `MappleSurfaceAddress.h`.
- **What's new relative to ours:**
  - **`AppleARMBacklight`** service with `backlight-control = true` and `IODisplayParameters.brightness {min,max,value}`:
    CoreBrightness finds it (`CBDisplayContaineriOS findBacklight`) and drives brightness through `setProperties` /
    `IOMFBBrightnessLevel` (nits, 16.16). That gives brightness *and* Night Shift with no injection; Night Shift's
    matrices come in on selector 55 (`IOMobileFramebufferSetMatrix`) and go to the display pipe's **CCORR**.
  - **Direct scanout** (`mp_fb_scanout=1`): a full-screen BGRA surface allocated physically contiguous below 4 GiB is
    scanned out by changing only OVL layer address/pitch, at a frame end found by polling OVL `INTSTA` bit 1; no CPU
    copy.
  - **Swap queue that waits for the GPU** (`mapple_gpu_surface_idle`): backboardd hands `SwapEnd` a surface right
    after committing GPU work; the framebuffer shows the newest swap whose surfaces have no pending GPU job, drops
    older ones, and redoes a copy if a new job touched a surface during it (`mp_fb_gpuwait`, 200 ms cap).
  - **Early present** (copy at SwapEnd/job-complete instead of the next tick), **SwapWait semantics** documented
    (options bits 0-2, bit 31 non-blocking), vsync timer phase-locked to the measured frame end, panel 60/120 Hz
    switching (not applicable to us), display-off fade + backlight off.
  - `mt6781_cpufreq_ui_activity()` called from every SwapEnd (UI boost, see item 8).
- **Ours:** `iokit/Kernel/MiniIOMobileFramebuffer.cpp` (has `fb_flip` OVL0 double buffering at `0x1400b000`, RDMA0
  frame-end probe, timer vsync); `MiniIOSurface.cpp`; brightness via `inject.c`'s backboardd hook + `r1_bl=1`
  (MT6370 over I2C5, `kern.redwood.backlight`).
- **Portability:**
  - `AppleARMBacklight`: direct; route its level into our MT6370 backlight driver. **S**, low risk, removes an
  injection.
  - CCORR Night Shift: needs register changes (MT6765's DISP_CCORR0 base and the 1.0 = 0x400 vs 0x800 scale must be
    checked); **S-M**.
  - Direct scanout and GPU-idle-gated swaps: need our PVR fence equivalent of `mapple_gpu_surface_idle`; OVL register
    layout on MT6765 is the same family as what `fb_flip` already drives. Directly relevant to our "camera frames
    painted twice" and 8-13 fps home-screen swipes: a swap shown before its GPU work is done is one way to get a
    re-paint, and a CPU copy per frame is a large share of frame time. **M**, medium risk.
- Note: their own status says photo wallpapers may not render yet; their GPU path is Mali/Panfrost and not portable.

### 5. Sensors: AppleSPU personalities fed from a sensor hub

- **Files:** `iokit/Kernel/arm/hid/MappleSPU.cpp/.h` (842+480 lines), `bsd/dev/mtk_scp.c` (619),
  `bsd/dev/mtk_scp_hub.h`, `xnu_kernel_mediatek_mt6781/osfmk/arm64/mt6781_scp.c` (223).
- **What it does:** one `IOHIDEventService` per sensor with the iPhone X SPU's usage pairs: accel `0xFF00/0x03`,
  gyro `/0x09`, compass `/0x0A`, proximity `/0x08`, ALS `/0x04`, orientation `0x20/0x8A`; with step 5 also CMA
  (steps/activity `0xFF0C/0x01`), raise-to-wake gesture (`/0x03`) and device motion (`/0x04,/0x05,/0x07`). Each
  carries `IOCFPlugInTypes` pointing at `AppleSPU.kext/PlugIns/AppleSPULib.plugin` and a `MappleSPUUserClient` opened
  with type `'ashd'`. `ReportInterval` / `BatchInterval` from iOS become the hub's rate/latency. Their class names
  keep Apple's ALS/compass plug-ins from attaching. Data source: the SCP's (Cortex-M tinysys) nanohub over IPI, with a
  1 MiB record ring in DRAM. Default `mp_spu_off=0x7de` = only accel + orientation published (the others still
  untrusted).
- **Ours:** `redwood_scp.c` (X6826B/MT6768 SCP, read-only probing), the r1 IMU (ICM-42607P, bit-banged SPI in
  `redwood_hyn.c` + `redwood_scp.c`, `r1_imu=1`, written, not compiled). No iOS-facing sensor service; backboardd's ALS
  wait is skipped by injection.
- **Portability:** the SCP half is **idea only** for r1: the r1's IMU is on AP GPIOs, not behind an SCP hub, and
  MT6765's SCP (if used at all on r1) would differ. The **MappleSPU half is direct**: feed accel/gyro samples from our
  IMU driver into `mapple_spu_record()`-style entry points; orientation is computed from gravity in the kernel
  (`mtk_scp_hub.h` has the pure code, held 300 ms). Requires item 3 (the in-kernel HID event service base).
- **Effort:** M (after item 3). **Risk:** low-medium: wrong axis signs rotate the UI; they expose
  `mp_imu_axes` nibble remaps (keep that knob).

### 6. Bluetooth: transport same as ours; two useful extras

- **Files:** `bsd/dev/mtk_bt.c` (1250), `bsd/dev/mtk_btsk.c` (324), `iokit/Kernel/arm/MappleBluetooth.cpp` (77),
  `bsd/dev/mt6631_wifi.c` (`mt6631_bt_*`: STP channel 0 over BTIF PIO, WMT `FUNC_CTRL`).
- **Lineage:** `mtk_bt.c` is our `redwood_mtkbt_skel.c` (struct `mtkbt_platform`, H4 framing, Skywalk kernel pipe
  `com.apple.uart.sk.bluetooth`, VSC answers) filled in for MT6781. Same quirks as our `redwood_connsys.c`
  `bt_quirks` (0xfc/0xfd vendor commands answered, 0x0c5b, supported-commands mask, 0x204a answered locally).
- **Extras worth taking:**
  - **BlueTool emulated in the kernel:** a second kernel control `com.apple.uart.bluetooth` answers BlueTool's
    built-in Broadcom boot script (reset, baud, RAM-firmware records, launch, address, calibration, sleep mode) as a
    BCM4357B1 would (`Read_Local_Version` = Broadcom 15, LMP subversion `0x420e`), termios kept through
    get/setsockopt. The script is selected by MobileGestalt's `WifiVendor`, which they supply from an `AppleOLYHAL`
    child of the WLAN controller (`MT6631WLAN.cpp`, `wifi-vendor` ADT property). Ours replaces BlueTool's XPC
    service by injection (`userland/src/inject.c` ~line 3145). **Direct port, S-M, low risk**; removes an injection.
  - `0xfce2` sub-command `0x0d` (profiling) answered with 8 bytes (iOS 16.7 aborts without; check 16.5),
    `sysctl kern.mapple_bt_quirks` to add mask bits / "done" opcodes without a rebuild, privacy-safe pairing/L2CAP
    log lines. **S**.
- **Not found:** SCO/eSCO routing (HFP), A2DP-specific code, Wi-Fi/BT coexistence configuration, LE audio. Their
  status confirms "a headset's microphone doesn't work yet". Their A2DP works with only the ACL path.
- **Keybag:** see item 9; their bluetoothd gets an unlocked-since-boot keybag from `MappleKeyStore`.

### 7. USB device mode in iOS (we have none)

- **File:** `bsd/dev/usb_link.c` (2029 lines).
- **What it does:** MT6781's MUSB (`mediatek,mt6781-usb20`, `0x11200000`) with T-PHY u2 port (`0x11ca0300`), left by
  LK after fastboot. Wakes the PHY with MediaTek's u2 "recover" sequence and forces VBUS/session/ID to device in
  `U2PHYDTM1` (no PMIC/charger code needed). Peripheral mode, PIO only, interrupts masked, a kernel thread polls the
  write-1-to-clear status registers; EP0 state machine after Linux `musb_gadget_ep0.c`.
  - Persona "link" (VID 0x1209/PID 0x0002): vendor-class bulk pair; streams the console log ring and takes text
    commands (peek/poke, pmic, i2c, wifi/bt/gpu/cpu/disp/torch subcommands, push/pull files with FNV check, run a
    program through the `mappled` daemon via `/dev/mapplelink`, reboot).
  - Persona "std" (switched when the `mapple-usbd` daemon attaches): **iPhone-like composite 05ac:12a8 with PTP and
    usbmux** (+ the link), the kernel only moving transfers through `/dev/linkptp` and `/dev/linkmux` and answering
    ep0 (PTP class requests, Microsoft OS descriptor, Apple vendor requests). So a Mac sees photos over PTP, and
    usbmux can carry lockdown-style services.
  - The link's heartbeat decides the charger input limit (500 mA while a tool is attached, else 2000 mA).
- **Ours:** none (STATUS: "USB while in iOS: not yet done"); we return to Android with a side-button hold.
- **Portability:** **needs register changes, but small**: MT6765 also has a MediaTek MUSB (`mediatek,mt6765-usb20`
  at `0x11200000`) and a T-PHY u2 port; the PHY base, the `U2PHYDTM1` offset and the infracfg clock gate need
  checking against the r1's Android DT. The MUSB register layout and EP0 logic are the same IP.
- **Effort:** M for the link persona (logs + push/pull + run: a big debugging win in stay-in-iOS images), L for the
  PTP/usbmux persona (userspace daemon in their `tools/mappled`, other repo). **Risk:** medium: a wrong PHY write can
  drop the cable until reboot; their read-only step 1 (`mp_usblink=1/2`) is the right first run.

### 8. CPU: DVFS, UI boost, all 8 cores, thermal

- **Files:** `xnu_kernel_mediatek_mt6781/osfmk/arm64/mt6781_cpufreq.c` (1218), `iokit/Kernel/arm/MappleCPU.cpp` (278),
  `iokit/Kernel/arm/AppleARMSMP.cpp` (modified), `osfmk/kern/sched_prim.c` (`sched_mapple_fast_cpus`).
- **What it does:** AP-side Linux cpufreq_v1 sequence (Vproc11/12 via pwrap, ARMPLL_LL/BL and CCIPLL `CON1` PCW +
  posdiv, Vsram tracking verified by readback), OPP tables taken **from LK's efuse-binned dvfsp tables passed by the
  loader** (`mp_dvfs_tbl=`), refuse to start without them. Governor every 16 ms (100 ms when idle at floor).
  **UI boost:** `mt6781_cpufreq_ui_activity()` from every framebuffer SwapEnd and every touch report holds the
  clusters at max for 250 ms. **Thermal:** LVTS sensors at `0x1100b000` with efuse calibration, step-down at 85 C,
  floor at 105 C, and the **hardware thermal reset armed before any DVFS write** (`mp_therm_hwreset=`; DVFS does not
  start if arming fails). SMP: PSCI `CPU_ON` for all 8 cores (`cpus=8` in their cmdline), one homogeneous cluster;
  CPU_OFF/suspend unimplemented (`panic("CPU halt/suspend is not implemented")`).
- **Ours:** `redwood_cpufreq.c` (POCO) and the r1 governor (`r1_cpufreq=`, MP0 2200 / MP1 1600 MHz, NTC thermal guard
  at 58 C), `osfmk/arm64/psci_smp.c`. 8th core blocked by our loader watcher.
- **Portability:** the **UI-boost hook is direct and cheap** (one call from `SwapEnd` and from the touch driver into
  our governor): S, low risk, likely helps swipe fps. LVTS is MT6781-specific (MT6765 uses the older AUXADC-based
  thermal controller): idea only. Arming the SoC's hardware thermal reset before DVFS: idea, M. The A76 scheduler
  preference is irrelevant (MT6765 is 8x A53). Their 8-core bring-up does not tell us how to free the core our loader
  keeps; that lives in `mapple_loader` (`XNU_SHIM`), not in this kernel.

### 9. Keybag / AKS (SEP emulation)

- **Files:** `iokit/Kernel/arm/MappleKeyStore.cpp` (683), `MappleKeyStorePasscode.h`.
- **What it does:** built-in `AppleKeyStore` with the system bag always loaded; selector 14 answers with a handle so
  keybagd never needs `systembag.kb`; lock state bits (locked / no passcode / unlocked since boot); **a real passcode**
  as a salted PBKDF2-HMAC-SHA256 verifier (20000 iterations) in `/private/var/keybags/mapple.kb`, two slots written
  alternately (torn-write safe), iOS's back-off delays, lock on lock-button, `messageClients` for first-unlock and
  status changes. Explicitly not data protection (no SEP). Wrap/unwrap are present.
- **Ours:** `iokit/Kernel/MiniAppleKeyStore.cpp` (993): no-passcode device; with `aks_keys=1` real bags + RFC 3394 key
  wrap; keychain in memory (lost at reboot).
- **Portability:** direct (pure software), selector numbers are 16.7's (check against `docs/x6826b-aks-selectors.txt`).
  The useful parts for us: "always-loaded system bag" (removes the need for `aks_restore`/a bag file, which is what our
  "Bluetooth needs the keybag permanently" item is about) and the persistent two-slot record file pattern. Passcode
  locking is optional. **S-M, low risk.**

### 10. Storage and persistence

- **Files:** `bsd/dev/mtk_ufs.c` (1293), `bsd/hfs_staged/` (Apple's HFS sources built in, `hfs_staged_iokit.c` replaces
  the kext's IOKit glue, registered from `bsd_init` before root mount), `tools/hfs_fpfs_test.c`.
- **What it does:** reuses the UFS host LK left up; polled SCSI on one GPT partition (`mp_ufsroot=xnusys`) exposed as
  `ufsroot0`; writes only with `mp_ufsrw=1` and re-checked against partition bounds right before issue. **Root is a
  read-write HFS+ volume**, persistent; FileProvider is satisfied by a user-space shim (`fpdvolume.dylib`) plus kernel
  answers to the APFS calls it makes (`hfs_fpfs.h`). Also: a **persistent console log** (4 MiB ring copied every 10 s
  to the partition tail, header `MAPPLECL`), and a **boot-args record** (`MAPPLEBR`) in the partition's last MiB
  because LK write-protects boot_a/b while iOS runs (`kern.mapple_nextbootargs`).
- **Ours:** `redwood_emmc.c` (X6826B eMMC partition as root, read-mostly), `redwood_ufs.c` (Qualcomm), our own
  `miscfs/apfs` + `ramfs`, `/var` store in progress.
- **Portability:** the UFS driver is not usable (r1 is eMMC/MSDC). **HFS+ built in is directly portable** and is a
  credible alternative to finishing our own APFS write path for a persistent `/var` (or a persistent root): Apple's
  code, journaled, already working with iOS 16 userland for them. The persistent log ring and the boot-args record in
  a partition tail are direct ideas for our eMMC driver. **Effort:** M (HFS import + eMMC write path behind a guard),
  **risk:** medium-high (writes to eMMC: our standing rules forbid `emmc_write=` in test runs; needs the user and a
  backup).

### 11. Video: IOSurfaceAccelerator scaler, JPEG encoder; no H.264

- **Files:** `xnu_kernel_mediatek_mt6781/iokit/Kernel/arm/MTKScaler.cpp/.h` (`AppleM2ScalerCSCDriver`),
  `MTKMDP.cpp` (MDP RDMA0 -> [RSZ0] -> WROT0 backend), `MTKJPEG.cpp/.h` + `MTKJPEGHW.cpp` (hardware JPEG encoder).
- **What it does:**
  - `AppleM2ScalerCSCDriver`: the IOSurfaceAccelerator service QuartzCore/VideoToolbox use for blits, scaling and
    NV12->BGRA. Selector 1 transfer (struct 0x170), completion via async notification (QuartzCore releases surfaces
    only in that callback), selectors 2/3/4/5/7 stubbed sensibly. **CPU backend** (bilinear/nearest, 8888 formats,
    NV12 BT.601/709) and an MDP hardware backend (`mp_scaler=2`) found by hand on MT6781.
  - `AppleJPEGDriver` (registry *name* only, class `MTKJPEGEncoder`) on `jpgenc@17030000`: VideoToolbox's JPEG encoder
    uses it; VEN MTCMOS power-up sequence, larb7 ports out of the M4U, bounce buffers below 4 GiB.
- **Ours:** none of these services. Safari video plays for them with CPU colour conversion ("most of one core").
- **Portability:** the scaler's **CPU backend is direct** (SoC-independent) and may well be what our Safari/web
  media path or CoreImage fallback is missing; MDP backend needs MT6765 MDP addresses (idea). JPEG: MT6765 has a
  jpgenc block too; the power/larb sequence needs MT6765 values (M).
- **H.264 encoder (VENC) / decoder (VDEC): not found.** The VEN power-up in `MTKJPEGHW.cpp` is the same domain
  MT6765's VENC lives in, so it is a starting point only.
- **Effort:** scaler CPU path S-M; JPEG M. **Risk:** low (scaler), medium (JPEG: DMA without IOMMU).

### 12. Wi-Fi

- **Files:** `bsd/dev/mt6631_wifi.c` (3894), `mt6631_wlan.c` (765, apple80211 ioctls on en0), `mt6631_wpa.c` (in-kernel
  WPA2-PSK), `iokit/Kernel/arm/MT6631WiFi.cpp` (EMI window), `MT6631WLAN.cpp` (registry personalities).
- **Lineage:** same as our `redwood_connsys.c` / `redwood_mtkwifi.c` / `redwood_apple80211.c` / `redwood_wpa.c` /
  `RedwoodWLAN.cpp` (X6826B MT6768 path the r1 uses). Their MT6781 specifics: CONNAC 1.x gen4m `SOC2_1X1`, A-die
  detection MT6631 vs MT6635 over the CONN SPI, EMI window at the Android-reserved `0xbf000000` hole
  (`mp_wifi_emi=`), firmware/NVRAM read from the root partition's tail when the root has none (their latest commit).
- **Extras:** the EMI and WFDMA memory reserved at boot before contiguous scanout surfaces fragment low memory
  (`mt6631_wifi_reserve`), and `AppleOLYHAL` with `wifi-vendor` (feeds item 6).
- **Portability:** nothing new we need for Wi-Fi itself; the early reservation is a good idea if we adopt direct
  scanout. **Not found:** coexistence (PTA/BT-coex) configuration beyond what WMT firmware does by default.

### 13. PMIC, battery, charger, RTC, power-off

- **Files:** `iokit/Kernel/arm/MTKPwrap.cpp` (pwrap WACS2 with write whitelists: interrupt regs, GPU rails with a
  voltage ceiling, audio codec ranges, regulators for CONNSYS, power-off), `MT6358PMIC.cpp` (PMIC interrupt groups
  via EINT 144, all disabled at start then enabled per child), `MT6358PowerOff.h` (EOSC calibration, DCXO to RTC
  mode, `PWRHOLD = 0`), `bsd/dev/mt6358_gauge.c` + `mt6358_ntc.h` (= our `redwood_mtkbatt.c` lineage: Android's
  percentage from `RTC_AL_MTH` plus coulomb counter), `MapplePowerSource.cpp`, `BQ27Z561Battery.cpp` (viva: gauge +
  SYV690 charger with a 160 s watchdog policy, Low Power Mode hooks into CPU/GPU ceilings).
- **Ours:** `redwood_mtkbatt.c`, `RedwoodBattery.cpp`, r1 `r1_batt=1`.
- **Portability:** MT6357's pwrap is the same IP (WACS2); the **whitelist pattern** and the **PMIC interrupt
  controller** (real IRQs instead of polling) are directly reusable with MT6357 register tables. A **clean power-off**
  (`MT6358PowerOff.h`) needs MT6357 registers (`PWRHOLD` is in `PPCCTL0` on both families; check). Charger: neither
  side drives it on MediaTek PMIC chargers; r1's MT6370 charger is close to their MT6371 (same driver family,
  `mt6370_pmu_*`), so their torch's careful MT6371 access is a model. **Effort:** S-M each. **Risk:** low (power-off),
  medium (charger).
- **RTC:** no new RTC driver found beyond reading the RTC alarm register for the gauge.

### 14. Torch, camera

- `bsd/dev/mtk_torch.c`: MT6371 FLED1 on I2C5 0x34, 25/37.5/50 mA, auto-off at 10 min / 45 C / faults, through
  `kern.mapple_torch` and a user-space `mapple-captured`. We already have a torch.
- `bsd/dev/mtk_cam.c`, `mtk_cam_ldo.h`, `xnu_kernel_mediatek_mt6781/osfmk/arm64/mt6781_cam.c`: camera power-up probe
  only; **camera is "missing" for them**. Ours is ahead (`redwood_mtkcam.c`, `redwood_r1cam.c`). Nothing to take.

### 15. Platform glue that makes stock iOS userland happy (no injection)

| File | Personality / interface | Ours |
|---|---|---|
| `iokit/Kernel/arm/MappleWatchdog.cpp` | `IOWatchdog` (watchdogd type 1) + a service with `IOPMUPrimary` so watchdogd's boot-stage write does not crash | check: not found in `xnu/files` |
| `iokit/Kernel/arm/MappleAMFI.cpp` | AMFI IOService, selector 6 (cdhash in trust cache) answers "trusted": installd/LaunchServices keep NSExtension appexes (widgets, poster providers) | `libkern/amfi/miniamfi.c` (policy only; no IOService found) |
| `bsd/kern/amfi_stub.c` + `MappleEntitlements.cpp` | entitlements parsed from the XML blob at signature add; every root-FS binary marked platform | `miniamfi.c`, `miniamfi_entitlements.cpp` |
| `iokit/Kernel/arm/MT6631WLAN.cpp` | `IO80211Controller` property-only classes, `IONetworkStack` user client, `AppleOLYHAL` | `RedwoodWLAN.cpp`, `MiniIONetworkStack.cpp` |
| `iokit/Kernel/arm/MappleGIC.cpp`, `MTKEINT.cpp` | GICv3 SPIs and MediaTek EINT as IOKit interrupt controllers (drivers use `IOInterruptEventSource`) | `osfmk/arm64/gic_v3_irq.c` (polling in many drivers) |
| `iokit/Kernel/arm/MTKIO.cpp` | GPIO + I2C v2 with interrupt completion | hand-driven GPIO/I2C per driver |
| `bsd/dev/mapple_sysctl.c` | `kern.mapple_link` (read-only diagnostics for unprivileged apps), `kern.mapple_nextbootargs` | — |
| `bsd/dev/proc_sampler.c` | periodic user pc/lr/backtrace of named daemons on the console | — (useful for "Safari spins") |

`MappleAMFI.cpp` is the most likely fix for missing extensions/widgets if we see them; `MappleWatchdog.cpp` matters
only if watchdogd is in our job list. Both are S, direct, low risk.

### 16. Not present in their kernel (say "not found", don't hope)

Modem/CCCI/cellular (nothing at all), suspend/deep idle/CPU_OFF (explicitly unimplemented), H.264/HEVC VENC/VDEC,
GPS, SCO/HFP, wired headset/accessory detect, USB host mode, AOP/always-on processor emulation beyond `MappleSPU`,
SEP (only the software keybag), Wi-Fi/BT coexistence tuning, camera streaming.

## MT6781-specific vs common, and what changes for MT6765/MT6357

- **Common (SoC-free, port as is):** `mapple_audio.c` and `sys/mapple_audio.h`, `speaker_guard.c`,
  `hid/*` (HID core, `MappleHID`, `MappleHIDKeyboard`, `MappleSPU` minus its SCP feed), `MappleKeyStore`,
  `MappleSurface`, `MTKScaler` CPU backend, `MappleAMFI`, `MappleEntitlements`, `amfi_stub.c`, `MappleWatchdog`,
  `mtk_bt.c` + `mtk_btsk.c` + BlueTool emulation, `mt6631_wlan.c`/`mt6631_wpa.c` (ours equivalent), `hfs_staged`,
  `proc_sampler.c`, the framebuffer's protocol parts and `AppleARMBacklight`.
- **MediaTek-common IP with different addresses on MT6765:** MUSB + T-PHY (`usb_link.c`), pwrap WACS2
  (`MTKPwrap.cpp`), EINT, I2C (MT6765 is the older I2C v1 register set: timing code differs), OVL/RDMA scanout,
  CONNSYS/WMT/STP/BTIF (ours already done for MT6768/MT6765), AFE memif + ADDA (MT6765 uses VUL, older offsets),
  MDP/JPEG (VEN power domain).
- **Chip-specific, idea only:** LVTS thermal, MCUPM-less A55/A76 DVFS tables, SCP sensor hub, UFS host, MT6366 codec
  register sequence (MT6357's map differs), panel DSC/VFP 120 Hz switching, Mali/Panfrost.

## Ranked top 10 for the r1

| # | What | Their files | Ours | Port | Effort | Risk |
|---|---|---|---|---|---|---|
| 1 | Mic capture: AFE UL memif into SRAM ring, polled; codec UL bring-up only while `/dev/...-mic` is open; HAL reads it | `mt6781_afe.c` (`ul_*`), `mtk_audio.c` (`mic_*`), `mapple_audio.c` | `redwood_mtkaudio.c` (no UL) | design direct; AFE (VUL) and MT6357 codec registers re-derived from a `tinycap` trace | M | low-med |
| 2 | `AppleARMBacklight` + `IODisplayParameters` (CoreBrightness, Night Shift via selector 55/CCORR) | `MappleFramebuffer.cpp` | inject.c brightness hook | backlight direct; CCORR needs MT6765 base/scale | S | low |
| 3 | UI-activity CPU boost from SwapEnd and touch | `mt6781_cpufreq.c` (`ui_activity`), callers in framebuffer/touch | r1 governor | direct | S | low |
| 4 | USB device "link" persona (logs, push/pull, run) in iOS; later PTP+usbmux | `usb_link.c` | none | MUSB same IP; PHY/clock addresses from r1 DT | M (L for std persona) | med |
| 5 | In-kernel IOHIDFamily: touch/buttons as real HID services, BT keyboards via IOHIDResource | `iokit/Kernel/arm/hid/*` | `/dev/redwood-touch` + inject.c | direct (verify 16.5 ABI) | M-L | med |
| 6 | GPU-idle-gated swap queue, early present, direct scanout | `MappleFramebuffer.cpp`, `MappleSurface.cpp` | `MiniIOMobileFramebuffer.cpp` (`fb_flip`) | needs PVR fence hook; OVL similar | M | med |
| 7 | BlueTool's Broadcom script answered in-kernel (`com.apple.uart.bluetooth`) + `AppleOLYHAL` WifiVendor | `mtk_bt.c` (`bcm_*`), `MT6631WLAN.cpp` | inject.c BlueTool takeover | direct | S-M | low |
| 8 | `AppleM2ScalerCSCDriver` (IOSurfaceAccelerator) with CPU backend | `MTKScaler.cpp/.h` | none | direct (CPU path) | S-M | low |
| 9 | AKS: always-loaded system bag, two-slot persistent record (passcode optional) | `MappleKeyStore.cpp`, `MappleKeyStorePasscode.h` | `MiniAppleKeyStore.cpp` | direct (check 16.5 selectors) | S-M | low |
| 10 | Persistence via built-in HFS+ (+ FileProvider APFS answers), persistent console log, boot-args record in partition tail | `bsd/hfs_staged/*`, `mtk_ufs.c` (log/bootrec parts) | own APFS/ramfs, `redwood_emmc.c` | HFS direct; storage driver ours (eMMC) | M | med-high (eMMC writes) |

Honourable mentions: `MappleSPU` sensor services fed by our ICM-42607P (after #5; M), `MappleAMFI` selector 6 for
extensions (S), PMIC interrupts + clean power-off through pwrap (S-M), register-whitelisted writers everywhere
(pattern, free), `proc_sampler.c` for the Safari spin (S).

Things they do **not** solve for us: cellular (no modem code), H.264 encode, HFP/SCO, sleep/low-power idle, GPS,
camera, the 8th core (that is a loader question).
