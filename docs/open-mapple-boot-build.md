# Open Mapple: boot chain, install, build and release, compared with the r1 port

Written 2026-11-18. This is research only: nothing in our tree was changed, and nothing was run on the device.

Source: the shallow checkouts of `open-mapple/*` in the session scratchpad (`mapple_loader`, `mapple_splitboot`,
`mapple_build`, `manifest`, `mapple_releases`, `mapple_vendor`, `xnu_device_realme_spaced`, `xnu_device_xiaomi_sweet`,
`xnu_device_google_sunfish`, plus `xnu_kernel_mediatek_common` where a boot or log question led into the kernel), and the
website's porting and build pages. Their reference phone is the realme 8i "spaced" (MT6781, **UFS**, a 64 MB boot
partition, iOS 16.7.16 on xnu-8796.141.3). Ours is the r1 (MT6765, **eMMC**, a 32 MB boot partition, iOS 16.5 on
xnu-8796.121.2).

Not checked out, so not reviewed here: `mapple_device_setup_scripts` (the porting page calls its tool `bringup.py`),
`mapple_manager` and `mapple_assembler`, which are private.

Paths are given relative to each repository. "Ours" means `/Users/calebstultz/Developer/hakios-r1`.

---

## 1. How XNU is started

### 1.1 Two loaders in one repository

`mapple_loader` has two loaders:

- **`mtk/loader/mtkloader.c` (3946 lines).** This is the same program as our `mtkloader/mtkloader.c` (3787 lines),
  forked from the same HakiOS code. `mtkconnsys.h` and `tcm.h` are byte-identical to ours. `mtkspi.h`, `mtkpwrap.h` and
  `start.S` differ by only a few lines. It still has the direct path we use (`run_xnu`: MMU off, XNU loaded into a fixed
  window, a watcher core, return to Android with the log in ramoops). Their **products no longer use that path**,
  though.
