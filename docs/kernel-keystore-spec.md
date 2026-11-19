# Kernel keystore without the one-way keybag switch (Open Mapple plan item 2.2)

Analysis and specification only (Opus, 2026-11-18). No code changed, nothing built, device not touched.
Inputs: `open-mapple-plan.md` 2.2, `open-mapple-kernel.md` §9, `open-mapple-userland.md` §4.2, `bluetooth-audio.md`,
`bluetooth-mtk-porting.md`, `bluetooth-x6826b.md`, HANDOFF-2026-11-16.md ("Bluetooth vs the user's persistent store"),
HANDOFF-2026-11-17-night.md (Bluetooth parts), `xnu/files/iokit/Kernel/MiniAppleKeyStore.cpp`, `userland/src/aks_restore.c`,
`userland/rc.launchd.aks`, `userland/src/var_store.c`, `userland/src/init.c`, `xnu/files/bsd/dev/redwood_emmc.c`,
`docs/x6826b-aks-selectors.txt`, a disassembly of the iOS 16.5 (20F66) `/usr/libexec/keybagd` (`otool -tV`, this session),
strings of the 20F66 `bluetoothd`, and Open Mapple's `xnu_kernel_mediatek_common/iokit/Kernel/arm/MappleKeyStore.cpp`
(683 lines, commit c033748).

Tags: **[V]** = verified in code, a disassembly or a log in this repo. **[I]** = inference, not checked. **[U]** = unknown.

## 0. Short answer

Yes, and most of the design is **already in our kernel**. Our default path (no `aks_keys`) already answers the system
bag request with a handle and wraps keys with a fixed, reversible transform, which is what MappleKeyStore does. What
Bluetooth images do differently is run keybagd (`--init` and the job), and that is the reason they use `aks_keys=1`.
`aks_keys=1` also switches the keychain to a second wrap format that older images cannot read.

Recommendation, in order:
1. **Smaller thing first, no code:** one unattended run of the Bluetooth build with the plain `rc.launchd`, keybagd and
   securityd still as jobs, and **no `aks_keys`**. This "no keybag" variant was proposed on 2026-11-17 and never run.
   If bluetoothd stays up, the problem is gone.
2. If (1) fails: add **`aks_keys=2`** (about 50 lines in `MiniAppleKeyStore.cpp`). keybagd and its bag file are used
   as today, but every key is written in the old reversible format. So everything written is readable by `aks_keys=0`,
   `=1` and `=2` images. Nothing in this mode is one-way.
3. Before the first saved-store Bluetooth session in either case, still take the `xnudata` backup (the dd command in
   HANDOFF-2026-11-16 §"Bluetooth vs the user's persistent store", needs approval). That is cheap insurance, not a
   requirement of the design.

## 1. How our AKS path works today

### 1.1 Files and functions

| Piece | Where | What it does |
|---|---|---|
| Kernel key store | `xnu/files/iokit/Kernel/MiniAppleKeyStore.cpp` (993 lines). Built by `xnu/redwood.patch:1905` (`iokit/Kernel/MiniAppleKeyStore.cpp optional iokitcpp`), started by `MiniAppleKeyStoreStart()` from `GenericARMPE.cpp:377` | `AppleKeyStore` service on IOResources with user client `AppleKeyStoreUserClient` |
| Bag maker | `userland/src/aks_restore.c`, signed with `aks_restore.entitlements` (`com.apple.keystore.device`) | Done once, the way a restore would: selector 0x02 creates a bag, 0x03 saves it, 0x04 unloads it, and the bag is written to `/private/var/keybags/systembag.kb` as a binary plist `{KeyBagKeys: <blob>}`. Does nothing if the file exists (`aks_restore.c:43-46`) **[V]** |
| Boot script | `userland/rc.launchd.aks` | `aks_restore`, then `keybagd --init` (launchd skips this boot task on a RAM-disk boot, run 423), then `exec /sbin/launchd.ios` **[V]** |
| Build | `userland/build.sh:376` copies `aks_restore` and `tolog` when `BT=1` or `RC=*aks*`; `MORE_JOBS += com.apple.bluetoothd com.apple.BlueTool com.apple.mobile.keybagd`; `RC=rc.launchd.aks` (HANDOFF-2026-11-16:126,172) **[V]** | |

