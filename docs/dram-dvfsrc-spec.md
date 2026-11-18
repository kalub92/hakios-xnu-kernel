# DRAM DVFSRC spec (r1, MT6765, LPDDR4X) — 2026-11-17

Analysis only; nothing implemented. Reference copies: `docs/ref/dvfsrc/` (haretic-* = github.com/haretic/android-kernel-mt6765-rabbitr1 main; vendor-* = github.com/danya2271/kuroneko_r_mt6765, MediaTek MT6765 vendor kernel). "guess" = not verified on the device.

**Bottom line:** VCORE 0.8 V in every log suggests the DRAM already runs at 3200 MT/s (guess; stage A settles it). If so, DVFSRC brings no speed; its only gain is idle power (~40 mA per haretic dts ~1003 comment), which needs the DRAM to float down — a later stage D. Stage A first; it decides whether B/C are worth it.

## SMC function ids (SMC64 fast SiP, `smc #0`, x0 = fid, x1..x3 args, return x0 [x1])

| fid | name | args | return | source |
|---|---|---|---|---|
| 0xC2000221 | MTK_SIP_SPM_FIRMWARE_STATUS | – | x0: -1 not set up, 0 none, 1 loaded, 2 kicked | haretic-mt6765-spm-sleep.c:48,53-55 |
| 0xC200022A | MTK_SIP_SPM_ARGS | x1 = 0 (SPM_ARGS_SPMFW_IDX), x2 = 1 (idx) | ignored | spm-sleep.c:50,58,349 |
| 0xC2000506 | MTK_SIP_DVFSRC_VCOREFS_CONTROL, INIT | x1 = 0, rest 0 | x0 == 0 ok; x1 = DRAM type | haretic-mtk-dvfsrc.c:57,694-702 |
| 0xC2000506 | same, START | x1 = 1, rest 0 | (x0 & 1) == 0 ok | haretic-mtk-dvfsrc.c:58,720-723 |
| 0xC2000226 | vendor 4.x MTK_SIP_KERNEL_SPM_VCOREFS_ARGS (CMD_0 init, CMD_1 kick with spm_flags in x2, CMD_3 pwrap cmd) | — | — | vendor-mtk_vcore_dvfs.c:10-51; fallback only if 0x506 returns -1 (NOT_SUPPORTED) — guess |

Vendor GKI driver uses 0x506 too, but INIT with x1 = 0, x2 = dvfsrc flag, x3 = vmode, and sub-cmd 0x3 (GET_SPM_INFO → x1 dram type) first (vendor-mtk-dvfsrc.c ~1002-1020). Haretic's plain INIT/START is proven on the r1; follow haretic.

Order (haretic): spm-sleep probe is subsys_initcall (STATUS ≥ 1, then SPM_ARGS), dvfsrc probe is later (INIT, START) — spm-sleep.c:345-350,376-380.

Our helper today: `sip_call(fid)` at xnu/files/bsd/dev/redwood_cpufreq.c:1171-1183 (x1..x3 = 0, returns x0 only). Add `sip_call4(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t *x1_out)` — same asm, x1/x2 as inputs, return x0, store x1.

## 1. Stage A — read-only decode, `r1_dram=1` (and printed by every other r1_dram value)

Bases (vendor-mt6765-dts-dramc-excerpt.txt = vendor mt6765.dts:1582-1591):
- DRAMC_AO CHA 0x1022a000 (CHB 0x10232000), DRAMC_NAO CHA 0x1022c000, DDRPHY_AO CHA 0x10228000 (CHB 0x10230000), DDRPHY_NAO CHA 0x1022e000. Map 0x2000 each.

Decode (vendor-mt6765-mtk_dramc.c:1127-1194, 1211-1215):
```
shu    = (rd(0x1022a000 + 0xe4) >> 1) & 3;            /* SHUSTATUS: 0 HPM, 1 LPM, 2 ULPM */
idx    = rd(0x10228000 + 0x510) >> 31;                 /* which DDRPHY PLL is active (CHA only) */
pcw    = rd(0x10228000 + (idx ? 0xd94 : 0xd9c) + 0x500*shu) >> 16;
pp     = rd(0x10228000 + (idx ? 0xda0 : 0xda8) + 0x500*shu);
prediv = (pp >> 18) & 3;  posdiv = pp & 7;
ck4    = (rd(0x10228000 + 0xd18 + 0x500*shu) >> 27) & 1;
mts    = (((52 >> prediv) * (pcw >> 8)) >> posdiv) >> ck4;   /* 3198->3200, 3068, 2392->2400, 1599->1600, 1534 */
```
Also print the same formula for shu 0,1,2 with the same idx (guess: per-shuffle registers valid for the inactive shuffles) → `r1dram: shuffles SHU0 %u SHU1 %u SHU2 %u MT/s`.

