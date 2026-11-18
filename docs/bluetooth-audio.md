# Bluetooth audio: why a connected speaker silenced everything, and the two fixes

Analysis only (Opus, 2026-11-18). No code was changed. Inputs: HANDOFF-2026-11-16.md (Bluetooth parts),
HANDOFF-2026-11-17-night.md ("Bluetooth session prep"), userland/build.sh, userland/src/{audio_plugin.c,
inject.c, inject_media.c, init.c, sound_test.c}, ios165/{select-system.py, system.manifest, rootfs.slt},
xnu/files/bsd/dev/{redwood_connsys.c, redwood_btsk.c, redwood_audio.c}, strings of the iOS 16.5 bluetoothd,
VirtualAudio.plugin and the dyld cache, logs/btS1.log. **GUESS** marks anything not read in code or a log.

## What happened in the hands-on session
The user paired a speaker in Settings and it showed Connected (the speaker played its own chime). After that,
nothing played: not on the speaker, not on the r1's speaker. Ringtone previews were silent and web audio in
Safari kept spinning. The session's log is not here yet.

## 1. Who would carry the audio, and what is missing

How A2DP works on iOS 16 (from strings in the iOS binaries):
- **bluetoothd** listens on the Mach service `com.apple.BTAudioHALPlugin.xpc`. Its job plist
  (`root/LaunchDaemons/com.apple.bluetoothd.plist`) lists the service, and btS1.log shows
  `xpc_connection_create_mach_service(com.apple.BTAudioHALPlugin.xpc, 0x1)`, where 1 means listener.
  bluetoothd sends each audio device and its properties (`kBTAudioMsgPropertyAllowAutoRoute`,
  `...Codec`, `...DeviceUID`) to the plug-in. It gets PCM back through shared memory
  (`kBTAudioMsgArgDeviceSharedMemoryOutput`) and does the SBC encoding itself
  ("A2DP configured at %2.1f KHz. Codec: SBC, Bitpool: %d ..."). It sends AVDTP media over L2CAP over ACL.
- **BTAudioHALPlugin.driver** runs inside mediaserverd as a CoreAudio HAL plug-in. It turns each of those
  devices into a HAL audio device with a UID ending in `-tacl` (A2DP) or `-tsco` (HFP).
- **VirtualAudio.plugin** (VAD, also in mediaserverd) creates a Bluetooth port only from such a HAL device
  ("creating a bluetooth A2DP headphones port with name ... and UID ...", "Bluetooth audio device with UID ...").
  MediaExperience then picks that port as the route.
- **Our HCI transport** carries the ACL packets: bluetoothd -> Skywalk channel `com.apple.uart.sk.bluetooth`
  (redwood_btsk.c `btsk_sync_tx`) -> `redwood_connsys_bt_send` -> `bt_txq` -> `bt_h4_length` frames
  types 1, 2, 3 and 5 -> STP channel 0 -> BTIF.

What this port has:
| Piece | State |
|---|---|
| BTAudioHALPlugin.driver | **Missing.** It is in the IPSW (`System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver`, binary 562,992 B), but `select-system.py` skips `System/Library/Audio` (it only keeps `/Library/Audio/Plug-Ins` = VirtualAudio, plus UISounds). It is in neither `system.manifest` nor `ios165/rootfs`. build.sh creates `System/Library/Audio/Plug-Ins/HAL` with only `RedwoodAudio.driver` in it ("Apple's are left out"). `root/hal` holds only RedwoodAudio, and init.c binds it over the HAL directory on flash-root boots. Its dependencies (`BluetoothAudio`, `AudioServerDriver` and the other frameworks) are in the dyld cache, and their Info.plists are in the manifest. Only the plug-in bundle itself is missing. |
| bluetoothd A2DP source / SBC encoder | Present, inside bluetoothd. It never got a plug-in to talk to. |
| ACL over HCI | Complete in principle. H4 type 2 is framed, and the controller's Read_Buffer_Size is ACL 1021 B x 6 and SCO 184 B x 1 (btS1.log), so 1026 B frames fit the 1100 B STP limit. Two weaknesses (spec B5): the Skywalk TX path drops a packet without any error when `bt_txq` (8 KB) is full, and the bridge loop reads only one RX frame per 2 ms pass. |
| SCO / HFP voice | Effectively absent. SCO is framed, but MTK combo chips usually route SCO over the PCM bus to the AFE, and nothing does that here (**GUESS** for MT6631 on the r1). |
| RedwoodAudio (`mtkaudio=2`) | Our HAL plug-in publishes devices named "Speaker", "Hawking", "Actuator" and "Codec" with transport `bltn`. VAD maps these onto the iPhone X routes. It knows nothing about Bluetooth. inject_media.c does not touch audio routing (it only covers the camera and Photos). |