### 1.2 Default mode (no `aks_keys`): what every stay image and the saved store use

- `kAKSGetSystem` (0x0e) returns **success, handle 0** (`MiniAppleKeyStore.cpp:818-824`) **[V]**.
- Wrap (0x0a): the output is `"HKwr"`, the class byte, three zero bytes, then the key XOR `0x5a+37*i` (`:447-455`,
  `:846-863`). Unwrap (0x0b) requires the `HKwr` mark and returns BadArgument otherwise (`:865-876`) **[V]**.
  This format is deterministic, so items survive a reboot. `keychain_test keep` was written for exactly that var_store
  check, and its header says "it should be -- and now is" (`keychain_test.c:9-13`) **[V]**.
- Lock state `0x6` (no passcode, unlocked since boot), device state DER with `sls=3` (`:72-75`, `:118-136`) **[V]**.
- Create, save, load, unload and set-system bag (0x02-0x06) are not handled, so they return Unsupported **[V]**.
  keybagd is not started: `mkjobs.py:35` DENY contains `keybagd`, and `rc.launchd` has no `--init` **[V]**.

### 1.3 `aks_keys=1`

- `MiniAppleKeyStoreStart` writes `no-effaceable-storage=1` into `/defaults` so keybagd reads the bag file as a plain
  plist (`:964-981`) **[V]**.
- `keysMethod` (`:512-776`) handles 0x02-0x06, 0x0e, 0x17, 0x0a and 0x0b. Bags hold a random device key and 16 random
  class keys (`:224-302`). The saved blob is `RKWB` v1: header, the device key in clear, and the class keys wrapped by it
  (`:247-253`, `:304-315`) **[V]**.
- `kAKSGetSystem` returns **NotFound** until a bag is set (`:666-684`). keybagd `--init` then loads `systembag.kb`,
  sets it (copied into a private slot, `:635-664`) and unloads its own handle **[V]**. The handoff log
  `bt-hands1.log:2495` shows `aks: the system bag set from handle 1 (now handle 2)`.
- **Wrap becomes RFC 3394 under the class key** (`:700-728`). Unwrap tries RFC 3394 first, then falls back to `HKwr`
  (`:749-753`). So an `aks_keys=1` kernel reads both formats and writes only the new one **[V]**.
- Entitlement gate: bag selectors need `com.apple.keystore.device`, wrap and unwrap need that or
  `com.apple.keystore.access-keychain-keys` (`:529-547`) **[V]**.

### 1.4 What `aks_keys=1` writes to a data store

1. `/private/var/keybags/systembag.kb`: about 600 B, the RKWB bag with **the device key in clear** **[V]**.
   `var_store` keeps it, because `keybags` is not in `SKIP` (`var_store.c:76-84`) **[V]**.
2. keybagd's own state: the sqlite db (`com.apple.mobile.keybagd.db`), `/var/keybags/backup/*` (unlinked at init),
   `/var/logs/keybagd_init.log` (logs are in SKIP) **[V]** strings and disassembly; exact files **[I]**.
3. **Every keychain key wrapped while the image runs is in RFC 3394 form** **[V]**. That covers new items and every item
   securityd rewrites. It probably also covers securityd's per-class **metadata keys** in `keychain-2.db`, which are
   wrapped through the same AKS call **[I]** (standard securityd design).

### 1.5 What is one-way about it

- **[V]** An `aks_keys=0` image cannot unwrap any RFC 3394 key. `method()` answers BadArgument (`:870-871`).
- **[V]** An `aks_keys=1` image needs `systembag.kb`. If the file is lost or replaced, the class keys are gone, and the
  RFC 3394 items cannot be read by any image.
- **[I], the serious one:** securityd treats an item it cannot decrypt as corrupt and **deletes** it. Open Mapple
  records the same behaviour: a wrong class answer "made securityd file every item as class 0 and delete it as corrupt on
  the first read" (`MappleKeyStore.cpp:555-557`). The likely result of booting an older, non-aks stay image against an
  `aks_keys=1` store is lost Wi-Fi passwords and Bluetooth link keys, not just unreadable ones. If a metadata key was
  rewrapped, every item of that class goes with it **[I]**.
