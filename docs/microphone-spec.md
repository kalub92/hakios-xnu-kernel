# r1 microphone: /dev/redwood-mic on MT6765 + MT6357

This is an analysis and a spec only (Opus, 2026-11-18). No source was changed or built, and no device was touched.
It is written so that a Sonnet agent can implement it without working anything out again.

Tags used on every value:
- **V**: verified from a source. The file is named.
- **R1**: read from the r1's own stock firmware (`rabbit(i)OS/firmware/stock/rabbit_OS_v0.8.293_*`: dtbo.img and super.img, read only).
- **T**: traced on the r1 under Android (logs/aud.trace).
- **G**: a guess. Each one carries its reason and how to check it.

The sources were fetched as single files into the scratchpad (`$SCRATCH/mic/`, under /tmp, so they may be gone after a reboot):
- MiCode/Xiaomi_Kernel_OpenSource, branch `dandelion-q-oss`, `sound/soc/mediatek/`. This is the same MT6765 legacy "common_int" driver that redwood_mtkaudio.c already cites:
  - common_int/mtk-soc-pcm-capture.c
  - common_int/mtk-soc-afe-control.c
  - common_int/mtk-auddrv-gpio.c
  - common_int/mtk-soc-digital-type.h
  - mt6765/mt6765-sound.c
  - mt6765/mtk-auddrv-afe.h
  - mt6765/mtk-auddrv-clk.c
  - mt6765/mtk-soc-afe-connection.c
  - codec/mt6357/mtk-soc-codec-6357.c
  - codec/mt6357/mtk-auddrv-ana.h
  - include/dt-bindings/pinctrl/mt6765-pinfunc.h
- Linux v6.12:
  - sound/soc/codecs/mt6357.c
  - sound/soc/codecs/mt6357.h
  - include/linux/mfd/mt6357/registers.h

  Every MT6357 address below was checked against registers.h.

Mainline has no mt6765 AFE driver, and haretic's tree has no audio nodes. The vendor driver is therefore the AFE reference, and mainline mt6357.c is a second reference for the codec.

---------------------------------------------------------------------------------------------------------------

## 1. What exists today

### 1a. redwood_mtkaudio.c (boot-arg `mtkaudio=N`): the downlink only

**Data path.** userland/src/audio_plugin.c writes 16-bit stereo 48 kHz frames to `/dev/redwood-audio` (redwood_audio.c). That calls the sink `mtkaudio_sink()`, which only copies into a 16384-frame ring under `mtkaudio_lock` and wakes the thread. The thread does the rest:
1. `power_up_analog()`
2. `power_up_afe()`
3. `feed_run()`
4. `power_down()`

It powers down after 1 s of silence. There are no interrupts: the thread polls every 5 ms.

**Hardware addresses.** All are mapped with `ml_io_map`:

| Block | Address | Notes |
|---|---|---|
| AFE | 0x11220000 | |
| infracfg_ao | 0x10001000 | |
| topckgen | 0x10000000 | |
| GPIO | 0x10005000 | |
| PMIC wrapper | 0x1000d000 | |
| AFE SRAM | 0x11221000 | 0x9000 bytes in total. DL1 uses the first 0x4000. |

**Clocks** (table `clocks_on`):
- infracfg MODULE_SW_CG_1_CLR (0x8c) = bit 25 (ifr_audio)
- topckgen CLK_CFG_3_CLR (0x78) = bit 31, and CLK_CFG_4_CLR (0x88) = bit 7: the mux gates
- AFE AUDIO_TOP_CON0 (0x000): clear bits 2, 25 and 26 (aud_afe, aud_dac, aud_dac_predis)
- infracfg MODULE_SW_CG_2_CLR (0xa8) = bit 4 (ifr_audio26m)

`clocks_off` reverses this.

**AFE downlink.** The DL1 memory interface reads the SRAM 0x11221000..0x11224fff. In the "normal" SRAM mode one 32-bit word is one frame, with L in the low half.

Setup:
- AFE_MEMIF_HDALIGN gets 0x7fff0000 under that mask.
- AFE_MEMIF_MSB bit 29 is cleared.

Routing and rate:
- I05/I06 are connected to O03/O04 and O28/O29 (AFE_CONN3/4/28/29).
- The rate is 48 kHz: AFE_DAC_CON1[3:0] = 10.
- AFE_ADDA_DL_SRC2_CON0 = 0x83001803.
- AFE_I2S_CON1 = 0xa0b.

Enables:
- AFE_ADDA_UL_DL_CON0 bit 0 (the ADDA block)
- AFE_DAC_CON0 bit 0 (the AFE)
- AFE_DAC_CON0 bit 1 (DL1)

The hardware read pointer is `AFE_DL1_CUR`, a physical address.

**The link to the PMIC.** SoC pins 136, 138 and 139 are set to function 1: AUD_CLK_MOSI, AUD_DAT_MOSI0 and AUD_DAT_MOSI1. Pin 137 (SYNC_MOSI) already has function 1. Their earlier modes are saved in `link_before[]` and restored at power-down.

**The MT6357 codec.** It is reached through the PMIC wrapper's WACS2 channel:
- `CMD` 0xc20: `(1<<31) | (reg>>1)<<16 | value` for a write, `(reg>>1)<<16` for a read
- `RDATA` 0xc24: the FSM is bits 18:16, 0 = idle, 6 = wait-for-valid-clear
- `VLDCLR` 0xc28

Every access is under `redwood_pwrap_lock()`. `pmic_set(reg, value, mask)` reads, changes and writes.

**The power-up order** is replayed from the Android trace:
1. The rails VAUD28 (0x1930), VAUX18, VIO28 and VLDO28: bit 0 of each.
2. `codec_dac_on`, which is TurnOnDacPower. It contains the **codec-common** writes:
   - audckbufEnable: DCXO_CW14 bit 13
   - NvregEnable: AUDDEC_ANA_CON11 bit 4 cleared
   - ClsqEnable: AUDENC_ANA_CON6 bit 0
   - Topck_Enable: AUD_TOP_CKPDN_CON0 &= ~0x66
   - PMIC AUDIO_TOP_CON0 = **0x002b (written whole)**
   - PMIC AFE_UL_DL_CON0 bit 0

   It also contains downlink-only writes: GPIO_MODE2, AFUNC_AUD_CON0/2 and DL_SRC2.
3. `codec_probe`
4. `codec_spk_on`
5. The AW87390 amplifier (bit-banged I2C0 on GPIO 82/83, address 0x58)
6. The AFE tables

`power_down()` reverses this:
- AFE: `dl1_stop`, then `dl1_close`, then the wait for AFE_DAC_MON bit 0, the pins and `clocks_off`
- Codec: `codec_off`, the amplifier, then `dac_off`. `dac_off` undoes all the codec-common writes: AUDIO_TOP_CON0 |= 0xff, AFE_UL_DL_CON0 bit 0 cleared, Topck/Clsq/Nvreg/audckbuf off.

**Constraints for the microphone.** Today every power-up and power-down tears down *shared* state:
- PMIC AUDIO_TOP_CON0
- PMIC AFE_UL_DL_CON0
- the codec-common writes
- AFE_DAC_CON0 bit 0
- AFE_ADDA_UL_DL_CON0
- the clock gates
- pin 136

A microphone that runs alongside playback needs these to be reference-counted (section 4, step 2).