Consequence: A2DP output cannot work in this build. Note, though, that a missing plug-in alone points to the
audio staying on the speaker: no HAL device means no VAD Bluetooth port, which means no route to switch to.
So the silence needs a second cause. The candidates are ranked below.

## 2. Candidate causes, ranked, and how to tell them apart

| # | Cause | Why | Unattended check | Needs the user |
|---|---|---|---|---|
| H1 | **The audio server is stuck or crashed after the A2DP/HFP connection.** The route change waits for a Bluetooth device that never appears, or mediaserverd dies. (**GUESS**) | A Safari spinner together with silent previews looks like clients blocked on mediaserverd, not audio playing into a dead device. Relevant strings in the cache: AudioToolbox's "error %d getting kAudioHardwarePropertyPlugInForBundleID for BTAudioHALPlugin.", "ignoring stream / stream format change because route change is pending", "We were waiting for a route change". | Not without a connected device. | Pair or connect the speaker with the L1-L6 logging below. Signs: mediaserverd pid changes in the 10 s `redwood-gl: programs` line; launchd "com.apple.mediaserverd ... crashed"; no `redwood-audio: N s written` after the connection; no `redwood: audio: playing` (L2); `sound_test route` does not return. |
| H2 | **The BT image is silent even without pairing** (RC=rc.launchd.aks, keybagd, `connsys_bt=2` power or clock side effects on the AFE/MT6357/AW87390, fresh RAM /var). | "Audio works" was confirmed on a non-BT image. Nobody tested sound in this image before the pairing. | **Yes (U1, run this first):** BT build + BASE `connsys_bt=2 aks_keys=1 idle=1`, rc.jobs line `/redwood/bin/tolog /redwood/bin/sound_test springboard route tone 3 system 1100 route &`. PASS = `redwood-audio: 3.xx s written ... of it sound` and route output "Speaker". | Optionally, play one ringtone preview before pairing next time. |
| H3 | Safari's spinner was the network (Wi-Fi/BT coexistence while ACL is active) and the silent previews come from ringer or volume state on the fresh RAM /var. These are two separate effects. (**GUESS**) | Each explains one symptom. | Partly: U1 shows whether the ringer is on (`sound_test springboard` waits for it). | Ask: did other web pages still load after pairing? |
| H4 | The route really moved to a Bluetooth port. | Needs a HAL Bluetooth device, and the plug-in is absent, so this is unlikely. | n/a | Present if VAD logs "creating a bluetooth A2DP headphones port" (L4). |
| H5 | The kernel BT bridge thread starved audio during the ACL burst. | The measured BT cost was at most ~10 % (btD/btE). The polling uses `delay(100)`. Unlikely. | No. | mediaserverd CPU in the programs line, plus `lost (the sink was full)` in the `redwood-audio` line. |

Can be verified with nobody there: U1 (H2), whether the plug-in loads and connects to bluetoothd (B4), whether the
spec A hook fires, and whether mediaserverd stays up. Needs the user and the speaker: anything that depends on a
connected device (H1, H4, A2DP sound).

## 3A. Minimal safe fallback: Bluetooth never takes audio away from the built-in speaker

Idea: bluetoothd has preference keys `DisableA2DP` and `DisableHFP` (strings in the 20F66 binary, next to the
coexistence and debug keys). Force both on inside bluetoothd. The speaker then pairs and connects without any
audio profile, MediaExperience sees no Bluetooth audio route, and everything plays on "Speaker". This only helps
if H1 is the cause. Run U1 first: if U1 fails, fix H2 instead, and A does not matter.

Where to hook: `userland/src/inject.c`, in libredwood.dylib, which is already injected into bluetoothd
(`in_bluetoothd`, set at about line 3575).
0. **Verify the read path first (5 minutes, no device needed):** in `ios165/rootfs/usr/sbin/bluetoothd`, find the
   xrefs to the CFString "DisableA2DP" (rea `find_xrefs_to_name`/`search_strings`, or `otool -tV` around the
   cfstring address). Record which call reads it (`CFPreferencesCopyAppValue`, `CFPreferencesGetAppIntegerValue`,
   `CFPreferencesCopyValue` or `_CFPreferencesCopyAppValueWithContainer`, all imported by bluetoothd), the domain
   (**GUESS**: `com.apple.MobileBluetooth.debug`), and when it is read (at start or at profile connect). If the
   read goes through NSUserDefaults or Foundation, interposing will not see it, so use step 3 instead.
