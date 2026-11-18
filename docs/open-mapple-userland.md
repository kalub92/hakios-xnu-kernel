# Open Mapple: user space and the "no injection" claim, compared with our port

Written 2026-11-18. This is research only: nothing in our tree was changed, and nothing of Open Mapple's was run.

Sources read (a shallow checkout in the session scratchpad, `open-mapple/`):
- `mapple_userland`: `audio/audio_plugin.c`, `mapple_audio.h`, `tools/launchd-add.py`, `tools/mappled/*`
- `mapple_build`: `tools/root-tweaks.py`, `tools/mkiosroot-macos.sh`, `tools/rootranges.py`, `tools/mkadt.py`, `core/mka.sh`, `installer/mapple-install.c`, `ios165/fetch-from-ipsw.py`
- `mapple_vendor/config/common.conf`
- the user-space-facing parts of `xnu_kernel_mediatek_common` (`bsd/kern/amfi_stub.c`, `bsd/kern/kern_exec.c`, `iokit/Kernel/arm/Mapple*.cpp`, `hid/MappleHID.cpp`, `hid/MappleSPU.cpp`, `bsd/dev/mtk_bt.c`, `mtk_audio.c`, `mapple_audio.c`) and `xnu_kernel_mediatek_mt6781/iokit/Kernel/arm/MTKJPEG.cpp`, `MTKScaler.cpp`, `MTKMDP.cpp`
- the public pages https://open-mapple.pages.dev/devices/spaced/ and https://open-mapple.pages.dev/mapple/

`mapple_manager` (the Mapple app, its tweak loader and `fpdvolume.dylib`) is a private repository. Only its release tarball is pinned in `common.conf`, so anything about how the app works inside is not found.

## 1. The "no injection" claim, checked

The claim mostly holds, with three caveats:

1. **They edit Apple's configuration files and plists, not binaries.** `root-tweaks.py` rewrites `/System/Library/xpc/launchd.plist` (launchd reads the edited cache because of the boot-arg `launchd_unsecure_cache=1`), the jetsam tables, `MediaToolbox.framework/*/SubServerConfiguration.plist`, the preference plists, and moves `VirtualAudio.plugin` and `DataClassMigrators`.
2. **One library is injected in every release.** `kern_exec.c` (`exec_extract_strings`, around line 5810) adds `DYLD_INSERT_LIBRARIES=/usr/lib/mapple/fpdvolume.dylib` to `fileproviderd` (`mka.sh`: "fileproviderd's volume fix, the one library injected"; it is on unless `mp_fpd=0`). It lives in the private repository, so we cannot see what it fixes.
3. **Tweaks are opt-in injection.** With `mp_inject=1` (the Jailbreak switch in the Mapple app), the kernel adds `/usr/lib/mapple/tweakloader.dylib` to every program inside an `.app/`, which includes SpringBoard but never daemons. `amfi_stub.c` then answers dyld's AMFI policy call (`__mac_syscall("AMFI", 90)`) so that `DYLD_INSERT_LIBRARIES` and interposing are allowed only in that case.

Everything else that our dylibs do, they do with:
- a kernel IOKit service that iOS already knows how to use,
- a device-tree property,
- a plist edit,
- or a separate daemon of their own, run as a launchd job.

The key enabler for that last option is `amfi_stub.c`: **every binary on the root filesystem is a platform binary**, so launchd will start their own programs (`mappled`, `mapple-captured`, `mapple-usbd`) as system jobs. Our `inject_sb.c` and `inject.c` (the BlueTool section) say launchd panics on "unexpected exec of non-platform binary", and that is why our code had to live inside Apple's processes.

## 2. Our mechanism, their mechanism, and whether we could drop our injection

"Drop?" means "could we stop injecting this if we copied their approach".