- **[V]** `aks_restore` skips any existing `systembag.kb` and keybagd's load fails on a non-RKWB file (`bagLoad`
  `:324-327`). A foreign or dummy bag file on an `aks_keys=1` boot therefore gives keybagd a fatal `kb_deserialize` and
  a reboot (§2.3).

Hence the handoff rule: once `aks_keys=1` has run on the store, every later image must have `aks_keys=1` and
`RC=rc.launchd.aks`.

## 2. How MappleKeyStore works (Open Mapple, `xnu_kernel_mediatek_common/iokit/Kernel/arm/MappleKeyStore.cpp`)

### 2.1 Facts **[V]** (file:line)

- Compiled only for `MT6781` (`:24`, `:683`). It is matched as an IOKit personality on IOResources
  (`iokit/KernelConfigTables.cpp:51-53`). Ours is attached by hand (`MiniAppleKeyStoreStart`).
- Selector enum "iOS 16.7 numbering" (`:42-70`).
- **`kAKSGetSystem` always returns success with handle 0** (`:627-634`, `MAPPLE_AKS_SYSTEM_HANDLE 0` `:73`). That is
  the "always has the system bag" part. The comment at `:9-11` says it is the userspace-reboot path.
- `kAKSSaveBag` returns the 8 bytes `"MAPPLEKB"` (`:622-626`). Open, unload, set-system, unlock and lock bag,
  assertions, unload session bags and notify-set all return success and do nothing (`:588-599`). Create bag (2) and
  load bag (6) are not handled, so they return **Unsupported** (`:663-665`).
- Wrap is the tag `"MAPPLEK1"` followed by the key XOR a fixed 32-byte pad (`:473-480`, `:532-540`). Unwrap
  requires the tag and returns NotPermitted otherwise (`:541-544`). The wrap answer gives back scalar[1], the class
  (`:554-562`). There is one format, it is deterministic, and no key material is stored anywhere.
- Lock bits: locked 0x1, no passcode 0x2, unlocked since boot 0x4 (`:80-82`, `:177-182`). Device state DER with
  `ss`, `sls`, `sb`, `sfa` and the other fields (`:434-455`), answered for selectors 17 and 35.
- Optional passcode: a PBKDF2-HMAC-SHA256 verifier with 20000 iterations, in `/private/var/keybags/mapple.kb`. The file
  has two slots written alternately, `F_FULLFSYNC` after each write (`:89`, `:113-175`). It has unlock (9), verify (42),
  change secret (15) and iOS's back-off delays (`:198-319`). The **only persistent kernel state is this file**, and it
  exists only once a passcode has been set.
- `messageClients(0xe3ffd00f)` on first unlock and `0xe3ffd00d` on status changes (`:95-96`, `:667-675`).
- No entitlement checks at all: any process may unwrap any item (`:570-682`).
- Every call is logged with IOLog (`:677-680`).
- Userland still installs a **plain-plist `systembag.kb` with `KeyBagKeys = "SWEET"`** at image build
  (`mapple_build/tools/mkiosroot-macos.sh:126-131`), and the device tree has `/defaults no-effaceable-storage=1`
  (`mapple_build/tools/mkadt.py:558-559`). keybagd `--init` runs as the stock launchd boot task on their writable root
  (`open-mapple-userland.md` §4).

### 2.2 What each client sees under Mapple

- **keybagd `--init`:** `GetSystem` succeeds, so keybagd does not load the bag (§2.3), maps the data volume, reads the
  "SWEET" file (it must exist, §2.3), and tries to register it as the backup bag (load bag returns Unsupported, which
  is logged and not fatal). The keybagd daemon then runs normally **[V]** for keybagd's logic, **[I]** for their logs.
- **securityd:** lock state "no passcode, unlocked since boot" and wrap/unwrap with `MAPPLEK1`. Its keychain persists on
  their HFS+ root **[I]** from their notes.
