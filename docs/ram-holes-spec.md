# All of RAM minus holes (Open Mapple plan item 2.9): port spec for the r1

Status: analysis only. Nothing was built, flashed or run. Written 2026-11-18.

Labels: **[V]** verified from code or a log line (file:line or log given); **[I]** inference (basis given);
**[A]** assumption that only a device run can confirm.

Sources: `mtkloader/{mtkloader.c,start.S,link.ld,mtkdbg.h,mtkmodem.h}`, `xnu-src/xnu-8796.121.2/osfmk/{arm64/arm_vm_init.c,arm/arm_init.c,arm/pmap/pmap.c,arm/pmap/pmap_data.h,vm/vm_resident.c}`
(our `xnu/redwood.patch` does not touch any of these: **[V]** grep), `xnu/files/...` drivers, `logs/{t3b,md*,vm*,bt-hands1}.log`,
Android's device tree and `/proc/iomem` as dumped in `rabbit(i)OS/r1port/{r1.dts,iomem.txt}` (how they were taken is
not recorded; they are Android's view of LK's tree), Open Mapple (read only) `mapple_loader/shim/{holes.c,fdt.c,loader.c}` (MIT) and
`xnu_kernel_mediatek_common/osfmk/...` (APSL). The haretic device tree is not in the repo; haretic's own modem probe
reads LK's `/reserved-memory` at run time (`docs/ref/haretic-modem/r1_md_probe.c:533-542`), i.e. the mainline port also
takes the reserved list from LK, which is what this spec does.

## 0. Summary

- Today XNU gets one contiguous window **0xc0000000 + 0x7e000000 = 2016 MB** (`mtkloader.c:35,43`), half of it above
  4 GB. Above the window there are only 32 MB more (up to dramc-rk1 at 0x13ffff000). **Every extra megabyte has to come
  from below 0xc0000000**, the 2 GB where LK, ATF, TEE, GenieZone, SSPM, SCP, the modem, the framebuffer, pstore and our
  own loader live.
- XNU only hands out pages from the kernel upwards (`avail_start = topOfKernelData`, `arm_vm_init.c:1940`; free pages run
  `first_avail..avail_end`, `pmap.c:2630-2650`). So the kernel must be loaded at the **bottom** of the new range;
  `physBase` moves down, and everything between physBase and the top that is not ours becomes a hole.
- Two stages, both behind a boot-arg, off by default:
  - **Stage 1 `r1_mem=1`: physBase 0x80000000.** 3072 MB range, 9 merged holes (278 MB: LK `/reserved-memory`
    entries plus 4 Android pools), **≈2794 MB for the VM (+778 MB)**. Nothing of the loader, pstore, LK or Android's boot
    path is in that range.
  - **Stage 2 `r1_mem=2`: physBase 0x48000000.** 3968 MB range, 14 merged holes (322 MB), **≈3646 MB (+1630 MB)**. Needs our
    own connsys/Wi-Fi windows (0x51000000) as holes, and a hang-rescue test, because LK runs over low DRAM on the
    reboot that rescues a hung run's log.
  - `r1_memcap=MB` caps either stage (2560, 3072, ...), so the size can go up step by step.
- **No driver breaks.** The new memory is all **below** 4 GB, so 32-bit DMA gets easier, not harder. The only memory
  added above today's top is 0x13e000000-0x13fffc000 (32 MB, above 4 GB), and every 32-bit DMA user already asks for
  memory below 4 GB and checks it. MSDC is PIO, audio is SRAM-only, Wi-Fi/BTIF use fixed windows, PVR reaches 36 bits.
- **Return to Android does not need the Android images in RAM**: on the r1 every exit is a DDR-reserve watchdog reset,
  and LK reloads boot_a from the eMMC (§3). What must survive: the loader's code, data and stacks, ramoops/pstore,
  minirdump, and (for the hang-rescue path only) XNU's own early memory across LK's reboot.
- **Recommendation: build Stage 1, behind `r1_mem=1`. Do not build Stage 2 until a workload needs it.** Measured headroom
  at 2 GB is fine (worst `available` 970 MB in a hands-on session), so this is headroom and parity with the iPhone X
  (3 GB) that the userland expects, not a fix for a known failure. Effort M, risk medium (Stage 1), medium-high (Stage 2).

## 1. How the memory range is set today

### 1.1 Loader (`mtkloader/mtkloader.c`)

