# Cellular Stage 2: from HS1 to the first AT exchange (spec)

Written 2026-11-17. Analysis only: nothing was built, flashed or committed. Follows docs/cellular-plan.md (Stages 0-1 are done, HANDOFF Updates 19-21).

Sources:
- **[K]** haretic/android-kernel-mt6765-rabbitr1 @ main, drivers/staging/mtk-ccci (eccci, MD_GENERATION 6293).
  - Short names below: ccif.c = eccci/hif/ccci_hif_ccif.c, modem.c = eccci/ccci_modem.c.
  - Cross-checked against MediaTek's 4.14 mt6765 `ccci_platform.c` (public tree Hadenix/kernel_umidigi_f2_mt6771_4.14).
- **[U]** Static reverse engineering of the stock blobs in xnu-deps/build/modem-blobs. ccci_mdinit still has its function names.
- **[S]** The live stock r1 (Android, read-only adb, 2026-11-17 18:06):
  - `/proc/ccci_dump`: the kernel's own CCCI boot log, with a hexdump of the runtime data it sent;
  - logcat (kernel + radio buffers);
  - /sys/kernel/ccci;
  - /proc/device-tree;
  - getprop;
  - `ls` of the NV partitions.

Marking: **(guess)** = inferred, not read in code or seen on the device. Everything is little-endian. MD-view addresses are what the modem sees. "nc smem" = AP 0x78000000, MD 0x40000000, 0x170000 bytes. "c smem" (CCB) = AP 0x7c000000, MD 0x46000000, 0x1600000 bytes.

---

## 0. What the stock device showed (the anchor for everything below)

From `/proc/ccci_dump` and logcat of the current Android boot (modem started at ~55.3 s):

```
ccci_mdinit: status init; open+close ccci_fs; CCCI_IOC_SET_MD_IMG_EXIST; open ccci_ipc_5; open ccci_fs; status reset
ccci_mdinit: 3x open ccci_ccb_ctrl ("ccb_configs_len: 20", users dhl/md_monitor/meta, "remap control addr:0x78018000 len:4096")
ccci_mdinit: CCCI_IOC_GET_MD_TYPE: 12; status bootup; "MD start ioctl called"
fsm: md_state 0->2; ccif_late_init, md_ccif_sram_reset, md_ccif_switch_ringbuf/reset_queue x2, ccif_start
md:  ...power on as our Stage 1..., "modem started 0"
ctrl: "control message 0x0,0x5555FFFF"            = HS1 (+53 ms after release)
fsm: 2->3; prepare_runtime_data; bootmode 0x0 boottype 0x1; sbp=0x0,wmid[0x3f]; c2k_flags 0x4;
     "ccb data size (include dsp raw): 178000"; "new version md use multi-MPU"; hexdump "AP runtime data" (§1.4);
     md_boot_stats 0x54430007 0x53320000; "send runtime data 0"
rpc_k: md_eint queries (src_pin/interrupts/dedicated for SIM1, SIM2)       (+130 ms)
ctrl: "control message 0x0,0x0"                    = HS2 (+404 ms after the reply)
fsm: 3->4 (READY); mdinit status ready
gsm0710muxd: opens ttyC0, reads "+EIND: 64", then "+EIND: 128", sends AT / ATZ / ATE0 (each "OK"),
     then AT+CMUX -> "+CMUX: READY" "OK"; mux up                            (+1.2 s after HS2)
```

Other stock facts:
- Steady state:
  - FS on CCIF queue 4 (start bit 0x10), channels 14 rx / 15 tx;
  - AT on queue 5 (0x20), channels 10 rx / 12 tx;
  - agps / ipc / rpc on queue 1 (0x2).
- The modem keeps writing NV files while running: logcat `ccci_fsd(1): O: X://NA77_005, flag 0x21010400, ret 1` (create + commit, on protect_f and protect_s).
- The FS opcode counts in the history ring (57 s .. 4 min) are below. The boot-time FS traffic had already scrolled out of the ring.

| Op | Count |
|---|---|
| 0x1001 OPEN | 75 |
| 0x1002 SEEK | 77 |
| 0x1003 READ | 6 |
| 0x1004 WRITE | 90 |
| 0x1005 CLOSE | 76 |
| 0x1009 GETFILESIZE | 58 |
| 0x1022 CMPT_READ | 157 |

---

## 1. Kernel protocol (from the open eccci, checked against [S])

### 1.1 ccci_header (16 B)

| Offset | Field |
|---|---|
| +0 | u32 data[0] |
| +4 | u32 data[1] |
| +8 | u16 channel |
| +10 | u16 seq_num:15, assert_bit:1 (bit 15) |
| +12 | u32 reserved |

- Data packets: data[0] = 0, data[1] = total length including the header.
  - Exception: on FS, data[0] bit 31 = "more fragments".
- Kernel messages: data[0] = 0xFFFFFFFF (CCCI_MAGIC_NUM), data[1] = message id, reserved = parameter.

### 1.2 HS1 (already seen by Stage 1)

- MD raises CCIF channel 15 (D2H_SRAM).
- CCIF SRAM (AP CCIF CHDATA = 0x10209100):
  - +0x00: ccci_header {0xFFFFFFFF, 0 = MD_INIT_START_BOOT, channel 0, reserved 0x5555FFFF = MD_INIT_CHK_ID};
  - +0x10: md_query_ap_feature (224 B): head 0x49434343, u8 feature_set[64], tail 0x49434343, reserved[152].
- Feature byte = support mask in the low nibble, version in the high nibble (ccci_modem.h:210).
  - Support values: 0 NOT_EXIST, 1 NOT_SUPPORT, 2 MUST, 3 OPTIONAL, 4 BACKWARD_COMPAT.
  - Our HS1: byte0 = 2 (MUST v0); 1..36 = 3 (OPTIONAL **v0**, not "version 3" as Update 21 wrote); rest 0.
  - redwood_md.c's `(feat & 0xf) == 2` test is correct.

### 1.3 The reply: two parts

Source: `md_cd_send_runtime_data_v2` (modem_sys1.c:822), `ccif_hif_fill_rt_header` (ccif.c:2123), `config_ap_runtime_data_v2_1` (modem_sys1.c:783).

**Part A: the feature list in smem**
- Location: RAW_RUNTIME_DATA = nc smem **+0xE800** (AP 0x7800E800, MD 0x4000E800). The AP part is at most 0x800 bytes; the MD part follows at +0xF000.
- Format: 64 entries, one per feature id 0..63, back to back, no padding:
  - `{u8 feature_id; u8 support_info; u8 rsv[2] = 0; u32 data_len; u8 data[data_len]}`
- Contents: §1.4. r1 total = 0x310 bytes.

**Part B: up_header and `ap_query_md_feature_v2_1` in the CCIF SRAM**
- up_header at CHDATA **+0xF0** (0x102091F0), written as four u32:
  - data[0] = 0
  - data[1] = 0xAC (16 + 156)
  - word 2 = 1 (channel CCCI_CONTROL_TX 1, seq 0, assert 0)
  - reserved = 0x5555FFFF