1. Interpose the call or calls found in step 0. Add a new entry to an `__interpose` table, the same way as
   `xpc_service_interpose` at about line 3183. When `in_bluetoothd` is set, `!bt_audio_on`, and the key equals
   "DisableHFP", or "DisableA2DP" with the fallback on: return `CFRetain(kCFBooleanTrue)` from Copy variants, or
   `1` with `*keyExistsAndHasValidFormat = true` from the Integer variant. Pass every other key through.
   Log each key once: `redwood: bluetoothd pref DisableA2DP (domain %s, %s) -> forced 1`, via
   `kern.redwood.host_keep`.
2. Boot-arg `bt_audio=1` (read with `sysctlbyname("kern.bootargs")` as the other hooks do) turns off **only** the
   A2DP override, for the real-fix runs. `DisableHFP` stays forced until there is an SCO path.
3. If the read cannot be interposed: have init.c (or the var skeleton, `ios165/mkvarskel.py`) write
   `/var/mobile/Library/Preferences/<domain>.plist` (and `/var/root/...` if bluetoothd runs as root;
   check `UserName` in its plist) with both keys true, before launchd starts bluetoothd.
4. Last resort, independent of iOS internals (kernel, redwood_connsys.c, before the controller): drop any L2CAP
   Connection Request for PSM 0x0019 (AVDTP) and answer it with result 0x0002 "PSM not supported", in both
   directions. This is about 60 lines in the bridge loop. Use it only if steps 1-3 fail.

Verification: unattended, the `forced 1` line appears, bluetoothd does not restart (`bluetoothd[N] is watched`
once), and U1 still passes. With the user: pair, then play a ringtone preview. It should sound on the r1. Settings
may show the speaker as connected without audio, or as not supported (**GUESS**).

## 3B. The real fix: A2DP output (ordered)

B0. Pin down the silence (U1, then one hands-on reproduction with L1-L6). B makes the route really switch, so a
    stuck audio server (H1) has to be understood first.
B1. **Get the plug-in without downloading 4.5 GB.** The Mac has about 3 GB free, and `rootfs.dmg`
    (096-04088-077.dmg) is stored uncompressed in the IPSW zip. Plan (**untested**): a ~40-line Python HTTP server
    on 127.0.0.1 that answers `Range` requests for `/rootfs.dmg` by range-fetching the IPSW at that member's data
    offset. Reuse `directory()`/`fetch()` from `ios165/fetch-from-ipsw.py`. Then run
    `hdiutil attach -readonly -nobrowse -noverify http://127.0.0.1:PORT/rootfs.dmg`, copy
    `System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver/` (binary, Info.plist, en.lproj,
    SpatialAudio.loctable, _CodeSignature) to `../ios-extra/BTAudioHALPlugin.driver` (outside the repo, like
    ../fw-audio), and detach. Only the blocks that are read get downloaded. If hdiutil refuses a non-HFS URL
    image: use a UDIF + APFS reader over the same ranges (e.g. pip `dissect.apfs` with an LZFSE chunk reader).
    That is bigger work.
B2. `userland/build.sh`, when BT=1 and `../ios-extra/BTAudioHALPlugin.driver` exists: copy it with
    `cp -R` to `root/lib/BTAudioHALPlugin.driver` and add
    `ln -s /redwood/lib/BTAudioHALPlugin.driver root/hal/BTAudioHALPlugin.driver` next to the RedwoodAudio link
    at about line 582. Check that root.apfs still fits (4 MB now, plus ~250 KB zlib) and that the payload builds.
B3. `userland/src/init.c`: the r1 boots with `emmc_root` from the system image on eMMC. There,
    `/System/Library/Audio/Plug-Ins/HAL` contains only the RedwoodAudio link, and the system image must not be
    rebuilt (no SYSTEM=1). So on a non-flash_root boot, when `/redwood/hal/BTAudioHALPlugin.driver` exists, run
    `bind_dir("/redwood/hal", "/System/Library/Audio/Plug-Ins/HAL")`, the same bindfs that flash_root uses at
    line 199. It must happen before launchd starts mediaserverd. Check line: `our audio devices + Bluetooth's`.