| What | Where | Value |
|---|---|---|
| XNU's base | `:35` `XNU_PHYS_BASE` | 0xc0000000 |
| XNU's size | `:43` `XNU_MEM_SIZE` (r1) | 0x7e000000 (2016 MB; "rounded down to 32 MB: arm_vm_init hung on 0x3eff4000", `:41`) |
| Smaller on request | `:189-194` `xnu_mem_size()`, boot-arg `xnumem=MB` | never more than XNU_MEM_SIZE |
| Kernel VA ↔ PA | `:56` `XNU_SLIDE = XNU_PHYS_BASE - KERNEL_VIRT_BASE`; `:604-607` `kva_to_pa()` | fixed at compile time |
| Kernel copy, ramdisk, ADT, boot_args | `run_xnu()` `:2380-2420` (segments to `kva_to_pa(vmaddr)`), `:2529-2549` (ramdisk at kernel end, ADT, boot_args page; `top = ba_pa + 0x4000`) | kernel text lands at physBase + 0x3004000 (`logs/t3b.log`: `kernel at 0xc3004000`) |
| boot_args | `:2552` physBase, `:2554-2555` memSize/memSizeActual, `:2557` deviceTreeP | |
| Return slide for XNU | `:2524` `slot[3] = XNU_SLIDE` (read by `xnu/files/osfmk/arm64/loader_return.c:233-237`) | |
| ADT `/chosen` | `:919-920` `dram-base`, `dram-size` | 0xc0000000, the size |
| Printed | `:2592` | `framebuffer 0x7efe0000 pitch 1920, XNU memory 0xc0000000+0x7e000000` **[V]** t3b |

The comment at `:37-42` already lists LK's reservations and says they are all below 0xc0000000 **[V]** against the
r1.dts dump.

### 1.2 Kernel (stock `arm_vm_init.c`, not patched by us)

- `:1831-1836` `gVirtBase/gPhysBase = args->virtBase/physBase`, `real_phys_size = args->memSize`.
- `:1847` `gPhysSize = mem_size = round_down(physBase + memSize) - physBase`; `:1849` `mem_actual`.
- `:1850-1855` `maxmem=` (parsed in `osfmk/arm/arm_init.c:499-504`, passed in `:545`) only shrinks it.
- `:1940` `avail_start = topOfKernelData`; `:1957` `avail_end = gPhysBase + mem_size`.
- `:1485-1512` `arm_vm_physmap_slide()` maps the whole range cacheable in the physmap.
- `:2203-2204` `sane_size`, `max_mem`; `:2366-2371` `first_avail = avail_start`.
- `pmap_data.h:75-78` `pa_valid(pa)` = in `[vm_first_phys, vm_last_phys)`; managed pages get their WIMG from
  `pp_attr_table`, unmanaged ones are mapped as asked (I/O).
- `pmap.c:2630` `pmap_next_page_hi`, `:2639` `pmap_next_page` walk `first_avail → avail_end` with no gaps.
- `vm_resident.c:1569` `vm_page_init(&vm_pages[i], phys_page)` for every page of that walk.
- `arm_init.c:660-669` reads `/chosen dram-base/dram-size` into `gDramBase/gDramSize`; their only use is
  `pmap.c:5659` `wimg_to_pte`: a `VM_WIMG_IO` mapping of a DRAM address becomes Device-GRE instead of Device-nGnRnE.
- Result today **[V]** t3b: `vm_page_bootstrap: 123855 free pages, 5169 wired pages` (= 2016 MB).

## 2. How Open Mapple's hole mechanism works

Loader side (MIT, `mapple_loader/shim`):
- `holes.c:11-65` `mem_holes()`: every `reg` under LK's `/reserved-memory` (`fdt.c:100-140` `fdt_reserved()`, no-map or
  not), plus the payload range (LK's initrd range, extended in place by mtkloader over its tables), plus LK's FDT itself,
  plus `#ifdef SPACED` Android's run-time Wi-Fi/connsys pools (`:29-31`, hard-coded because LK's tree only has
  `size`/`alloc-ranges` for them). Each hole is rounded out to 16 KiB, clipped to `[lo, hi)`, sorted, merged (`:36-63`).
- `loader.c:141-168`: unless `mp_mem4g=0`, holes over `[0x40000000, end)`; **if any hole lies below the kernel's
  top, or the kernel would land on the shim itself, fall back to the old 2 GB window** (`:151-156`). Otherwise
  `physBase = 0x40000000`, `memSize = end - 0x40000000`.
- `loader.c:241-249`: the list goes into the boot_args 16 KiB page at `+0x2000` as `"MPHOLES1"`, count, ranges.

Kernel side (APSL, `xnu_kernel_mediatek_common`, compiled in only for MT6781):
- `osfmk/arm64/arm_vm_init.c:1485-1617`: the list is read from `topOfKernelData - 0x4000 + 0x2000`, validated
  (sorted, page-aligned, inside the range: panics otherwise) (`mapple_mem4g_init`); `mapple_pa_in_hole` (binary
  search), `mapple_hole_skip`, `mapple_hole_bytes_in`, `mapple_physmap_map` (physmap maps everything **but** the holes,
  "so the CPU has no cacheable alias to prefetch through into memory the EMI MPU protects"), `mapple_mem4g_report`.