**Boot-args and other entry points.**
- `mtkaudio=1` dumps the registers.
- `mtkaudio=3/4/5` run self-tests (a tone, the AFE sine, the codec sine) before the sink is set.
- `redwood_mtkaudio_init()` is called from redwood_input.c:238.

### 1b. The interface the userland already expects (redwood_mic.c, the POCO's driver, `mic=4`)

audio_plugin.c, through `codec_open()`, `codec_read()` and `codec_close()`, and record_test (AVAudioRecorder, 48 kHz mono 16-bit WAV) need exactly this:

- **Node:** `/dev/redwood-mic`, a character device made with `cdevsw_add(-1, ...)` and `devfs_make_node(..., UID_ROOT, GID_WHEEL, 0666, "redwood-mic")`.
- **Format:** **mono, s16le, 48000 Hz.** A read returns whole 2-byte samples.
- **open:** sets `ios_wants = 1`. The driver thread brings the path up while it is set. Several opens are fine. `d_close` runs on the last close and sets `ios_wants = 0`, and the path goes down.
- **read:** never blocks. The plugin opens with O_NONBLOCK, but this does not depend on it.
  - It returns 0 bytes until `RING_KEEP` = 1920 frames (40 ms) are waiting.
  - After that it returns `min(asked, available)`.
  - If more than `RING_MOST` = 4800 frames (100 ms) are waiting, the oldest are skipped down to 40 ms.
  - A short read clears "primed", so the next read waits for 40 ms again.

  The plugin pads short reads with silence and logs `the microphone stops: X s asked for, Y s of it from the kernel (peak P of 32768)`.
- **The ring:** 16384 frames of int16. `ring_written` is a release-stored 64-bit counter. `ring_read` is owned by the one reader.
- **write/ioctl/select:** `eno_*`.

**Reuse:** copy redwood_mic.c's ring, read, open, close and cdevsw code (the block under `/* ---- mic=4: the microphone for iOS ---- */`, about 90 lines) into the r1 driver unchanged. Do **not** link to redwood_mic.c. It is always compiled (bsd/conf/files:152), but on the r1 it never makes its node, because `redwood_mic_dsp_up()` only runs with `adsp=1 mic=4`. So no name clash is possible as long as the r1 is never booted with `adsp=1 mic=4`.

---------------------------------------------------------------------------------------------------------------

## 2. The uplink: register-level sequence

### 2a. Topology: which input the r1's microphones use

From the r1's stock vendor partition (**R1**), super.img at offsets ~1774471000 (audio_device.xml) and ~1774500900 (AudioParamOptions.xml):
- `MTK_DUAL_MIC_SUPPORT=yes`
- `MTK_AUDIO_NUMBER_OF_MIC=2`
- `MTK_AUDIO_MIC_INVERSE=no`

So the stock HAL uses the path `builtin_Mic_DualMic`:
```
Audio_MicSource1_Setting=ADC1, Audio_ADC_1_Switch=On, Audio_ADC_2_Switch=On,
Audio_Preamp1_Switch=IN_ADC1, Audio_Preamp2_Switch=IN_ADC3
```

The preamp enum `{OPEN, IN_ADC1, IN_ADC2, IN_ADC3}` goes into bits 7:6 of AUDENC_ANA_CON0 or CON1. That means IN_ADC1 = AIN0, IN_ADC2 = AIN1 and IN_ADC3 = AIN2 (**V**: mtk-soc-codec-6357.c `AudioPreAmp1_Sel`, `PreAmp_Mux_function`; mainline `pga_mux_map`).

| Mic | Codec input | Path | Mic bias |
|---|---|---|---|
| Main microphone | **AIN0** | PGA L, ADC L, uplink channel L (AFE I03) | **MICBIAS0** |
| Reference microphone | **AIN2** | PGA R, ADC R, channel R (AFE I04) | MICBIAS0 |
| Headset microphone | AIN1 | MICBIAS1 | The r1 has no jack, so this is probably unconnected (**G**). |

The mic biases are **V** from the mainline routes: AIN0 and AIN2 need MICBIAS0, AIN1 needs MICBIAS1.

Two caveats:
- **G:** that the board is wired the way its HAL config says. This is a reference-design XML, and the second microphone might not be fitted. Section 3, E4 settles it.
- **Default for iOS: channel L (AIN0).** The probe in section 4 records L and R at once and reports both.

The ACC/DCC microphone mode and the PGA gain the stock HAL picks are **not** in these files:
- `Audio_MIC1_Mode_Select` is set by the HAL from its parameters.
- `Audio_PGA1/2_Setting` comes from its gain tables.

Use the driver defaults:
- **ACC**: `mAudio_Analog_Mic1_mode = AUDIO_ANALOGUL_MODE_ACC` (**V**)
- **PGA index 3 = 18 dB**: `mAudio_Ana_Volume[MICAMP1] = 3` (**V**). The steps are 0, 6, 12, 18 and 24 dB, with a maximum index of 4 (mainline `capture_tlv`, `AUDPREAMPLGAIN_MAX`).

Keep both as boot-args, and confirm them with E2/E4.

The **SoC pins** for the uplink, from the r1's own dtbo.img (**R1**; fragment@14, `audgpio`):

| State | Pins | Function |
|---|---|---|
| `aud_dat_miso_on` | GPIO 142, 143 | 1 (AUD_DAT_MISO0/1) |
| `aud_clk_mosi_on` | GPIO 136 | 1 |
| `aud_clk_miso_on` | GPIO 140 | 1 |

`AudDrv_GPIO_Request(true, ADDA_UL)` selects only `aud_clk_mosi_on` and `aud_dat_miso_on` (**V**, mtk-auddrv-gpio.c). Pin 140 (CLK_MISO) and pin 141 (SYNC_MISO) are not requested for the uplink by the published driver (**G** that they are not needed: section 3, E5).

### 2b. MT6357 codec power-up

This is TurnOnADcPowerACC for ADC1 and then ADC2, as one sequence. Addresses are PMIC addresses through `pmic_set()`, with the AUD base 0x2080.