- **bluetoothd:** `MKBDeviceUnlockedSinceBoot` is true, the `com.apple.mobile.keybagd.first_unlock` notification comes
  from keybagd, and the `SecItem*` calls for link keys work. bluetoothd imports `_MKBDeviceUnlockedSinceBoot` and
  `_SecItemAdd/CopyMatching/Update/Delete`, and its strings include `BTKCSetData`, `_BTKCGetDataCopy ... accessgroup` and
  `com.apple.mobile.keybagd.first_unlock` **[V]** strings of 20F66 bluetoothd.

### 2.3 keybagd `--init` on iOS 16.5, from the disassembly **[V]**

`load_devicebag` is at `0x100012c30` in `ios165/rootfs/usr/libexec/keybagd`:

1. `0x100012c54`: `aks_get_system` (`0x10001b870`, selector **0x0e** at `0x10001b8d8`).
2. Only if the answer is `0xe00002f0` (NotFound, compared at `0x100012c58-c64`): read the file, `aks_load_bag`
   (`0x10001b1f4`, selector **0x06** at `0x10001b25c`), `kb_set` (set system, selector **0x05**), then `aks_unload_bag`
   (`0x10001b328`, selector **0x04** at `0x10001b364`).
3. `0x100012cec`: `aks_get_system` again. Any error is fatal: "failed to load system bag".
4. Unless the boot-args contain `-apfs_shared_datavolume` (`0x10000cee0`): `KBMapDeviceBag("/private/var/")`
   (`0x100004f78`: statfs, getattrlist, "has_data_protection"). An error is fatal: "data volume mapping".
5. `0x100012d3c`: **`KBLoadSystemKeyBag`** (`0x100001dfc`, reads `<var>/keybags/systembag`). **A NULL result is fatal
   with "kb_load", even when step 1 succeeded.** That explains why Open Mapple still ships a "SWEET" file.
6. `0x100012d68`: `register_backup_bag` (`0x100012610`): load bag (0x06), set backup bag (0x15, at `0x10001cd1c`),
   unload (0x04). **Its result is ignored.**
7. The backup caches are unlinked. "DONE MKB_INIT".

The fatal path (`0x100012228`: "fatality", NVRAM `auto-boot`, "Rebooting...") ends the run on our port **[I]**. This
matches the kernel comment about run 468.

Consequences:

- **[V]** With today's default kernel, which answers `GetSystem` with success, keybagd `--init` still needs *some*
  `systembag.kb`. Without the file it fails at step 5 and the run reboots.
- **[V]** In step 6 the kernel sees the file's bytes through selector 0x06 even when it never has to load them as the
  system bag. That gives the new mode a compatibility hook without changing keybagd.

## 3. Port design: `aks_keys=2` ("system bag always there, writes stay readable")

We do not copy MappleKeyStore. Its license is unclear: `NOTICE` covers Apple's files under APSL and the ios-sweet and
ios-viva files under MIT, but this file cites "ios-sapphire" and has no notice. It is also MT6781-gated, and our file
already has its essentials. Only the idea is ported. No GPL code is involved.

### 3.1 Behaviour

| Selector (16.5) | `aks_keys=0` (today) | `aks_keys=1` (today) | **`aks_keys=2` (new)** |
|---|---|---|---|
| 0x0e GetSystem | success, 0 | NotFound until a bag is set | **success, 0, always**. keybagd skips loading and setting the bag |
| 0x02 / 0x03 / 0x04 CreateBag / SaveBag / UnloadBag | Unsupported | real bags | as `=1`, so `aks_restore` keeps working |
| 0x06 LoadBag | Unsupported | real load | as `=1`, **plus** the first RKWB bag loaded becomes the system copy (the same copy `SetSystem` makes, `:644-663`) if none is set. That covers keybagd's `register_backup_bag` (§2.3 step 6) |
| 0x05 SetSystem | Unsupported | copy | as `=1` (keybagd never calls it in this mode) |
| 0x0a Wrap | **HKwr** | RFC 3394 | **HKwr**: fall through to `method()` (`:846-863`) |
| 0x0b Unwrap | HKwr only | RFC 3394, then HKwr | as `=1` (`:730-762`): reads both. RFC 3394 needs the adopted bag |
| 0x15 SetBackupBag | Unsupported | Unsupported | Unsupported; keybagd ignores the result **[V]** (§2.3 step 6) |
| state, config, UUID, assertions, notify | as today | as today | as today |