- **`shim/` (viva's shim, from ios-viva, MIT, dipa-studio).** This is what the products use. The device builds with
  `loader -DXNU_SHIM=1 -DXNU_TOUCH_ILI=1` (`xnu_device_realme_spaced/device.conf`). mtkloader then runs `run_shim()`
  (`mtkloader.c:2738`):
  1. It copies the `MAPPLEPL` payload (kernel segments, ADT and a 608-byte command line, made by
     `mapple_build/tools/mkpayload.py`) to where LK put the initrd (`/chosen linux,initrd-start`).
  2. It appends its own tables after the payload: the touch firmware blocks and LK's OPP/DVFS table.
  3. It moves `linux,initrd-end` in LK's device tree, in place, so those tables become a hole.
  4. It adds boot-args for the touch firmware, LVTS calibration and DVFS (`ili_xnu_args`, `lvts_args`, `dvfs_args`),
     and safe-boot args when asked.
  5. It points OVL0 layer 0 back at LK's first framebuffer, with per-pixel alpha off.
  6. It enters the shim the way LK enters a Linux Image (`chain_image`).

  **This path never returns.** There is no watcher and no reset-into-Android. Getting back to Android means rebooting.

### 1.2 What the shim does (`shim/loader.c`, `holes.c`, `fdt.c`, `identity.c`, `bootrec.c`)

**Payload and command line**
- It finds `MAPPLEPL` at `linux,initrd-start`.
- It reads the **boot-args record** (`MAPPLEBR`, `shim/bootrec.h`) from the root partition's last MiB, using
  read-only polled UFS `READ(10)`s through a transfer list of its own. If the record is valid and its `base_hash`
  matches the payload's own line, the record's line replaces the payload's.

**Memory map (`mp_mem4g`, the default)**
- XNU's RAM starts at DRAM base `0x40000000`, not at a carved-out window.
- The **holes** are:
  - every `/reserved-memory` `reg` in LK's FDT (TEE, ATF, SSPM, SCP, modem, framebuffer)
  - the payload and its appended tables
  - LK's FDT itself
  - on spaced, Android's runtime pools for Wi-Fi and connsys, hard-coded under `#ifdef SPACED`, because LK's tree
    does not list them.
- The hole list goes into the boot_args page at `+0x2000` (`"MPHOLES1"`).
- The kernel reads the list and skips those pages (`xnu_kernel_mediatek_common/osfmk/arm64/arm_vm_init.c:1496-1570`,
  `mapple_mem4g_init`, `mapple_hole_skip`).
- On spaced this gives 3573 MB of 4 GB to iOS (13 holes, per the porting page). If the kernel would land in a hole, the
  shim falls back to the old contiguous 2 GB window.

**`boot_args` (Revision 2 / Version 2)**
- `virtBase` and `physBase`; `memSize` covers DRAM base to end.
- `topOfKernelData` is the kernel, then the ADT, then one 16 KB boot_args page.
- `Video` is LK's framebuffer, 32 bpp.
- `deviceTreeP` is the ADT's VA. The ADT is built on the Mac by `mapple_build/tools/mkadt.py` from the IPSW's
  `DeviceTree.d22ap.im4p` plus the device's options (`adt --spaced --ufs --audio --torch`).
- `CommandLine[608]`.
- At `+0x1000`, the original payload line (`"MAPPLEPC"`) when a record replaced it, so the kernel can show both.

**ADT fix-ups at boot**
- The serial number is the phone's own, taken from `androidboot.serialno` in LK's bootargs (`identity.c`).
- The random seed comes from LK's `kaslr-seed` plus two counter reads.

**Kernel image**
- The kernel's chained fixups are resolved on the Mac by `mkpayload.py`, so the shim only copies flat segments.
- It jumps to the entry point with the MMU and caches off and `x0` set to the boot_args PA.

### 1.3 CPU bring-up

- **Shim path:** XNU brings up every core itself through PSCI CPU_ON from the ADT's cpu nodes. The release uses
  `cpus=8` (6×A55 and 2×A76). No core is reserved for the loader.
- **Direct path (theirs and ours):** the same `core_order[]`, the `XNU_CORES` device-tree trimming and the watcher on
  `core_order[XNU_CORES]`. Theirs adds two things:
  - `XNU_PARK`: on MT6781 a core that has been switched off cannot be switched on again. So at the end of a run,
    XNU's other cores spin in a pen page (`0x4cdff000`), and that page is added as a `no-map` `/reserved-memory` node
    to LK's FDT before Android starts (`mtk/loader/splitboot.h`, `fdt_reserve_pen`).
  - `RETURN_CPUS`: a PSCI AFFINITY_INFO report after the run.

**r1 relevance:** our "cpus=8 blocked by the loader watcher" exists only because the watcher sits on a core. A
no-return boot (the shim model) removes that limit. The 8th core is "dropped / nothing to gain" in `docs/STATUS.md`,
so this is a side benefit, not a reason to switch.

### 1.4 Boot-args length

- Both projects silently cut the line at 607 bytes:
  - their `mkpayload.py:186`: `cmdline.encode()[:BOOT_LINE_LENGTH - 1]`
  - our `mtkloader.c:2565`: `memcpy(... sizeof(XNU_BOOT_ARGS) < BOOT_LINE_LENGTH ? ... : BOOT_LINE_LENGTH - 1)`
- Neither raises an error.
- Their release line is 463 characters. They keep it short by putting long-lived settings into the ADT and the record,
  and by `merge()` in `core/mka.sh`, which lets device keys override common ones instead of repeating them.
- **Ours is more at risk:** `tools/r1-auto.sh` BASE + SHOTS + `r1-run.sh`'s `emmc_lba/emmc_blocks` + test extras can
  approach 608 characters.
  - Cheapest fix: `_Static_assert(sizeof(XNU_BOOT_ARGS) <= 608, ...)` in `mtkloader.c`, or a length check in
    `tools/xnu-cycle.sh`. Effort S.

### 1.5 SplitBoot: boot menu and the way back to Android

**Menu (`mtkloader -DSPLIT_MENU=1`, `mtk/loader/splitmenu.h`, `mtkloader.c:3686-3790`)**
- At every power-on it draws "Android / iOS" on LK's framebuffer.
- Volume keys move the selection and Power starts. Volume up and Power are read from the MT6358 `TOPSTATUS` over
  PWRAP; volume down from the keypad.
- After a timeout (15–30 s, `mp_menu_secs`) it starts the default (`mp_menu_default=android|ios`, from the payload line
  or the boot-args record).
- A key that was already held when the menu appeared does not count (debounced: 3 samples up, then 3 down).
- iOS is offered only if `root_check()` passes (`mtkroot.h`): HFS+ header at 1 KiB, a matching alternate header at the
  end of the volume, and 128 MiB of room after it. Otherwise the menu starts Android and shows the reason.
- A one-shot request from Android through pmsg (`boot_xnu_flag`, the same mechanism as our `tools/boot-xnu.sh`)
  pre-selects iOS.

**Back to Android:** reboot and pick Android, or let the timeout run. Nothing in iOS hands control back.

**Install (`mapple_splitboot/installer/split.c`, `mapple_build/installer/mapple-install.c`, run as a static aarch64
binary from TWRP)**
- `split-carve` carves `xnusys` (22 GiB) off the end of `userdata`.
- `fastboot flash xnusys root.img` writes the root.
- `split-chain` saves Android's boot image into xnusys's "saved area", then chains the loader in front of Android's
  kernel (`chainboot.py` logic, the same `CHAIN_OFFSET 0x40000` as ours).
- After an Android OTA, you run the zip again; it chains the new boot image again.
- `split-uninstall` restores boot and the old GPT.
- On failure, an undo journal puts back whatever was written.

**Where things are stored**

| Item | Open Mapple (spaced) | Ours (r1) |
|---|---|---|
| Loader | boot (chained before Android's kernel) | boot_a (same) |
| XNU kernel + ADT | **boot**, payload (13 MB kernel, `payload_max 0x3fe0000` = 64 MB partition) | boot_a, payload (21.3 MB incl. 4.5 MB `root.apfs` ramdisk) |
| iOS root | partition: `userdata` (single) or `xnusys` (split), one HFS+ volume, read-write | `root.apfs` ramdisk in boot + `system.apfs` (2.4 GB) in `xnusys_a` logical partition in super, read-only |
| Boot-args | payload line + `MAPPLEBR` record in the root partition's last MiB (kernel writes it) | compiled into mtkloader (`XNU_EXTRA_ARGS`) |
| Firmware (touch, connsys) | ustar in the root partition's tail (`end-63 MiB`, 15 MiB), copied from the phone by the installer | touch FW compiled into mtkloader (`touchfw.h`) |
| Android's boot backup | root partition "saved area" (`end-127 MiB`) | `backup/` on the Mac |
| Return to Android | reboot → menu | side-button watcher → snapshot → reset → Android, or one-shot run |

**Could this give the r1 a boot menu and end the 32 MB squeeze?**

- **The menu: yes, as an idea; the code cannot be reused (see §5, GPL).**
  - The r1 has no volume keys, but our loader already reads the side button (KP_MEM1 bit 1, the watcher).
  - Example: "press the side button within N s for iOS; otherwise Android", or a short press to toggle and a timeout
    to start. The scroll wheel (OCH1970 on I2C6) is not driven by the loader, so leave it out.
  - Effort S. Risk low, as long as the default stays Android.
- **The squeeze: their layout does not remove it by itself.** Their kernel payload still lives in boot; they simply
  have 64 MB. What does transfer is the principle behind it: *only the loader needs to be in boot.*
  - Our loader can already read eMMC: `mtkloader/msdc.h` (PIO, about 11 MB/s), `gpt_find()`, and the `SYSTEM_LBA`
    extent path for `xnusys_a` in super (`mtkloader.c:610-700`, `tools/r1-xnusys.sh`).
  - Put `build-payload.bin` (or only `root.apfs` and the kernel) in its own logical partition in super, for example
    `xnuboot_a`, created with `create-logical-partition` exactly like `xnusys_a`.
  - Have mtkloader read and check it (length + CRC/SHA in a small header; on failure, start Android).
  - Results:
    - boot_a shrinks to Android + about 48 KB of loader, so the 1.4 MB of headroom becomes about 21 MB.
    - **Each test run becomes `adb push` + `dd` into the extent from Android, not a fastboot reflash of boot_a.**
      That removes the reboot-to-bootloader step from `xnu-cycle.sh` and most of the risk of flashing boot.
    - Reading 21 MB at 11 MB/s costs about 2 s per boot.
  - Effort S–M (the read path exists; what is new is the header check and a writer script). Risk low–medium: a bad
    extent means Android starts, as long as the check comes first.
  - Their equivalent ingredient is `mtkroot.h`'s pattern: `root_rd` reads plus `root_check` before XNU is chosen.
    It is MIT and the pattern can be copied.

---

## 2. Where iOS's root and /var live

**Open Mapple**
- One **case-sensitive journaled HFS+ volume** (16 GiB in single boot) holds the whole OS: the root, both cryptexes,
  and /var.
- The kernel mounts it read-write (`rd=ufsroot0 mp_ufsrw=1 mp_rootrw=1`) through `bsd/dev/mtk_ufs.c`, which is UFS
  only.
- `mtk_ufs.c` bounds every write to the named partition and checks it again right before the command is issued.
  Writes are refused without `mp_ufsrw=1`.
- HFS is Apple's OSS HFS-627.100.6, compiled into the kernel (`bsd/hfs_staged/`, `options HFS_STAGED`, MT6781 config
  only).
- The image is built on the Mac by `mapple_build/tools/mkiosroot-macos.sh`: `hdiutil`, then GNU rsync out of the IPSW's
  APFS images. That decompresses LZFSE/LZBITMAP files, so the kernel needs no AppleFSCompression.
- `root-tweaks.py` adjusts the result.
- The installer verifies the flashed root by hashing every allocated block (`mapple-install verify-root`, ranges from
  `rootranges.py`).
- Persistence is therefore native: /var is simply part of the read-write root.
- Cost: Files needs `fpdvolume.dylib` injected into fileproviderd so the HFS+ volume shows as APFS. It is the one
  injection in their release.

**Partition tail layout** (`mapple-install.c` header, checked by `xnu_kernel_mediatek_common/tools/mapple_tail_test.py`):

| Offset from the partition's end | Contents |
|---|---|
| `end-128 MiB` | saved area (Android boot backup at +1 MiB) |
| `end-64 MiB` | settings 1 MiB |
| `end-63 MiB` | firmware 15 MiB (ustar) |
| `end-48 MiB` | log 47 MiB |
| `end-1 MiB` | boot-args record |
| `end-1 KiB` | HFS+ alternate header |

**History:** `mapple_splitboot/tools/mkdatavol.py` and `var-store-read.py` show that they earlier ran our current
scheme: an APFS/HFS OS image plus a separate HFS+ data volume or a `var_store` two-slot archive in a `darwin`
partition. These files are MIT (HakiOS) AND GPL-3.0 (their changes). They have moved past it.

**Ours**
- The ramdisk `root.apfs` (md0) plus the read-only `system.apfs` on eMMC.
- Our APFS is our own read-only implementation (`xnu/files/bsd/miscfs/apfs/`).
- /var is a RAM filesystem, saved on an orderly exit by `userland/src/var_store.c` into `xnudata` through the
  `/dev/remmc_var` window (`redwood_emmc.c`, `emmc_write=1 var_store=1`). Anything after the last save is lost on a
  panic or hang (see `docs/cellular-stage4.md`).

**Moving the r1 to their model would take:**
- (a) HFS staged into our kernel. APSL, from xnu-8796.141.3 into our 8796.121.2: close versions, but a multi-hundred
  file import.
- (b) A **writable eMMC block device**. `redwood_emmc.c` is read-only by construction; MSDC writes (CMD24/25) would
  need the same bound-and-recheck discipline as `mtk_ufs.c`.
- (c) A larger partition. `userdata` carve (`tools/userdata-carve.py` / `gpt-carve.py`), or super's free ~5 GB.
  Uncompressed HFS+ is noticeably larger than our 2.4 GB APFS.
- (d) An HFS+ root build (`mkiosroot-macos.sh` logic).
- (e) The Files/fileproviderd workaround.

Effort L. Risk high: first real file-system writes to the eMMC. Current rules forbid `emmc_write` in test runs, so the
user must approve this explicitly.

A smaller step with most of the benefit: keep the read-only root, and give /var its own small HFS+ volume read-write
on `xnudata`. That is exactly their old `mkdatavol.py` design; reimplement it rather than copy, because of the GPL.
It still needs (a) and (b), only bounded to one partition. Effort M–L.

---

## 3. Logging and debug channel

**Theirs, in order of usefulness for long hands-on sessions:**

1. **`mp_conlog`, the persistent console log** (`xnu_kernel_mediatek_common/bsd/dev/mtk_ufs.c:1121-1250`, hooked in
   `osfmk/console/serial_console.c:285`).
   - Every character that reaches `_cnputs` (kernel printf and daemons' /dev/console) also goes into a 4 MiB RAM ring.
   - A kernel thread copies the new ring blocks every `mp_ufslogsecs` seconds (default 10) to the 4 MiB + 4 KiB before
     the boot-args record.
   - It writes a `"MAPPLECL"` header (version, ring size, total bytes, sequence) and syncs the cache.
   - Before the first write it checks that the area is clear of the HFS volume.
   - You read it back from a recovery or Android with `dd`.
   - The result: the last 4 MiB of console, at most 10 s stale, survives a hang, a panic or a hard power-off, with no
     return path needed. The release ships with `mp_conlog=0`; it is a developer switch.
2. **USB link `mp_usblink=3`** (`bsd/dev/usb_link.c`, 2029 lines; host `mapple_build/tools/mapple-link.py` with a
   textual UI).
   - The phone's **MUSB** (`mediatek,mt6781-usb20` at `0x11200000`, T-PHY u2 at `0x11ca0300`) runs in device mode.
   - It streams the live console and takes commands: `peek/poke`, `pmic`, `i2c`, `push/pull` files, `run` a program
     through `mappled`, `bootrec` to edit the next boot's args, and `reboot`.
   - It reattaches across re-enumeration. A later persona (`05ac:12a8`: PTP + usbmux + the link) is referenced
     ("patch 0182", `mapple-usbd`).
   - It exists only in DEVELOPMENT kernels.
3. **Loader-side pstore:**
   - `XNU_BIG_PSTORE` rewrites `ramoops.console_size=0x40000` to `0x80000` in LK's command line, in place and at the
     same length, so that Android's ramoops keeps a 512 KB console zone.
   - `XNU_LOG_KEEP` keeps matching lines from the dropped middle part.
   - `XNU_LOG_CONSOLE` keeps console lines rather than the msgbuf.
   - The direct path only.

**Ours:** ramoops console (about 190 KB tail survives, `tools/r1-auto.sh` comment) plus `kern.redwood.host_keep` (64 KB)
copied by mtkloader into the pmsg zone, both read after the run returns to Android. Nothing survives a run that does
not return, and nothing streams live.

**For full logs of long hands-on sessions on the r1:**

- **Port `mp_conlog` onto `xnudata`.** We already have a bounded eMMC write path (the var window in `redwood_emmc.c`,
  CRC header `XNUVAR01`, `tools/r1-varhdr.py`).
  - Add a 4–16 MiB ring and a flush thread that writes the ring blocks plus a header into a reserved stretch of
    `xnudata` (not the var slots).
  - The `_cnputs` hook is about one line. Read it back with `adb shell dd` after the session. The format is simple
    enough to rewrite in about 60 lines of Python.
  - Effort S–M. Risk medium: periodic eMMC writes need the user's approval under the current rules, and wear is about
    4 MiB per 10 s at worst, usually far less.
  - **This is the single most useful item for long hands-on sessions.**
- **USB link.** MT6765 also uses MediaTek's MUSB at `0x11200000` per Android's tree. Verify it and the T-PHY base
  against the r1's live FDT; our `docs/ref` does not show them.
  - The device-mode code would need PHY and clock changes ("needs register changes").
  - It would also be the first step toward "USB in iOS" (open problem).
  - Effort M–L. Risk medium: USB clocks and PHY power on a port that also charges.
- **`XNU_BIG_PSTORE`.** S, as an idea, but our r1 already uses the ramoops ftrace zone for the marker
  (`MARKER_PA 0x47d5f000`) and minirdump for the panic log. Check the r1's ramoops layout before resizing.

---

## 4. Porting guide and what an Open Mapple "r1" would need

**What the porting page asks for** (https://open-mapple.pages.dev/docs/guides/porting):
- An unlocked bootloader, root, fastboot and BROM (mtkclient) recovery.
- Backups of boot, lk and the GPT.
- `bringup.py <name> --android-device --android-kernel --fdt fdt.dtb --gpt gpt.bin --ours ...` produces `BRIEF.md`
  (facts plus read-only commands to run), `report.md` (which existing drivers cover what) and a skeleton `device.conf`
  with every status set to `missing`.

**Bring-up order**, each step behind its own boot-arg:
1. Loader logging to ramoops.
2. Kernel, ADT and cores.
3. Read-only storage.
4. Carve and write `xnusys`; root mounts; read-write.
5. LK framebuffer.
6. Keys, then touch.
7. Full DRAM minus holes.
8. PMIC, thermal, DVFS.
9. GPU at LK's rails.
10. Connsys, audio and the rest.
11. `device.conf` status lines.

Safe boot (hold volume down) appends `mp_bootrec=0 mp_mem4g=0` and turns off GPU, Wi-Fi and Bluetooth steps. The page
covers UFS only and does not mention eMMC.

**The structure:**
- A device repo holds `device.conf`, `configs/<dev>_defconfig` (`#include common_defconfig`, `#include <soc>.config`,
  `#include <dev>.config`), `<dev>.config` (`CONFIG_DEVICE`, `CONFIG_BUTTONS`, ...) and `pull-firmware.sh`.
- `device.conf` keys used by `core/mka.sh`: `soc`, `defconfig`, `firmware`, `cmdline`, `adt`, `loader`, `dev_loader`,
  `single_loader`, `split_loader`, `single_cmdline`, `payload_max`, `root_size`, `split_size`, feature flags
  (`light_ui`, `bluetooth`, `resize`, `audio`) and `status <part> <works|mostly|partial|missing> <text>`.
- `lunch mapple_<dev>[_single|_split]-<user|userdebug>`; `mka mapple`.
- `sweet` and `sunfish` are Qualcomm devices with a `cmdline` and `adt` and one `status build` line ("build-only").
  They show how thin a device repo is when the SoC layer does the work.

**Could the r1 be an Open Mapple device?** A sketch of a `device.conf`:

```
device r1
	soc	mt6765
	cmdline	... rd=<emmc root> cpus=8 ...      # every r1 boot-arg would need an mp_ name and a driver behind it
	adt	--r1 --emmc ...                      # mkadt.py has no MT6765/eMMC/PowerVR options
	loader	-DXNU_SHIM=1                         # shim has no eMMC (MSDC) reader; bootrec.c is UFS only
	payload_max	0x1f00000                    # 32 MB boot: the 13 MB kernel fits, Android+kernel is tight
```

**What is missing on their side:**
- an MT6765 SoC layer and an `ARM64_BOARD_CONFIG_MT6765` (65 files in their common kernel are gated on
  `ARM64_BOARD_CONFIG_MT6781`)
- an eMMC root driver (theirs is UFS only)
- a PowerVR GPU (they have `mapple_gpu_mali` and `mapple_gpu_adreno` only)
- the MT6357 PMIC/codec and the MT6370 backlight
- the CLDMA modem (their cellular is "missing")
- our camera.

**Rule conflicts:** their ground rules forbid injection ("no iOS binary patched, no code injected", with one
exception), while our userland depends on `inject*.c` across backboardd, SpringBoard, mediaserverd and others. An r1
device would not be accepted upstream as it is today, and adopting their base also means moving to iOS 16.7.16 and
xnu-8796.141.3.

**Migration path, if it is ever wanted** (rough order):
1. Rebase our drivers as an MT6765 layer onto `xnu_kernel_mediatek_common`.
2. Add an eMMC root driver modeled on `mtk_ufs.c`.
3. Teach the shim the MSDC reader.
4. Build an HFS+ root.
5. Replace each injection with a kernel-side fix.

That is XL: months, with most of the risk in step 5.

**Recommendation: cherry-pick into our tree.** The boot, log and memory pieces in §6 are small and mostly MIT/APSL.

**Release and install ideas worth copying (no code needed):**
- A signed `MANIFEST` (`ssh-keygen -Y verify`, the SHA-256 of every file, the source commits and the exact boot-args;
  `mapple_releases/MANIFEST`).
- Pinned downloads with size and SHA-256 (`mapple_vendor/config/common.conf`).
- iOS assembled on the user's machine from Apple's IPSW, so the release carries nothing of Apple's.
- The installer's "verify, then chain, keep the original boot in a saved area, undo journal" flow. That last part would
  replace our `backup/` on the Mac with an on-device backup.

---

## 5. Licences and provenance (for reuse in our public fork)

Our fork, `kalub92/hakios-xnu-kernel`, has no LICENSE file in the tree. The README says: XNU patch and derived files
APSL 2.0, everything else MIT, per the file headers.

| Their component | Licence | Reuse in our tree |
|---|---|---|
| `mapple_loader` (mtkloader, shim, bootrec, holes, identity, mtkroot.h) | MIT: HakiOS authors 2025-26, dipa-studio 2026 (shim), Open Mapple authors 2026 | **OK.** Keep the copyright and permission notice (add their lines to the file headers and a NOTICE). |
| `mtk/loader/splitmenu.h`, `splitmenu_test.c` | **GPL-3.0-only**, Pritish Joshi (DrtSinX98) | **Do not copy.** A loader built with it is GPL-3.0 as a whole. Write our own menu (an idea is not copyrightable). |
| `mapple_splitboot` (split.c, menu tools, fdt-pen-test) | GPL-3.0-only. Files marked "MIT AND GPL-3.0-only" (gpt-carve.py, mkdatavol.py, var-store-read.py) are HakiOS MIT plus their GPL changes | Only the HakiOS original (which we already have) is MIT. Their changes are GPL: do not copy them. |
| `mapple_build` tools (mkpayload, mkadt, adt.py, bootimg, mapple-link, mkiosroot-macos, root-tweaks) | MIT (dipa-studio / ios-viva; HakiOS for mkxnup.py and fetch-from-ipsw.py) | OK with notices. `mapple-install` built with `MAPPLE_SPLIT` becomes GPL; the core `mapple-install.c` is MIT. |
| `fonts/terminus` | SIL OFL 1.1 | OK if the OFL text ships with it; not needed (we make our font on the Mac). |
| `xnu_kernel_mediatek_common` / `_mt6781` changes to Apple files, HFS staged | APSL 2.0 (Apple's notices kept) | OK: same licence as our XNU patch. Mark the changes and keep the notices. |
| ios-sweet / ios-viva added kernel files, darwin-xnu-build | MIT (dipa-studio, blacktop) | OK with notices. |
| Mapple app, `mapple-assemble` | **Proprietary**: "all rights reserved, free to use with Open Mapple" | Do not use or redistribute. |
| Device repos (`xnu_device_realme_spaced`) | MIT; NOTICE and LEGAL.md inherited from hakios-mt6768 | OK; mostly config, nothing r1-specific. |

**Provenance rules they follow, which we should match:**
- Nothing of Apple's or the vendor's in the repositories.
- Firmware is pulled from the user's own phone.
- The touch firmware and the identity header are generated and git-ignored. Our `mtkloader/touchfw.h` is git-ignored
  the same way; verify it is not committed.
- Register facts come from vendor GPL Linux sources, read and not copied, with the source named.

Their LICENSE files show they were forked from **our upstream** (`oosexgang/hakios-xnu-kernel`), so much of the MIT
code is our own lineage. Credit "the Open Mapple authors" or "dipa-studio" for anything new.

---

## 6. Ranked findings

Each entry gives: Open Mapple path → what it does → our counterpart → portability to MT6765/r1 → effort → risk.

1. **Payload off the boot partition** (idea from `mtkroot.h` + `run_shim`; mechanism already ours)
   - What it does: only the loader lives in boot; kernel and ramdisk are read from a partition and checked before XNU
     is chosen.
   - Ours: `tools/chainboot.py` (32 MB check), `mtkloader/msdc.h`, the `SYSTEM_LBA` path, `tools/r1-xnusys.sh`.
   - Portability: direct, with our own code (an `xnuboot_a` logical partition in super).
   - Effort S–M. Risk low–medium.
   - Ends the boot squeeze, and turns `xnu-cycle.sh` into a `dd` from Android instead of a fastboot reflash.
2. **Persistent console log ring** (`common/bsd/dev/mtk_ufs.c:1121-1250`, `serial_console.c:285`)
   - What it does: a 4 MiB console ring flushed to flash every 10 s; it survives hangs and power-offs.
   - Ours: ramoops tail + 64 KB `host_keep`.
   - Portability: needs register changes (MSDC write path on `xnudata`, reusing the `redwood_emmc.c` var window).
   - Effort S–M. Risk medium (eMMC writes; user approval needed).
3. **All of DRAM minus holes, `mp_mem4g`** (`shim/holes.c`, `shim/fdt.c`, `shim/loader.c`, `common/osfmk/arm64/arm_vm_init.c:1496-1570`)
   - What it does: XNU gets DRAM from `0x40000000` minus LK's reserved-memory and runtime pools; 3.5 of 4 GB on spaced.
   - Ours: a fixed `0xc0000000` window of 2016 MB.
   - Portability: needs register changes (the r1's reserved-memory plus Android's runtime pools from `/proc/iomem`:
     connsys, CCCI/modem, SCP).
   - Effort M. Risk medium–high: EMI MPU violations if a pool is missed; the 2 GB fallback must stay.
4. **Boot-args record + safe boot** (`shim/bootrec.h`, `bootrec.c`, `mtkloader.c:1747-1813`)
   - What it does: next boot's args are set from running iOS without reflashing; the record applies only to the
     payload line it was made against; a held key gives a safe boot.
   - Ours: args compiled into the loader.
   - Portability: idea + MIT code; store it in `xnudata` and read it with `msdc_read`.
   - Effort S–M. Risk low.
5. **A boot menu instead of the watcher and one-shot flag**
   - What it does: Android or iOS at every power-on, default and timeout, a root check before iOS.
   - Ours: `tools/boot-xnu.sh` + dtbo_b one-shot + `XNU_STAY` + side-button watcher.
   - Portability: idea only (GPL code). Use the side button through KP_MEM1, which the loader already reads.
   - Effort S. Risk low if Android stays the default.
6. **USB link for live log and commands** (`common/bsd/dev/usb_link.c`, `mapple_build/tools/mapple-link.py`)
   - What it does: MUSB device mode with console stream, peek/poke, push/pull and run.
   - Ours: none (no USB in iOS).
   - Portability: needs register changes; MT6765 is also MUSB, verify the PHY and clocks.
   - Effort M–L. Risk medium.
7. **Read-write HFS+ root** (`common/bsd/hfs_staged/`, `mtk_ufs.c` write discipline, `mkiosroot-macos.sh`)
   - What it does: native persistence, no var_store.
   - Ours: `var_store.c` + read-only APFS.
   - Portability: needs an eMMC write driver plus an HFS import.
   - Effort L. Risk high.
8. **Root and partition-tail conventions and their self-check** (`mtkroot.h` `root_check`, `tools/mapple_tail_test.py`, `mapple-install.c` regions)
   - What it does: one map of every area, checked by a test; iOS is refused if the root is incomplete.
   - Ours: constants spread over `r1-varhdr.py`, `darwin-write.py` and `redwood_emmc.c`.
   - Portability: idea + MIT code.
   - Effort S. Risk low.
9. **Boot-args length guard** (both projects truncate silently)
   - Ours: `mtkloader.c:2565`.
   - Portability: direct.
   - Effort S. Risk none.
   - Worth doing now: r1 test lines are long.
10. **Release and install hygiene** (`mapple_releases/MANIFEST(.sig)`, `release.sh`, `mapple_vendor/config/common.conf`, `mapple-install verify-root/chain/save`)
    - What it does: signed manifests, pinned inputs, root verified on the device, Android boot backed up on the device
      with undo.
    - Ours: `backup/` on the Mac, manual flashing.
    - Portability: idea; MIT code for the non-split parts.
    - Effort M. Risk low.

Also noted, lower value:
- `XNU_BIG_PSTORE` (S; check the r1's ramoops zones first).
- `XNU_PARK` pen (only if the r1 shows the same "core off cannot come back" behaviour).
- The `bringup.py` brief generator (not checked out; its approach of reading the live FDT, GPT and Android trees and
  writing a report matches our `docs/mtk-porting.md`).