- `:1647-1652` physmap slide uses `mapple_physmap_map`; `:2035-2039` `mem_actual/max_mem_actual -= hole bytes`;
  `:2398-2401` `sane_size/max_mem -= hole bytes`; `:2571-2574` panic if the boot allocations ran into a hole.
- `osfmk/arm/pmap/pmap_data.h:74-87`: `pa_valid()` is false inside a hole (so holes map as I/O, as before).
- `osfmk/arm/pmap/pmap.c:2568-2570, 2636-2645`: `pmap_next_page_hi()` (used by `pmap_steal_memory`) skips holes.
- `osfmk/vm/vm_resident.c:1394-1396, 1573-1579`: a hole page keeps its slot in `vm_pages[]` (the array is indexed by
  page number) but gets `vmp_error = TRUE` and is never freed.
- Users: `iokit/Kernel/arm/MapplePlatformExpert.cpp:99` prints the report; `MT6631WiFi.cpp:25-40` and `bsd/dev/mtk_scp.c:460-461`
  refuse a DMA window that is not inside a hole.

Differences that matter for us:
1. Mapple's products never return to Android (`docs/open-mapple-boot-build.md` §1.1). We return, through a watcher that
   runs loader code during the whole run and a reset that rescues XNU's log from DRAM.
2. Mapple loads the kernel at DRAM base 0x40000000. Our loader **is** at 0x40080000 (`link.ld`), with its stacks at
   0x40060000-0x40080000 (`start.S:46-49, 146-147, 216-217, 323-324`) and the payload behind it; so our base must be
   higher, and only one base, not a scan.
3. Mapple gates at compile time (`#if defined(MT6781)`). Ours must be a run-time switch with `hole_count == 0` meaning
   exactly today's behaviour.

## 3. What "return to Android" needs to survive

**[V]** from code:
- Every r1 exit is `reset_board()` (`mtkloader.c:1048-1061`: TOPRGU watchdog in DDR-reserve mode), from
  `xnu_returned()` (`:1611`), `loader_fault()` (`:1118`) and the watcher's dead-man/side-button paths.
- After that reset LK runs again, reloads boot_a from the eMMC, and the loader's marker branch
  (`:3711-3722`) rescues/keeps the log and calls `chain_android()` (`:1068-1094`), which uses the **freshly loaded**
  Android Image at `_start + CHAIN_OFFSET` and LK's **fresh** DTB. **So the Android Image, its ramdisk and LK's DTB in
  RAM do not need to survive XNU.**

What must survive the XNU run (all in Stage 2's range, none in Stage 1's):

| Region | PA | Why | Source |
|---|---|---|---|
| Loader code/data/bss, return stub | 0x40080000.. (`link.ld`); stub e.g. `return stub at 0x4008012c` **[V]** t3b | XNU jumps back to it (`loader_return.c:277` identity-maps it); the watcher core runs it all run long | code |
| Loader stacks | 0x40060000-0x40080000 | boot, watcher (`-0x18000`), fault (`-0x10000`), return (`-0x8000`) | `start.S` |
| XNU Mach-O inside the payload | behind the Android Image in the boot image (≤32 MB from 0x40080000) **[A]** | `xkernel` symbol names in `show_backtrace()`; losing it only loses names | `mtkloader.c:299,1393-1413,2423` |
| Run marker | 0x47d5f000 (ramoops ftrace zone) | tells the next boot "rescue" vs "done" | `mtkloader.c:202` |
| ramoops console / pmsg | 0x47d1f000+0x40000, 0x47d60000+0x10000 | the kept log and host_keep lines | `mtkdbg.h:21-27` |
| Panic log ("pram") | 0x47d70000+0x10000 (minirdump) | | `mtkloader.c:884-887` |