| # | Register (address) | Value / mask | Meaning | Tag |
|---|---|---|---|---|
| C0 | rails | as `power_up_analog()` | VAUD28 and the others. **Only if the codec-common part is not up yet** (refcount, section 4). | T (DL) |
| C1 | DCXO_CW14 (0x07ac) | 0x2000 / 0x2000 | audckbufEnable. Common. | V, T |
| C2 | AUDDEC_ANA_CON11 (0x221e) | 0 / 0x0010 | NvregEnable. Common. | V, T |
| C3 | AUDENC_ANA_CON6 (0x2194) | 1 / 1 | ClsqEnable (26 MHz CLKSQ). Common. | V, T |
| C4 | GPIO_MODE3_CLR (0x00c0) | 0xffff whole | set_capture_gpio(true) | V |
| C5 | GPIO_MODE3_SET (0x00be) | 0x0249 whole | PMIC GPIO 12..15 = CLK_MISO, DAT_MISO0/1, SYNC_MISO | V (vendor, mainline) |
| C6 | GPIO_MODE3 (0x00bc) | 0x0249 whole | the vendor writes it too | V |
| C7 | AUDDEC_ANA_CON11 (0x221e) | 0x0020 / 0x0020 | ADC CLKGEN on | V |
| C8 | AUDENC_ANA_CON3 (0x218e) | 0x0000 whole | ADC clock from CLKGEN (13 MHz) | V |
| C9 | AUDDEC_ANA_CON12 (0x2220) | 0x0100 / 0x2500 | LCLDO_ENC on | V |
| C10 | AUDDEC_ANA_CON12 (0x2220) | 0x2500 / 0x2500 | LCLDO_ENC remote sense. The downlink uses bits 0x1055 of this register: no overlap. | V |
| C11 | AUDENC_ANA_CON8 (0x2198) | 0x0021 whole | MICBIAS0 on, VREF bits 6:4 = 2 = **1.9 V** | V |
| C11b | AUDENC_ANA_CON9 (0x219a) | 1 / 1 | MICBIAS1 on, **only when probing AIN1** | V |
| C12 | AUDENC_ANA_CON0 (0x2188) | gain<<8 / 0x0700 | SetMicPGAGain, L. Default gain 3. | V |
| C13 | AUDENC_ANA_CON1 (0x218a) | gain<<8 / 0x0700 | the same, R | V |
| C14 | AUDENC_ANA_CON0 | (sel_L<<6)\|0x01 / 0xf0ff | preamp L input (sel_L 1 = AIN0) and PGA L on. The vendor writes 0x0041 / 0xf0ff. | V |
| C15 | AUDENC_ANA_CON0 | 0x5000 / 0xf000 | ADC L input = preamp (bit 14), ADC L power-up (bit 12) | V |
| C16 | AUDENC_ANA_CON1 | (sel_R<<6)\|0x01 / 0xf0ff | preamp R input (sel_R 3 = AIN2) and PGA R on. The vendor writes 0x00c1. | V |
| C17 | AUDENC_ANA_CON1 | 0x5000 / 0xf000 | ADC R on | V |
| C18 | AUD_TOP_CKPDN_CON0 (0x208c) | 0 / 0x0066, then wait 250 µs | Topck_Enable. Common. | V, T |
| C19 | AUDIO_TOP_CON0 (0x2112) | `top_con0()`, see below | power on the clocks: the vendor writes 0x8000 / 0xdfbf | V |
| C20 | AFE_TOP_CON0 (0x2110) | 0x0000 whole | normal paths, no sine | V |
| C21 | AFE_UL_DL_CON0 (0x2108) | 1 / 1 | the codec's digital AFE on. Common. | V |
| C22 | AFE_ADDA_MTKAIF_CFG0 (0x2134) | 0x0000 whole | MTKAIF TX format (protocol 1) | V |
| C23 | AFE_AUD_PAD_TOP (0x214c) | 0x3100 / 0xff00 | aud_pad TX FIFOs on. The downlink owns the low byte (0x31). | V (mainline `AUD_PAD_TX_FIFO_NORMAL_PATH_ENABLE`) |
| C24 | AFE_UL_SRC_CON0_H (0x210c) | 0x0000 whole | not DMIC | V |
| C25 | AFE_UL_SRC_CON0_L (0x210e) | 0x0001 whole | UL SRC on | V |

**DCC variant** (`mtkmic_mode=1`, **V** from TurnOnADcPowerDCC):
1. After C10, write AFE_DCCLK_CFG0 (0x2146) = 0x0402, 0x0402, 0x0400, 0x0401, then AFE_DCCLK_CFG1 (0x2148) = 0x0100.
2. C11 becomes AUDENC_ANA_CON8 0x0000 / 0xff00, then 0x0021 / 0x00ff.
3. Before C14: AUDENC_ANA_CON0 0x0004 / 0xf8ff (DC precharge).
4. After C14: bit 1 = 1 (DCCEN).
5. Steps 3 and 4 apply to CON1 the same way.
6. After C17: clear bit 2 (the precharge) of CON1, then of CON0.

**`top_con0()`** (PMIC AUDIO_TOP_CON0). This is the one write the two paths fight over:

| Running | Value | Where it comes from |
|---|---|---|
| Downlink only | **0x002b** | T, as today |
| Uplink only | **0x8040** | the vendor's 0x8000 / 0xdfbf applied to the idle value 0x00ff |
| Both | **0x8000** | the vendor's update applied to 0x002b (**G**: worked out arithmetically, never observed) |
| Neither | **0x00ff** | V, T |

Bit meanings (mainline mt6357.h):

| Bit | Name |
|---|---|
| 15 | PDN_LPBK |
| 7 | PDN_AFE |
| 6 | PDN_DAC |
| 5 | PDN_ADC |
| 3 | PDN_I2S_DL |
| 2 | PWR_CLK_DIS |
| 1 | PDN_TESTMODEL |
| 0 | PDN_RESERVED |

Every write of this register goes through `top_con0()`, including the downlink's `codec_dac_on` entry (today `{PMIC_AUDIO_TOP_CON0, 0x002b, 0, 250}`) and the `dac_off` entry.

**Codec uplink power-down** (TurnOnADcPowerACC(false), **V**). The order matters: the uplink source is stopped before the ADCs, and the ADCs before the bias.
1. AFE_UL_SRC_CON0_L bit 0 = 0.
2. AFE_AUD_PAD_TOP = 0x3000 / 0xff00.
3. **If the downlink is down:** AFE_UL_DL_CON0 bit 0 = 0. Then AUDIO_TOP_CON0 = `top_con0()`. The vendor writes 0x00ff / 0x00ff here and then sets bit 5.
4. AUDENC_ANA_CON0 = 0 / 0xf000, then 0 / 0x0fff.
5. AUDENC_ANA_CON1 = 0 / 0xf000, then 0 / 0x0fff.
6. AUDENC_ANA_CON8 = 0x0000 (MICBIAS0 off). Also AUDENC_ANA_CON9 bit 0 = 0, if AIN1 was used.
7. AUDDEC_ANA_CON12 = 0x0100 / 0x2500, then 0 / 0x2500.
8. AUDENC_ANA_CON3 = 0.
9. AUDDEC_ANA_CON11 bit 5 = 0.
10. set_capture_gpio(false): GPIO_MODE3_CLR = 0xffff, GPIO_MODE3 = 0x0000, GPIO_DIR0 (0x0088) = 0 / 0xf000.
11. **If the downlink is down:** Topck off (AUD_TOP_CKPDN_CON0 0x66 / 0x66), Clsq off, Nvreg off (bit 4 = 1), audckbuf off (DCXO_CW14 bit 13 = 0).

For DCC, the vendor also does the following:
- In step 4: clear bit 1 (DCCEN), then mask 0xfffb, then bit 2.
- At the end: AFE_DCCLK_CFG0 back to off. Mainline says GEN_OFF, PDN, then DIV_STOP. The vendor default is 0x2061 (**G**: write 0x2061).

### 2c. The SoC side: the AFE uplink (ADDA UL into VUL memory)

AFE offsets are relative to 0x11220000 (**V**: mtk-auddrv-afe.h):

| Register | Offset |
|---|---|
| AFE_VUL_BASE | 0x080 |
| AFE_VUL_END | 0x088 |
| AFE_VUL_CUR | 0x08c |
| AFE_ADDA_UL_SRC_CON0 | 0x114 |
| AFE_ADDA_UL_SRC_CON1 | 0x118 |
| AFE_ADDA_TOP_CON0 | 0x120 |
| AFE_ADDA_UL_SRC_MON0 | 0x148 |
| AFE_ADDA_UL_SRC_MON1 | 0x14c |
| AFE_ADDA_IIR_COEF_02_01 ... _10_09 | 0x290 ... 0x2a0 |
| AFE_CONN9 | 0x440 |
| AFE_CONN10 | 0x444 |
| AFE_ADDA_MTKAIF_RX_CFG0 | 0xe20 |
| AFE_ADDA_MTKAIF_RX_CFG1 | 0xe24 |
| AFE_ADDA_MTKAIF_RX_CFG2 | 0xe28 |
| AFE_AUD_PAD_TOP_CFG | 0xe40 |