| Our mechanism (file, process) | What it does | Open Mapple's mechanism (file) | Drop? / port effort / risk |
|---|---|---|---|
| `inject.c`, backboardd: virtual HID service fed from `/dev/redwood-touch`; buttons | Touch and buttons | **Kernel IOHIDEventServices**: `hid/MappleHID.cpp` (a minimal `IOHIDEventService`, plus `IOHIDEventServiceUserClient` compiled from IOHIDFamily-1915.140.3), `MappleHIDTouch`, `GT9896.cpp`/`ILI7807S.cpp` (touch), `MT6358Keys.cpp`/`MTKKeypad.cpp` (keys). backboardd opens them with type `'esuc'`. | **Yes.** The code ports directly: our touch drivers call `mapple_hid_touch()` and `mapple_hid_key()`. Effort **L**. Risk medium: backboardd's digitizer matching (our notes record its "waiting for a main screen digitizer" trouble). It also removes the 5 s ALS wait hack and the touch-service timing race. |
| `inject.c`, backboardd: skips the 5 s wait for the ambient light sensor | Boot delay | `hid/MappleSPU.cpp` publishes an ALS HID service (0xFF00/0x04), with `mp_als_default` lux reported before the first reading | **Yes**, once any ALS service exists (even a fixed value). Effort **M** (it needs the kernel HID base above). Risk low. |
| `inject.c`, backboardd: orientation from `kern.redwood.accel` into `BKOrientationManager` | Rotation | `MappleSPU.cpp`: accelerometer and orientation (0x20/0x8A) HID services, opened through AppleSPULib (`'ashd'` user client), fed by the SCP sensor hub (`mtk_scp.c`) | **Yes.** Our `redwood_scp.c` already reads the hub. Effort **M-L**. Risk medium: the AppleSPULib user-client protocol has to be right. |
| `inject.c`, backboardd: backlight hook (`BrightnessSystem`, `CBBrightnessControlAvailable` forced) | Brightness slider | `MappleFramebuffer.cpp` (their `AppleCLCD`/IOMobileFramebuffer): backlight levels, `mp_fb_blmin`, `mp_fb_blcal` (nits range for CoreBrightness), panel off at selector 12 | **Probably.** I did not find how they make CoreBrightness enable the slider, so treat this as unverified. Effort **M**. Risk medium. |
| `inject.c`, backboardd and `com.apple.WebKit.GPU`: renderer kept on GLES, shader-table fills, metallib workarounds (`camtl`) | GPU drawing | A real Metal driver (`MappleMTLDriver.bundle`, Mesa-based, from `mapple_gpu_mali`), and the full IPSW root so `QuartzCore/default.metallib` is present | **Only with a Metal driver for the GE8320.** That is out of scope here (it is the GPU agent's area). Effort **L+**. |
| `inject.c`, containermanagerd: `personasAreSupported` → NO | Stops a 20/s retry loop | **Nothing in user space.** A persistent `/var`, stock usermanagerd and the kernel's personas. `kern_exec.c` only relaxes persona spawns for Sileo. | **Maybe.** Check whether our kernel config enables `CONFIG_PERSONAS` and whether usermanagerd runs. Effort **S** to test. Risk low. |
| `inject.c`, bluetoothd: exit watchers, LE scan-entry fix, HCI quirks | Bluetooth | Kernel: `mtk_bt.c` (H4 framing plus controller-answer rewrites, e.g. LE Read Periodic Advertiser List Size answered in the kernel), `mtk_btsk.c` (Skywalk pipe), `MappleBluetooth.cpp` (registry node `"bluetooth"` with `local-mac-address`). `mtk_bt.c` says it is "the part of HakiOS's redwood_connsys.c" that does not depend on the phone. | **Mostly.** Move the HCI quirks into `redwood_connsys.c`. The bluetoothd scan-entry fix (`bt_entry_fix`) has no counterpart in their tree: not found. Effort **M**. |
| `inject.c`, BlueTool: in-process stand-in for `com.apple.BlueTool` | Broadcom boot script | `mtk_bt.c` registers the kernel control `com.apple.uart.bluetooth` and answers BlueTool's built-in Broadcom script (HCI vendor commands, termios get/setsockopt) in the kernel. `mkadt.py` publishes `wifi-vendor` (MobileGestalt `WifiVendor` picks the script). | **Yes.** Stock BlueTool runs unmodified. Our controller is the same MT6631-class CONNSYS. Effort **M**. Risk low-medium. |
| `inject_sb.c`, SpringBoard: "activated" | Lock screen instead of hello | **Device tree** (`mkadt.py`): `/chosen` `certificate-production-status=0`, `effective-production-status-ap=0`, `certificate-security-mode=1`, `effective-security-mode-sep=1`. mobileactivationd's Hactivation branch then reports Activated. `root-tweaks.py activation()` seeds `data_ark.plist` (`-ActivationState Activated`). mobileactivationd runs. | **Yes.** Our `mtkloader/mtkloader.c:937` and `xnuloader/loader.c:3109` write production status **1** (production-fused). Change them to 0/0/1/1, add `effective-security-mode-sep`, run mobileactivationd, and seed the data ark. Effort **S**. Risk low-medium: other code may read the fusing (debug UI, Apple ID); test it. |
| `inject_mad.c`, mobileactivationd: `MGCopyAnswer` interposed for the same four keys | App Store activation | The same device-tree properties | **Yes**, the same change as the row above. Effort **S**. |
| `inject_sb.c`: no setup assistant | Skip Buddy | Plists: `com.apple.purplebuddy` (`SetupDone`, `SetupFinishedAllSteps`, `SetupVersion 11`, `daemonizeNeedsToRun`), `.notbackedup` mini-buddy flags off, `com.apple.managedconfiguration.notbackedup`, `FBLaunchWatchdogExceptions` for purplebuddy | **Yes.** We already ship `userland/var/.../com.apple.purplebuddy.plist`; add the `.notbackedup` keys. Effort **S**. |
| `inject_sb.c`: `sb_delay` (apps registered anew each boot because `/var` is empty) | Home-screen layout | A persistent writable root. Staged apps are installed once by DataMigration with only `SystemAppMigrator` (the other migrators are moved to `DataClassMigrators.disabled`). | **Yes, once `/var` persists** (§4). |
| `inject_sb.c`: snapshots as JPEG/PNG not ASTC; plain vibrancy; lock-screen clock without a GPU | GPU stand-ins | A Metal GPU | **No**, not without Metal. |
| `inject_sb.c`: side-button long press; poster-switcher block | UI bugs | Not found | Keep. |
| `inject_sb.c`: battery state from a charger stack | Battery | `MapplePowerSource.cpp` (an `IOPMPowerSource`, fuel gauge polled every 30 s) | **Check** whether the r1 still needs it. We already have `redwood_battery.c` and `redwood_mtkbatt.c`. |
| `inject_sb.c`: Darwin-notification remote control (`com.redwood.unlock`, `layers`, `switcher`) | Test harness | `mappled` over the USB link (`run`, `autorun/`), development builds only | Keep (it is harness only). |
| `inject_media.c`, mediaserverd: skip CoreImage portrait prewarm | Crash loop | **Plist**: `StartRecorderServer=false` in `MediaToolbox.framework/*/SubServerConfiguration.plist`. That turns off mediaserverd's whole capture side. | **Not compatible with our camera.** That plist turns off CMCapture in mediaserverd, and our camera lives there. Keep, or adopt only if the camera moves to a daemon. |
| `inject_media.c`: flashlight vendor stand-in | Torch | `mapple-captured` daemon (launchd job edited into the cache; capture Mach services removed from mediaserverd's job): serves `com.apple.coremedia.flashlight` → sysctl `kern.mapple_torch` | **Only if** we accept their whole design (recorder server off). |
| `camera_device.c` + `camera_isp.c` (in mediaserverd) | Camera | `mapple-captured.c`: re-implements CoreMedia's capture XPC server (FigXPCServer, CAImageQueue preview, JPEG stills) with **a test pattern only**. Their site lists Camera as "WIP, release 2". | **No. Ours is ahead.** It has real frames and Camera.app works. Idea worth taking: `pardon_api_violations` and Camera's "Most Compatible" format pref (`com.apple.camera` `CAMUserPreferenceCaptureEncodingBehavior=1`, so JPEG instead of HEIC). Effort **S**. |
| `inject_tweaks.c` in Preferences/iconservicesagent/SpringBoard: tweak loader | Tweaks | `tweakloader.dylib` put in by the kernel at exec (`mp_inject=1`, `.app` processes only), and the Mapple app's switch. Safe boot turns it off. | Same idea. Theirs is kernel-gated by a boot-arg (§5). |
| `inject_tweaks.c`: Wallet row nil-guard, CoreImage guard, fault handler in Preferences | Settings crashes | **Nothing needed**: the full IPSW plus a Metal device means CoreImage gets a context. | **No**, not without Metal. Alternative to test: drop the Wallet row by not having `Passbook.app` registered. Not tried. |
| `inject_app.c` | Crash logging | DEVELOPMENT kernel and `mappled` | Keep (dev only). |
| `inject_kc.c` (akd keychain class) | Apple ID | Apple ID, iCloud and the App Store are out of scope (README ground rules) | Keep (we have nothing to copy). |
| `audio_plugin.c` | Audio HAL | `mapple_userland/audio/audio_plugin.c`, the same code base (a real AudioServerPlugIn, no injection) | §3 |
| `init.c` (pid 1 before launchd: ramfs `/var`, var skeleton, staged apps, `/var/jb`, ringtone bind) | Boot | Stock `/sbin/launchd` is pid 1 on a writable root (`rd=ufsroot0`). Directories are made once by `mkiosroot`/`root-tweaks` (`/private/var/containers/*`, the logd stores with `.migrated` markers, `systembag.kb`). | **Only with a persistent writable root** (§4). Effort **L**. |
| `rc.jobs` / `com.redwood.demo` / `mkjobs.py` | Test runner, job set | Stock `launchd.plist` with a few jobs deleted. Their own jobs are added to the cache (`launchd-add.py`) with `JetsamProperties`. Test programs go in `/private/var/root/mapple-dev/autorun`, run by `mappled`. | Our harness stays. Their job edits (§4.1) are worth copying. |

## 3. Audio HAL plug-in: theirs vs `userland/src/audio_plugin.c`

The two files are the same program. With mapple/redwood and Darwin/iOS renamed, `diff` shows only these differences.

**Earpiece (receiver) route: they have it, we don't.**
- The device `"Codec"` gains a 3-channel output (`OBJECT_CODEC_OUTPUT`, terminal type `'rspk'`) and a mute control on output element 3 (`OBJECT_RECEIVER_MUTE`, `RECEIVER_ELEMENT 3`). VirtualAudio's `Device_Codec_Aspen` builds the receiver port only when that mute control exists.
- `receiver_write()` takes channel 3 if it has anything, otherwise mixes channels 1 and 2.
- `route_to()` sends the new ioctl `MAPPLE_AUDIO_SET_ROUTE` (`_IOW('r', 2, uint32_t)`, 0 = speaker, 1 = receiver), whichever device last carried non-silent samples. The kernel (`mtk_audio.c`, `mp_audio_rcv`) then switches between the codec's handset driver and the line-out plus amplifier.
- Their known limit (site): the speakerphone button does not move sound off the earpiece. The route follows the samples, not VirtualAudio's chosen port.
- Our counterpart: none. Portability: the plug-in part is **direct**. The kernel side needs the MT6357 handset path (HPL/RCV registers differ from the MT6366). Does the r1 have an earpiece at all? It is a speaker-only device, I believe, so this is **probably not needed**. Effort S (plug-in), M (kernel).

**NaN volume fix: they have it.** `wanted = isnan(wanted) ? 0 : wanted;` in SetPropertyData for the volume. A NaN passes both clamps and would play at the loudest. Effort: **S, one line, take it**. Risk: none.

**Microphone.** Both have the same `"Codec"` input and the same `'imic'` data source. Their comment still says "silence until the phone's microphone has a driver", but their kernel has it: `mtk_audio.c` step 5, "AIN0 through the codec's UL and VUL12 to /dev/mapple-mic". The UL memif is in `osfmk/arm64/mt6781_afe.c` (`mt6781_afe_ul_start/_position/_stop`). Their site says "Microphone: Mostly. The main mic records; a headset mic doesn't work."
- So **our plug-in already needs no change for the mic**. The gap is only our kernel `redwood_mtkaudio.c` capture path.
- Their MT6781 AFE UL/VUL12 code plus the MT6366 UL (ADC) setup are the reference. The MT6765 AFE and MT6357 codec are the same family, but the register offsets differ, so this "needs register changes". The kernel agent should read `mt6781_afe.c`; I did not read it in depth.

**Bluetooth audio coexistence.** Neither plug-in has anything for Bluetooth. Theirs keeps the stock `BTAudioHALPlugin.driver` beside it, in `/System/Library/Audio/Plug-Ins/HAL`, because their root is the full IPSW, and bluetoothd runs unmodified. Their site says headphones play music; a headset mic doesn't work (so no HFP mic). One difference that may matter for our "all audio silent after pairing":
- Their speaker-route code drops the speaker samples only while the route is the receiver.
- Ours writes unconditionally.
- That does not explain our silence. Nothing in their plug-in addresses BT-to-speaker fallback, so for that bug: not found.

**Kernel-side difference worth noting.** Their `mapple_audio.c` has a "speaker guard" (`speaker_guard.c`) and amplifier-off after 3 s of silence. Ours has its own. That is not a user-space item.

**What we have that they don't:** the kept-log lines (`kern.redwood.say`) for "is with the HAL / playing / stopped", used for BT debugging.

## 4. System image, launchd, keybag, persistence, Wi-Fi, iOS version

### 4.1 Image and launchd job selection
- **No trimming at all.** `mkiosroot-macos.sh` rsyncs the whole OS image plus both cryptexes (SystemOS, AppOS) onto a 16 GB (configurable) case-sensitive journaled **HFS+** volume. The shared cache is hard-linked to `/System/Library/Caches/com.apple.dyld`, and cryptex files get root-path links. Files are decompressed on copy, because their kernel has no AppleFSCompression.
- So their image ships `Library/Ringtones`, every `/System/Library/Audio/Plug-Ins/HAL` driver (BTAudioHALPlugin included), all `.metallib`s, `mediaparserd`, Wallet, the full fonts and the VideoToolbox codec bundles.
- Ours (`ios165/select-system.py` + `build.sh`) has none of these: `BTAudioHALPlugin`, `Library/Ringtones`, `QuartzCore/default.metallib` and `*.videocodec` all count 0 in `system.files`. We add BTAudioHALPlugin and ringtones back separately in `build.sh`.
- The reason is space: our root goes in RAM or a small partition, theirs on `userdata`.
- **launchd** (`root-tweaks.py launchd()`), with stock `launchd.plist` otherwise:
  - `_PanicOnCrash` is renamed `_PanicOnCrasX` in every job, so a crashing daemon does not panic. *We should check whether our job set relies on avoiding such crashes instead.*
  - Removed: `com.apple.CommCenter` (no baseband yet), `com.apple.fairplayd.H2`, `com.apple.lskdd` and `com.apple.wifip2pd` (each relaunched about once a second), and `com.apple.bluetoothd` unless `device.conf` has `bluetooth 1`.
  - mediaserverd's capture Mach services move to `org.openmapple.mapple-captured`. The jetsam override is 200 MB in `com.apple.jetsamproperties.*.plist`. Their own daemons get `JetsamPriority 180` (above backboardd's 170).
- **Preferences** written:
  - `com.apple.springboard`: `BKNoWatchdogs`, `FBLaunchWatchdogExceptions`, `MCForceShowCloudConfigurationUI=false`
  - Buddy keys
  - optional "light UI": the `com.apple.Accessibility` keys `ReduceMotionEnabled`, `EnhancedBackgroundContrastEnabled`, `ReduceMotionReduceSlideTransitionsPreference`, `AXSDidResetD22Preferences`
  - **Light UI is directly useful for our 8-13 fps swipes.** It is a plist only: S effort, no risk.
- `VirtualAudio.plugin` is kept only with the audio plug-in; otherwise it is moved to `HAL.disabled` (it crashes mediaserverd with no device).

### 4.2 Keybag and data protection
- **Kernel** `MappleKeyStore.cpp`:
  - `kAKSGetSystem` always returns a fixed system-bag handle, so `keybagd --init` never needs a bag file. They still write a plain-plist `systembag.kb` ("SWEET") at image build.
  - Key wrap is a fixed tag plus a static XOR pad, so it is deterministic and keychain items survive reboots with no key material on disk.
  - An optional passcode is kept as a PBKDF2 verifier in `/private/var/keybags/mapple.kb`, in two slots, with iOS's retry delays. It locks the UI only, and the site states there is no hardware data protection.
- **Ours**: `MiniAppleKeyStore.cpp`. The default "reversible transformation" is similar. `aks_keys=1` adds AES keywrap and needs a bag made by `aks_restore`, which is why "BT needs the keybag".
- **Takeaway:** answer `kAKSGetSystem` with a constant handle and make the default wrap deterministic, and the "keybag permanently" decision disappears for Bluetooth and the keychain on saved data. Portability **direct** (it is pure kernel C++, no hardware). Effort **S-M**. Risk:
  - any keys already wrapped with `aks_keys=1` stop reading unless both formats are accepted;
  - it is not a security boundary, and neither is ours.

### 4.3 Persistence of /var
- **Theirs:**
  - Root and `/var` are one writable HFS+ volume on `userdata` (single boot) or a carved partition (SplitBoot). The kernel carries a staged HFS+ driver (`bsd/hfs_staged/`).
  - `rootranges.py` makes a sparse fastboot image of allocated blocks only. The recovery's `mapple-install verify-root` checks every allocated block's SHA-256.
  - The partition tail holds settings, firmware (ustar), a log area and the boot-args record (`MAPPLEBR`, last MiB, `bootrec.c`).
- **Ours:** ramfs `/var` and `var_store.c` archiving it into the `darwin` partition.
- **Takeaway:** a real writable filesystem removes `sb_delay`, the staged-app copy in `init.c`, the `var_store` save-on-exit choreography and the per-boot app registration.
- Portability: **idea only / L**. It needs the HFS+ driver in our kernel, our eMMC write path enabled for a whole partition (our rules currently forbid `emmc_write=` in tests) and a partition big enough for a full root (about 8 GB). Risk: **high** (flash writes; Android coexistence).

### 4.4 Wi-Fi provisioning
- **Theirs:** WPA2 is joined from Settings. wifid computes the PMK and hands it in `APPLE80211_IOC_ASSOCIATE`, and the kernel `mt6631_wpa.c` runs the 4-way and group handshakes. PBKDF2 is only for the USB link's `wifi join`.
- **Ours:** `redwood_wpa.c` + `redwood_apple80211.c` is the same model, with the passphrase provisioned by `tools/provision.sh`.
- Nothing new to take, unless our Settings join path is not wired. Check whether we accept the PMK from `ASSOCIATE`.

### 4.5 iOS 16.7.16 (20H392) vs our 16.5 (20F66)
- **Pins:** `common.conf` pins the iPhone10,3/10,6 16.7.16 IPSW by URL, size and SHA-256. `root-tweaks.py` hard-codes `BUILD, VERSION = '20H392', '16.7.16'` for the data ark and migration.
- **Kernel ABIs written for "iOS 16.7":**
  - the AKS selector numbering;
  - `IOHIDEventServiceUserClient` from IOHIDFamily-1915.140.3 ("the version the iOS 16.7 userland ships");
  - IOMobileFramebuffer selectors;
  - the AppleJPEGDriver ABI and the `IOSurfaceAccelerator` selectors.
  If we copied any of these to 16.5, the selector numbers must be re-checked against 20F66.
- **What on our side depends on 16.5:** every injected hook checks shared-cache addresses of 20F66 (`inject.c`: "Addresses ... are those of iOS 16.5 (20F66)"; e.g. `0x1c3ea6298` for `personasAreSupported`, `0x1d5b54790` for the shader table, bluetoothd `+0x90238`). **Moving to 16.7.16 would break every address-checked hook.** The hooks are checked and would refuse rather than crash, but they would do nothing.
- The less we inject, the cheaper an update becomes. 16.7.x also brings newer WebKit and security fixes.

## 5. The Mapple app, IPA sideloading, tweaks, safe mode

From the site and `mka.sh`. The app source is private.
- **Mapple.app** is in `/Applications` with a set-UID-root `mapplehelper` (`chmod 4755`), so it is a system app. Its Home page shows versions, per-core load, RAM, storage, sensor status and active launchd services (stoppable, except core ones), and whether this is a safe boot.
- Settings it offers: Hide notch, Light UI, Developer settings (sensors, USB link). Changes go into the boot-args record on the partition tail. The **boot partition is never written**, so safe boot always has the factory line.
  - Kernel side: `mapple_sysctl.c`, sysctl `kern.mapple_bootrec`, `mapple_safeline.h`.
- **Safe mode:** hold Volume Down after "Booting Open Mapple", or use "Reboot to safe mode" in the app. mtkloader adds `mp_bootrec=0 mp_mem4g=0`, and every hardware step boot-arg (`mp_audio_steps`, `mp_cam_steps`, `mp_torch_steps`, `mp_vib_steps`, `mp_hidres`, ...) reads `mp_bootrec=0` as "use 0 / the safe default". The tweak loader is not added.
  - **Directly useful idea:** we already gate everything by boot-args, and mtkloader already knows a hold gesture. A "safe line" (all `r1_*` steps off) chosen by holding a key at boot would give the user a way back from a bad stay-in-iOS image without Android. Effort **S-M** (loader plus kernel). Risk low. It must not touch the protected hold-to-exit behaviour.
- **IPA sideloading:** apps are installed as system apps, not sandboxed from each other, ad-hoc signing accepted, FairPlay unsupported. The mechanism is not found (private). Two pieces exist in the kernel:
  - every root-filesystem binary is a platform binary, and entitlements are parsed from the signature's XML blob (`MappleEntitlements.cpp`);
  - a sandbox stub answers container-path and extension calls, so PlugInKit does not reject appexes and tccd can grant Photos to assetsd (`amfi_stub.c` "Sandbox" policy).
  - Plus `MappleAMFI.cpp`: an `AppleMobileFileIntegrity` IOService whose selector 6 (trust-cache cdhash check) answers "trusted". Without it, ExtensionFoundation fails with "Unable to find AMFI" and LaunchServices drops every NSExtension (widgets, poster providers).
  - **Check whether ours has these three:** `miniamfi.c` exists; the user-client selector 6 and the sandbox ops 4/5/6 are what to compare. This may explain missing widgets or wallpaper posters. Effort **S-M** each. Portability **direct** (no hardware).
- **Tweaks:** the Jailbreak switch downloads the Procursus rootless bootstrap and Sileo, checked against fixed checksums. Tweaks need `mp_inject=1` and a reboot. With `mp_inject=1`, Sileo's persona-99 root spawns are honoured without real personas (`kern_exec.c` `mapple_persona_missing_ok`), and `mp_inject_debugged=1` marks injected app processes debugged so tweaks may patch code pages.
  - Ours: `/var/jb` on ramfs and our own loader. Their kernel-side gating (only `.app` processes, never daemons, off in safe boot) is a cleaner model to copy into our `kern.exec_inject`. Effort **S**.
- **USB in iOS (dev and standard):**
  - `mapple-usbd.c` is a user-space daemon that speaks PTP/MTP over `/private/var/mobile/Media` and the usbmux device side, bridged to lockdownd on `127.0.0.1:62078`. The kernel `bsd/dev/usb_link.c` only moves bulk transfers through `/dev/linkptp` and `/dev/linkmux`.
  - `mappled` runs commands over `/dev/mapplelink` (dev builds), with `mstat`/`vln`/`bootrec` tools.
  - The daemon is **directly portable**; the USB device controller is not (MT6765 uses MediaTek's MUSB, and I did not check what `usb_link.c` drives). It is relevant to our "no adb in iOS" rule. Effort: daemon **S**, kernel gadget **L**.
- **Low Power Mode bridge** (`mappled.c`): listens for powerd's `com.apple.system.lowpowermode` notification and copies it into a kernel sysctl that caps GPU and panel duty. A **direct** idea for our governor (`r1_gov`). Effort **S**.

## 6. Video, Safari/WebKit media, camera

- **Video decode / Safari:** their site says Safari plays video, "frames are converted on the CPU, using most of one core". No video decoder driver was found; VideoToolbox's software decoders run because the full root ships them. Our image drops `*.videocodec`; our `inject_media.c` relies on `VCPHEVC.videocodec` for HEIC. **Our Safari audio spin and download failure may be missing-file problems.**
  - Check `mediaparserd` (we have it only with `MEDIAPARSERD=1`).
  - Check the `MediaToolbox`/`AudioToolbox` codec bundles and `/System/Library/Audio/Plug-Ins` that `SKIP_SYSTEM_LIBRARY = (... 'Audio' ...)` removes. Only `UISounds` is added back.
  - Effort **S** (re-run `select-system.py` with these kept). Risk: image size.
- **Video encode:** not found. No H.264/HEVC encoder in any repo; recording is not listed on their device page.
- **JPEG encode on hardware:** `MTKJPEG.cpp` + `MTKJPEGHW.cpp` publish a service *named* `AppleJPEGDriver`, which VideoToolbox's `JPEGH1.videoencoder` finds by name. Selector 3 encodes YUYV/NV12 IOSurfaces to JFIF on MediaTek's jpgenc. MT6765 has a JPEG encoder too, so this "needs register changes". It would speed our stills. Effort **M**.
  - Their `MTKScaler.cpp`/`MTKMDP.cpp` (`AppleM2ScalerCSCDriver`, IOSurfaceAccelerator: NV12→BGRA and scaling on the MDP engines, CPU fallback) is relevant to our camera and preview cost. That is for the kernel/GPU agent.
- **Camera in user space:** `mapple-captured` (a test pattern) is behind our `camera_device.c`. Take only the two plist settings in §2.

## 7. Ranked top 10 for the r1

| # | What | Open Mapple path | Our counterpart | Portability | Effort | Risk |
|---|---|---|---|---|---|---|
| 1 | Activation by device tree (0/0/1/1 security status) + data ark; drop `inject_mad.c` and the activation hook in `inject_sb.c` | `mapple_build/tools/mkadt.py` (`/chosen` props), `root-tweaks.py activation()` | `mtkloader/mtkloader.c:937`, `xnuloader/loader.c:3109`, `inject_mad.c`, `inject_sb.c` | direct | S | low-med |
| 2 | Keystore: constant system-bag handle + deterministic wrap; Bluetooth/keychain without `aks_restore` | `xnu_kernel_mediatek_common/iokit/Kernel/arm/MappleKeyStore.cpp` | `xnu/files/iokit/Kernel/MiniAppleKeyStore.cpp`, `aks_restore.c` | direct | S-M | med (old wrapped keys) |
| 3 | `_PanicOnCrash` rename + remove the respawning jobs (lskdd, wifip2pd, fairplayd.H2, CommCenter when not testing cellular) + Light UI accessibility prefs for swipe fps | `mapple_build/tools/root-tweaks.py` | `userland/mkjobs.py`, `userland/var/...` prefs | direct | S | low |
| 4 | NaN guard on the plug-in's volume | `mapple_userland/audio/audio_plugin.c` (SetPropertyData) | `userland/src/audio_plugin.c` | direct | S | none |
| 5 | Mic capture: the AFE UL/VUL12 + codec UL path behind `/dev/*-mic` (plug-in already ready) | `xnu_kernel_mediatek_common/bsd/dev/mtk_audio.c` step 5, `xnu_kernel_mediatek_mt6781/osfmk/arm64/mt6781_afe.c` | `xnu/files/bsd/dev/redwood_mtkaudio.c` | needs register changes (MT6765/MT6357) | M | med |
| 6 | BlueTool answered in the kernel (`com.apple.uart.bluetooth` control) and the HCI quirks in the kernel; drop the BlueTool and most bluetoothd hooks in `inject.c` | `bsd/dev/mtk_bt.c`, `MappleBluetooth.cpp` | `redwood_connsys.c`, `RedwoodBluetooth.cpp`, `inject.c` | direct (it descends from our code) | M | low-med |
| 7 | AMFI IOService selector 6 + sandbox stub ops 4/5/6; check against our miniamfi for widgets, appexes and Photos grants | `iokit/Kernel/arm/MappleAMFI.cpp`, `bsd/kern/amfi_stub.c` | `xnu/files/libkern/amfi/miniamfi.c` | direct | S-M | low |
| 8 | Image contents for media: keep `/System/Library/Audio` plug-ins, `*.videocodec`, mediaparserd, ringtones in `select-system.py` (Safari audio and downloads) | `mkiosroot-macos.sh` (full root) | `ios165/select-system.py`, `userland/build.sh` | direct | S | image size |
| 9 | Kernel HID event services for touch, buttons, ALS and orientation; drop the backboardd input, ALS and orientation hooks | `iokit/Kernel/arm/hid/MappleHID.cpp`, `MappleSPU.cpp` | `inject.c` (backboardd sections), `redwood_input.c`, `redwood_scp.c` | direct code, our drivers feed it | L | med |
| 10 | Safe-line boot (key held, all `r1_*` steps 0) + platform-binary marking for root-fs programs, so our helpers can be plain launchd daemons instead of injected code | `bsd/dev/mapple_safeline.h`, `mapple_sysctl.c`, `amfi_stub.c` (`MNT_ROOTFS` → `CS_PLATFORM_BINARY`) | `mtkloader/`, `miniamfi.c` | idea / direct | S-M | low (do not touch hold-to-exit) |

Further out: a persistent writable HFS+ root (§4.3, L, high risk), a Metal driver (removes the GPU hooks and both Settings guards, GPU agent's area), `mapple-usbd` for USB in iOS (needs a gadget driver), and the hardware JPEG encoder.