- `ap_query_md_feature_v2_1` at CHDATA **+0x100** (0x10209200), 156 B. Zero it, then fill:

| Offset | Field | r1 value |
|---|---|---|
| +0x00 | head_pattern | 0x43434349 |
| +0x04 | u8 feature_set[64] | all 0 except [1] = 0x10 (as stock: mask 0, version 1) |
| +0x44 | share_memory_support | 2 (MULTI_MD_MPU_SUPPORT, because feature 27 is MUST) |
| +0x48 | ap_runtime_data_addr | 0x4000E800 |
| +0x4C | ap_runtime_data_size | 0x800 |
| +0x50 | md_runtime_data_addr | 0x4000F000 |
| +0x54 | md_runtime_data_size | 0x800 |
| +0x58 | noncached_mpu_start | 0x40000000 |
| +0x5C | noncached_mpu_total | 0x170000 |
| +0x60 | cached_mpu_start | 0x46000000 |
| +0x64 | cached_mpu_total | 0x1600000 |
| +0x68 | u32 reserve[12] | 0 |
| +0x98 | tail_pattern | 0x43434349 |

- This does not collide with the SRAM tail (+0x1F4: 0x7274626E, 0x40000800, 0x2800) that Stage 1 writes.

**DBM sub-region** (`md_cd_smem_sub_region_init`, modem_sys1.c:722), written before the send:
- nc smem +0xFF50 (MD 0x4000FF50, 0xB0 bytes):
  - 0x44444444 0x44444444;
  - 40 × 0x00000000 (PBM enabled);
  - 0x44444444 0x44444444.

**Raise** (`md_ccif_send`, ccif.c:1095):
1. If AP CCIF BUSY (+0x04) has bit 15 set, stock silently skips. We log it and retry once after 10 ms.
2. Otherwise write BUSY = 1<<15, then TCHNUM (+0x0C) = 15.

No ack exists. HS2 is the only sign the modem read the reply (stock: 404 ms).

### 1.4 Runtime-data contents for r1

This byte-matches the stock dump; only the seed and time vary. Encoding: word0 = id | support<<8 (2 = MUST, 1 = NOT_SUPPORT, 0 = NOT_EXIST), then data_len, then the data words. Addresses are MD view except id 13.