Boot-arg: `aks_keys=2`. It is **off by default**; the default stays `aks_keys=0`. `no-effaceable-storage` is set because
`gAKSKeys != 0` (`:964`); no change is needed there.

### 3.2 Code changes (all in `xnu/files/iokit/Kernel/MiniAppleKeyStore.cpp`, no new file)

1. `keysMethod`, at the top: when `gAKSKeys == 2` and the selector is `kAKSWrapKey`, return `kIOReturnInvalid` so
   `method()` writes HKwr (keep the entitlement check before it).
2. `kAKSGetSystem`: when `gAKSKeys == 2`, answer `out[0] = 0`, success.
3. `kAKSLoadBag`: after a successful `bagLoad`, when `gAKSKeys == 2 && gAKSSystem < 0`, copy the bag into a new slot
   and set `gAKSSystem`. Factor the copy out of `kAKSSetSystem` (`:647-659`) into one helper used by both.
   `redwood_host_keep_add("aks: mode 2: the bag of the file adopted (handle %d)")`.
4. A counter line, kept once at the first unwrap after 60 s or so (or from a sysctl): `aks: wraps N (HKwr), unwraps HKwr
   N, kw N, refused N`. This is the pass line that proves no RFC 3394 key was written.
5. Self-test (the one runnable check), boot-arg `aks_selftest=1`, only with `aks_keys=2`. At start: create a bag, wrap a
   fixed 32-byte key the `aks_keys=1` way (`kwWrap` with class key 7), and unwrap it through the mode-2 unwrap path
   with that bag as system. Then wrap through HKwr and unwrap. Unload the bag and log
   `aks: self-test: kw ok, HKwr ok` or which part failed. This is the only way to exercise the compatibility branch
   unattended, because a RAM `/var` cannot carry an `aks_keys=1` keychain into a mode-2 boot. About 25 lines.

Total about 50-60 lines plus comments. Userland needs **no change**: the BT build with `RC=rc.launchd.aks` already
runs `aks_restore` and `keybagd --init`. Only the boot-arg changes from `aks_keys=1` to `aks_keys=2`. Optionally,
document `aks_keys=2` in `build.sh:11-12` and `bluetooth-mtk-porting.md:81`.

Dropped on purpose (YAGNI): Mapple's passcode (`mapple.kb`, selectors 0x09/0x0f/0x2a/0x69: no passcode is wanted),
`messageClients` (no lock transitions happen without a passcode), lock bag 0x0d and unload session bags 0x37 (never
logged as missing in our runs **[I]**; `aks_log=1` will show it if they are).

### 3.3 Selector numbers checked against 16.5

Two sources, both for **iOS 16.5 (20F66)**:

- **S1:** `docs/x6826b-aks-selectors.txt`, the output of `tools/dsc/aks-selectors.py` over AppleKeyStore.framework in
  the 20F66 dyld cache. `tools/dsc/dsc.py:4` points at `ios165/20F66__iPhone10,3_6`. The txt carries no build stamp; its
  provenance comes from the tool's default path **[V]**.
- **S2:** keybagd's statically linked libaks, read with `otool` this session: `mov w1,#sel` before `IOConnectCallMethod`
  **[V]**.
- **S3:** the selector worked in our own runs (empirical) **[V]**.