Order: `mtk_capture_pcm_open` (clocks), then `hw_params`, `prepare` (`set_adc_in` and `set_adc_enable`) and `start`. All are **V** unless marked otherwise.

| # | Register | Value / mask | From |
|---|---|---|---|
| A0 | the downlink's `clocks_on` and `afe_probe` tables | **only if the AFE is not up yet** (refcount) | AudDrv_Clk_On (T) |
| A1 | AUDIO_TOP_CON0 (0x000) | 0 / bit 24 | AudDrv_ADC_Clk_On: aud_adc gate (mtk-auddrv-clk.c, the non-CCF branch) |
| A2 | AFE_MEMIF_HDALIGN (0x3fc) | 0x7fff0000 / 0x7fff0000 | set_sram_mode normal, as in `dl1_params` (T) |
| A3 | AFE_MEMIF_MSB (0x0cc) | 0 / bit 29 | the same |
| A4 | AFE_VUL_BASE (0x080) | **0x11225000** | set_mem_blk_addr. The address is **G**: SRAM right after DL1's 16 KB, inside the 0x9000 bytes of audio_sram. |
| A5 | AFE_VUL_END (0x088) | **0x11228fff** | 16 KB = 4096 stereo frames = 85 ms (**G**, the same as DL1) |
| A6 | (code) | clear the buffer | |
| A7 | AFE_MEMIF_MSB | 0 / bit 28 | SetMemIfFormatReg (16-bit) |
| A8 | AFE_MEMIF_HDALIGN | 0 / bit 5 | VUL isAlign = 0 |
| A9 | AFE_MEMIF_HD_MODE (0x3f8) | 0 / bits 11:10 | VUL isHD = 0 (16-bit) |
| A10 | AFE_CONN_24BIT (0x06c) | 0 / bits 9 and 10 | O09/O10 are 16-bit |
| A11 | AFE_AUD_PAD_TOP_CFG (0xe40) | 0x00000031 whole | set_chip_adc_in: "enable aud_pad_top fifo, need set after GPIO enable, pmic miso clk on". Run it after C4 to C25. |
| A12 | AFE_ADDA_MTKAIF_CFG0 (0xe00) | 0 whole | "reset MTKAIF_CFG0, no lpbk, protocol 1" |
| A13 | AFE_ADDA_TOP_CON0 (0x120) | 0 / bit 0 | internal ADC |
| A14 | IIR_COEF_02_01 (0x290) | 0x00000000 | the software IIR (35 Hz high-pass at 48 kHz) |
| A15 | IIR_COEF_04_03 (0x294) | 0x00003fb8 | |
| A16 | IIR_COEF_06_05 (0x298) | 0x3fb80000 | |
| A17 | IIR_COEF_08_07 (0x29c) | 0x3fb80000 | |
| A18 | IIR_COEF_10_09 (0x2a0) | 0x0000c048 | |
| A19 | AFE_ADDA_UL_SRC_CON0 (0x114) | **0x00060400** whole | bits 19:17 = 3 (`Soc_Aud_ADDA_UL_SAMPLERATE_48K`), bit 10 = IIR on, bits 9:7 = 0 (UL_IIR_SW) |
| A20 | AFE_ADDA_MTKAIF_RX_CFG0 (0xe20) | 0 / bit 0 | rxif data mode = AMIC |
| A21 | AFE_ADDA_UL_DL_CON0 (0x124) | 1 / 1 | SetADDAEnable. Common: the downlink sets it too. |
| A22 | AFE_ADDA_UL_SRC_CON0 | 1 / 1 | UL SRC on, giving 0x00060401 |
| A23 | GPIO 136 to function 1 if not already; GPIO 142 and 143 to function 1 | save the old modes | AudDrv_GPIO_Request(ADDA_UL); pin numbers R1 |
| A24 | AFE_CONN9 (0x440) | 1<<3 / 1<<3 | I03 to O09 (SetAdcToVul) |
| A25 | AFE_CONN10 (0x444) | 1<<4 / 1<<4 | I04 to O10 |
| A26 | AFE_DAC_CON1 (0x014) | 10<<16 / 0xf<<16 | VUL at 48 kHz (`mMemIfSampleRate[VUL] = {AFE_DAC_CON1, 16, 0xf}`, I2S_48K = 10) |
| A27 | AFE_DAC_CON1 | 0 / bit 27 | VUL stereo (`mMemIfChannels[VUL]`, bit 27 = mono) |
| A28 | AFE_DAC_CON0 (0x010) | 1<<3 / 1<<3 | VUL memif on (`mMemAudioBlockEnableReg`) |
| A29 | AFE_DAC_CON0 | 1 / 1 | the AFE on. Common. |
| A30 | check | AFE_VUL_CUR must move by about 0x780 (480 frames) in 10 ms | the same check as `power_up_afe()` does for DL1 |

**Interrupts:** none. Android uses an AFE IRQ (`irq_add_substream_user`). Here a thread polls AFE_VUL_CUR every 5 ms, like DL1.

**Calibration:** none. With MTKAIF protocol 1 (CFG0 = 0 on both sides), the published MT6765 and MT6357 drivers have no MTKAIF phase calibration (**V**: no calibration code in either driver for this pair). If the samples come back as noise, look here first (open unknown 8).

**Reading the samples (code, G):**
- `cur = rd(AFE_VUL_CUR)`. A value of 0 means `VUL_BASE`, as in get_ulmem_frame_index.
- `hw = (cur - VUL_BASE) & ~3`.
- Copy the words from `rd_at` to `hw` (wrapping at 0x4000) out of the uncached SRAM mapping.
- Sample = `ch ? (int16_t)(w >> 16) : (int16_t)(w & 0xffff)`. That L is in the low half is **G**, by analogy with DL1's normal mode. Test T1 checks it.
- The result goes to `ring_put()`.

A stall longer than 85 ms loses a lap without being detected. That is acceptable, as for DL1.

**AFE uplink power-down** (mtk_capture_alsa_stop and pcm_close, then set_adc_enable(false)):
1. AFE_DAC_CON0 bit 3 = 0.
2. AFE_CONN9 bit 3 = 0 and AFE_CONN10 bit 4 = 0.
3. AFE_ADDA_UL_SRC_CON0 bit 0 = 0.
4. **If the downlink is down:** AFE_ADDA_UL_DL_CON0 bit 0 = 0.
5. Wait 150 µs.
6. AUDIO_TOP_CON0 bit 24 = 1 (the ADC clock gated).
7. Restore pins 142 and 143. Restore 136 only if the downlink is down.
8. **If the downlink is down:** AFE_DAC_CON0 bit 0 = 0, wait for AFE_DAC_MON bit 0 to clear (at most 0.1 s), then `clocks_off`.

### 2d. Built-in test sources (V)

Each of these exercises one link of the chain without the microphone:

- **AFE sine into the VUL memif** (tests the memif, the SRAM and the word layout):
  - on: AFE_SGEN_CON0 (0x1f0) = 0x04ac2ac1, then the amplitude 2<<17 / 7<<17 and 2<<5 / 7<<5 (1/32 of full scale, as `sgen_on`), and AFE_SGEN_CON2 (0x1dc) = **0x25** / 0x3f (outputs O09/O10)
  - off: SGEN_CON0 = 0, SGEN_CON2 = 0x3f / 0x3f

  Source: set_chip_sine_gen_enable, output case O09/O10.