| id | name | sup | len | data |
|---|---|---|---|---|
| 0 | BOOT_INFO | 2 | 16 | 0, 0, 0, 0 (boot_channel 0 = CONTROL_RX, booting_start_id = mdlog<<8 \| bootmode = 0 NORMAL, 0, 0) |
| 1 | EXCEPTION_SHARE_MEMORY | 2 | 8 | 0x40000000, 0x10000 |
| 2 | CCIF_SHARE_MEMORY | 2 | 8 | 0x40028000, 0xB0140 (= used ring bytes, ccif.c:1758; = Stage 1's "0xb0140") |
| 3 | SMART_LOGGING | 1 | 0 | |
| 4 | MD1MD3 | 1 | 0 | |
| 5 | MISC_HIF_DMA_REMAP | 2 | 16 | 0 0 0 0 |
| 6 | MISC_RTC_32K_LESS | 2 | 16 | 0 0 0 0 |
| 7 | MISC_RANDOM_SEED | 2 | 16 | random u32, 0, 0, 0 |
| 8 | MISC_GPS_COCLOCK | 2 | 16 | 0 0 0 0 |
| 9 | MISC_SBP_ID | 2 | 16 | sbp 0, rat 0x3F, 0, 0 |
| 10 | MISC_CCCI | 2 | 16 | 3 (bit0 seq check, bit1 poll status), 0, 0, 0 |
| 11 | MISC_CLIB_TIME | 2 | 16 | UTC secs lo, hi, tz 0, dst 0 |
| 12 | MISC_C2K | 2 | 16 | 4, 0, 0, 0 |
| 13 | MD_IMAGE_START_MEMORY | 2 | 8 | 0x8E000000 (**AP phys**), 0x0163F500 (md1_chk img_size, +176) |
| 14 | CCISM_SHARE_MEMORY (SCP) | 2 | 8 | 0x40010000, 0x8000 |
| 15 | CCB_SHARE_MEMORY | 2 | 16 | 0x40018000, 0x1000, 0x46000000, 0x178000 |
| 16 | DHL_RAW | 2 | 8 | 0x46200000, 0x01400000 |
| 17 | DT_NETD | 2 | 8 | 0x40019000, 0x1000 |
| 18 | DT_USB | 2 | 8 | 0x4001A000, 0x1000 |
| 19 | EE_AFTER_EPOF | 2 | 16 | 0 0 0 0 |
| 20 | AP_CCMNI_MTU | 2 | 4 | 0xDF0 |
| 21-24 | CUSTOMER_VAL, FAST_HEADER, -, C2K_MEID | 1 | 0 | |
| 25 | LWA | 2 | 8 | 0x40170000, 0 |
| 26 | AUDIO_RAW | 2 | 8 | 0x4001B000, 0xD000 |
| 27 | MULTI_MD_MPU | 2 | 0 | |
| 28 | CCISM_SHARE_MEMORY_EXP | 2 | 8 | 0x400DC400, 0x1E400 |
| 29 | MD_PHY_CAPTURE | 1 | 0 | |
| 30 | MD_CONSYS | 2 | 8 | 0x46000000, 0x01400000 |
| 31 | MD_USIP | 1 | 0 | |
| 32 | MD_MTEE_ENABLE | 2 | 4 | 0 |
| 33, 34 | MD_POS, UDC_RAW | 1 | 0 | |
| 35-63 | | 0 | 0 | |

Rules (`ccci_md_prepare_runtime_data`, modem.c:2002):
- If the MD marks an id MUST (2) and the AP cannot do it, the boot fails. Only id 0 is MUST in our HS1.
- For OPTIONAL ids, the AP may answer NOT_SUPPORT (1). The stock values above are the safe choice. Do not trim them in 2a.
- A static table is enough for 2a. The algorithm (OPTIONAL+ver match → MUST) gives the same result for our HS1 bytes.

### 1.5 After the reply

**HS2**
- The MD writes a bare 16-byte ccci_header into the **queue-0 RX ring** and raises CCIF bit 0:
  - data[1] = 0 (MD_NORMAL_BOOT);
  - channel 0;
  - reserved 0 (≠ 0x5555FFFF; stock "control message 0x0,0x0").
- The AP sends nothing back. State → READY.
- Budget: one 30 s BOOT_TIMEOUT covers HS1 + HS2. The FSM holds the counter while FS requests are in flight (`fs_ongoing`). This shows the MD does FS work before HS2 (guess: it reads NVRAM before declaring ready).

**Between HS1 and HS2**
- FS (ch 14/15, queue 4) and kernel RPC (ch 32/33, queue 1) already run. Everything else waits for READY.

**Other control-channel messages** (ch 0, keyed on data[1]):

| data[1] | Message | AP action |
|---|---|---|
| 0x4 | MD_EX (reserved 0x45584350) | exception path |
| 0x6 | MD_EX_REC_OK (reserved 0x45524543) | exception path |
| 0x8 | MD_EX_PASS | exception path |
| 0x5 | CCCI_DRV_VER_ERROR | exception path |
| 0x201 / 0x202 / 0x205 / 0x207 / 0x20A | C2K | ignore |

**Exception handshake** (on CCIF bit 16 D2H_EXCEPTION_INIT):
1. AP: H2D_EXCEPTION_ACK (16).
2. MD: 17, then 18.
3. AP: switch to the exception rings and send H2D_EXCEPTION_CLEARQ_ACK (17).
4. MD: 19.
5. MD_EX / REC_OK / PASS then arrive on exception queue 6.

Stage 2 only logs bit 16 and dumps the MDCCCI_DBG / MDSS_DBG regions (+0x0, +0x800). It does not implement the handshake.

**System messages** (ch 2 rx / 3 tx, queue 0):
- Only 0x105 MD_GET_BATTERY_INFO gets an answer: {0xFFFFFFFF, 0x105, ch 3, reserved = battery mV}. Stock haretic answers 0.
- 0x114 / 0x116 are test messages, echoed as 0x115 / 0x117.
- Everything else (SIM_TYPE 0x107, TX_POWER 0x102, RF_TEMP 0x103/0x104, SWTP 0x110, RF_HOPPING 0x121, WDT 0x1000, ...) is callback-only. No reply is needed.

**Status poller**: only runs when mdlogger is open. Skip it.

### 1.6 CCIF rings and mechanics

Stage 1's layout is confirmed byte-exact against `ccci_create_ringbuf`. There is no flow-control block (FLOW_CTRL_ENABLE is off).

Queue blocks start at nc smem +0x28000 (CCISM_MCU), back to back. Block length = 40 + rx + tx:

| Offset | Content | Writer |
|---|---|---|
| +0x00 | 0xEE0000EE ×2 | |
| +0x08 | rx.read | AP |
| +0x0C | rx.write | MD |
| +0x10 | rx.len | |
| +0x14 | tx.read | MD |
| +0x18 | tx.write | AP |
| +0x1C | tx.len | |
| +0x20 | rx buf[rx.len], then tx buf[tx.len] | |
| +len−8 | 0xFF0000FF ×2 | |

Queue offsets inside CCISM_MCU (cumulative, from the sizes):

| Queue | Offset | Size (40 + RX K + TX K) |
|---|---|---|
| q0 | +0x00000 | 0x28 + 80K + 128K |
| q1 | +0x34028 | 0x28 + 80K + 40K |
| q2 | +0x52050 | 0x28 + 40K + 8K |
| q3 | +0x5E078 | 0x28 + 80K + 40K |
| q4 | +0x7C0A0 | 0x28 + 20K + 20K |
| q5 | +0x860C8 | 0x28 + 20K + 20K |
| q6 | +0x900F0 | 0x28 + 64K + 64K |
| q7 | +0xB0118 | 0x28 |
| end | +0xB0140 | (= the 0xB0140 in feature 2) |

The 2a code should compute the offsets from the size tables, not hard-code them; ringbufs_init already walks them.

**Ring packet** (positions mod length; it wraps byte-wise):
1. 0xAABBAABB, then u32 L (L = ccci packet length including its 16-byte header).
2. L data bytes, padded to 8.
3. 0xCCDDEEFF, 0xCCDDEEFF.

**Ring operations:**
- **readable:** (write − read) mod len ≥ 32, the header magic matches, and the footer words are at the expected place. Then read L bytes from read + 8.
- **Consume:** read = round_up(read + L + 16, 8) mod len. Write it back after copying.
- **AP TX** (`md_ccif_op_send_skb`):
  1. Check `writeable(L)`: need = round_up(L + 16, 8) < free, where free = (r == w) ? len − 1 : (r < w ? len − w − 1 + r : r − w − 1).
  2. Write the packet at tx.write.
  3. dsb, then update tx.write.
  4. If BUSY bit q is clear: BUSY = 1<<q, TCHNUM = q. If it is set, do not raise; the MD sees the data anyway.

**RX interrupt mechanics**: RCHNUM → ACK = RCHNUM & 0xFFFF first, then drain every queue whose bit was set. Also poll all rings: stock drains rings without a bit, and so should our poll loop.

**Sequence numbers**:
- TX: per channel, from 0, post-increment; header.assert_bit = 1.
  - Exception: on CCCI_FS_TX (15) and CCCI_RPC_TX (33), assert_bit = 0 unless the state is BOOT_WAITING_FOR_HS2.
- RX check: if assert && last && ((seq − last) & 0x7FFF) != 1 → stock forces an assert. We log only.
- Stock FS replies: the request on ch14 had seq 0x38F and the reply on ch15 had seq 0x38F, but that is two independent counters that happen to agree.

### 1.7 Channel table (port_cfg.c, CCIF, MD1)

| Port | tx ch | rx ch | queue | Notes |
|---|---|---|---|---|
| control (kernel) | 1 | 0 | 0 | HS1, HS2, MD_EX |
| system (kernel) | 3 | 2 | 0 | |
| ccci_fs | 15 | 14 | 4 (exp 1) | needed from HS1 on |
| ccci_rpc (kernel + user) | 33 | 32 | 1 (exp 1) | needed from HS1 on |
| ttyC0 = CCCI_UART2 (AT / mux) | 12 | 10 | 5 | |
| ccci_md_log_ctrl = UART1 | 8 | 6 | 1 | |
| ttyC1 = MD_LOG | 43 | 42 | 2 | |
| ttyC2 IPC_UART | 40 | 38 | 1 | |
| ccci_ipc_* | 36 | 34 | 1 | task id inside the payload |
| status poll | 68 | 67 | 0 | |
| ims* | 52-59 | | 6 | |
| ccci_c2k_agps | 168 | 167 | 1 | |
| ccci_bip | 176 | 175 | 1 | |
| ccci_imsm | 170 | 169 | 1 | |
| ccmni (data) | | | | CLDMA, Stage 3 |

For Stage 2 we need only: control, system, FS, RPC, ttyC0. Anything else that arrives is logged (channel, length) and dropped.

### 1.8 Kernel RPC (port_rpc.c)

Neither stock nor haretic boot waits for every op. The MD asked these during the stock boot: the md_eint queries at +130 ms and GET md_product_name_model_id.

**Request** (ch 32, queue 1): ccci_header with reserved = buffer index (≤ 2), then:
- u32 op_id;
- u32 para_num (≤ 6);
- para_num × {u32 len; data; pad to 4}.

**Reply** (ch 33, queue 1): same buffer, with:
- op_id |= 0xFFFF0000;
- new para_num and params;
- data[1] = new total length;
- data[0] and reserved echoed.

| op | name | r1 answer |
|---|---|---|
| 0x4002 | GET_GPIO_NUM (name) | 2 × {4, pin}: GPIO_SIM1_HOT_PLUG 47, SIM2_HOT_PLUG 46, SIM1_SIO 40, SIM1_SRST 39, SIM1_SCLK 38, SIM2_SCLK 37, SIM2_SRST 36, SIM2_SIO 35 (live DT node `gpio`, compatible gpio_usage_mapping). Any other name: 2 × {4, −5} |
| 0x4005 | GET_EINT_ATTR (name, …, u32 type) | Ok: {4, 0}, {4, value}. Values below this table. Unknown name: 2 × {4, −11} |
| 0x400E | DTSI_QUERY ({u8 req, u8 idx, char name[64]}) | {4, 0}, {u32 0x0F0F0F0F, char[64] filled with 0x0F}. Both md_attr_node properties on r1 are 0x0F0F0F0F; stock logs that value |
| 0x4006 / 0x4007 | GPIO_VAL / ADC_VAL | 2 × {4, level}; ADC: −1 |
| 0x4003 | GET_ADC_NUM | 2 × {4, −5} |
| 0x4001 | GET_TDD_EINT_NUM | 2 × {4, −5} |
| 0x4004 | EMI_CLK_TYPE | 3 × {4, −5} |
| 0x4009 | GET_GPIO_ADC (161 B or 425 B struct) | {4, 0} + output struct (32 or 88 B) filled with 0x0F bytes except the found pins |
| 0x4012 | TRNG | {4, 0}, {4, random} |
| 0x4013 | QUERY_CARD_TYPE | echo the inputs with op \| 0xFFFF0000 |
| 0x4321 | IT_OP | {4, 1}, {4, 0xA5A5} |
| 0x400F | QUERY_AP_SYS_PROPERTY | user space in stock (ccci_rpcd: `property_get(key)`). Answer with an empty value (guess: {4, 0}, {0-length string}), or {4, −1} |
| 0x4010-0x4015 | SAR index, DRDI copy, mdcap AAC, TC1 | user space in stock. Reply {4, −1} (guess). See unknown U5 |
| anything else | | 1 × {4, −1} |

EINT attribute values for 0x4005, from the live DT nodes md1_sim1/2_hot_plug_eint:

| Attribute (type) | SIM1 | SIM2 |
|---|---|---|
| src_pin (6) | 1 | 2 |
| interrupts (0, NUM) | 0 | 1 |
| debounce (1) | 10000 | 10000 |
| dedicated (5) | 0 | 0 |
| sockettype (4) | 0 | 0 |

- The query name is "MD1_SIM1_HOT_PLUG_EINT" (stock asks for SIM1 and SIM2).
- The DT stores some cells as two u32; stock reads the second cell, which gives the "sim_info: 1, 1" log.
- POLA (2) and SENS (3): not in the DT, so 2 × {4, −12}.

### 1.9 CCB control page (resolved)

Source: [U] libccci_util `ccci_ccb_init_users` plus [K] and vendor `ccb_configs`.

- If the page is missing, the MD asserts in ccci_shm_bm.c before HS2 (haretic's note).
- stock mdinit fills it before DO_START_MD. The kernel never writes it.
- Page: nc smem +0x18000 (AP 0x78018000), 4 KB. It is not cleared at first boot, so we zero it ourselves.
- Layout: one 64-byte block per `ccb_configs` row, at **row × 64**. Each block is zeroed, then:

| Offset in block | Value |
|---|---|
| +0x14 | dl_page_size |
| +0x18 | dl_buff_size |
| +0x1C | 0xEEFF0011 |
| +0x34 | ul_page_size |
| +0x38 | ul_buff_size |
| +0x3C | 0xEEFF0011 |

All other words stay 0; the MD owns the indices and the front pattern.

Rows (r1 = haretic's table; the totals match stock's logged 0x70000 / 0x8000 / 0x100000 exactly):

| Row | User | dl_page | ul_page | dl_buff | ul_buff |
|---|---|---|---|---|---|
| 0 | DHL ctrl | 1024 | 1024 | 32768 | 32768 |
| 1 | DHL exception | 20480 | 128 | 122880 | 128 |
| 2 | DHL PS | 512 | 1024 | 36864 | 32768 |
| 3 | DHL HWLOGGER1 | 512 | 128 | 32768 | 128 |
| 4 | DHL HWLOGGER2 | 512 | 128 | 32768 | 128 |
| 5 | DHL HWLOGGER3 | 512 | 128 | 131072 | 128 |
| 6-16 | DHL dummy | 128 | 128 | 128 | 128 |
| 17 | DHL padding | 128 | 128 | 640 | 128 |
| 18 | MD_MONITOR | 512 | 1024 | 16384 | 16384 |
| 19 | META | 66560 (65K) | 64512 (63K) | 532480 (520K) | 516096 (504K) |

- DHL sum = 458752 = 0x70000. MDM = 0x8000 at 0x46070000. META = 0x100000 at 0x46078000.
- MediaTek's public 4.14 mt6765 table has META ul 65K × 8, which would give 0x104000. Stock logs 0x100000, so the r1 table is haretic's (63K) **(guess: haretic copied it from the stock r1 kernel)**.
- Feature 15 sum = 0x178000 ✓.

### 1.10 Ioctls ccci_mdinit drives (for reference; our kernel does the equivalent itself)

Order: [U] matches [S].

1. **SET_MD_IMG_EXIST** 0x4004431D, u32[4] = 0.
2. **CCB setup** (§1.9), on /dev/ccci_ccb_ctrl:
   - GET_CCB_CONFIG_LENGTH 0x8004433F;
   - GET_CCB_CONFIG 0xC0184340 × 20;
   - CCB_CTRL_INFO 0xC0104347;
   - mmap and fill.
3. **SET_BOOT_DATA** 0x4040432F, u32[16]. Only [0] and [1] feed the runtime data; the rest only feed the kernel's RAT/WM bookkeeping.

| Word | Value |
|---|---|
| [0] | mdlog mode, 0 on user/userdebug unless memdump_wait |
| [1] | sbp code: 0 here (`vendor.mtk.md.sbp` = 0) |
| [2] | dbg_dump, 0xFFFFFFFF if unset |
| [3] | operator subid 0 |
| [4] | c2k cap check 0 |
| [5..10] | char[24] RAT config "C/Lf/Lt/W/T/G" (ro.vendor.mtk_protocol1_rat_config) → rat bits 0x3F |
| [11] | sysenv md_type |
| [12..15] | 0 |

4. GET_MD_TYPE 0x8004431F → 12 (`vendor.ril.active.md`).
5. **DO_START_MD** 0x430D.
6. Read /dev/ccci_md1_sta events (0x38 B) until 0xA (READY) or 0xB (EXCEPTION).
7. GET_MD_STATE 0x80044301 == 2.

Notes:
- There is no SET_MD_BOOT_MODE call. Boot mode comes from the LK atag: 0 = normal, never META.
- The md_query/runtime path is entirely kernel.
- mdinit's only property wait is `vendor.service.nvram_restore` (Ready / Pre_Ready, ~14 s, then it continues) while choosing the SBP code.

---

## 2. Closed userland behaviour

Full detail of the recovered behaviour is in §1.10 and below.

### 2.1 FS server ("ccci_fsd", a thread of ccci_mdinit)

- Mode: on r1, read()/write() packets on /dev/ccci_fs (/sys/kernel/ccci/version = 6).
- It waits for md_sta event 9 (HS1) before serving, so in our kernel the FS server must be live from HS1.

**Request packet** (one CCCI packet per read, ch 14):

| Offset | Field |
|---|---|
| +0x00 | data0: bit31 = MORE (more fragments follow); bit30 = kernel wakeup flag (strip it); other bits echoed |
| +0x04 | data1 = packet length including the header, ≤ 0x40DC |
| +0x08 | channel (& 0xFFFF == 14) \| seq<<16 |
| +0x0C | reserved = slot 0..4 (< 5); one reassembly buffer per slot |
| +0x10 | u32 op_id (0x1001..0x1025) |
| +0x14 | u32 num_args (1..6) |
| +0x18 | num_args × {u32 len (≠ 0); data; pad to 4} |

- Total payload ≤ 0x40C8.
- Fragments: the first (MORE = 1) is copied whole. Each continuation repeats the 16-byte header and the op word, and contributes its bytes from offset 0x14 on. The last has MORE = 0.

**Reply** (ch 15, queue 4):

| Offset | Field |
|---|---|
| +0x00 | header: data0 with MORE cleared, data1 = length, channel 15 + our seq, reserved = same slot |
| +0x10 | 0xFFFF0000 \| op |
| +0x14 | num_out |
| +0x18 | num_out × {u32 len; data; pad4} |

- The first out arg is always the s32 status.
- If the body after the op word exceeds 0xD80 bytes, split it. Each fragment = 16-byte header (data0 bit31 set on all but the last) + op word + ≤ 0xD80 bytes. This is the stock 3476 + 44 split.

**Ops** (in → out):

| Op | Name | In | Out |
|---|---|---|---|
| 0x1001 | OPEN | wname, u32 flags | handle (1..128) or err |
| 0x1002 | SEEK | h, s32 off, whence 0/1/2 | newpos or err; whence > 2 → −2 |
| 0x1003 | READ | h, len | st, nread, data |
| 0x1004 | WRITE | h, data, len (that order) | st, nwritten |
| 0x1005 | CLOSE | h | st; fsync if flags & 0x01030000 |
| 0x1006 | CLOSEALL | | |
| 0x1007 / 0x1008 | CREATEDIR / REMOVEDIR | wname | |
| 0x1009 | GETFILESIZE | h | st, size |
| 0x100A | GETFOLDERSIZE | | |
| 0x100B | RENAME | old, new | |
| 0x100C | MOVE | | |
| 0x100D | COUNT | | |
| 0x100E | GETDISKINFO | | st, struct[0x54] |
| 0x100F | DELETE | | |
| 0x1010 | GETATTRIBUTES | | |
| 0x1011 | OPENHINT | wname, flags, hint[8] | h, hint |
| 0x1012-0x1014 | FINDFIRST / FINDNEXT / FINDCLOSE | | |
| 0x1015 / 0x1016 | LOCKFAT / UNLOCKFAT | | 0 / 1 |
| 0x1017 | SHUTDOWN | | no args |
| 0x1018 | XDELETE | | |
| 0x1019 / 0x101C | CLEAR/SETDISKFLAG | | 0 |
| 0x101A | GETDRIVE | | 'Z' |
| 0x101B | GETCLUSTERSIZE | | |
| 0x101D-0x1020 | OTP ops | | |
| 0x1021 | RESTORE | wname | restore from the raw nvram binregion via libnvram |
| 0x1022 | CMPT_READ | wname, struct cmpt | 4 args: 8 B, 4 B, 4 B, data |
| 0x1023 | BIN_REGION_ACCESS | | −2 always |
| 0x1024 | CMPT_WRITE | | |
| 0x1025 | GETFILEDETAIL | | |

CMPT_READ (0x1022) is the most frequent op on the wire: open + size + seek + read + close in one request. The cmpt struct:

| Offset | Field |
|---|---|
| +0x00 | opid_map: bit0 open, bit1 size, bit2 seek, bit3 read, bit4 close |
| +0x0C | open flags |
| +0x14 | offset |
| +0x18 | whence |
| +0x20 | length |

The exact out-arg sizes are **(guess)**. 2c must check them against the transcript (§3.3).

**Names**
- UTF-16LE. Keep the low byte, map `\` → `/`, and reject `..`.
- The first two characters are the drive:

| Drive | Path | On the r1 |
|---|---|---|
| Z: | /mnt/vendor/nvdata/md | NVRAM/NVD_DATA, NVD_IMEI, CALIBRAT, ... |
| X: | /mnt/vendor/protect_f/md | |
| Y: | /mnt/vendor/protect_s/md | |
| W: | /vendor/firmware | DSP: dsp_<postfix>.bin, postfix "1_ulwctg_n" |
| V: | /mnt/vendor/nvdata/md_cmn | empty |
| U: | /data/vendor/mdlpm | |
| T: | /vendor/etc/mdota | |
| S: | /mnt/vendor/nvcfg | |
| R: | /vendor/etc/md | |
| Q: | /data/vendor_de/md | |

**Flags**
- 0x100 → read-only, else O_RDWR.
- 0x10000 → CREATE.
- 0x20000 → CREATE_ALWAYS (truncate).
- 0x10000000 → nonblock.
- 0x200 SHARED, 0x400 NO_DIR, 0x01000000 COMMITTED, 0x20000000 PROTECT only decide fsync.
- Stock examples: 0x700 = RO; 0x10400 = create; 0x21010400 = create + committed + protect.

**Errors**
- 0 OK, −2 PARAM, −5 TOO_MANY_FILES, −9 FILE_NOT_FOUND, −10 INVALID_HANDLE, −16 ACCESS_DENIED, −18 GENERAL_FAILURE, −19 PATH_NOT_FOUND, −49 PATH_OVER_LEN.
- A missing file returns −9. The MD then recreates it from defaults or calls RESTORE.
- **That is exactly what must never reach the real partitions**: all creates and writes go to the RAM overlay.

**Boot-critical** **(guess)**:
- The MD reads NVD_IMEI, CALIBRAT and NVD_DATA files and protect_f/protect_s before HS2.
- If calibration is missing it would "rebuild" defaults (RF unusable, harmless with RF off). If the IMEI is missing it would try RESTORE (0x1021).
- The 2c test serves real files, so neither path should trigger.

### 2.2 ccci_rpcd

- Serves RPC ops 0x400F-0x4015 on /dev/ccci_rpc, which the kernel port routes to user space:
  - QUERY_AP_SYS_PROPERTY;
  - SAR table index;
  - md1drdi image copy from md1img (the "AMMS DRDI" path);
  - mdcap AAC;
  - TC1.
- mdinit does not wait for it. An unanswered RPC may stall the MD task that sent it **(guess)**. Stage 2b replies to all of them; see U5.

### 2.3 AT channel and multiplexing

- Stock does not use MIPC: `ro.vendor.mtk_mipc_support` is empty and gsm0710muxd runs.
- muxd sequence on ttyC0 (ch 10 rx / 12 tx):
  1. Wait for the URC "\r\n+EIND: 128\r\n" (modem ready for AT). "+EIND: 64" comes first.
  2. "AT\r" → OK.
  3. "ATZ\r" → OK.
  4. "ATE0\r" → OK.
  5. "AT+CMUX=..." → "+CMUX: READY".
- So **plain AT works on ttyC0 before CMUX**. Stage 2d never sends CMUX.
- ttyC0 is a char port without a user header: each AT write is one CCCI packet {0, 16 + n, ch 12, 0} + text.
- mtkfusionrild opens no nodes itself; it sits behind muxd's /dev/radio/pttycmd*. Not needed in Stage 2.

### 2.4 nvram_daemon

- Restores AP NVRAM from the raw `nvram` partition when files are missing, creates the md/NVRAM directories, and sets `vendor.service.nvram_init` / `nvram_restore` = Ready.
- Not needed by the modem itself. Our FS server presents the directories.

---

## 3. Live transcript plan

### 3.1 Done now (read-only)

Raw outputs are in the session scratchpad, not copied into the repo.

**ccci device nodes**
- Major 504 for ports (ccci_fs 4, ttyC0 3, ttyC1 7, ccci_rpc 20, ccci_ccb_ctrl 154, ...).
- 251 for md*_sta, 505 for ccci_monitor.
- /dev/radio/pttycmd1..11 are muxd's ptys.

**/sys/kernel/ccci**
- boot "md1:4" (READY), kcfg "[ccci_drv_ver]:V2 [MTK_MD_CAP]:12", lk_md "LK load MD success!", md1_postfix `1_ulwctg_n`, version 6.
- `/proc/ccci_dump` is the gold mine: it holds the init/boot log and the runtime-data hexdump. History ring: 16 bytes of every packet on every channel, but it wraps within ~1 min.

**Props**
- gsm.version.baseband MOLY.LR12A.R3.MP.V230, vendor.mtk.md.sbp 0, vendor.ril.active.md 12, ro.vendor.mtk_protocol1_rat_config C/Lf/Lt/W/T/G, ro.vendor.md_apps.load_gencfg GEN93_USER, build userdebug.
- ro.vendor.mtk_mipc_support unset (muxd mode). vendor.service.nvram_init / nvram_restore = Ready.

**Live DT**
- gpio-usage-mapping and the EINT nodes in §1.8.
- md_attr_node rf_set_idx / product_name_model_id = 0x0F0F0F0F.
- mddriver mediatek,md_generation 6293, mdhif_type 3.

**NV tree** (names and sizes only, no contents)

Mounts:

| Mount | Device | Size | Used |
|---|---|---|---|
| nvdata | p6 | 53 MB | 3.3 MB |
| nvcfg | p5 | 24 MB | |
| protect_f | p9 | 3 MB | |
| protect_s | p10 | 5.6 MB | |

All four are ext4, mounted rw, commit=1. Raw partitions: nvram p14, proinfo p13.

390 files, 2.2 MB in all:

| Path | Files | Bytes |
|---|---|---|
| nvdata/md/NVRAM/CALIBRAT | 168 | 856262 |
| nvdata/md/NVRAM/NVD_DATA | 182 | 387222 |
| nvdata/md/NVRAM/NVD_IMEI | 4 (LD0B_001 384, FILELIST 36, NV01_000 96, NV0S_000 144) | 660 |
| nvdata/md/NVRAM/INFO_FILE | 1 | 1628 |
| nvdata/md/NVRAM/BACKUP and NVD_CORE | 0 (empty) | |
| nvdata/md3, md_cmn, media | 0 (empty) | |
| nvdata root: AllFile, AllMap, RestoreFlag | 3 | 891166 |
| nvdata/APCFG/APRDCL (incl. MD_SBP 26, FILE_VER 5846) | 7 | 6018 |
| nvdata/APCFG/APRDEB | 4 | 2550 |
| protect_f/md: LD40_001 19264, NA77_005 9488, ST33_004 2784, ST34_001 832, MT00A001/MT00B001 232, LDA2_000 128, MTFW_000 106 | 8 | 33066 |
| protect_s/md: the same minus MT00A/B | 6 | 32602 |
| nvcfg: fg/*, sensor/*.json | 7 | 583 |

- Files the FS server must present: everything under nvdata/md, protect_f/md, protect_s/md. Possibly nvcfg (S:), but no md files there.
- The largest single file is 19 KB, so a 2.2 MB in-RAM tree is fine.

**strace**: present at /system/bin/strace (936 KB, from 2026-11-08). ccci_mdinit is pid 936 (the FS thread was tid 952 at boot).

### 3.2 Not run: needs the user's go-ahead (drops the cellular connection for ~5-60 s)

**Option A, cheapest: no strace at all.**
- After any modem restart, read `/proc/ccci_dump` **within ~20 s**. The HISTORY region then still holds the first 16 bytes of every boot-time packet: HS2, FS ops, RPC ops and AT, with timestamps.
- How to restart the modem: **(guess)** the MTK muxreport path:

  ```
  setprop vendor.ril.mux.report.case 2; setprop vendor.ril.muxreport.run 1
  ```

  Fallback: airplane mode does NOT restart the modem. A reboot works only if we can read the dump within ~1 min of `ccci_mdinit` starting; the stock dump of this boot shows the history begins at 57 s, so too late by ~2 s. A reboot plus `adb wait-for-device shell cat /proc/ccci_dump` the moment adb is up may just make it.

**Option B, full FS transcript including paths:** strace on every thread of mdinit, then the same restart trigger:

```
adb -s $HAKIOS_SERIAL shell 'T=$(ls /proc/$(pidof ccci_mdinit)/task | sed "s/^/-p /" | tr "\n" " ");
  strace -tt -T -xx -s 96 -e trace=openat,close,ioctl,read,write,mmap,munmap,fsync,renameat,unlinkat,mkdirat \
  -o /data/local/tmp/mdinit.st $T & sleep 1; setprop vendor.ril.mux.report.case 2; setprop vendor.ril.muxreport.run 1; sleep 60; kill %1'
```

- -s 96 is enough for header + op + about 30 UTF-16 characters of the filename.
- **Do not pull the raw file.** READ/WRITE replies contain up to 96 bytes of calibration/IMEI data.
- Reduce it on the device with a toybox awk script (~20 lines, not yet written) to op / path / length / status lines. Pull only that summary, then `rm` the raw file.

The ioctl arguments (SET_BOOT_DATA, CCB) are pointers that strace cannot decode. They are already known statically and from the dump.

**Option C, tiny read-only CCB check (only if 2a's CCB guess fails):** a ~50-line static aarch64 tool (built with clang `--target=aarch64-linux-gnu -nostdlib` + ld.lld, both on the Mac) that:
1. opens /dev/ccci_ccb_ctrl;
2. runs GET_CCB_CONFIG_LENGTH and GET_CCB_CONFIG × n;
3. mmaps 4 KB read-only and prints the first 0x500 bytes.

It does not call CCB_CTRL_INFO, which changes kernel state. Pushing and running a custom binary is outside "read-only adb commands", so it needs approval.

### 3.3 What the transcript must answer

1. Which FS ops and files come before HS2, and how many writes happen before HS2.
2. Which RPC ops come before HS2, especially 0x400F-0x4015.
3. The exact CMPT_READ reply shape.
4. Time from HS2 to "+EIND: 128".

---

## 4. Implementation plan

All of Stage 2 runs as the kernel thread already in xnu/files/bsd/dev/redwood_md.c, gated by boot-args. One thread polls every 2 ms: no IRQ, no character devices, no userland. It keeps md_power_off on every exit, as Stage 1 does. A userland daemon and cdevs come in Stage 3/4, when AT has to reach an app.

### 2a: runtime-data reply → HS2 (or the modem's next message). ~250 lines. Spec in §5.

### 2b: rings, control/system channels, kernel RPC answers. ~350 lines

- `ring_tx(q, ch, payload, len)`, a generic `ring_rx` dispatcher over queues 0-6, per-channel TX seq and RX seq log.
- RPC answers (§1.8, including the userland ops 0x400F-0x4015 answered with errors), sysmsg 0x105 / 0x114 / 0x116, log-and-drop for the rest.
- FS still answers −9 to everything. That is safe: writes are dropped. 2b only measures whether the modem reaches HS2 on errors.
- Test (`r1_md=4`). Lines:
  - `r1md: rpc op %#x name "%s" -> %d,%d` for every RPC;
  - `r1md: sys msg %#x resv %#x`;
  - `r1md: RESULT 2b: HS2 at %u ms` or `r1md: RESULT 2b: NO HS2 (fs %u rpc %u other %u packets)`;
  - `r1md: RESULT 2b: EXCEPTION (bit 16)`.
- Pass: every RPC answered; HS2 seen, or a stable list of FS requests the modem repeats.

### 2c: FS server with the RAM overlay. ~650 lines (FS ~350 + ext4 reader ~300)

**Getting the NV tree to our side: weighed options**

- **(A) recommended: a read-only ext4 reader over redwood_emmc.c reads of p6/p9/p10 (+p5).**
  - The data never leaves the device's own partitions and is always the current copy. The modem writes NA77_005 at runtime under Android, so a copy would go stale.
  - Needed features: superblock, group descriptors (32 or 64 bit), inodes, extent trees (depth ≤ 1 at these sizes), linear directory scan. Ignore htree indices: the leaf blocks are linear-readable. No journal replay.
  - **Refuse to serve** if `s_feature_incompat & RECOVER` (needs_recovery) is set: log `r1md: ext4 %s needs journal recovery, refusing`. Android with commit=1 and a clean reboot should leave it clear.
  - Cache files on first open: 2.2 MB worst case.
- **(B) one-time copy taken under Android** (tar of the four directories) into the payload, or into the xnudata partition.
  - The payload variant is built on the Mac, so calibration and IMEI would leave the device. **That violates the user's rule. Rejected.**
  - The xnudata variant stays on the device but is a second, stale copy that someone has to keep in sync. It needs a root `dd`-style write that the user must approve, and it saves only ~300 lines. Fallback only, if (A) stalls.
- **(C) Stage-2 shortcut** for 2c-α: serve only what the transcript shows before HS2, still through (A). No shortcut that copies data.

**Overlay**
- Every OPEN with CREATE/CREATE_ALWAYS, every WRITE, DELETE, RENAME, CREATEDIR and RESTORE operates on an in-RAM node (copy-on-write of the file the first time it is written).
- Cap: 4 MB. Above that: `r1md: fs overlay full`, and the op returns −18.
- RESTORE (0x1021) returns −18 and logs. We never touch the raw nvram partition.
- The eMMC driver must already refuse writes to p5/p6/p9/p10/p13/p14. Add an assert in the write path if it is not there: `r1md: REFUSED emmc write to NV partition %s`.
- Reads see overlay over disk. The overlay is discarded at power-off.

**Logging** (one line per op; the first 300 ops in full, then counts):
- `r1md: fs %s %s flags %#x -> %d (size %u, overlay %s)`
- `r1md: fs summary: open %u read %u write %u (overlay %u B) create %u missing %u`
- Every distinct path that returned −9: `r1md: fs MISSING %s`.

**Test** (`r1_md=5`):
- Pass: HS2 seen with 0 MISSING paths under NVD_IMEI and CALIBRAT, and no exception.
- Lines: `r1md: RESULT 2c: HS2 at %u ms, fs ops %u, missing %u, overlay %u B`.

### 2d: first AT exchange, RF off. ~150 lines

- After HS2, read ttyC0 (ch 10, queue 5) and log URCs (`r1md: at < "%s"`) until "+EIND: 128" (≤ 20 s).
- Then send one at a time, waiting ≤ 3 s for "OK"/"ERROR" each, and log `r1md: at > %s` / `r1md: at < %s`:
  1. "AT\r"
  2. "ATE0\r"
  3. "AT+CFUN?\r"
  4. "AT+CGSN\r"
  5. "AT+CPIN?\r"
- The IMEI line is logged masked: `r1md: at < +CGSN <15 digits, last 4: %s>`. The full IMEI never goes to the log.
- A hard filter in the AT send function refuses anything except that list: no CFUN=1, no EGMR, no CMUX. Log `r1md: at REFUSED %s`.
- If `AT+CFUN?` reports anything but 0 or 4: log it and send "AT+CFUN=4\r" (radio off), never 1. **(guess: the modem boots with RF off until rild sends CFUN=1; stock rild does that)**.
- Test (`r1_md=6`). Pass: "OK" to AT, a 15-digit CGSN, and a CPIN reply ("READY" or "SIM PIN").
- Lines: `r1md: RESULT 2d: AT ok, CGSN ok, CPIN %s, CFUN %s`.

### Sizes

| Sub-stage | Lines |
|---|---|
| 2a | 250 |
| 2b | 350 |
| 2c | 650 |
| 2d | 150 |
| **Total** | **~1,400** |

This is well below the plan's 2,500-4,000, because the kernel does only MD1 / CCIF / five channels.

### Unknowns, ranked (highest risk first)

| # | Unknown | Resolution |
|---|---|---|
| U1 | Does the MD need FS answers before HS2? Likely yes, given `fs_ongoing` and the 404 ms gap | 2a's log shows it; 2b tests error-only answers |
| U2 | Which NV files are read or written before HS2, and the exact CMPT_READ reply shape | Transcript §3.2, or 2b's log of requests |
| U3 | Is the ext4 on-disk state clean when we boot (needs_recovery)? | 2c's reader checks and logs it first |
| U4 | Is the CCB page fill right? Rows and the 0xEEFF0011 pattern are [U]+[K]; the META row choice is a guess | Option C tool if the MD asserts in shm_bm |
| U5 | Which user-space RPCs (0x400F-0x4015) the MD sends at boot, and whether an error reply is acceptable. DRDI copy from md1img would be a big one (~200 lines, reads md1img via redwood_emmc) | 2b log |
| U6 | MD sequence check of our TX (feature 10 bit 0) | Keep seq right. Fallback: send 2 instead of 3 (guess) |
| U7 | "+EIND: 128" timing without rild/muxd services, and whether CFUN starts at 0 or 4 | 2d |
| U8 | Exception handshake: not implemented | If the MD raises bit 16, Stage 2 reads only the debug regions; the full EE path is extra (~200 lines) |

### Kill criteria

- **2a:** two runs with a reply that byte-matches the stock dump (except seed/time) give neither HS2 nor any RX ring packet within 10 s, and no exception bit.
  - Then compare against a stock Option-A capture once.
  - If still nothing: stop Stage 2 (the reply path or MPU / smem visibility is wrong in a way we cannot see).
- **2b/2c:** the MD raises D2H_EXCEPTION_INIT on 3 consecutive runs at the same point, after every requested file was served, with no new request between runs.
- **Overall (unchanged from the plan):** no HS2 although every requested file was served, or about three weeks without passing 2c.
- **Safety stop (immediate):** any eMMC write attempt to an NV partition; any AT command outside the 2d list reaching the wire.

---

## 5. Sub-stage 2a spec (for the implementer)

**Files**
- xnu/files/bsd/dev/redwood_md.c only.
- Boot-arg value `r1_md=3` = Stage 1 + 2a. `r1_md=2` must behave exactly as now.
- Keep the xnu-src copy in sync, as for Stage 1.

**A. Before the release** (in smem_prepare, after the rings; only for r1_md ≥ 3)

1. **CCB control page:**
   - Zero nc smem +0x18000..+0x18FFF.
   - For row r in 0..19 of the §1.9 table, at p = +0x18000 + r × 64, write:

     | Offset | Value |
     |---|---|
     | +0x14 | dl_page |
     | +0x18 | dl_buff |
     | +0x1C | 0xEEFF0011 |
     | +0x34 | ul_page |
     | +0x38 | ul_buff |
     | +0x3C | 0xEEFF0011 |

   - Log `r1md: ccb ctrl: 20 rows, dhl %#x mdm %#x meta %#x` (sums 0x70000, 0x8000, 0x100000). Check that they total 0x178000; if not, log `CCB SUM WRONG` and power off.
2. **DBM:** at +0xFF50, 0x44444444 ×2, then 40 × 0, then 0x44444444 ×2. Log `r1md: dbm written`.

**B. Static table** `rt_feat[]` with {id, sup, len, words[4]} exactly as §1.4.
- Fill id 7 with `early_random()` (or `read_random`).
- Fill id 11 with `clock_get_calendar_microtime` seconds (lo / hi).
- id 13 size: use md1_chk img_size if mtkmodem.h already passes it; else the constant 0x0163F500 (stock), with a log line saying which.
- id 2 size: the `used` value ringbufs_init computed (must be 0xB0140). Assert it.

**C. On HS1** (read_hs1 returned true, inside wait_hs1, first time only), call `send_rt_data()`:
1. Check that every id with HS1 byte & 0xF == 2 has sup 2 in rt_feat. Else log `r1md: 2a: MD requires feature %d we lack` and power off.
2. Build the 64 entries into nc smem +0xE800 (32-bit writes). Check the total ≤ 0x800 (expect 0x310). Log:
   ```
   r1md: 2a: runtime data %#x bytes at smem +0xe800
   ```
   plus two dump lines of the first 16 words (compare with §1.4).
3. Write the up_header at CCIF CHDATA+0xF0: 0, 0xAC, 1, 0x5555FFFF.
4. Write the v2_1 struct at CHDATA+0x100 (zero 156 B first; fields as in §1.3; feature_set[1] = 0x10).
5. `dsb sy`.
6. Read BUSY. If bit 15 is set: log `r1md: 2a: CCIF busy bit 15 set (%#x), retry in 10 ms`, and retry once.
7. Write BUSY = 0x8000, then TCHNUM = 15.
8. Log `r1md: [%u ms] 2a: runtime data sent (BUSY %#x)`.

**D. Watch after the send**
- Extend the loop: up to 10 s after the send instead of 500 ms. Keep the 10 ms poll; 2 ms is fine too.
- Each iteration:
  - RCHNUM → ACK, as now. Log new bits.
  - For each queue q 0..6, look at rx.read/rx.write (block offsets from the size tables; RX buffer at block + 0x20).
  - While the ring is readable (§1.6):
    1. Copy the packet. Log ≤ 40 packets:
       ```
       r1md: [%u ms] rx q%d ch %u seq %u len %u: %08x %08x %08x %08x | %08x %08x %08x %08x
       ```
       (header + first 16 payload bytes).
    2. Decode:
       - ch 0, data[1] == 0, reserved != 0x5555FFFF → `r1md: [%u ms] HS2 (MD_NORMAL_BOOT) received`, set hs2.
       - ch 0, other data[1] → `r1md: control msg %#x resv %#x`.
       - ch 14 → `r1md: fs request op %#x nargs %u name "%s"` (if arg0 looks like UTF-16: ASCII low bytes up to 64 chars; never log data from WRITE args).
       - ch 32 → `r1md: rpc request op %#x nargs %u arg0 "%s"`.
       - ch 2 → `r1md: sys msg %#x resv %#x`.
    3. Advance rx.read (round_up(read + L + 16, 8) mod len).
  - Validation failure (bad 0xAABBAABB / footer): log once per queue `r1md: q%d rx ring garbage at %#x (w %#x)` and stop reading that queue.
  - Bit 16 → `r1md: [%u ms] D2H_EXCEPTION_INIT after the runtime data`. Then dump 32 words each of nc smem +0x0 (MDCCCI_DBG) and +0x800 (MDSS_DBG) as `r1md: ee dbg +%#x: ...`. Stop.
  - Nothing is ever sent back in 2a: FS and RPC stay unanswered.
- End conditions:
  - 2 s after HS2;
  - 10 s without HS2;
  - the exception bit.
- Log the boot status words every second, as now.

**E. Result line** (exactly one), then the existing md_power_off:
- `r1md: RESULT 2a: HS2 at %u ms after the reply; rx packets %u (fs %u rpc %u ctl %u sys %u other %u)`
- `r1md: RESULT 2a: NO HS2 in %u ms; rx packets %u (fs %u rpc %u ctl %u sys %u other %u), boot status %#x %#x`
- `r1md: RESULT 2a: MD EXCEPTION %u ms after the reply`

**Test**
- BASE + `r1_md=3`, SECS 120, as for md1d. The device returns to Android.
- Check the Android modem afterwards: baseband version and service state, as in Update 21.

**Pass**
- Either HS2, or ≥ 1 FS/RPC request decoded with no exception. The second case is the expected one: per U1 the modem probably waits on its first FS reads.

**Expected (guess):** first RPC md_eint / GPIO queries on ch 32 within ~150 ms, FS requests on ch 14 (Z:\NVRAM\..., X:/Y: files), no HS2 until those are answered.