DVFSRC (0x10012000, read only while spm_sel ungated: topckgen CLK_CFG_5 0x10000090 bit31 clear):
- +0x00 BASIC_CONTROL (bit0 = enabled, vendor-mtk-dvfsrc.c:471-474; bit15 = force enable)
- +0x04 SW_REQ: DRAM opp [1:0], VCORE opp [3:2] (haretic :31-32, 465-476; vendor mt6761_set_dram_level/vcore_level :520-537)
- +0x08 SW_REQ2: same fields (haretic regulator votes VCORE here; vendor calls it EXT_SW_REQ)
- +0x48 VCORE_REQUEST (VSCP [31:30]), +0x16c SW_BW, +0x160 SW_EXT_BW
- +0xdc LEVEL: [15:0] target (0 = idle), [31:16] current, one-hot; bit places {0,1,3,4,8,9} = levels 0..5 (haretic :794-817)
- +0x300 TARGET_FORCE (one-hot force, vendor :564-590 — do not use)
- level table: level i = {vcore, dram}: {0,0},{1,0},{2,0},{2,1},{3,1},{3,2}; dram opp 0/1/2 = 1534/2392/3200; VCORE opp 0,1 = 650 mV, 2 = 700 mV, 3 = 800 mV (haretic-mtk-dvfsrc-regulator.c:107-112)

SPM (0x10006000; vendor-mt6765-mtk_spm_reg.h): SPM_SW_RSV_9 +0x658 (low 16 bits = SPM DVFS level, vendor spm_get_dvfs_level), SPM_DVFS_EVENT_STA +0x69c, SPM_DVFS_LEVEL +0x6a4, SPM_DVFS_STA +0x1c8, SPM_DVFS_CON1_STA +0x704, PCM_FSM_STA +0x178.

Log:
`r1dram: now SHU%u pll%u pcw %#x pre %u pos %u ck4 %u = %u MT/s | CTRL %#x LEVEL %#x SW_REQ %#x SW_REQ2 %#x RSV9 %#x | SPMFW %ld VCORE %u uV`