| Name | Mapple (16.7) | Ours | 16.5 | Verified by |
|---|---|---|---|---|
| open | 0 | 0x00 | 0x00 | S1 (`sub_1f22c9938`) |
| create bag | 2 | 0x02 | 0x02 | S1 (`aks_create_bag` → `sub_1f22d38f8`), S3 (`aks_restore`) |
| save bag | 3 | 0x03 | 0x03 | S1, S3 |
| unload bag | 4 | 0x04 | 0x04 | S1, S2 `0x10001b364`, S3 |
| set system | 5 | 0x05 | 0x05 | S1, S2 `0x10001b98c`, S3 (`bt-hands1.log:2495`) |
| load bag | 6 | 0x06 | 0x06 | S1, S2 `0x10001b25c`, S3 |
| lock state | 7 | 0x07 | 0x07 | S1 |
| lock device | 8 | 0x08 | **unverified** | S1 shows `sel=None` for `_aks_lock_device` (computed). Not needed |
| unlock device | 9 | 0x09 | 0x09 | S1 |
| wrap / unwrap | 10 / 11 | 0x0a / 0x0b | 0x0a / 0x0b | S1, S3 (keychain works) |
| unlock / lock bag | 12 / 13 | 0x0c / – | 0x0c / 0x0d | S1 |
| get system | 14 | 0x0e | 0x0e | S1, S2 `0x10001b8d8` |
| change secret | 15 | – | 0x0f | S1 (not needed) |
| device state / extended | 17 / 35 | 0x11 / 0x23 | **not resolved statically** (S1: register `x21` in `sub_1f22d7370`) | S3 only: our 0x11/0x23 answers are read by securityd and SpringBoard in every run |
| bag UUID | – | 0x17 | 0x17 | S1 |
| set backup bag | – | – | 0x15 | S1, S2 `0x10001cd1c` (left Unsupported) |
| assert hold / drop | 26 / 27 | 0x1a / 0x1b | 0x1a / 0x1b | S1 |
| configuration | – | 0x1f | 0x1f | S1 |
| lock now | 32 | 0x20 | **unverified** | not in S1 output. Not needed |
| generation | – | 0x28 | 0x28 | S1 |
| verify password | 42 | – | 0x2a | S1 (not needed) |
| unload session bags | 55 | – | 0x37 | S1 (not needed) |
| assert promote / consume | – / 101 | 0x64 / 0x65 | 0x64 / 0x65 | S1 |
| notify set / is set | 102 / 103 | 0x66 / 0x67 | 0x66 / 0x67 | S1 |
| change secret epilogue | 105 | – | 0x69 | S1 (not needed) |
| prewarm | – | 0x73 | 0x73 | S1 |
| IOGeneralInterest messages | 0xe3ffd00d / 0xe3ffd00f | – | **unverified** for 16.5 (would need MobileKeyBag disassembly) | not used by this design |

Every number this design relies on (0x02-0x07, 0x0a, 0x0b, 0x0e, 0x15) is verified by at least two sources. Mapple's
16.7 numbering agrees with 16.5 everywhere it could be checked.

### 3.4 Build integration

Nothing new: the file is already in the build (`redwood.patch:1905`, `GenericARMPE.cpp:377`). Kernel rebuild plus
payload, as for any kernel change. The BT userland build command stays as in HANDOFF-2026-11-16:172 with
`RC=rc.launchd.aks` and `LDID=...` (`aks_restore` must be signed, otherwise create bag is refused by the entitlement gate).

## 4. Compatibility

### (a) The saved store as it is today (never had a keybag)

- **[V]** No `systembag.kb`: no image with `aks_restore` has ever run against it (task statement; HANDOFF-2026-11-16:418
  "no decision yet").
- **[I]** It very likely holds a `keychain-2.db` with **HKwr-wrapped** items. Stay images use WIFID=1, which adds
  securityd (`build.sh:130-131`), and `keychain_test keep` was used to check persistence. Not verified on the device.
- **Under step 1 (no aks_keys, no `--init`):** nothing new is written except what bluetoothd, keybagd (as a job) and
  securityd write. Keychain writes are HKwr, which every image reads. keybagd as a daemon may create its db under
  `/var/keybags` **[I]**; non-BT images never start keybagd, so that is inert.
- **Under `aks_keys=2`:** the first boot's `aks_restore` writes a real RKWB `systembag.kb`. All keychain writes are HKwr.
  Afterwards:
  - an `aks_keys=0` image reads every item, and ignores the bag file because it has no `--init` **[V]** from code;
  - an `aks_keys=2` image reads every item;
  - an `aks_keys=1` image loads the bag and reads HKwr through the fallback **[V]**, but **its own new writes would be
    RFC 3394 again**. So, rule: once mode 2 exists, **never boot `aks_keys=1` against the saved store**. Better, make it
    impossible: after mode 2 passes, change BT builds to `aks_keys=2` and retire `=1` for anything but throw-away runs.