- **The codec's uplink sine table** (tests PMIC to MTKAIF to the AFE without analog):
  - on: PMIC AFE_TOP_CON0 (0x2110) bit 1 = 1 (UL_SINE_ON), AFE_SGEN_CFG0 (0x2140) = 0x0080, AFE_SGEN_CFG1 (0x2142) = 0x0101
  - off: bit 1 = 0, CFG0 = 0, CFG1 = 0x0101

  Source: SineTable_UL2_Set; mainline "SGEN UL Enable" = AFE_TOP_CON0 bit 1.

---------------------------------------------------------------------------------------------------------------

## 3. Evidence from stock Android (device agent, later; adb only)

This is for the device agent to run later. Do not run it now.

Everything here leaves no persistent change. It touches tracefs, the ALSA mixer at runtime (restored afterwards) and `/data/local/tmp`. It needs root, as the earlier logs/aud.trace capture did. Use `export ANDROID_SERIAL=$HAKIOS_SERIAL`. Save every output under `logs/mic-*`. Tell the user before starting (HANDOFF rules: audio was paused by the user).

**E0. Tools and identity**
```
adb shell 'id; su -c id; getprop ro.build.fingerprint; which tinymix tinycap tinyplay; ls /data/local/tmp'
```

**E1. ALSA layout** (find the capture PCM of the VUL memif: name `mt-soc-ul1-pcm`, or a stream with "UL1" or "Capture" in it):
```
adb shell 'cat /proc/asound/cards /proc/asound/pcm; ls -l /dev/snd'
adb shell 'for f in /proc/asound/card0/pcm*c/info; do echo == $f; cat $f; done'
```

**E2. The mixer, idle**, and after a HAL recording if E4b is possible:
```
adb shell tinymix -D 0 > logs/mic-tinymix-idle.txt
grep -E "MicSource|ADC_[12]_Switch|Preamp[12]_Switch|PGA[12]_Setting|MIC1_Mode|MIC2_Mode|Mic_Power_Mode" logs/mic-tinymix-idle.txt
```