B4. Unattended run (BT build, `bt_audio=1`, no pairing). Pass:
    (a) the init bind line is ok;
    (b) bluetoothd logs a peer on `com.apple.BTAudioHALPlugin.xpc` with the pid of mediaserverd (L5);
    (c) mediaserverd keeps its pid, and `redwood: audio: the speaker is with the HAL` appears once (L2);
    (d) U1 still passes (`redwood-audio: 3.xx s written`, route "Speaker"), so the plug-in does not break
    built-in audio.
B5. Kernel transport, before the hands-on run (redwood_btsk.c, redwood_connsys.c):
    (i) `redwood_connsys_bt_send` returns bool. `btsk_sync_tx` stops at the first slot that does not fit and
    leaves it for the next sync, so nothing is ever silently dropped (a dropped ACL packet leaks the host's ACL
    credit and stalls the stream);
    (ii) `BT_TXQ` goes from 8192 to 16384;
    (iii) the bridge loop drains every pending RX frame per pass (0 ms wait after the first), so
    Number_Of_Completed_Packets events are not delayed by 2 ms each;
    (iv) the L6 counters.
B6. Hands-on: pair the speaker with `bt_audio=1` (A2DP on, HFP still off), then play a ringtone preview and
    Music. Expected:
    - VAD: "creating a bluetooth A2DP headphones port" (L4);
    - bluetoothd: "A2DP configured at 44.1 KHz. Codec: SBC, Bitpool ..." and "A2DP streaming to device" (L5);
    - L6: ACL out at about 40 KB/s, 0 drops, txq max under 16 KB;
    - sound on the speaker. Stopping playback should give "A2DP streaming duration".
B7. Later and optional: HFP (needs the SCO-over-PCM path through the AFE, a project of its own), AAC, and
    absolute volume.

Unknowns that remain, honestly about 6:
1. whether the hdiutil-over-HTTP extraction works;
2. whether the plug-in loads in mediaserverd under this port (code signature, sandbox lookup of the
   service, any entitlement or MobileGestalt check of its own);
3. the plug-in to bluetoothd handshake and the shared memory on this XNU;
4. whether VAD and MediaExperience accept the port with the D22 (iPhone X) tables;
5. ACL throughput and timing over STP/BTIF with Wi-Fi coexistence;
6. what actually wedged the audio server (H1).

Items 2-3 can be settled unattended in one run (B4). Items 4-6 need the user. Expect 2-4 hands-on sessions.

## 4. Log lines for the next hands-on session (the kept log is 64 KB)

Make room first. These lines would flood the log during an A2DP connection:
- bt_dump's ACL-data clause (`length >= 6 && bytes[0] == 2` counted up to 300 lines in redwood_connsys.c ~1444);
- the "everything for 30 s after Create_Connection" window, 500 lines (cut it to 60);
- the `bt channel: TX slot 90..200` lines in redwood_btsk.c ~142 (cut them);
- the broad bluetoothd filters "tate", "ession", "nable" and "ower" in inject.c `os_log_watched` (keep them for
  the first 60 s after start only).

Add, each line capped:
- **L1** (already exists, just grep for it): the `redwood-gl: programs` line with mediaserverd's pid, and launchd's
  crash lines.
- **L2** audio_plugin.c `say()`: also send `!`-kept to `kern.redwood.say` (as inject_mad.c does), but only for
  "is with the HAL", "playing" and "stopped after" (cap 40). This shows whether IO ever reaches our Speaker
  after pairing.
- **L3** kernel (already exists): `redwood-audio: N s written ...`. Use it as is.
- **L4** libredwood-media in mediaserverd: interpose `_os_log_impl` as inject.c does for bluetoothd. Keep
  formats containing "luetooth", "route change", "BTAudioHALPlugin" or "port with name" (cap 40). VAD is a bundle
  outside the cache, so its calls are interposable. AudioToolbox lives in the cache, so its calls may not be
  (**GUESS**).
- **L5** bluetoothd `os_log_watched`: add "A2DP", "AVDTP", "HFP", "audio-device", "HAL plugin" and "treaming"
  to `wanted` (cap 60). Also log the first peer pid on the `com.apple.BTAudioHALPlugin.xpc` listener (wrap the
  connection handler in the existing xpc_service interpose).
- **L6** kernel bridge: one line every 10 s while any ACL traffic flows:
  `bt: acl out N pkts/B in N pkts/B, txq max B, drops N, nocp N`. Also one line per L2CAP Connection Request
  seen: PSM 0x0019/0x0017/0x0003/0x0001, direction, and result (cap 20).
- A background `sound_test route` every 30 s (cap 10 runs), so the route is on record before and after pairing.