Pass: rate ∈ {1534, 2400, 3200}; SHU0 decodes to 3200 (haretic's spmfw idx 1 = pcm_allinone_lp4_3200, dts 989-996). If SHU0 ≠ 3200: B/C forbidden (wrong SPM firmware).

Optional cross-check from stock Android (if it has the vendor dramc driver): `cat /sys/bus/platform/drivers/*dramc*/read_dram_data_rate` (guess on path; attr at mtk_dramc.c:1297-1310).

## 2. Stage B — start, `r1_dram=2`

Refuse if spm_sel gated or stage A failed. Steps:
1. `sip_call(0xC2000221)` ≥ 1 (our logs: 1 = same state Linux starts from).
2. `sip_call4(0xC200022A, 0, 1)` — spmfw idx 1.
3. `sip_call4(0xC2000506, 0, 0, &type)` INIT; x0 must be 0. Else `r1dram: INIT failed %ld, nothing started`, stop.
4. **Pre-vote current state (ours, not haretic's):** SW_REQ = `(old & ~0xF) | dram_opp | vcore_opp << 2`; dram_opp from stage-A rate (1534→0, 2400→1, 3200→2), vcore_opp from VCORE (≥800000→3, ≥700000→2, else 0). Reason: START with no request goes to level 0 = 1534 MT/s / 0.65 V (haretic-mtk-dvfsrc-devfreq.c:83-92), undervolting what LK left on VCORE (GPU at 460 MHz, display, ISP).
5. `sip_call4(0xC2000506, 1, 0)` START; need (x0 & 1) == 0.
6. `delay(1)`; poll LEVEL[15:0] == 0 ≤ 10 ms; then poll LEVEL[31:16] == expected one-hot bit ≤ 10 ms (haretic :616-645; MT6765 10 ms at :820-827, commit 9435277d98 "took over 1 ms from the lowest"). LEVEL[31:16] reads 0 transiently mid-switch: not a failure.
7. Read SW_REQ back; if START cleared it (unknown), rewrite at once.
8. Log once: CTRL +0x0, 0x0c, 0x14, 0x18, 0x48, 0x84, 0xd8, 0xe0..0xfc, 0x300, 0x604. Vendor GKI writes after INIT (vendor-mtk-dvfsrc.c ~1236, dvfsrc_mt6765_setting_0): 0xC=0x00240009, 0x14=0x09000000, 0x18=0x003E362C, 0x24=0x33, 0x28=0x4C, 0x3C=7, 0x40=0x38, 0x44=0x80C0, 0x78=0x80C0, 0x48=0x000C0000, 0x50=0x36, 0x84=0x20000000, 0xD8=0x14, 0x9C=3, 0xE0=0x00010000, 0xE4=0x00020101, 0xE8=0x01020012, 0xEC=0x02120112, 0xF0=0x00230013, 0xF4=0x01230113, 0xF8=0x02230213, 0xFC=0x03230323, 0x300=0x20000000, 0x604=0xC, 0x180=0x407F, 0x0=0x407B, 0x0=0x17B, 0x300=0. Haretic does not write this and works, so START presumably does it in ATF (guess). If CTRL bit0 is 0 after START, compare against this table; do not write it blind.

Log: `r1dram: START x0 %#lx SPMFW %ld CTRL %#x LEVEL %#x SW_REQ %#x`, `r1dram: regs …`, then `r1dram: now …` again.
Pass: SPMFW 2, CTRL bit0 set (guess), LEVEL one-hot, rate and VCORE unchanged.

Ordering: in `r1_init` (redwood_cpufreq.c ~1484-1539) before `r1_cpufreq` (~1517), so a hang comes before anything is raised. CPU ≥1750 MHz needing DRAM 3200 (haretic dts ~283-347 required-opps) is bandwidth, not electrical. `r1_gpu` (redwood_pvr.c:728-757) then sees DVFSRC running and votes SW_REQ2 VCORE; max of SW_REQ/SW_REQ2 wins, no conflict.

## 3. Stage C — level, `r1_dram=3200`

Stage B, then if current ≠ level 5: SW_REQ = `(v & ~0xF) | 0xE` (dram 2, vcore 3; haretic :465-476), delay 1 µs, same idle + current polls (bit 9). Existing log line kept: `r1dram: SW_REQ %#x -> %#x, LEVEL %#x = %u MT/s`. On timeout keep the request and log; do NOT put SW_REQ back (existing code at redwood_cpufreq.c:1277-1280 does — change it; a revert is a second shuffle).

Policy: fixed highest-level vote, never floating. Haretic floats via devfreq/passive governor, which needs every VCORE consumer to vote; we do not yet. Append LEVEL to the 10 s `r1therm` line.

## 4. Failure handling and exit

- Hang: loader watcher core feeds TOPRGU every 100 ms (mtkloader/mtkloader.c:1986-1998); TOPRGU armed 30 s, DDR_RESERVE|EN (:2593-2601). DRAM wedged mid-shuffle also stalls the watcher (runs from DRAM) → TOPRGU (32 kHz, independent of SPM) resets → Android. Recovery holds. Risk (guess): DDR-reserve exit mid-shuffle may make preloader do a full DRAM init — ramoops/log lost, Android still boots.
- Orderly exit: `reset_board` (mtkloader.c:1046-1056) is a 1 s TOPRGU DDR-reserve reset. TOPRGU resets the SPM; LK reloads spmfw (SPMFW=1 at our start fits). No stop SMC exists or is needed.
- Change `redwood_r1_dram_restore()` (redwood_cpufreq.c:1284-1292; called at loader_return.c:393): if DVFSRC was started, do not write the old SW_REQ (0) back — that drops DRAM to 1534 right before the DDR-reserve reset. Keep the vote (or write the stage-A value). `redwood_r1_gpu_restore` (loader_return.c:394) is harmless while SW_REQ holds VCORE 3.

## Test plan (one stage per run; unattended OK — failure ends in Android ≤30 s)

- A: `r1_dram=1 membench=1`, then `r1_dram=1 membench=2`.
- B: `r1_dram=2 membench=1` / `=2`. Fail = no `r1dram: START` line, an Android reset without the orderly-end banner, or `now` changed.
- C: `r1_dram=3200 membench=1` / `=2`. Pass = `LEVEL 0x2000000 = 3200 MT/s`.
- `r1_init` benches only on `membench==1` (redwood_cpufreq.c:1536); the membench=2 (bench_all thread) call is being added there.
- Compare `membench: on core …` and `membench: all N at once, together: …`. Already 3200 in A → same within noise. 1534 in A → expect ~1.5-2× reads (guess).

## 5. Expected gain

- A shows 3200: no speed gain. B/C only enable a later stage D (screen-off/idle drop to 1534, ~40 mA, and SPM suspend), which first needs VCORE votes from display, GPU, camera. B is still worthwhile groundwork (harmless with the pre-vote); C is a no-op.
- A shows < 3200: C is the real performance win.