- Bluetooth's own files (`/var/mobile/Library/Preferences/com.apple.MobileBluetooth.*.plist`, LE device databases under
  `/var/containers/Shared/SystemGroup/...`) **[I]** paths. Images without bluetoothd never read them.
- **Answer to "is anything written that the old path cannot read afterwards?"** Under `aks_keys=2`: **no** keychain
  key in a format `aks_keys=0` cannot read **[V]** by construction, checked with the counter line (§3.2 item 4). The
  files that appear (bag, keybagd db, BT plists) are ignored by the old path. Under `aks_keys=1`: yes (§1.5).

### (b) Throw-away stores created under `aks_keys=1`

- **[V]** None persists. Every `aks_keys=1` run so far had no `var_store` and no `emmc_write`, so each was a RAM `/var`
  that is gone (the HANDOFF stay commands explicitly drop them).
- If one ever exists: a mode-2 boot adopts its RKWB bag through keybagd's `register_backup_bag` (§2.3 step 6) and reads
  its RFC 3394 items. Proven unattended by `aks_selftest=1` only; there is no real store to test against. New writes are
  HKwr, so the store slowly becomes all-HKwr as items are rewritten **[I]**: securityd rewrites on update, not on read.

### Making the first saved-store try fully reversible

1. **Backup (strongly recommended, needs the user's approval).** Run the HANDOFF-2026-11-16:278 `dd` of `xnudata` to
   `/data/local/tmp/xnudata-preBT.img` with sha256, after `df /data` (about 1.9 GB). Restoring it is the full undo.
2. **var_store slots are not a backup.** Saves alternate between two slots (`var_store.c:33-36`), so the second save of
   the session overwrites the last pre-BT archive. A session that saves more than once (`var_every`, or the save at run
   end plus another) leaves no pre-BT copy on the device.
3. **Keychain writes off the saved store (optional):** there is no read-only load today. `/dev/remmc_var` exists only
   with `emmc_write=1` (`redwood_emmc.c:755-756`, `:1080-1083`), and `var_store load` needs it. A `var_ro=1` mode (create the
   node, refuse writes, do not start `var_store watch`) would let a session load the saved data with BT on and write
   nothing back. Cost: S, about 20 lines in `redwood_emmc.c` plus one line in `init.c`. Its value is small: it cannot
   test reconnect after a reboot, which is the one thing only the saved store can show. Recommended only if the user
   wants a look before committing.
4. **Rollback without dd:** under step 1 or `aks_keys=2`, booting the previous non-BT stay image against the store is
   expected to just work (HKwr everywhere). Delete `/var/keybags/systembag.kb` if wanted; old images ignore it. Under
   `aks_keys=1` there is no rollback without the dd image.

## 5. Unattended test plan (throw-away RAM `/var`: no `var_store`, no `emmc_write`)

The BASE args are those of the btP3 run (HANDOFF-2026-11-17-night:167), 240 s each, `tools/r1-auto.sh`.

**T0: no-keybag variant, no code change (run first).**
Build: the BT command, but `RC=rc.launchd` (no `aks_restore`, no `--init`) and `RC_JOBS=rc.jobs.aks`, which runs
`keychain_test` round trip, keep and read, plus `psgrep keybagd`. The keybagd, securityd, bluetoothd and BlueTool jobs
stay. Args: `<BASE> connsys_bt=2 aks_log=1 idle=1`, no `aks_keys`.
Pass:
- `bt: 6 of 6 answered`
- `bluetoothd[N] is watched` exactly once, and pid N still in the last `programs` line
- `bt_test: ... powered 1` / `central manager state 5`
- no `Bluetooth error - restarting`
- `psgrep keybagd` shows it running at 60 s and 180 s
- `keychain_test: read back: 0`
- `aks: first 0xa by securityd ... -> 0x0` (the wrap was HKwr, because `aks_keys` is off)
- reason 0 end, no panic, back in Android

Fail signature: bluetoothd restarts or dies early (the X6826B run-608 "identity root" crash). Then go to T1.

**T1: `aks_keys=2`** (after the §3.2 change). BT build as today (`RC=rc.launchd.aks`, signed `aks_restore`). Args:
`<BASE> connsys_bt=2 aks_keys=2 aks_log=1 aks_selftest=1 idle=1`.
Pass:
- `aks: self-test: kw ok, HKwr ok`
- `aks_restore: the system bag made`
- keybagd `****** DONE MKB_INIT ****` in the tolog output, and no `FATAL KEYBAG ERROR`
- `aks: first 0xe by keybagd [] in 0 -> 0x0`
- `aks: mode 2: the bag of the file adopted`
- **no** `aks: the system bag set from handle` (keybagd must not have taken the load path)
- `keychain_test: add: 0`, `read back: 0`
- counter line `kw 0` written: no RFC 3394 wraps
- the BT pass lines of T0, plus btP3's (101 `04 3e` reports, peer pid on `BTAudioHALPlugin.xpc`, `redwood-audio` sound)

**T2: regression of `aks_keys=1`** (same kernel): btP3's args unchanged. Pass: btP3's lines plus
`aks: the system bag set from handle 1`. This proves mode 2 did not change `=1`.

**T3: default image** (same kernel, no `aks_*`): the sz1 run. Pass: as sz1, plus `keychain_test` read back 0 if
`RC_JOBS=rc.jobs.aks`.

Hands-on only:
- Pairing the speaker, then Bluetooth off and on in Settings (bluetoothd rereads link keys through `BTKCGetData`), and
  reconnecting after the speaker is power-cycled. This can be done on a throw-away store.
- **Reconnect after a reboot without re-pairing**: needs the saved store (`var_store=1 emmc_write=1`), after the backup.
  This is the real proof that keychain items written by bluetoothd persist and unwrap.
- Booting the old non-BT stay image afterwards and checking that Wi-Fi still knows its network. That is the
  reversibility proof on real data.

## 6. Risks, unknowns, effort, recommendation

Risks:
1. **[U]** Whether bluetoothd needs keybagd `--init`, or only the keybagd and securityd jobs. Never isolated: X6826B
   run 608 added `aks_keys=1`, keybagd and securityd together. T0 settles it in one run.
2. **[I]** keybagd `--init` fatal paths (no bag file, data-volume mapping) reboot the run. `aks_keys=2` keeps
   `aks_restore` in front, so the file always exists. Do not ship a dummy "SWEET" file the way Mapple does: it would make
   a later `aks_keys=1` boot fail at `kb_deserialize` (§1.5).
3. **[I]** securityd may delete items it cannot unwrap. That is why mode 2 must never write RFC 3394. The counter line
   enforces it per run; the self-test covers the compatibility read.
4. **[V]** The bag file keeps the device key in clear (true today under `=1`). Mode 2 changes nothing about security:
   the keychain is protected from other programs by the entitlement gate, not from someone holding the flash. The same
   holds for HKwr and for Mapple.
5. **[I]** keybagd running for the first time on the saved store may create its db or migrate state. Old images do not
   start keybagd, so this is inert, but it is not proven.
6. **[U]** The saved store's actual keychain contents (HKwr expected). Look at them once in the hands-on session before
   any BT write: `ls -l /var/Keychains`, `keychain_test read`.
7. A2DP and the "all audio silent after pairing" problem (`bluetooth-audio.md`) are independent of this work.

Effort:
- T0: one build variant and one 240 s run (about 30 min).
- `aks_keys=2`: about 60 lines in one file plus a kernel and payload rebuild, then T1-T3 (about half a day).
- Optional `var_ro=1`: S.
- Hands-on: one throw-away session (pair, toggle), then one saved-store session after the backup (pair, reboot,
  reconnect, boot the old image).

**Recommendation: do the smaller thing first (T0). Build `aks_keys=2` only if T0 fails. Do not port MappleKeyStore
as such**, because its only new property versus our default path, "GetSystem always succeeds", is already ours; its
passcode code is not wanted; and its license is unclear. In both outcomes, go to the saved store only after the dd
backup, and retire `aks_keys=1` for the saved store.