And across the reset (hang-rescue path only, `rescue_log()` `:1666`): XNU's own message buffer, page tables
(`cpu_ttep`) and statics are read **after LK has run again**. Today they sit at 0xc0000000+, which LK does not
touch. With Stage 2 the kernel sits at 0x4b004000+; where LK puts its own code, heap and the Android ramdisk
(iomem shows `51b00000-52803fff : reserved` **[V]**, LK's DTB at 0x47880000 in 349 runs **[V]**) is not known **[A]**. A
clean return (`xnu_returned`, side button) saves the log **before** the reset and is not affected.

Also: the rescue must know which base the hung run used. With a fallback (§5.3) it can differ from what the boot-args
say, so the loader must store the base next to the marker (three copies, as the marker).

## 4. The reserved regions of the r1

Range of interest 0x40000000-0x140000000 (4 GB). "DT" = the r1.dts dump of LK's tree as Android booted it; "md0" = the
loader's own walk of LK's tree printed at run time (`mtkmodem.h:220-246`), **[V]** in 44 runs (`logs/md*.log`).

| # | Region | PA, size | Source | Status | In stage |
|---|---|---|---|---|---|
| 1 | mblock-20 ap_md_c_smem (modem cached shared mem) | 0x7c000000 +0x1600000 | DT, md0 | V | 2 |
| 2 | mblock-19 ap_md_nc_smem | 0x78000000 +0x170000 | DT, md0 | V | 2 |
| 3 | mblock-18 md_mem_usage (modem image, LK-loaded) | 0x8e000000 +0x10000000 | DT, md0; handoff `base 0x8e000000 size 0x7e00000` | V | 1, 2 |
| 4 | mblock-17 ccci_tag_mem | 0x9fff0000 +0x10000 | DT, md0 | V | 1, 2 |
| 5 | mblock-16 SCP-reserved | 0x9f900000 +0x600000 | DT | V (DT) | 1, 2 |
| 6 | mblock-15 SPM-reserved | 0x77ff0000 +0x10000 | DT | V (DT) | 2 |
| 7 | mblock-14 framebuffer (LK's, still scanned by OVL0) | 0x7efe0000 +0x420000 | DT, `mtkdbg.h:39-40`, t3b `framebuffer 0x7efe0000` | V | 2 |
| 8 | mblock-13 minirdump (our panic log) | 0x47d70000 +0x10000 | DT, `mtkloader.c:886` | V | 2 |
| 9 | mblock-12 pstore (our ramoops, marker) | 0x47c90000 +0xe0000 | DT, iomem `persistent_ram`, `mtkdbg.h` | V | 2 |
| 10 | mblock-11 aee_debug_kinfo | 0x47c80000 +0x10000 | DT | V (DT) | 2 |
| 11 | mblock-10 SSPM-reserved | 0x7f700000 +0xc0000 | DT | V (DT) | 2 |
| 12 | mblock-9 tee-secmem | 0x7f400000 +0x200000 | DT | V (DT) | 2 |
| 13 | mblock-8 atf-log | 0xbff30000 +0x40000 | DT | V (DT) | 1, 2 |
| 14 | mblock-7 atf-ramdump | 0xbff70000 +0x80000 | DT, md0 | V | 1, 2 |
| 15 | mblock-6 atf-reserved (ATF/BL31) | 0x47d80000 +0x50000 | DT. An EMI MPU violation record at **0x47d81b80** is printed by r1cam in every run ("an old record", t3b `[129.024438]`) | V (DT) | 2 |
| 16 | mblock-5 log_store (preloader/LK boot log, written every boot) | 0x7f7c0000 +0x40000 | DT | V (DT) | 2 |
| 17 | mblock-4 gz-log | 0x7f600000 +0x41000 | DT | V (DT) | 2 |
| 18 | mblock-3 gz (GenieZone, EL2) | 0x7f800000 +0x800000 | DT | V (DT) | 2 |
| 19 | mblock-2 dramc-rk1 (DRAM calibration page) | 0x13ffff000 +0x1000 | DT, `mtkloader.c:39` | V (DT) | 1, 2 |
| 20 | mblock-1 dramc-rk0 | 0xbffff000 +0x1000 | DT | V (DT) | 1, 2 |
| 21 | Our CONNSYS EMI (Wi-Fi/BT firmware, logs) | 0x51000000 +0x400000 | `xnu/files/bsd/sys/redwood_input.h:97-98`; remapped for CONNSYS by `redwood_connsys.c:2493` (infra +0x380 = PA>>24) | V | 2 |
| 22 | Our Wi-Fi WPDMA rings/buffers + BTIF TX FIFO | 0x51400000 +0x100000 | `redwood_input.h:99-100`, `redwood_mtkwifi.c:232-317`, `redwood_connsys.c:504,538` | V | 2 |
| 23 | Loader image + stacks + payload | 0x40000000 - end of boot image (≤ 0x42080000) | `link.ld`, `start.S`, 32 MB boot_a | V (address), A (size) | below both bases |
| 24 | LK's DTB | 0x47880000 +0x27000 | t3b `LK dtb 0x47880000`, iomem `47880000-478a6fff reserved` | V | below both bases; not needed during the run |
| 25 | Android scp_share | 0x8d000000 +0x300000 | DT node has only size/alloc-ranges; iomem gap `8cffffff`..`8d300000` | address I (gap V) | 1, 2 |
| 26 | Android sspm_share | 0x9f3f0000 +0x510000 | as above; gap 0x9f3f0000-0x9f8fffff = exactly 0x510000 | I | 1, 2 |
| 27 | Android consys-reserve-memory | 0xbe000000 +0x400000 | as above; Mapple's spaced has the same pool at 0xbe000000 | I | 1, 2 |
| 28 | Android wifi-reserve-memory | 0xbf000000 +0x300000 | as above; Mapple's MT6631 driver uses "the Android-reserved 0xbf000000 hole" | I | 1, 2 |

Not holes, with the reason:
- Android's `ssmr` CMA (reusable, 320 MB), `ion-carveout-heap` (it lands at 0xcfffb000, inside **today's** window), and
  iomem's Linux-only `reserved` ranges at 0xfeff6000, 0x127000000-0x13effffff etc.: Linux allocations, absent while the
  loader and XNU run. Today's layout already uses that memory, which shows Android's run-time pools are not live during
  XNU **[V]** (by running). Rows 25-28 are holed anyway (15 MB) because SCP and SSPM firmware, started by the
  preloader/LK, keep running during XNU **[A]** that they do not touch their share pools; cheap insurance, as Mapple did.
- Android's ramdisk at 0x51b00000 and LK's working memory: not needed during the run (§3); matter only for Stage 2's
  hang rescue.

Merged hole lists (16 KiB pages):
- **Stage 1** (base 0x80000000, top 0x140000000): 0x8d000000-0x8d300000, 0x8e000000-0x9e000000, 0x9f3f0000-0x9ff00000,
  0x9fff0000-0xa0000000, 0xbe000000-0xbe400000, 0xbf000000-0xbf300000, 0xbff30000-0xbfff0000, 0xbfffc000-0xc0000000,
  0x13fffc000-0x140000000 (9 merged). ≈278 MB → **≈2794 MB** (2809 without rows 25-28).
- **Stage 2** (base 0x48000000): the above plus 0x51000000-0x51500000, 0x77ff0000-0x78170000, 0x7c000000-0x7d600000,
  0x7efe0000-0x7f644000, 0x7f700000-0x80000000 (14 merged). ≈322 MB → **≈3646 MB**.
- Cap tops (computed): Stage 1 `r1_memcap=2560` → top 0x130000000; Stage 2 `r1_memcap=2560` → 0xfc000000,
  `r1_memcap=3072` → 0x11c000000.
- 0x44000000 does not work as a base: the kernel text would land at 0x47004000 and run over pstore/ATF at
  0x47c80000-0x47dd0000 (Mapple would fall back). 0x48000000 is the lowest clean 32 MB-aligned base with the
  0x51000000 hole ~150 MB above the kernel start, enough for kernel (≈72 MB incl. the 48 MB gap below the text) +
  bootstrap allocations.

## 5. Port to our tree

### 5.1 Boot-args

| Arg | Read by | Meaning |
|---|---|---|
| `r1_mem=0` (default, also absent) | loader, kernel | today's layout, no hole list, every new code path inert |
| `r1_mem=1` | loader | Stage 1: base 0x80000000 |
| `r1_mem=2` | loader | Stage 2: base 0x48000000 |
| `r1_memcap=MB` | loader | VM memory (non-hole bytes from base up) at most MB; top = base + MB + holes below that point, rounded down to 32 MB |
| `xnumem=MB` | loader | unchanged, applies only with `r1_mem=0` |

The loader reads these from its compiled-in line (`boot_arg()`, `:169-187`). The line gets ~20 characters longer:
keep the 608 check (`:2565-2578`) in mind with `r1_md=` runs.

### 5.2 Loader changes (`mtkloader.c`)

1. `static uint64_t xnu_base = XNU_PHYS_BASE, xnu_size;` replacing the constant in `:48` (`SYSTEM_PA`), `:56/606`
   (`XNU_SLIDE`/`kva_to_pa`), `:2524` (slot[3]), `:2552-2557`, `:2592`. Keep `/chosen dram-base/dram-size`
   (`:919-920`) **at the old values** 0xc0000000/0x7e000000: widening them changes `wimg_to_pte` (`pmap.c:5659`) from
   Device-nGnRnE to Device-GRE for every I/O mapping of DRAM, i.e. the modem smem, the CONNSYS EMI and the Wi-Fi
   windows; nothing else reads them.
2. `mem_layout(dtb)` called in `mtkloader_main()` before `run_xnu()`:
   - holes = every `reg` under LK's `/reserved-memory` (extend `mtkmodem.h`'s walker, our own code; no need to copy
     Mapple's `fdt.c`), + rows 21-22 + rows 25-28 + `[lk_dtb, lk_dtb + totalsize)`, clipped to `[base, 0x140000000)`,
     16 KiB-rounded, sorted, merged (the algorithm of `holes.c:36-63`; if any code is copied keep its MIT notice).
   - cross-check: LK's tree must contain the 20 `mblock-*` entries of §4 with exactly those regs. Any difference,
     a hole below `base + 0x6000000` (kernel + ramdisk + ADT + boot allocations), payload end above 0x42080000, more than
     48 holes, or `XNU_ROOT_EMMC == 0` (the 2.4 GB in-RAM system image at `SYSTEM_PA` cannot live with holes) →
     `r1mem: FALLBACK (<reason>): the 2 GB layout`, `xnu_base = 0xc0000000`, no list.
   - store `xnu_base` next to the run marker (three copies, `marker_set` style) for `rescue_state()/rescue_log()`.
3. `run_xnu()`: write `"R1HOLES1"`, count, ranges at `ba_pa + 0x2000` (same place as Mapple; our `top = ba_pa + 0x4000`
   matches their `topOfKernelData - 0x4000 + 0x2000`), memSize = top - base, memSizeActual = memSize.
4. Print: `r1mem: layout N: base 0x.. size 0x.., H holes (M MB), cap C MB, VM V MB` and one `r1mem: hole 0x..-0x..`
   per hole.
5. Watcher: `watcher_main()` (`:1955-1966`) already maps 1-8 GB as normal memory; unchanged. It reaches XNU's
   statics through `kva_to_pa` (now variable): fine.

### 5.3 Kernel changes (a patch to stock files, added to `xnu/redwood.patch`; port of Mapple's, APSL notice kept)

All guarded by a run-time `r1_hole_count != 0`:
- `arm_vm_init.c`: `r1_mem_init(args)` after `:1855` (read list at `topOfKernelData - 0x4000 + 0x2000`, validate as
  Mapple's `mapple_mem4g_init`; bad list → panic with the hole index, since the loader printed it); `mem_actual`,
  `max_mem_actual` (after `:1855`) and `sane_size`, `max_mem` (`:2203-2204`) minus hole bytes; physmap
  (`:1512`) through a hole-skipping `r1_physmap_map` (Mapple `:1584-1600`); after `:2366` panic if
  `[topOfKernelData, avail_start)` touches a hole.
- `pmap_data.h:75-78` `pa_valid()`: false inside a hole. **Mandatory**: without it `ml_io_map` of the modem regions
  (`redwood_md.c:2520,2533`), now inside `[vm_first_phys, vm_last_phys)`, would take the managed-page path and get
  cacheable attributes.
- `pmap.c:2630` `pmap_next_page_hi()`: skip holes (Mapple `:2636-2645`).
- `vm_resident.c:1569`: hole pages `vmp_error = TRUE`, not freed (Mapple `:1573-1579`).
- Report, kept: `r1mem: hole 0x..-0x.. N KiB` each and `r1mem: RAM 0x..-0x.., H holes of M MiB, V MiB for the VM`
  through `redwood_host_keep_add()` (`xnu/files/bsd/dev/redwood_mali.c:174`), from the platform expert's start.
- Exported `bool r1_pa_in_hole(uint64_t)` for drivers' sanity checks (see §6).

`ramfs` (`xnu/files/bsd/miscfs/ramfs/ramfs.c:1754`) sizes `/private/var` as `max_mem / 4`: it grows from ~500 MB to
~700/900 MB. Intended, but note it.

## 6. Physical-address assumptions in our drivers

| Driver | Memory it gives a DMA engine | Limit | With holes |
|---|---|---|---|
| eMMC (`redwood_emmc.c:13,53,160`) | none: MSDC0 in PIO | — | unaffected **[V]** |
| Camera (`redwood_r1cam.c:933-980`) | 4 frame pieces, `kmem_alloc_contig(max_pnum = 4 GB-1)`, checked `pa+size ≤ 4 GB`, SMI larb3 MMU off ("the DMA's addresses are physical", t3b `[129.020344]`) | 32 bit | more memory below 4 GB: allocation gets easier **[V]** |
| CLDMA modem (`redwood_cldma.c:233-249`) | contig, below 4 GB, checked; 4 MSB fields kept 0 (`:314`) | 32 bit | same **[V]** |
| Modem smem/image/ccci (`redwood_md.c:2520-2533`, handoff args) | LK's regions, `ml_io_map` | fixed | rows 1-4 are holes; `pa_valid` change required **[V]** |
| CONNSYS/WMT, Wi-Fi WPDMA, BTIF (`redwood_connsys.c:436,504,2493`, `redwood_mtkwifi.c:433`) | fixed 0x51000000 (4 MB, 16 MB-aligned for the infra remap) and 0x51400000 (1 MB) | fixed | rows 21-22: in Stage 2's range, must be holes **[V]** |
| Audio AFE + mic (`redwood_mtkaudio.c:2340`, `:593` VUL in AFE SRAM 0x11225000) | AFE SRAM only | — | unaffected **[V]** |
| PowerVR (`redwood_pvr.c:1195-1206`) | contig arena checked against `PHYS_BUS_WIDTH` (36, t3b); GPU MMU 40-bit | 36 bit | unaffected **[V]** |
| Display (`MiniIOMobileFramebuffer.cpp:565-598`) | scans LK's framebuffer (row 7); the flip path's constants are the X6826B's | fixed | row 7 hole **[V]** |
| SCP (`redwood_scp.c:317-325`) | checks its DRAM is below 0xc0000000 | — | MT6768 (X6826B) code, not used on the r1 **[I]** |

No bounce buffers exist or are needed: no driver hands arbitrary user memory to a 32-bit engine. Above today's top
there are only 32 MB (0x13e000000-0x13fffc000), already above 4 GB like half of today's window; nothing new can
break there.

## 7. Test plan (unattended, `tools/r1-auto.sh`, BASE + run args, reason-0 return each time)

Order: S1a `r1_mem=1 r1_memcap=2560`, S1b `r1_mem=1`; Stage 2 only if decided: S2a `r1_mem=2 r1_memcap=3072`, S2b `r1_mem=2`.
Each step must pass every line before the next. Every run ends with the normal reason-0 return; **stop at the first
failure** and run once with `r1_mem=0` to confirm the device is back to the known state.

| Check | Pass lines (kept log unless "loader") |
|---|---|
| Layout chosen | loader: `r1mem: layout 1: base 0x80000000 size 0xc0000000, 9 holes (278 MB)` (or the capped size); **no** `r1mem: FALLBACK` |
| Kernel agrees | `r1mem: RAM 0x80000000-0x140000000, 9 holes of 278 MiB, V MiB for the VM` with V within 2 MB of the loader's figure |
| Size reported | `vm_page_bootstrap: N free pages`: N ≥ 155000 (cap 2560), ≥ 170000 (Stage 1 max), ≥ 188000 (cap 3072), ≥ 225000 (Stage 2 max); today 123855 |
| Memory actually used | new `r1_memtest=1` (Task R4): `r1mem: test: T MiB touched (L MiB below 0xc0000000, H MiB above 4 GB), 0 bad, EMI MPUS 0x.. -> 0x.. unchanged` with L ≥ 0.8 × the new range's VM MiB |
| Camera | rc.jobs.mv7 scenario as t3reg: `still: tap 1: ... WRITTEN`, `tap 1: THE PHOTO IS IN THE LIBRARY`; no `FRAME MEMORY NOT IN ONE PIECE BELOW 4 GB`; r1cam's `EMI protection: ... not in the frame memory` (the old 0x47d81b80 record only) |
| Audio + mic | same run: `IMG_000N.MOV: video 'avc1' 1920x1080` and `audio N samples (... at 16 kHz), peak P` with P > 500 (t3reg: 2041) |
| Wi-Fi chip (Stage 2 especially) | `bt: 6 of 6 answered` and `connsys: step 15 ... scan 1 done ... N heard` with N > 0 |
| Modem, radio off | c4d1 configuration: `r1md: RESULT 4: data_up (fake bearer)` and 0 × `AT> AT+EFUN=1` |
| Side-button path (watcher reads XNU through the new slide) | `sidebtn_test=SECS`: the watcher snapshot shows `cpu_ttep 0x...` and a heartbeat, then `XNU returned`/`reason` lines, Android boots |
| Return to Android intact | `=== XNU returned to mtkloader: reason 0 (end)`, `=== kept for the host by the kernel: N bytes (0 dropped)`, Android `adb` answers, pstore console present (r1-auto.sh's normal checks) |
| Hang rescue (Stage 2 mandatory, Stage 1 once) | loader built `-DWDT_TEST=1` (existing option: the watcher starves the watchdog): after the reset `=== XNU log (from DRAM; ...)` contains this run's `r1mem: RAM` line and a `memory: free ...` line from the last 10 s. Needs the owner's OK: it is a loader build option, not a behaviour change |

## 8. Risks and effort

| Risk | Stage | Mitigation |
|---|---|---|
| A reserved region missed → EMI MPU violation, secure abort or silent corruption of TEE/ATF/modem | 1: low (only LK-listed regions in range, cross-checked) / 2: medium | fail-closed cross-check, physmap without holes, memtest with MPUS before/after |
| SCP/SSPM firmware writing their Android share pools | 1, 2 | rows 25-28 holed |
| LK's reboot overwrites XNU's early memory → hung run's log lost | 2 (kernel at 0x4b..) | WDT_TEST run; clean returns unaffected |
| Kernel or boot allocations land on a hole | 1, 2 | loader refuses (base + 96 MB clean), kernel panics with a clear line rather than corrupting |
| `pa_valid` / physmap change wrong when off | all | every change behind `r1_hole_count != 0`; `r1_mem=0` must reproduce `vm_page_bootstrap: 123855` |
| Rescue uses the wrong base after a fallback | 1, 2 | base stored with the marker |
| Boot-args over 608 with `r1_md=` | all | existing check prints `BOOT-ARGS TOO LONG` |

Effort: loader S-M (~150 lines), kernel M (~180 lines, mostly Mapple's), memtest S (~80 lines), 4-6 device runs per
stage.

## 9. Recommendation

- **Do Stage 1, off by default, and stop there.** +778 MB (2 GB → ~2.8 GB, close to the iPhone X's 3 GB the jetsam
  tables and apps assume **[I]**), with nothing in the new range but LK-listed, MPU-protected firmware regions that the
  loader can check against LK's own tree at every boot.
- **Do not do Stage 2 now.** +850 MB more costs the hang-rescue guarantee (unknown LK footprint), puts our own CONNSYS
  windows and pstore next to the kernel, and buys headroom nobody has run out of: the worst `available` in any 2 GB
  log is 970 MB (`logs/bt-hands1.log`), typically 1.4-1.6 GB (t3b `available 1591`).
- Priority: below the camera and cellular work; it fixes no reported problem.

## 10. Implementation tasks (Sonnet-sized)

**R1 — loader layout (mtkloader/mtkloader.c, mtkloader/mtkmodem.h).**
Variable `xnu_base/xnu_size` (all uses in §5.2), `mem_layout()` with the hole table of §4, LK DT walk (reuse
`mtkmodem.h`'s FDT helpers: add `dt_reserved_regs(dtb, out, max)`), cross-check, fallback, base stored with the marker,
`R1HOLES1` list at `ba_pa + 0x2000`, prints. Off (`r1_mem` absent) must produce byte-identical boot_args to today.
Pass: with `r1_mem=0` loader prints `XNU memory 0xc0000000+0x7e000000` and the kernel `vm_page_bootstrap: 123855 free
pages`; with `r1_mem=1` loader prints `r1mem: layout 1: base 0x80000000 size 0xc0000000, 9 holes (278 MB)` and nine
`r1mem: hole` lines.

**R2 — kernel holes (patch to arm_vm_init.c, pmap_data.h, pmap.c, vm_resident.c; regenerate xnu/redwood.patch).**
Port Mapple's functions renamed `r1_*`, run-time guarded; report via `redwood_host_keep_add`. Keep the APSL header.
Pass (kept): `r1mem: RAM 0x80000000-0x140000000, 9 holes of 278 MiB, V MiB for the VM`; `vm_page_bootstrap` ≥ 170000;
the run returns with reason 0. With `r1_mem=0`: no `r1mem:` kernel line, 123855 free pages.

**R3 — cap (mtkloader.c).** `r1_memcap=MB`. Pass: `r1_mem=1 r1_memcap=2560` → loader `cap 2560 MB`, kernel `V` within
32 MB of 2560, `vm_page_bootstrap` ≥ 155000.

**R4 — memory test (new function in xnu/files/osfmk/arm64/loader_return.c next to `redwood_memory_line`, boot-arg
`r1_memtest=1`, kernel thread 60 s after boot).** Allocate 16 MiB chunks (`kernel_memory_allocate`, `KMA_NOPAGEWAIT`)
until free memory is below 300 MiB or 64 chunks; write `pa ^ 0x5a5a...` to every 8th word of every page, read back, count
pages by PA band, read EMI_MPUS (0x102191f0) before and after, free all. Pass (kept): `r1mem: test: T MiB touched (L
MiB below 0xc0000000, H MiB above 4 GB), 0 bad, EMI MPUS 0x40010920 -> 0x40010920 unchanged`, then the run's normal
`memory: free ...` line with `available` back within 50 MB of before.

**R5 — device runs (Sonnet, after R1-R4, tell the user first).** §7's table, Stage 1 only: S1a then S1b, plus one
`sidebtn_test=` run. Pass: every line of §7 for both runs; any miss → stop, one `r1_mem=0` run, report.

**R6 — docs.** STATUS.md line for item 2.9 with the measured `vm_page_bootstrap` and `r1mem: RAM` lines.
