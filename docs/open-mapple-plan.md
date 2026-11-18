# Plan: using Open Mapple for the r1 port

Written 2026-11-18 from four research reports: `open-mapple-kernel.md`, `open-mapple-userland.md`, `open-mapple-boot-build.md`, `open-mapple-gpu.md`.

## Verdict

Cherry-pick, do not migrate. Open Mapple (github.com/open-mapple) descends from the same HakiOS code as this tree and supports a MediaTek phone (realme 8i, MT6781), so many files have direct counterparts here. But its kernel is UFS-only and gated on MT6781 in 65 files, it has no cellular, no PowerVR, no camera, no sleep, no video encoder, and it targets iOS 16.7.16 (our injected hooks are pinned to 16.5 addresses). Becoming an Open Mapple device would cost months and lose cellular and camera.

Reuse rules: their MIT parts (loader, shim, build tools) and APSL kernel changes may be copied with their copyright notices kept. Their boot-menu code (`splitmenu.h`, SplitBoot) is GPL-3.0 and must not be copied into our loader. The Mapple app is proprietary. Their kernel interfaces are numbered for 16.7; every selector we copy must be re-checked against 16.5.

## Phase 1: small, low risk, no rule changes (start now)

| # | Item | Their file | Ours | Gain |
|---|---|---|---|---|
| 1.1 | NaN volume guard | `mapple_userland/audio/audio_plugin.c` | `userland/src/audio_plugin.c` | avoids full-loudness playback on a bad volume value |
| 1.2 | UI-activity CPU boost (250 ms at max on each swap and touch) | `mt6781_cpufreq.c` | `redwood_cpufreq.c` | responsiveness |
| 1.3 | Boot-args length check at build time | loader | `mtkloader.c`, run scripts | no silent truncation |
| 1.4 | launchd and prefs clean-up: jobs that respawn every second removed, Light UI accessibility prefs | `mapple_build` | `userland/build.sh`, rc files | less idle CPU, maybe swipe fps |
| 1.5 | Pass benchmark and render-thread profiler on pvr; passes per frame with `glfetch=0` | `mapple_gpu_mali` `passbench.m`, `prof.py` | new test tools | tells us where swipe time goes before any graphics work |
| 1.6 | Fence batching (one IOKit call) | mesa patch 0005 | pvr/Zink glue | about 6% of a core |
| 1.7 | Daemon backtrace sampler | `proc_sampler.c` | new | diagnose the Safari audio spin |

## Phase 2: medium, fixes open problems

| # | Item | Their file | Ours | Fixes |
|---|---|---|---|---|
| 2.1 | Microphone: AFE uplink + codec uplink, powered only while `/dev/redwood-mic` is open | `mt6781_afe.c` (`ul_*`), `mtk_audio.c` (`mic_*`) | `redwood_mtkaudio.c` | microphone. Design ports; MT6765 VUL offsets and the MT6357 sequence must be derived (`microphone-spec.md`) |
| 2.2 | Keystore that always has the system bag (no bag file) | `MappleKeyStore.cpp` | AKS path, `aks_restore` | Bluetooth without the permanent keybag switch. Needs a compatibility path for stores written under `aks_keys=1` |
| 2.3 | Kernel backlight service + Night Shift | `MappleFramebuffer.cpp` | `MiniIOMobileFramebuffer.cpp`, `inject.c` hook | drops the injected brightness hook |
| 2.4 | BlueTool answered in the kernel, Wi-Fi vendor service | `mtk_bt.c`, `MT6631WLAN.cpp` | `redwood_connsys.c`, `inject.c` | drops Bluetooth injection |
| 2.5 | IOSurfaceAccelerator service (CPU backend first) | `MTKScaler.cpp` | none | possibly Safari media and CoreImage fallback; verify first with 1.7 |
| 2.6 | Frame-end flip, vsync locked to the panel, present only after the GPU is done | `MappleFramebuffer.cpp`, `MappleSurface.cpp` | `MiniIOMobileFramebuffer.cpp` | double-painted camera frames, tearing |
| 2.7 | Activation by device tree | loader `/chosen` values | `mtkloader.c:937`, `inject_mad.c` | drops activation injection |
| 2.8 | AMFI selector 6 and sandbox ops compared with ours | `MappleAMFI` | `miniamfi.c` | widgets, extensions, Photos grants |
| 2.9 | All of RAM minus holes (about 2 GB to about 3.5 GB) | `holes.c`, `arm_vm_init.c` patch | loader + `arm_vm_init` | memory headroom. A missed reserved region faults; modem and connsys regions must be in the hole list |

## Phase 3: needs the owner's decision (changes a standing rule or the flash layout)

| # | Item | Why a decision |
|---|---|---|
| 3.1 | Payload (kernel + ramdisk) in a logical partition in super, only the loader in boot | changes the flash layout; ends the 32 MB limit and the reflash per test run |
| 3.2 | Persistent console log ring on `xnudata`, flushed every 10 s | eMMC writes during every run, which the rules forbid today. Would have captured the whole Bluetooth hands-on session |
| 3.3 | System image contents: keep audio plug-ins, video codecs, mediaparserd, ringtones, metallibs | needs a system image rebuild (`SYSTEM=1`), forbidden today. Likely fix for Safari audio and downloads |
| 3.4 | Boot-args record + safe boot + our own side-button boot menu (Android default) | changes how the r1 starts and returns to Android |
| 3.5 | Graphics driver loaded from the system volume instead of the boot image (frees 1.7 MB) | driver updates then need a system-image write; moot if 3.1 is done |

## Phase 4: large

- USB device link (`usb_link.c`): live console, file transfer, commands; first step to USB in iOS. MUSB matches; PHY and clock addresses to verify.
- Kernel HID services for touch, buttons, sensors (`iokit/Kernel/arm/hid`, `MappleSPU`): drops backboardd input injection; route to Bluetooth keyboards.
- All eight cores started by XNU with no watcher core (tied to 3.4).
- Metal driver ported onto Zink over pvr: about 4 render passes per home-screen frame instead of 20 to 56; months, high risk. Decide only after 1.5.
- Persistent read-write root or `/var` (their HFS+ import): high risk on eMMC.

## Not available from Open Mapple (stays our own work)

Cellular, video recording (no H.264 encoder there either), Bluetooth call audio, sleep, GPS, camera.

## Open question their work does not answer

Why all audio went silent after pairing a speaker: their Bluetooth audio works with the stock Apple plug-in and nothing Bluetooth-specific in the kernel, which points at our audio plug-in or route handling. Next hands-on session decides.