**E3. The live configuration** (it should match section 2a's reading of super.img):
```
adb shell 'grep -n -A7 "builtin_Mic_DualMic\" value=\"turnon" /vendor/etc/audio_device.xml; \
  grep -E "DUAL_MIC|NUMBER_OF_MIC|MIC_INVERSE" /vendor/etc/audio_param/AudioParamOptions.xml; getprop | grep -iE "audio|mic"'
```

**E4. A trace of a capture plus an acoustic loopback.** This is the decisive one: which AIN is live, and every codec write.

1. On the Mac, make a 1 kHz, 3 s, 48 kHz stereo s16 WAV at -12 dBFS with a short python script. Push it to `/data/local/tmp/t1k.wav`.
2. Start the trace (same events as aud.trace):
   ```
   adb shell su -c 'T=/sys/kernel/tracing; [ -d $T/events ] || T=/sys/kernel/debug/tracing; echo 0 > $T/tracing_on; echo > $T/trace; \
     echo 16384 > $T/buffer_size_kb; echo "regmap:regmap_reg_write i2c:i2c_write" > $T/set_event; echo 1 > $T/tracing_on'
   ```
3. Set the stock DualMic path (section 2a) and record 5 s while the tone plays. `<UL>` is the device number from E1. If the local tinycap has no `-T`, run it in the background and kill it after 5 s.
   ```
   adb shell su -c 'tinymix "Audio_MicSource1_Setting" ADC1; tinymix "Audio_ADC_1_Switch" On; tinymix "Audio_ADC_2_Switch" On; \
     tinymix "Audio_Preamp1_Switch" IN_ADC1; tinymix "Audio_Preamp2_Switch" IN_ADC3; \
     (sleep 1; tinymix Speaker_Amp_Switch On; tinyplay /data/local/tmp/t1k.wav -D 0 -d 0; tinymix Speaker_Amp_Switch Off) & \
     tinycap /data/local/tmp/mic_dual.wav -D 0 -d <UL> -c 2 -r 48000 -b 16 -T 5; wait'
   ```
   tinyplay device 0 is "MultiMedia1_PLayback", as in aud.trace.
4. While it records, a second shell snapshots the pins (E5).
5. Stop the trace and restore the mixer:
   ```
   adb shell su -c 'T=/sys/kernel/tracing; [ -d $T/events ] || T=/sys/kernel/debug/tracing; echo 0 > $T/tracing_on; cat $T/trace' > logs/mic.trace
   adb shell su -c 'tinymix "Audio_Preamp1_Switch" OPEN; tinymix "Audio_Preamp2_Switch" OPEN; tinymix "Audio_ADC_1_Switch" Off; \
     tinymix "Audio_ADC_2_Switch" Off; T=/sys/kernel/tracing; [ -d $T/events ] || T=/sys/kernel/debug/tracing; echo > $T/set_event'
   adb pull /data/local/tmp/mic_dual.wav logs/
   ```
6. Repeat step 3 with `Audio_Preamp1_Switch IN_ADC2` (AIN1 on L) into `mic_ain1.wav`. MICBIAS1 is not switched by these controls, so expect silence unless AIN1 is wired (**G**).
7. On the Mac (`python3 -I`), compute for each channel of each WAV: the peak, the RMS before the tone (0 to 0.9 s) and the RMS during it (1.2 to 3.8 s). **The channel that rises by 15 dB or more is a live microphone.** L = AIN0 and R = AIN2 for mic_dual.

From logs/mic.trace, read every `1000d000.pwrap` write to 0x2188/0x218a (gain and input), 0x2198/0x219a (bias), 0x2146 (DCC, if written), 0x2112, 0x210e, 0x214c and 0x00bc–0x00c0, and every `audclk@11220000` write (0x114, 0x080/0x088, 0x440/0x444, 0x010/0x014, 0xe00–0xe40). Compare them with the tables in section 2, and mark every row "T" or "differs".

Note: tinycap bypasses the HAL. The HAL's mode and gain (ACC/DCC, `Audio_PGA*_Setting`) therefore stay at whatever tinymix reports in E2. **E4b (optional, needs the user or a recorder app):** run the same trace around a HAL recording, for example `adb shell am start -a android.provider.MediaStore.RECORD_SOUND` and the user taps record. Then `tinymix -D 0` *during* the recording gives the real `Audio_MIC1_Mode_Select` and `Audio_PGA1_Setting` values.

**E5. Pin modes during the capture:**
```
adb shell su -c 'cat /sys/devices/platform/10005000.pinctrl/mt_gpio 2>/dev/null | grep -E "^ *1(3[6-9]|4[0-3])[: ]"; \
  cat /sys/kernel/debug/pinctrl/*/pinmux-pins 2>/dev/null | grep -E "pin 1(3[6-9]|4[0-3]) "'
```

**E6. Register snapshots during the capture, if regmap debugfs exists:**
```
adb shell su -c 'ls /sys/kernel/debug/regmap/; ls /sys/kernel/debug | grep -iE "aud|soc"'
adb shell su -c 'cat /sys/kernel/debug/regmap/*pwrap*/registers' | grep -iE "^0*(2188|218a|218e|2198|219a|2112|2110|2108|210c|210e|2134|2146|214c|bc|208c):"
adb shell su -c 'cat /sys/kernel/debug/regmap/*11220000*/registers' | grep -iE "^0*(0|10|14|6c|80|88|8c|cc|114|120|124|3f8|3fc|440|444|e00|e20|e40):"
```

---------------------------------------------------------------------------------------------------------------

## 4. Implementation, in order

Everything goes in **one file, xnu/files/bsd/dev/redwood_mtkaudio.c**: it already has the tables, `run()`, `pmic_set`, the pins and the SRAM mapping, and the uplink must share the downlink's power state. No new file and no build-list change. sys/redwood_input.h only gains one prototype (step 7). Copy the file to `xnu-src/xnu-8796.121.2/bsd/dev/` and build with `./xnu-build.sh -j10 -k > logs/xnubuild.log 2>&1`.

### Step 1. Boot-args

Parse them in `redwood_mtkaudio_init`. The uplink needs `mtkaudio>=2` (the shared code) and works the same alongside it.

| Boot-arg | Meaning | Default |
|---|---|---|
| `mtkmic=N` | 1: also dump the uplink registers (step 3). 2: /dev/redwood-mic for iOS. 3: the self-test (step 6), then 2. | none |
| `mtkmic_ch=0\|1` | the channel iOS gets: 0 = L (AIN0), 1 = R (AIN2) | 0 |
| `mtkmic_in_l=1..3`, `mtkmic_in_r=1..3` | preamp inputs (1 = AIN0, 2 = AIN1 with MICBIAS1 too, 3 = AIN2) | 1 and 3: the stock DualMic |
| `mtkmic_gain=0..4` | PGA index, 6 dB steps | 3 |
| `mtkmic_mode=0\|1` | ACC or DCC | 0 |
| `mtkmic_bias=0..7` | MICBIAS0 VREF, AUDENC_ANA_CON8 bits 6:4: 1.7/1.8/1.9/2.0/2.1/2.5/2.6/2.7 V | 2 = 1.9 V |
| `mtkmic_shift=0..4` | digital gain in the kernel, a saturating left shift. This is the calibration knob. | 0 |
| `mtkmic_settle_ms` | samples dropped after power-up (bias and IIR settling, pop) | 100 |

### Step 2. Shared power, reference-counted

Add `static LCK_MTX_DECLARE(power_lock, ...)` and `static unsigned users` (bits `USE_DL=1`, `USE_UL=2`). Everything that powers up or down takes `power_lock`. Not `mtkaudio_lock`: the sink must never wait behind a pwrap sequence.

- Add `uint8_t shared` to `struct step`. In `run()`, skip a step with `shared` set when the *other* path's bit is set in `users`. Mark as shared:
  - in `dac_off`: the AFE_UL_DL_CON0, AUD_TOP_CKPDN_CON0, AUDENC_ANA_CON6, AUDDEC_ANA_CON11 bit 4 and DCXO_CW14 entries
  - in `dl1_close`: AFE_ADDA_UL_DL_CON0 and AFE_DAC_CON0 bit 0
  - all of `clocks_off`
  - the uplink's equivalents (sections 2b/2c, the "If the downlink is down" steps)
- Replace both PMIC_AUDIO_TOP_CON0 table entries (in `codec_dac_on` and `dac_off`) with a `top_con0()` code step. Do this by splitting each table at that entry, so the replay order stays the same. Compute it from `users` *after* the change (section 2b).
- Downlink `power_down()`: skip the AFE_DAC_MON wait, the restore of pin 136 and `clocks_off` while `users & USE_UL`. Restore 138 and 139 as now.
- `power_up_afe()` and the uplink's A0 run `clocks_on` and `afe_probe` only if `!afe_up`. `afe_up` stays true while either path uses the AFE. Clear it only when the last user leaves.
- Rails and codec-common (C0 to C3, C18): run them if `!(users & USE_DL)` when the uplink comes up. The downlink's `codec_dac_on` already does them, and they are idempotent.
- On a failed power-up, the failing path goes down with its own bit already cleared, so the shared parts are torn down only if the other path is not running.

### Step 3. The dump (`mtkmic=1`; nothing written)

Add these to `afe_dump` and `pmic_dump`:
- AFE: AFE_ADDA_UL_SRC_CON0/1, AFE_VUL_BASE/END/CUR, AFE_CONN9/10, AFE_ADDA_MTKAIF_RX_CFG0/1/2, AFE_AUD_PAD_TOP_CFG, AFE_ADDA_UL_SRC_MON0/1
- PMIC: AUDENC_ANA_CON0/1/3/8/9/10, AFE_UL_SRC_CON0_H/L, GPIO_MODE3, AFE_DCCLK_CFG0/1, AFE_SGEN_CFG0/1

Also print the modes of pins 140 to 143 next to 136 to 139.

### Step 4. The uplink

Write the tables for sections 2b and 2c in the existing `struct step` style, with one "source"/"G" comment per row:
- `ul_codec_on`, `ul_codec_off`
- `ul_afe_on`, `ul_afe_off`
- the DCC alternates as code branches on `mtkmic_mode`

Gain, bias and input selects are code steps, because they depend on boot-args. Map a second SRAM window: `vul = ml_io_map(0x11225000, 0x4000)`. DL1's mapping stays 0x4000.

`mic_up()`:
1. Take `power_lock`.
2. `users |= USE_UL`.
3. Run C0 to C25.
4. Run A0 to A30.
5. Release `power_lock`.

`mic_down()` runs the AFE power-down, then the codec power-down, then `users &= ~USE_UL`. The AFE goes first, mirroring the downlink's order.

On any failed table, `mic_down()` and keep a `mic_broken` flag. As in redwood_mic.c, after 3 failures stay off.

### Step 5. /dev/redwood-mic and its thread

Copy redwood_mic.c's ring, read, open, close and cdevsw block (section 1b) unchanged. A thread `mtkmic_thread`, started from init when `mtkmic>=2`, loops:
1. Wait for `ios_wants`, sleeping 20 ms.
2. `mic_up()`.
3. Every 5 ms, until `!ios_wants`: copy the new VUL words to the ring, taking channel `mtkmic_ch`, applying `mtkmic_shift` and dropping the first `mtkmic_settle_ms`.
4. Keep the per-second peak, RMS and mean. Log them like redwood_mic.c's `listen()`: the first 3 seconds, then every 30th. Use the prefix `mtkmic:`.
5. `mic_down()`.

Also feed `redwood_audio_record_fed()` (L and R duplicated) when `redwood_audio_recording_mic()` is true, so that `rec_sound=N rec_sound_mic=1` plus `./xl sound` fetches the raw microphone with iOS out of the loop.

### Step 6. Kernel self-test (`mtkmic=3`)

This runs in `mtkaudio_thread` after the existing level>=3 downlink test and before `redwood_audio_set_sink()`. It needs `mtkaudio>=3` for T4 and works unattended. It records L and R together and logs per channel the peak, the RMS in dBFS and the mean, for each phase:

| Test | What runs | Pass |
|---|---|---|
| T1 | `mic_up()` with the codec skipped (A0 to A30 only, the clocks shared), then the AFE sine into O09/O10 for 0.5 s | VUL_CUR advances about 1920 B per 10 ms, and peak ≥ 500 on both channels. If L and R look swapped or interleaved, the word layout guess is wrong. |
| T2 | the full `mic_up()`, the codec's UL sine table for 0.5 s, then off | peak ≥ 500. This proves MTKAIF and the pins 142/143. A pass in T1 but not T2 points at the link: pins, PMIC GPIO_MODE3, AUD_PAD_TOP or open unknown 7. |
| T3 | 1 s of room silence with the mic bias on | logs the noise floor per channel. A live microphone is not digital silence: RMS above -90 dBFS and a nonzero mean. Exactly 0 means a dead path. |
| T4 | acoustic loopback: the downlink up at the same time (`users = DL\|UL`), 300 ms of quiet, then the existing 440 Hz tone (`tone_left = REDWOOD_AUDIO_RATE`, `feed_run(true)`) with `mtkaudio_tone=8000` | per channel, the RMS during the tone minus the RMS of the quiet window ≥ **10 dB**, and peak ≥ **1000** (both thresholds **G**: tune after the first run, using `mtkmic_gain` and `mtkmic_shift`) |
| T5 | if `mtkmic_in_l=2` was asked for, repeat T4 with AIN1 and MICBIAS1 | |

Then `mic_down()` and a summary line: `mtkmic: SELFTEST T1 x T2 x T3 x T4 L x R x`.

### Step 7. The quiesce

Add `redwood_mtkaudio_quiesce()` to loader_return.c's list, next to the call to `redwood_haptic_quiesce()`. If `users & USE_UL`, it writes directly with `pmic_write_raw`, without the lock or sleeping:
- AFE_UL_SRC_CON0_L = 0
- AUDENC_ANA_CON0 = 0
- AUDENC_ANA_CON1 = 0
- **AUDENC_ANA_CON8 = 0** (MICBIAS0 off)
- AUDENC_ANA_CON9 bit 0 cleared

This keeps the bias from staying on into Android, and on a panic exit. Declare the function in sys/redwood_input.h.

### Step 8. Tests

**8a. Kernel only, with no userland change:**
```
tools/r1-auto.sh mic0 "<BASE> mtkaudio=3 mtkmic=3" 120
```
Read the `mtkaudio:` and `mtkmic:` lines.

**8b. The iOS path, with playback at the same time.** Make `userland/rc.jobs.mic`:
```
# Microphone acoustic loopback (RC_JOBS=rc.jobs.mic): kernel args mtkaudio=2 mtkmic=2.
/redwood/bin/sound_test springboard
/redwood/bin/tolog /redwood/bin/sound_test wait 4 raw 3 &
/redwood/bin/record_test 10
```
Build with `RC_JOBS=rc.jobs.mic` and run:
```
tools/r1-auto.sh mic1 "<BASE> mtkaudio=2 mtkmic=2" 240
```
Pass when all of these hold:
- `the microphone's first samples` appears.
- `the microphone stops: ... s of it from the kernel` covers at least 90% of what was asked for.
- record_test's `the file: ... peak P` has P ≥ 1000.
- In `dBFS RMS of each tenth of a second`, the maximum over tenths 40–80 is at least 10 dB above the median over tenths 5–35.

Both thresholds are **G**. Also check that the downlink's `played:` line in the same run shows no underruns caused by the uplink (shared power).

Neither run needs a stay image or Wi-Fi. Both return to Android by themselves. Ask the user first: the HANDOFF says audio is paused by the user.

### Power-down ordering (the summary)

- **Uplink alone:** stop VUL, then disconnect, UL SRC off, ADDA off, wait 150 µs, ADC clock off, pins, AFE off and clocks off. Then the codec: UL SRC_L off, PAD TX off, TOP_CON0, the ADCs and PGAs off, **then** the mic bias off, LCLDO_ENC off, CLKGEN off, the capture GPIO off, then common off.
- **Uplink while the downlink plays:** the same, minus every shared step. AUDIO_TOP_CON0 goes to 0x002b.
- **Downlink while the uplink records:** today's order, minus the shared steps. AUDIO_TOP_CON0 goes to 0x8040.

### Risks

1. **Shared state between the two paths** (the main risk).
   - The downlink's power-up writes AUDIO_TOP_CON0 = 0x2b. That sets PDN_ADC, which kills a running microphone with no error: the samples go flat.
   - Its power-down clears AFE_UL_DL_CON0, AFE_ADDA_UL_DL_CON0 and AFE_DAC_CON0 bit 0, gates the clocks and takes pin 136, the clock of both directions.
   - Step 2 is not optional. T4 and 8b exercise it, so check the downlink tone is still heard and still counted in T4.
2. **Pop and DC step.** Turning the bias and preamp on puts a transient through the 35 Hz IIR. It goes into the recording, not the speaker. `mtkmic_settle_ms=100` drops it. With DCC, the precharge sequence is mandatory.
3. **Mic bias left on.** The bias draws current and keeps a microphone live. It is turned off on close, on every failure path (`mic_down()` runs on failure) and in the quiesce (step 7). The iOS plugin closes the node when its input stops. A crashed mediaserverd closes it too, through the kernel.
4. **VUL SRAM placement (G).** If T1 fails with VUL_CUR stuck or a bus error, put VUL in DRAM: a physically contiguous 16 KB below 4 GB from `IOMallocContiguous` or kalloc plus `kvtophys`. AFE_VUL_BASE then takes a DRAM physical address (Android's DRAM case, `AudDrv_Emi_Clk_On`), and reading needs `dc ivac` like redwood_mic.c.
5. **Pins 140 and 141** might need function 1 for the MISO clock and frame. The published driver says no. E5 and T2 decide.
6. **The r1's codec driver is a later revision than the published one.** The downlink trace already showed writes that are not in the source, such as AUDDEC_ANA_CON8 and ZCD_CON0. E4's trace is the check for the uplink. Until then the uplink tables are source values (V), not traced (T).
7. **pwrap contention.** The uplink only uses the PMIC on up and down. That costs about 40 transactions under `redwood_pwrap_lock`, the same as the downlink. No polling of the PMIC while recording.
8. **The downlink sink's `feed_broken`** must not be set by an uplink failure: the two paths have separate failure flags.

---------------------------------------------------------------------------------------------------------------

## Open Mapple reference: a working MediaTek microphone in the same lineage

The code is checked out read-only at `$SCRATCH/open-mapple/`, and docs/open-mapple-kernel.md summarises it. Open Mapple's realme 8i port (MT6781 + MT6366) records through the same design as this spec. **Adapt their code; do not write from scratch.** Their kernel changes are APSL like ours: keep their notices in any function copied or adapted, and add a line naming the source file.

| Theirs | File | Ours (this spec) |
|---|---|---|
| `mt6781_afe_ul_start()` / `_position()` / `_stop()` | xnu_kernel_mediatek_mt6781/osfmk/arm64/mt6781_afe.c | A1 to A30 and the AFE uplink power-down (section 2c) |
| `mic_codec_up()` / `mic_codec_down()` | xnu_kernel_mediatek_common/bsd/dev/mtk_audio.c | C1 to C25 and the codec uplink power-down (section 2b) |
| `mic_poll()`, `mic_source()`, `mic_ring` | mtk_audio.c | step 5 |
| `/dev/mapple-mic`, `audio_read()` | bsd/dev/mapple_audio.c | /dev/redwood-mic (section 1b) |
| the reader | mapple_userland/audio/audio_plugin.c | ours is already written (audio_plugin.c `codec_read`) |

**Independent confirmation.** Their values come from MT6781 Android and are the same as the MT6765 values derived above:
- `UL_SRC_CON0_48K = 0x00060400`
- the IIR coefficients `{0, 0x3fb8, 0x3fb80000, 0x3fb80000, 0xc048}`
- AFE_ADDA_TOP_CON0 bit 0 = 0, MTKAIF_RX_CFG0 bit 0 = 0, MTKAIF_CFG0 = 0
- the ADC clock: AUDIO_TOP_CON0 bit 24 cleared
- the 48 kHz rate code 10
- **the uplink ring in AFE SRAM directly after the 16 KB playback ring** (their `UL_SRAM = AFE_SRAM + 0x4000`). This makes guess A4/A5 (0x11225000..0x11228fff) much more likely, and on hardware that works.
- the read position: `(CUR - base) & ~15` (16-byte steps). Use the same.
- L = the low half of each word, read as `ul[i] & 0xffff`, AIN0 on the left. This supports guess 8 (the word layout).
- the codec: AIN0, mic bias 0 at 1.9 V (VREF 2), the codec powered **only while the node is open**, uplink-source-off before ADC-off before bias-off.

**What differs, so do not copy as-is:**
- **Memif.** MT6781 uses VUL12: AFE_VUL12_* registers, AFE_CONN21/22, enable AFE_DAC_CON0 **bit 31**, rate and channels in AFE_VUL12_CON0. MT6765 has **VUL**: base/end/cur 0x080/0x088/0x08c, AFE_CONN9 bit 3 and AFE_CONN10 bit 4, rate and mono in AFE_DAC_CON1 bits 19:16 and 27, enable AFE_DAC_CON0 **bit 3**. Use section 2c's values.
- **Codec register map.** MT6366 (MT6358 family) puts the encoder at 0x2388.. and the bias at 0x239a, and its PMIC GPIO mode is 0x00de. It also has an ADC RC-tune readback (0x23a0) that MT6357 does not have in either source: drop that retry. MT6357 uses section 2b's addresses: 0x2188/0x218a, 0x2198, 0x218e, 0x2220, 0x221e, 0x210c/0x210e, 0x214c, GPIO_MODE3 0x00bc..0x00c0. Their MT6366 order (bias, then per ADC: gain, precharge clear, input AIN0, PGA on, 1 ms, ADC input, ADC power) is a valid alternative to the vendor's order in C11 to C17.
- **PGA.** Their Android records at 12 dB (index 2). The MT6357 driver default is 18 dB (index 3). Keep `mtkmic_gain`. If E4b is not done, start at 2, as theirs.
- **AFE pad.** They write SoC AFE_AUD_PAD_TOP 0x30 and then 0x31 (a toggle). The MT6765 vendor writes AFE_AUD_PAD_TOP_CFG (0xe40) = 0x31 directly. If T2 fails, try their toggle.
- **Pins.** They switch one MISO mode register (GPIO 184..). Ours are GPIO 142 and 143 (function 1), plus 136, which the downlink shares.

**Structure to take from them (it simplifies section 4).**
- **One thread.** Their `mic_poll()` runs in the same polling loop as the playback feeder. It brings the microphone up when the node is open and down when it closes, and copies new words into `mic_ring`. A single thread means the two paths cannot interleave power sequences, so `power_lock` is not needed.

  For us: make `mtkaudio_thread`'s idle wait wake on either `feed_wanted` or `ios_wants`, with `msleep` timed at 20 ms or a wakeup from `mic_dev_open`. Call `mic_poll()` every tick of `feed_run()` and of the idle loop.

  The shared-state rules of step 2 (`users`, `shared` steps, `top_con0()`) still apply. Only the extra lock and thread go away.
- **Common parts stay up while either path runs.** `mt6781_afe_ul_start()` refuses unless the AFE is already open (`afe_open`), so the AFE and codec-common bring-up is not duplicated. Ours: A0 and C0 to C3 run only when `users == 0` before the change.
- **On failure the microphone stays off** (`mic_failed`): no retry storm. This matches step 4.
- **Their `read()` always fills the buffer**, padding with zeros. **Keep redwood_mic.c's semantics** instead (partial reads, priming at 40 ms): our audio_plugin.c `codec_read()` is written for it, and counts short reads.

---------------------------------------------------------------------------------------------------------------

## Device results (2026-11-18): value tags updated (details: HANDOFF-2026-11-17-night.md, "Microphone")

- **G to T:** ACC/DCC: the r1's Android records in **DCC** (`Audio_MIC1/2_Mode_Select` DCCMODE during capture), PGA **18 dB** (index 3), preamp inputs IN_ADC1/IN_ADC3 both live (tone +60 dB on L and R in logs/mic_dual.wav). The default is now `mtkmic_mode=1`.
- **DCC order (T, logs/mic.trace):** DCC clock 0x2146 = 0x402, 0x400, 0x401 (no 0x2148 write); bias 0x2198 = 0x21; AUDENC_ANA_CON0/1 = 0x302 (gain + DCCEN) for both; then per channel precharge (0x306), input+PGA (0x347 / 0x3c7), ADC on (0x5347 / 0x53c7), precharge off (0x5343 / 0x53c3). This replaces the section 2b DCC list.
- **top_con0 "both": 0x0000 (T)**, not 0x8000: Android writes 0 when the speaker starts under a running capture, and 0x0040 after the speaker stops. Uplink alone 0x8040 is T.
- **AFE uplink (T):** A2, A11, A14..A22, A24..A29 all seen in the trace as in section 2c; DAC_CON1 0xa000a. Android's VUL buffer is 0x11221000..0x11224fff; our 0x11225000..0x11228fff (G) also **works** (self-test T1 pass, unknown 6 resolved). Word layout L = low half: verified (T1/T2 pass on both channels without `mtkmic_swap`).
- **Link (T2 pass):** pins 140/141 are **not** needed (already function 1 at boot anyway), no MTKAIF calibration needed (unknowns 5, 7, 8 resolved).
- **Thresholds:** T1/T2 peak 500 and T4 10 dB / peak 1000 held on the first run (T4: +37 dB L, +30 dB R). The iOS-path test (8b) needed `mtkmic_settle_ms=800` (now the default): with 100 the file's first seconds carry the start-up transient. 8b result: 92% coverage, file peak 4693, +14 dB.
- **Not done:** E5/E6 (no debugfs on this Android build; `su` is not needed, adb is root), the AIN1 repeat of E4, speech through the microphone.

---------------------------------------------------------------------------------------------------------------

## Open unknowns (10)

1. Which AIN the microphones are physically on. The stock config says AIN0 is the main mic and AIN2 the reference (R1), but the board is not verified. E4 or T4 settles it.
2. ACC or DCC as the stock HAL uses it (`Audio_MIC1_Mode_Select`). E2/E4b, or try both in T4.
3. The PGA gain the HAL uses for recording. E4b. Until then the default index is 3 (18 dB).
4. Writes the r1's later codec driver makes that the published TurnOnADcPowerACC lacks. E4 trace.
5. AFE writes that Android's regmap elides (no change, so no trace). Mitigated by writing every source value (as the downlink does).
6. Whether VUL works from SRAM at 0x11225000. T1. This is now likely: Open Mapple's MT6781 places its uplink ring in the same position.
7. Whether the SoC pins 140 (CLK_MISO) and 141 (SYNC_MISO) are needed for protocol 1. E5, T2.
8. The VUL word layout in normal SRAM mode (L in the low half), and whether the MTKAIF link is error-free without calibration. T1, T2.
9. The combined AUDIO_TOP_CON0 value 0x8000 (worked out, not observed). T4 and 8b.
10. The thresholds for the loopback pass (10 dB, peak 1000) and the right `mtkmic_gain`/`mtkmic_shift`. Tune them on the first run.
