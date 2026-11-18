/*
 * The speaker of the rabbit r1 (MT6765) as the sink of /dev/redwood-audio (see sys/redwood_audio.h):
 * the SoC's audio front end (AFE, 0x11220000) plays out of its own sample memory (0x11221000) to the
 * DAC in the PMIC (MT6357), whose line output drives an Awinic AW87390 amplifier (I2C0, address 0x58).
 * Only with boot-arg mtkaudio=N: every address below belongs to this device.
 *
 *   1   reads only: the clocks' gates, the pins, the AFE's and the codec's registers as the
 *       bootloader left them, each beside what Android leaves there when nothing plays
 *   2   the same, then the sink: when iOS writes, everything is powered up and fed until it has
 *       been silent for a second, then powered down
 *   3   before that, one second of 440 Hz made here and played through the same path
 *   4   before that, one second of the AFE's own sine generator into the DAC (another pitch), which
 *       needs no sample memory: it tells a dead analog path from a feeder that feeds wrongly
 *   5   before that, and before the AFE is touched at all, one second of the codec's own sine
 *       generator into its DAC, which needs no link from the SoC: it tells a dead link from a dead
 *       analog side. At every stage of 4 and 5 the codec's and the AFE's monitor registers are printed
 *       a few times: what changes between two looks is moving
 *   mtkaudio_skip0=M    leave out write i of "up 0/7" (bit i of M; bit 16: the AFE's), to find which matters
 *   mtkaudio_sgen_gain=N  the line output's gain while the codec's generator plays (20: 12 dB under 0 dB;
 *                         0 is Android's 8 dB over; the generator's own level is not in any source)
 *   mtkaudio_below=N    how far under full scale iOS's loudest is, in eighths of a dB (96: 12 dB)
 *   mtkaudio_tone=N     the peak of the 440 Hz tone, of 32767 (2000: 24 dB under full scale)
 *
 * Nothing here was worked out from a data sheet. It is what Android wrote while it played, replayed:
 * logs/aud.trace has every register write (regmap and i2c tracepoints) of `tinymix Speaker_Amp_Switch
 * On`, six seconds of `tinyplay` on "MultiMedia1_PLayback" at 48 kHz stereo 16 bit, and the switch off;
 * logs/aud.dmesg has the amplifier driver's own account of its writes and its one delay. What the
 * writes mean, and which bits of a register one of them owns, was read from MediaTek's driver of that
 * generation, as published in MiCode/Xiaomi_Kernel_OpenSource, branch dandelion-q-oss (GPL: read and
 * named, not copied):
 *
 *   sound/soc/mediatek/common_int/mtk-soc-pcm-dl1.c            the order of everything on the AFE's side
 *   sound/soc/mediatek/common_int/mtk-soc-afe-control.c        SetI2SDacOut, SetI2SDacEnable, get_dlmem_frame_index
 *   sound/soc/mediatek/common_int/mtk-auddrv-afe.c             Afe_Set_Reg: regmap_update_bits
 *   sound/soc/mediatek/common_int/mtk-auddrv-gpio.c            AudDrv_GPIO_Request
 *   sound/soc/mediatek/mt6765/mt6765-sound.c                   the registers' bits; set_chip_sine_gen_enable
 *   sound/soc/mediatek/mt6765/mtk-soc-afe-connection.c         SetConnectionState, SetIntfConnectionFormat
 *   sound/soc/mediatek/mt6765/mtk-auddrv-clk.c                 AudDrv_Clk_On, AudDrv_Clk_Off
 *   sound/soc/mediatek/mt6765/mtk-auddrv-afe.h                 the AFE's register offsets
 *   sound/soc/mediatek/codec/mt6357/mtk-soc-codec-6357.c       TurnOnDacPower, Speaker_Amp_Change, TurnOffDacPower
 *   sound/soc/mediatek/codec/mt6357/mtk-auddrv-ana.c           Ana_Set_Reg: read, change, always write
 *   include/dt-bindings/pinctrl/mt6765-pinfunc.h               the pins' functions
 * and from Linux drivers/clk/mediatek/clk-mt6765.c (which gate is which bit) and
 * include/linux/mfd/mt6357/registers.h (the codec's register names).
 *
 * Two things about the trace decide how it is replayed:
 *
 *   The AFE's writes go through regmap_update_bits, which writes nothing when the bits already have
 *   their value. So the trace is short of every write that changed nothing under Android, where the
 *   AFE had played before; after the bootloader those may well change something. They are in the
 *   tables below all the same, with the value the source gives, marked "source". (Two of them the
 *   trace confirms from the side: AFE_I2S_CON1 and AFE_ADDA_DL_SRC2_CON0 are seen as 0xa0b and
 *   0x83001803 once their enable bit is set.) The codec's writes always reach the wrapper: there the
 *   trace is whole.
 *
 *   A traced value is the whole register after the change. Where the source says which bits the
 *   write owns, only those are changed here (a mask in the table); where it does not -- the r1's codec
 *   driver is a later revision than the published one -- the traced value is written whole, marked
 *   "whole".
 *
 * Not replayed, though in the trace:
 *   the AFE's interrupt (irq_add_user/irq_remove_user: AFE_IRQ_MCU_CON0, _CNT1, _EN, and the 275
 *   acknowledgements in AFE_IRQ_MCU_CLR): nothing here takes interrupts, the pointer is polled;
 *   the sine generator a second tinymix switched on during the capture (AFE_SGEN_CON0/CON2 at 160.6 s):
 *   it is what mtkaudio=4 uses, and nothing else;
 *   the `tinymix` listings (pids 3315 and 3318) that switch the clocks and the DAC interface on and off
 *   to read controls; the charger's, the fuel gauge's and the thermal driver's PMIC registers (AUXADC,
 *   FGADC, BM, RTC); and the I2C controller's own clock gates (infracfg 0x80/0x84 bit 11, 0x88/0x8c
 *   bit 18, topckgen 0xa4/0xa8 bit 7, once per transfer): the bus is driven by hand here.
 * Not in the trace, and written: the three pins that carry the samples to the PMIC (below).
 *
 * The order is the capture's: the codec and the amplifier first, the AFE after them, and down in the
 * order Android went down (the AFE, then the codec and the amplifier). No source gives a reason for
 * it; it is the one order known to make a sound.
 *
 * "up 0/7" is what the trace cannot have: what Android's drivers write once, at their start. On the
 * AFE's side the published driver writes nothing then (InitAfeControl and the probes in
 * mtk-soc-pcm-dl1.c and mtk-soc-pcm-routing.c only take memory and map registers). On the codec's,
 * mt6357_codec_probe starts dc_trim_thread, which measures the headphone's offset through
 * OpenTrimBufferHardware: that sets up the charge pump's clock (AUDNCP_CLKDIV_CON0/1/2/4) and some
 * bias and reference bits and does not undo them. The published Speaker_Amp_Change writes the charge
 * pump's clock again each time; the r1's, by the trace, does not, so there it is left from that one
 * time -- and after the bootloader it has never happened (AUDDEC_ANA_CON2 reads 0 here where Android
 * idles at 0x233, which is exactly what that thread leaves). Without its clock the charge pump makes
 * no negative rail and the line output cannot move. A second source for the codec's bits is Linux
 * sound/soc/codecs/mt6357.c and mt6357.h (v6.12).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/proc.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/* the blocks a step of a table can be in */
enum { B_AFE, B_INFRA, B_TOP, B_GPIO, B_MAPPED, B_PMIC = B_MAPPED, B_AMP, B_TOPCON };
/* B_TOPCON is no block: the step is the codec's AUDIO_TOP_CON0, whose value top_con0() works out from who is using the codec */

static const struct {
	uint32_t phys, size;
} block_at[B_MAPPED] = {
	[B_AFE]   = { 0x11220000, 0x1000 },     /* audio@11220000 */
	[B_INFRA] = { 0x10001000, 0x1000 },     /* infracfg_ao */
	[B_TOP]   = { 0x10000000, 0x1000 },     /* topckgen */
	[B_GPIO]  = { 0x10005000, 0x1000 },
};
static vm_offset_t block[B_MAPPED];

#define SRAM_PHYS               0x11221000u     /* audio_sram@11221000, 0x9000 bytes */
#define DL1_BYTES               0x4000u         /* what tinyplay had of it: AFE_DL1_BASE 0x11221000, AFE_DL1_END 0x11224fff */
#define DL1_FRAMES              (DL1_BYTES / REDWOOD_AUDIO_FRAME)       /* 4096: 85 ms; a power of two */

/* infracfg_ao and topckgen (clk-mt6765.c): status of the gates the tables set and clear */
#define INFRA_CG1_STA           0x94            /* bit 25: ifr_audio, the AFE's bus clock */
#define INFRA_CG2_STA           0xac            /* bit 4: ifr_audio26m */
#define TOP_CLK_CFG_3           0x70            /* bits 25:24 audio_sel, bit 31 its gate */
#define TOP_CLK_CFG_4           0x80            /* bits 1:0 aud_intbus_sel, bit 7 its gate */

/* the AFE (mt6765/mtk-auddrv-afe.h) */
#define AUDIO_TOP_CON0          0x000
#define AFE_DAC_CON0            0x010
#define AFE_DAC_CON1            0x014
#define AFE_CONN3               0x02c
#define AFE_CONN4               0x030
#define AFE_I2S_CON1            0x034
#define AFE_DL1_BASE            0x040
#define AFE_DL1_CUR             0x044
#define AFE_DL1_END             0x048
#define AFE_CONN_24BIT          0x06c
#define AFE_MEMIF_MSB           0x0cc
#define AFE_ADDA_DL_SRC2_CON0   0x108
#define AFE_ADDA_DL_SRC2_CON1   0x10c
#define AFE_ADDA_UL_DL_CON0     0x124
#define AFE_SGEN_CON2           0x1dc
#define AFE_SGEN_CON0           0x1f0
#define AFE_ADDA_PREDIS_CON0    0x260
#define AFE_ADDA_PREDIS_CON1    0x264
#define AFE_DAC_MON             0x2ec
#define AFE_IRQ_MCU_CON0        0x3a0
#define AFE_IRQ_MCU_CLR         0x3a8
#define AFE_MEMIF_HD_MODE       0x3f8
#define AFE_MEMIF_HDALIGN       0x3fc
#define AFE_CONN28              0x4bc
#define AFE_CONN29              0x4c0
#define AFE_ADDA_DL_SDM_DCCOMP_CON 0xc50
#define AFE_ADDA_DL_SDM_FIFO_MON 0xc60
#define AFE_ADDA_DL_SRC_LCH_MON 0xc64
#define AFE_ADDA_DL_SDM_OUT_MON 0xc6c
#define AFE_ADDA_MTKAIF_CFG0    0xe00
#define AFE_ADDA_MTKAIF_TX_CFG1 0xe14
#define AFE_ADDA_MTKAIF_MON0    0xe34
#define AFE_ADDA_MTKAIF_MON1    0xe38
#define AFE_AUD_PAD_TOP_CFG     0xe40
#define AUDIO_TOP_CON1          0x004
#define AFE_ADDA_TOP_CON0       0x120
/* the uplink's (mt6765/mtk-auddrv-afe.h) */
#define AFE_VUL_BASE            0x080
#define AFE_VUL_END             0x088
#define AFE_VUL_CUR             0x08c
#define AFE_ADDA_UL_SRC_CON0    0x114
#define AFE_ADDA_UL_SRC_CON1    0x118
#define AFE_ADDA_UL_SRC_MON0    0x148
#define AFE_ADDA_UL_SRC_MON1    0x14c
#define AFE_ADDA_IIR_COEF_02_01 0x290           /* to _10_09, 0x2a0 */
#define AFE_CONN9               0x440
#define AFE_CONN10              0x444
#define AFE_ADDA_MTKAIF_RX_CFG0 0xe20
#define AFE_ADDA_MTKAIF_RX_CFG1 0xe24
#define AFE_ADDA_MTKAIF_RX_CFG2 0xe28

/* the MT6357 (include/linux/mfd/mt6357/registers.h; the legacy names in codec/mt6357/mtk-auddrv-ana.h) */
#define PMIC_SMT_CON1           0x002c
#define PMIC_GPIO_MODE2         0x00b6
#define PMIC_GPIO_MODE2_SET     0x00b8
#define PMIC_GPIO_MODE2_CLR     0x00ba
#define PMIC_DCXO_CW14          0x07ac
#define PMIC_AUD_TOP_CKPDN_CON0 0x208c
#define PMIC_AUDNCP_CLKDIV_CON0 0x20b4
#define PMIC_AUDNCP_CLKDIV_CON1 0x20b6
#define PMIC_AUDNCP_CLKDIV_CON2 0x20b8
#define PMIC_AUDNCP_CLKDIV_CON4 0x20bc
#define PMIC_AFE_TOP_CON0       0x2110          /* bit 0: the DAC plays the sine table, not the link (mt6357.h, DL_SINE_ON) */
#define PMIC_AFUNC_AUD_MON0     0x2124
#define PMIC_MTKAIF_FIFO_LOG_MON1 0x212a
#define PMIC_MTKAIF_MON0        0x212c          /* to MON3, 0x2132 */
#define PMIC_MTKAIF_CFG0        0x2134
#define PMIC_MTKAIF_RX_CFG0     0x2136          /* to RX_CFG3, 0x213c */
#define PMIC_AFE_SGEN_CFG0      0x2140
#define PMIC_AFE_SGEN_CFG1      0x2142
#define PMIC_AFE_AUD_PAD_TOP    0x214c
#define PMIC_AFE_AUD_PAD_TOP_MON 0x214e
#define PMIC_AFE_AUD_PAD_TOP_MON1 0x2150
#define PMIC_AUDDEC_ANA_CON1    0x220a
#define PMIC_AUDDEC_ANA_CON3    0x220e
#define PMIC_AUDDEC_ANA_CON9    0x221a
#define PMIC_AUDNCP_CLKDIV_CON3 0x20ba
#define PMIC_AFE_UL_DL_CON0     0x2108
#define PMIC_AFE_DL_SRC2_CON0_L 0x210a
#define PMIC_AUDIO_TOP_CON0     0x2112
#define PMIC_AFUNC_AUD_CON0     0x2116
#define PMIC_AFUNC_AUD_CON2     0x211a
#define PMIC_AUDENC_ANA_CON6    0x2194
#define PMIC_AUDDEC_ANA_CON0    0x2208
#define PMIC_AUDDEC_ANA_CON2    0x220c
#define PMIC_AUDDEC_ANA_CON4    0x2210
#define PMIC_AUDDEC_ANA_CON6    0x2214
#define PMIC_AUDDEC_ANA_CON7    0x2216
#define PMIC_AUDDEC_ANA_CON8    0x2218
#define PMIC_AUDDEC_ANA_CON10   0x221c
#define PMIC_AUDDEC_ANA_CON11   0x221e
#define PMIC_AUDDEC_ANA_CON12   0x2220
#define PMIC_AUDDEC_ANA_CON13   0x2222
#define PMIC_ZCD_CON0           0x2288
#define PMIC_ZCD_CON1           0x228a
/* the uplink's (codec/mt6357/mtk-auddrv-ana.h, include/linux/mfd/mt6357/registers.h) */
#define PMIC_GPIO_DIR0          0x0088
#define PMIC_GPIO_MODE3         0x00bc
#define PMIC_GPIO_MODE3_SET     0x00be
#define PMIC_GPIO_MODE3_CLR     0x00c0
#define PMIC_AFE_UL_SRC_CON0_H  0x210c
#define PMIC_AFE_UL_SRC_CON0_L  0x210e
#define PMIC_AFE_DCCLK_CFG0     0x2146
#define PMIC_AFE_DCCLK_CFG1     0x2148
#define PMIC_AUDENC_ANA_CON0    0x2188
#define PMIC_AUDENC_ANA_CON1    0x218a
#define PMIC_AUDENC_ANA_CON3    0x218e
#define PMIC_AUDENC_ANA_CON8    0x2198
#define PMIC_AUDENC_ANA_CON9    0x219a
#define PMIC_AUDENC_ANA_CON10   0x219c

/*
 * One write of a replay. With a mask, only those bits of the register are given `value`'s (read,
 * change, write); without, `value` is written as it is. `wait_us` is waited after it.
 */
struct step {
	uint8_t  block;
	uint16_t reg;
	uint32_t value, mask;
	uint16_t wait_us;
	uint8_t  shared;        /* USE_* bits: left out while any of these paths is still running (see `users') */
};
#define COUNT(t)                (sizeof(t) / sizeof((t)[0]))
#define USE_DL                  1u              /* the speaker (downlink) is up */
#define USE_UL                  2u              /* the microphone (uplink) is up */
#define USE_BOTH                (USE_DL | USE_UL)

/*
 * Who has the codec and the AFE up. Only the one thread (mtkaudio_thread) changes it, in the order
 * "own bit set before an up, cleared before a down", so a step marked `shared' is left out of a down
 * (or, for the uplink's common parts, of an up) while the other path still needs what it does.
 * Read by redwood_mtkaudio_quiesce() on a way out where nothing may wait.
 */
static volatile uint32_t users;

/*
 * AudDrv_Clk_On (mt6765/mtk-auddrv-clk.c), as traced at 158.580197: the AFE's bus clock, the gates
 * of its two clock multiplexers (the set and clear registers of CLK_CFG_3/4; what the multiplexers
 * select is not written in the trace, and not here: mtkaudio=1 prints it), the AFE's own three gates
 * in AUDIO_TOP_CON0 (a6fd4078, a4fd4078, a0fd4078: bits 2, 25, 26 cleared one by one), the 26 MHz
 * of the link to the PMIC.
 */
static const struct step clocks_on[] = {
	{ B_INFRA, 0x8c, 0x02000000, 0, 0 },            /* MODULE_SW_CG_1_CLR: ifr_audio */
	{ B_TOP, 0x78, 0x80000000, 0, 0 },              /* CLK_CFG_3_CLR: audio_sel's gate */
	{ B_TOP, 0x88, 0x00000080, 0, 0 },              /* CLK_CFG_4_CLR: aud_intbus_sel's gate */
	{ B_AFE, AUDIO_TOP_CON0, 0, 1u << 2, 0 },       /* aud_afe */
	{ B_AFE, AUDIO_TOP_CON0, 0, 1u << 25, 0 },      /* aud_dac */
	{ B_AFE, AUDIO_TOP_CON0, 0, 1u << 26, 0 },      /* aud_dac_predis */
	{ B_INFRA, 0xa8, 0x00000010, 0, 0 },            /* MODULE_SW_CG_2_CLR: ifr_audio26m */
	{ B_AFE, AUDIO_TOP_CON0, 1u << 29, 1u << 29, 0 }, /* source: "enable audio sys DCM"; set already in every traced value */
};

/*
 * mtk_pcm_dl1_params (common_int/mtk-soc-pcm-dl1.c): AllocateAudioSram -> set_sram_mode (the sample
 * memory in its normal mode, which is two 16-bit samples to a word), then set_mem_block ->
 * set_mem_blk_addr (where the buffer is). The buffer is then cleared (code, below).
 */
static const struct step dl1_params[] = {
	{ B_AFE, AFE_MEMIF_HDALIGN, 0x7fff0000, 0x7fff0000, 0 },        /* traced: 0x7fff0000 */
	{ B_AFE, AFE_MEMIF_MSB, 0, 1u << 29, 0 },                       /* source */
	{ B_AFE, AFE_DL1_BASE, SRAM_PHYS, 0, 0 },                       /* traced: 0x11221000 */
	{ B_AFE, AFE_DL1_END, SRAM_PHYS + DL1_BYTES - 1, 0, 0 },        /* traced: 0x11224fff */
};

/*
 * mtk_pcm_prepare: SetMemIfFetchFormatPerSample -> SetMemIfFormatReg (16-bit samples in memory),
 * SetConnectionFormat for the DAC's two pairs of outputs (O3/O4 and O28/O29 carry 16 bits; traced
 * as 3019e013, 3019e003, 2019e003, 0019e003), SetI2SDacOut (CleanPreDistortion; SetDLSrc2: 48 kHz in
 * the top four bits, with set_adda_dl_src_gain and SetSdmLevel; the DAC interface at 48 kHz, I2S,
 * 32-bit words), SetI2SDacEnable(true) (the AFE, the ADDA block, the interpolator, the interface, in
 * that order). The pins follow (code): AudDrv_GPIO_Request(true, ADDA_DL).
 */
static const struct step dl1_prepare[] = {
	{ B_AFE, AFE_MEMIF_MSB, 0, 1u << 28, 0 },                       /* source */
	{ B_AFE, AFE_MEMIF_HDALIGN, 0, 1u << 0, 0 },                    /* source */
	{ B_AFE, AFE_MEMIF_HD_MODE, 0, 3u << 0, 0 },                    /* traced: 0 */
	{ B_AFE, AFE_CONN_24BIT, 0, 1u << 3, 0 },
	{ B_AFE, AFE_CONN_24BIT, 0, 1u << 4, 0 },
	{ B_AFE, AFE_CONN_24BIT, 0, 1u << 28, 0 },
	{ B_AFE, AFE_CONN_24BIT, 0, 1u << 29, 0 },
	{ B_AFE, AFE_ADDA_PREDIS_CON0, 0, 0, 0 },                       /* source */
	{ B_AFE, AFE_ADDA_PREDIS_CON1, 0, 0, 0 },                       /* source */
	{ B_AFE, AFE_ADDA_DL_SRC2_CON0, 0x83001802, 0, 0 },             /* source; traced with bit 0 set */
	{ B_AFE, AFE_ADDA_DL_SRC2_CON1, 0xf74f0000, 0, 0 },             /* source; traced at 172.061242 */
	{ B_AFE, AFE_ADDA_DL_SDM_DCCOMP_CON, 0x1d, 0x3f, 0 },           /* source: AUDIO_SDM_LEVEL_NORMAL */
	{ B_AFE, AFE_I2S_CON1, 0x00000a0a, 0, 0 },                      /* source; traced with bit 0 set */
	{ B_AFE, AFE_DAC_CON0, 1, 1, 0 },                               /* traced: 1 */
	{ B_AFE, AFE_ADDA_UL_DL_CON0, 1, 1, 0 },                        /* traced: 1 */
	{ B_AFE, AFE_ADDA_DL_SRC2_CON0, 1, 1, 0 },                      /* traced: 0x83001803 */
	{ B_AFE, AFE_I2S_CON1, 1, 1, 0 },                               /* traced: 0xa0b */
};

/*
 * mtk_pcm_dl1_start: SetIntfConnection twice (the memory's left and right, I5 and I6, to O3/O4 and
 * to O28/O29: SetDl1ToI2s1Dac and SetDl1ToI2s1Dac2 in mt6765/mtk-soc-afe-connection.c), SetSampleRate
 * and SetChannels (48 kHz is 10, stereo is 0), SetMemoryPathEnable: from here the hardware reads the
 * buffer round and round.
 */
static const struct step dl1_start[] = {
	{ B_AFE, AFE_CONN3, 1u << 5, 1u << 5, 0 },                      /* traced: 0x20 */
	{ B_AFE, AFE_CONN4, 1u << 6, 1u << 6, 0 },                      /* traced: 0x40 */
	{ B_AFE, AFE_CONN28, 1u << 5, 1u << 5, 0 },                     /* traced: 0x20 */
	{ B_AFE, AFE_CONN29, 1u << 6, 1u << 6, 0 },                     /* traced: 0x40 */
	{ B_AFE, AFE_DAC_CON1, 10, 0xf, 0 },                            /* source: Soc_Aud_I2S_SAMPLERATE_I2S_48K */
	{ B_AFE, AFE_DAC_CON1, 0, 1u << 21, 0 },                        /* source: not mono */
	{ B_AFE, AFE_DAC_CON0, 1u << 1, 1u << 1, 0 },                   /* traced: 3 */
};

/*
 * Speaker_Amp_Change(true) (codec/mt6357/mtk-soc-codec-6357.c), which begins with
 * TurnOnDacPower(SPEAKERL), as traced from 158.544730: the crystal's audio buffer, the PMIC's pins
 * of the link, the line output's common mode, the 26 MHz and the digital clocks, the DAC's digital
 * side; then the charge pump, the regulators, the line output's driver, its gain, the DAC, the
 * driver's input to the DAC. The three waits are the source's udelay(250) and udelay(100), and are
 * in the trace's times.
 */
static const struct step codec_dac_on[] = {
	{ B_PMIC, PMIC_DCXO_CW14, 0x2000, 0x2000, 0 },          /* audckbufEnable: traced a2b5 */
	{ B_PMIC, PMIC_GPIO_MODE2_CLR, 0xffff, 0, 0 },          /* set_playback_gpio */
	{ B_PMIC, PMIC_GPIO_MODE2_SET, 0x0249, 0, 0 },
	{ B_PMIC, PMIC_SMT_CON1, 0x0ff0, 0x0ff0, 0 },           /* traced fff */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON6, 0x0280, 0x0fff, 0 },
	{ B_PMIC, PMIC_AUDDEC_ANA_CON7, 0x0028, 0, 0 },
	{ B_PMIC, PMIC_AUDDEC_ANA_CON11, 0, 1u << 4, 0 },       /* NvregEnable: traced 0 */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON2, 0x0400, 0x0400, 0 },    /* hp_pull_down(true): traced 633 */
	{ B_PMIC, PMIC_AUDENC_ANA_CON6, 1, 1, 0 },              /* ClsqEnable: traced a1 */
	{ B_PMIC, PMIC_AUD_TOP_CKPDN_CON0, 0, 0x0066, 250 },    /* Topck_Enable: traced 0 */
	{ B_TOPCON, 0, 0, 0, 250 },                            /* top_con0(): 0x002b whole while only the speaker runs: the source's mask (0xc4) would leave 0x3b of the 0xff before */
	{ B_PMIC, PMIC_AFUNC_AUD_CON2, 0x0006, 0, 0 },
	{ B_PMIC, PMIC_AFUNC_AUD_CON0, 0xcba1, 0, 0 },
	{ B_PMIC, PMIC_AFUNC_AUD_CON2, 0x0003, 0, 0 },
	{ B_PMIC, PMIC_AFUNC_AUD_CON2, 0x000b, 0, 0 },
	{ B_PMIC, PMIC_AFE_UL_DL_CON0, 1, 1, 0 },
	{ B_PMIC, PMIC_AFE_DL_SRC2_CON0_L, 0x0001, 0, 0 },      /* setDlMtkifSrc */
};

/*
 * "up 0/7", the codec's part: what Android has had since its start and the r1's Speaker_Amp_Change
 * therefore leaves out. Run here, between TurnOnDacPower and the rest, because that is where the
 * published Speaker_Amp_Change has the same writes and the codec's clocks run by now. Every one is
 * printed with what the register held before and holds after, and can be left out (mtkaudio_skip0).
 *   0, 1   AUDDEC_ANA_CON2: "Reduce ESD resistance of AU_REFN", the DAC's reference (published
 *          Speaker_Amp_Change), and the headphone's stability bits OpenTrimBufferHardware leaves: with
 *          both the register is the 0x233 Android has while it plays and when idle (traced)
 *   2-5    the charge pump's clock: "Turn on DA_600K_NCP_VA18", "Set NCP clock as 604kHz", "Toggle
 *          RG_DIVCKS_CHG", "soft start mode as default mode: 150us" (OpenTrimBufferHardware, and the
 *          published Speaker_Amp_Change, in this order before the pump is enabled)
 *   6      AUDDEC_ANA_CON9: the drivers' bias current (the same two functions)
 *   7      PMIC_AFE_TOP_CON0: "set DL in normal path, not from sine gen table" (setDlMtkifSrc)
 *   8      AFE_AUD_PAD_TOP: "enable aud_pad RX fifos", which only the published driver's reference
 *          sequence for a downlink has (PMIC_REG_CLEAR_Set); its TurnOnDacPower does without
 * Not here, being the headphone's or the earpiece's alone: ZCD_CON2, AUDDEC_ANA_CON1, AUDDEC_ANA_CON3,
 * the trim codes in AUDDEC_ELR_0. mt6357_codec_init_reg itself only switches things off that the
 * tables here set anyway.
 */
static const struct step codec_probe[] = {
	{ B_PMIC, PMIC_AUDDEC_ANA_CON2, 0x0200, 0x0200, 0 },
	{ B_PMIC, PMIC_AUDDEC_ANA_CON2, 0x0033, 0x00ff, 0 },
	{ B_PMIC, PMIC_AUDNCP_CLKDIV_CON1, 0x0001, 0, 0 },
	{ B_PMIC, PMIC_AUDNCP_CLKDIV_CON2, 0x002c, 0, 0 },
	{ B_PMIC, PMIC_AUDNCP_CLKDIV_CON0, 0x0001, 0, 0 },
	{ B_PMIC, PMIC_AUDNCP_CLKDIV_CON4, 0x0002, 0, 0 },
	{ B_PMIC, PMIC_AUDDEC_ANA_CON9, 0x0092, 0, 0 },
	{ B_PMIC, PMIC_AFE_TOP_CON0, 0x0000, 0, 0 },
	{ B_PMIC, PMIC_AFE_AUD_PAD_TOP, 0x0031, 0x00ff, 0 },
};

/*
 * "up 0/7", the AFE's part (bit 16 of mtkaudio_skip0): the I2S clock's power-down bit, set in every
 * value of AUDIO_TOP_CON0 the trace has and clear after the bootloader (aud_top_con_pdn_i2s in
 * mt6765/mtk-auddrv-clk.c: "power off I2S clock"). The DAC's path does not use that clock; it is set
 * so that the register is Android's to the bit. The other differences from Android found after the
 * bootloader need nothing: AFE_MEMIF_HDALIGN and AFE_CONN_24BIT's bits 3, 4 are written by the tables
 * (its bits 0, 1 are the outputs O0/O1, another interface's); AFE_ADDA_MTKAIF_CFG0 is 0 in both;
 * AFE_ADDA_DL_SDM_DCCOMP_CON gets the published driver's level in its low six bits and no source
 * writes the rest.
 */
static const struct step afe_probe[] = {
	{ B_AFE, AUDIO_TOP_CON0, 1u << 6, 1u << 6, 0 },
};

/* Speaker_Amp_Change(true), the rest, as traced from 158.545519. */
static const struct step codec_spk_on[] = {
	{ B_PMIC, PMIC_AUDNCP_CLKDIV_CON3, 0x0000, 0, 250 },    /* the charge pump */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON12, 0x1055, 0x1055, 0 },
	{ B_PMIC, PMIC_AUDDEC_ANA_CON13, 0x0001, 0, 100 },
	{ B_PMIC, PMIC_AUDDEC_ANA_CON8, 0x0000, 0, 0 },         /* whole: not in the published source */
	{ B_PMIC, PMIC_ZCD_CON0, 0x0100, 0, 0 },                /* whole, as the trace shows it written: each replaces the last */
	{ B_PMIC, PMIC_ZCD_CON0, 0x000a, 0, 0 },
	{ B_PMIC, PMIC_ZCD_CON0, 0x000b, 0, 0 },
	{ B_PMIC, PMIC_AUDDEC_ANA_CON10, 0x0055, 0, 0 },
	{ B_PMIC, PMIC_AUDDEC_ANA_CON4, 0x0110, 0, 0 },
	{ B_PMIC, PMIC_AUDDEC_ANA_CON4, 0x0112, 0, 0 },
	{ B_PMIC, PMIC_AUDDEC_ANA_CON4, 0x0113, 0, 0 },
	{ B_PMIC, PMIC_ZCD_CON1, 0x0000, 0x0f9f, 0 },           /* apply_speaker_gain: the line output's gain, Android's */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON11, 1, 1, 0 },
	{ B_PMIC, PMIC_AUDDEC_ANA_CON0, 0x3009, 0, 0 },
	{ B_PMIC, PMIC_AUDDEC_ANA_CON6, 0x0201, 0, 0 },
	{ B_PMIC, PMIC_AUDDEC_ANA_CON4, 0x011b, 0, 0 },
};

/*
 * The amplifier, between the last write above and the one below: Android's aw87xxx driver writes
 * its "Off" profile (the first line), then "Music" (aw87xxx_dev_reg_update in logs/aud.dmesg; the
 * i2c_write events at 158.546028 to 158.555136). The one wait is the profile's own (its "reg=0xfe
 * ... delay 2 ms"). Read back afterwards under Android: register 0x00 is 0x76, 0x01 is 0x07.
 */
static const struct step amp_on[] = {
	{ B_AMP, 0x01, 0x00, 0, 0 },
	{ B_AMP, 0x02, 0x07, 0, 0 }, { B_AMP, 0x02, 0x00, 0, 0 }, { B_AMP, 0x02, 0x0c, 0, 0 }, { B_AMP, 0x66, 0x38, 0, 0 },
	{ B_AMP, 0x01, 0x03, 0, 2000 },
	{ B_AMP, 0x67, 0x23, 0, 0 }, { B_AMP, 0x02, 0x07, 0, 0 }, { B_AMP, 0x02, 0x00, 0, 0 }, { B_AMP, 0x02, 0x1c, 0, 0 },
	{ B_AMP, 0x03, 0x08, 0, 0 }, { B_AMP, 0x04, 0x05, 0, 0 }, { B_AMP, 0x05, 0x0c, 0, 0 }, { B_AMP, 0x06, 0x07, 0, 0 },
	{ B_AMP, 0x07, 0x4e, 0, 0 }, { B_AMP, 0x08, 0x06, 0, 0 }, { B_AMP, 0x09, 0x08, 0, 0 }, { B_AMP, 0x0a, 0x4b, 0, 0 },
	{ B_AMP, 0x61, 0xb3, 0, 0 }, { B_AMP, 0x62, 0x24, 0, 0 }, { B_AMP, 0x63, 0x09, 0, 0 }, { B_AMP, 0x64, 0x24, 0, 0 },
	{ B_AMP, 0x65, 0x15, 0, 0 }, { B_AMP, 0x79, 0x7a, 0, 0 }, { B_AMP, 0x7a, 0x6c, 0, 0 }, { B_AMP, 0x78, 0x80, 0, 0 },
	{ B_AMP, 0x66, 0x38, 0, 0 }, { B_AMP, 0x76, 0x00, 0, 0 }, { B_AMP, 0x78, 0x00, 0, 0 }, { B_AMP, 0x68, 0x1b, 0, 0 },
	{ B_AMP, 0x69, 0x5b, 0, 0 }, { B_AMP, 0x70, 0x1c, 0, 0 }, { B_AMP, 0x71, 0x00, 0, 0 }, { B_AMP, 0x72, 0xfe, 0, 0 },
	{ B_AMP, 0x73, 0x4f, 0, 0 }, { B_AMP, 0x74, 0x24, 0, 0 }, { B_AMP, 0x75, 0x02, 0, 0 },
	{ B_AMP, 0x01, 0x07, 0, 0 },
	{ B_AMP, 0xff, 0x50, 0, 0 },
};

/* The end of Speaker_Amp_Change(true): hp_pull_down(false), traced as 233 at 158.555468. */
static const struct step codec_on_end[] = {
	{ B_PMIC, PMIC_AUDDEC_ANA_CON2, 0, 0x0400, 0 },
};

/*
 * Speaker_Amp_Change(false), as traced from 176.163985, up to where the r1's driver switches the
 * amplifier off: the driver's input opened, the DAC off, the gain to its least, the driver, the
 * common mode, the regulators and the charge pump off.
 */
static const struct step codec_off[] = {
	{ B_PMIC, PMIC_AUDDEC_ANA_CON4, 0, 3u << 2, 0 },        /* traced 113 */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON2, 0x0400, 0x0400, 0 },    /* hp_pull_down(true): 633 */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON0, 0, 0x000f, 0 },         /* 3000 */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON11, 0, 1, 0 },             /* 0 */
	{ B_PMIC, PMIC_ZCD_CON1, 0x0f9f, 0, 0 },                /* DL_GAIN_N_40DB_REG */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON4, 0, 1u << 0, 0 },        /* 112 */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON4, 0, 1u << 1, 0 },        /* 110 */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON6, 0, 0xff00, 0 },         /* 1 */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON6, 0x0200, 0xff00, 0 },    /* 201 */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON7, 0x00a8, 0x00ff, 0 },    /* a8 */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON10, 0x0100, 0x0100, 0 },   /* 155 */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON8, 0x0004, 0, 0 },         /* whole: not in the published source */
	{ B_PMIC, PMIC_ZCD_CON0, 0x0000, 0, 0 },                /* whole: not in the published source's way down */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON13, 0, 1, 0 },             /* 0 */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON12, 0, 0x1055, 0 },        /* 0 */
	{ B_PMIC, PMIC_AUDNCP_CLKDIV_CON3, 1, 1, 0 },           /* 1 */
};

/* The amplifier off (176.164174): its "Off" profile is this one write. */
static const struct step amp_off[] = {
	{ B_AMP, 0x01, 0x00, 0, 0 },
};

/* TurnOffDacPower, as traced from 176.164470: the wait is its udelay(250). */
static const struct step dac_off[] = {
	{ B_PMIC, PMIC_AFE_DL_SRC2_CON0_L, 0x0000, 0, 0 },      /* setDlMtkifSrc(false) */
	{ B_PMIC, PMIC_AFUNC_AUD_CON2, 0x0000, 0, 0 },
	{ B_PMIC, PMIC_AFUNC_AUD_CON0, 0xcba0, 0, 0 },
	{ B_PMIC, PMIC_AFE_UL_DL_CON0, 0, 1, 0, USE_UL },
	{ B_TOPCON, 0, 0, 0, 250 },
	{ B_PMIC, PMIC_GPIO_MODE2_CLR, 0xffff, 0, 0 },          /* set_playback_gpio(false) */
	{ B_PMIC, PMIC_SMT_CON1, 0, 0x0ff0, 0 },                /* traced f */
	{ B_PMIC, PMIC_AUD_TOP_CKPDN_CON0, 0x0066, 0x0066, 0, USE_UL }, /* Topck_Enable(false) */
	{ B_PMIC, PMIC_AUDENC_ANA_CON6, 0, 1, 0, USE_UL },              /* ClsqEnable(false): a0 */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON2, 0, 0x0400, 0 },         /* hp_pull_down(false): 233 */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON11, 1u << 4, 1u << 4, 0, USE_UL }, /* NvregEnable(false): 10 */
	{ B_PMIC, PMIC_DCXO_CW14, 0, 0x2000, 0, USE_UL },               /* audckbufEnable(false): 82b5 */
};

/* mtk_pcm_dl1_stop, as traced from 164.601345: the memory no longer read, the connections undone. */
static const struct step dl1_stop[] = {
	{ B_AFE, AFE_DAC_CON0, 0, 1u << 1, 0 },
	{ B_AFE, AFE_CONN3, 0, 1u << 5, 0 },
	{ B_AFE, AFE_CONN4, 0, 1u << 6, 0 },
	{ B_AFE, AFE_CONN28, 0, 1u << 5, 0 },
	{ B_AFE, AFE_CONN29, 0, 1u << 6, 0 },
};

/*
 * mtk_soc_pcm_dl1_close -> SetI2SDacEnable(false), EnableAfe(false), as traced from 164.602485
 * (83001802, a0a, 0, 0): the wait is SetI2SDacEnable's usleep_range(125, 150), a sample at 8 kHz.
 * set_chip_afe_enable then waits for AFE_DAC_MON to say the AFE has stopped (code).
 */
static const struct step dl1_close[] = {
	{ B_AFE, AFE_ADDA_DL_SRC2_CON0, 0, 1, 0 },
	{ B_AFE, AFE_I2S_CON1, 0, 1, 0 },
	{ B_AFE, AFE_ADDA_UL_DL_CON0, 0, 1, 150, USE_UL },
	{ B_AFE, AFE_DAC_CON0, 0, 1, 0, USE_UL },
};

/* AudDrv_Clk_Off, as traced from 164.602855: AudDrv_Clk_On backwards, the AFE's interrupts cleared on the way. */
static const struct step clocks_off[] = {
	{ B_INFRA, 0xa4, 0x00000010, 0, 0, USE_BOTH },            /* MODULE_SW_CG_2_SET: ifr_audio26m */
	{ B_AFE, AFE_IRQ_MCU_CLR, 0xffff, 0, 0, USE_BOTH },
	{ B_AFE, AFE_IRQ_MCU_CLR, 0xffff, 0, 0, USE_BOTH },
	{ B_AFE, AUDIO_TOP_CON0, 1u << 2, 1u << 2, 0, USE_BOTH },
	{ B_AFE, AUDIO_TOP_CON0, 1u << 25, 1u << 25, 0, USE_BOTH },
	{ B_AFE, AUDIO_TOP_CON0, 1u << 26, 1u << 26, 0, USE_BOTH },
	{ B_TOP, 0x84, 0x00000080, 0, 0, USE_BOTH },              /* CLK_CFG_4_SET */
	{ B_TOP, 0x74, 0x80000000, 0, 0, USE_BOTH },              /* CLK_CFG_3_SET */
	{ B_INFRA, 0x88, 0x02000000, 0, 0, USE_BOTH },            /* MODULE_SW_CG_1_SET */
};

/*
 * mtkaudio=4, set_chip_sine_gen_enable (mt6765/mt6765-sound.c) for the outputs O3/O4: 0x04ac2ac1 and
 * 0x22 are also what the trace has at 160.630539. The amplitude there is half of full scale (6 in bits
 * 7:5 and 19:17, Soc_Aud_SGEN_AMP_DIV_2); here it is a thirty-second (2, Soc_Aud_SGEN_AMP_DIV_32), put
 * in the way set_chip_sine_gen_amplitude puts it.
 */
static const struct step sgen_on[] = {
	{ B_AFE, AFE_SGEN_CON0, 0x04ac2ac1, 0, 0 },
	{ B_AFE, AFE_SGEN_CON0, 2u << 17, 7u << 17, 0 },
	{ B_AFE, AFE_SGEN_CON0, 2u << 5, 7u << 5, 0 },
	{ B_AFE, AFE_SGEN_CON2, 0x22, 0x3f, 0 },
};
static const struct step sgen_off[] = {
	{ B_AFE, AFE_SGEN_CON0, 0, 0, 0 },
	{ B_AFE, AFE_SGEN_CON2, 0x3f, 0x3f, 0 },
};

/*
 * mtkaudio=5, the codec's own sine table into its DAC. Linux sound/soc/codecs/mt6357.c has it as the
 * route "DAC Mux" = "Sine Generator": the supplies AUDIO_TOP_PDN_AFE_TESTMODEL (bit 1 of the codec's
 * AUDIO_TOP_CON0, cleared), "SGEN Enable" (bit 7 of AFE_SGEN_CFG0), "SGEN MUTE" (bit 6, clear) and the
 * mux itself (bit 0 of AFE_TOP_CON0, DL_SINE_ON). The two configuration words are the ones the
 * published driver writes for the same table towards the microphone's side (SineTable_UL2_Set: 0x0080
 * and 0x0101). The published "SineTable_DAC_HP" control is empty; how loud the table is, no source says:
 * the line output's gain is turned down first (apply_speaker_gain, by code: mtkaudio_sgen_gain).
 */
static const struct step pmic_sgen_on[] = {
	{ B_PMIC, PMIC_AUDIO_TOP_CON0, 0, 1u << 1, 0 },
	{ B_PMIC, PMIC_AFE_SGEN_CFG0, 0x0080, 0, 0 },
	{ B_PMIC, PMIC_AFE_SGEN_CFG1, 0x0101, 0, 0 },
	{ B_PMIC, PMIC_AFE_TOP_CON0, 1, 1, 0 },
};
static const struct step pmic_sgen_off[] = {
	{ B_PMIC, PMIC_AFE_TOP_CON0, 0, 1, 0 },
	{ B_PMIC, PMIC_AFE_SGEN_CFG0, 0x0000, 0, 0 },
	{ B_PMIC, PMIC_AUDIO_TOP_CON0, 1u << 1, 1u << 1, 0 },
	{ B_PMIC, PMIC_ZCD_CON1, 0x0000, 0x0f9f, 0 },           /* the gain back to Android's (traced: 0) */
};

/*
 * ---- the microphone (boot-arg mtkmic=N): the uplink, VUL, of the same AFE and the same codec ----
 *
 * docs/microphone-spec.md has every value with where it comes from. Tags in the comments: V is from
 * MediaTek's published driver of this generation (MiCode/Xiaomi_Kernel_OpenSource, dandelion-q-oss:
 * mtk-soc-codec-6357.c TurnOnADcPowerACC/DCC, mtk-soc-pcm-capture.c, mtk-soc-afe-control.c,
 * mt6765-sound.c, mtk-auddrv-afe.h, mtk-auddrv-gpio.c; Linux sound/soc/codecs/mt6357.c), R1 is from the
 * r1's own stock firmware, G is a guess with a boot-arg (mtkmic_*) or a constant to tune without a build.
 * The shape (one thread, up when the node is open and down when it closes, the codec's input before the
 * AFE's, the uplink source off before the ADCs before the bias) follows Open Mapple's MT6781 port:
 * xnu_kernel_mediatek_mt6781/osfmk/arm64/mt6781_afe.c (ul_start/ul_position/ul_stop) and
 * xnu_kernel_mediatek_common/bsd/dev/mtk_audio.c (mic_codec_up/down, mic_poll), APSL, adapted.
 */
#define UL_BYTES                0x4000u         /* the uplink's buffer: 16 KB = 4096 stereo frames = 85 ms (G, as DL1) */
#define UL_FRAMES               (UL_BYTES / 4)
#define UL_SRAM_PHYS_DEFAULT    0x11225000u     /* G: the SRAM right after DL1's 16 KB (Open Mapple's UL_SRAM = AFE_SRAM + 0x4000); mtkmic_vul= */
#define UL_SRAM_LAST            (SRAM_PHYS + 0x9000u)

/* C1..C10, the codec's common parts first (C0, the rails, is code: rails_on()). Common parts are left out while the speaker is up. */
static const struct step ul_codec_a[] = {
	{ B_PMIC, PMIC_DCXO_CW14, 0x2000, 0x2000, 0, USE_DL },          /* C1 audckbufEnable (V, T) */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON11, 0, 0x0010, 0, USE_DL },        /* C2 NvregEnable (V, T) */
	{ B_PMIC, PMIC_AUDENC_ANA_CON6, 1, 1, 0, USE_DL },              /* C3 ClsqEnable, the 26 MHz CLKSQ (V, T) */
	{ B_PMIC, PMIC_GPIO_MODE3_CLR, 0xffff, 0, 0 },                  /* C4 set_capture_gpio(true) (V) */
	{ B_PMIC, PMIC_GPIO_MODE3_SET, 0x0249, 0, 0 },                  /* C5 PMIC GPIO 12..15 = CLK_MISO, DAT_MISO0/1, SYNC_MISO (V) */
	{ B_PMIC, PMIC_GPIO_MODE3, 0x0249, 0, 0 },                      /* C6 the vendor writes it too (V) */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON11, 0x0020, 0x0020, 0 },           /* C7 ADC CLKGEN on (V) */
	{ B_PMIC, PMIC_AUDENC_ANA_CON3, 0x0000, 0, 0 },                 /* C8 ADC clock from CLKGEN, 13 MHz (V) */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON12, 0x0100, 0x2500, 0 },           /* C9 LCLDO_ENC on (V) */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON12, 0x2500, 0x2500, 0 },           /* C10 its remote sense; the speaker owns 0x1055 of this register (V) */
};
/* DCC only (mtkmic_mode=1, TurnOnADcPowerDCC), after C10: the DC calibration's clock */
static const struct step ul_codec_dcc_clock[] = {
	{ B_PMIC, PMIC_AFE_DCCLK_CFG0, 0x0402, 0, 0 },
	{ B_PMIC, PMIC_AFE_DCCLK_CFG0, 0x0402, 0, 0 },
	{ B_PMIC, PMIC_AFE_DCCLK_CFG0, 0x0400, 0, 0 },
	{ B_PMIC, PMIC_AFE_DCCLK_CFG0, 0x0401, 0, 0 },
};
/* C18..C25, after the inputs (C11..C17, ul_codec_inputs()) */
static const struct step ul_codec_b[] = {
	{ B_PMIC, PMIC_AUD_TOP_CKPDN_CON0, 0, 0x0066, 250, USE_DL },    /* C18 Topck_Enable (V, T) */
	{ B_TOPCON, 0, 0, 0, 0 },                                       /* C19 power on the clocks: 0x8040 alone, 0x8000 with the speaker (see top_con0) */
	{ B_PMIC, PMIC_AFE_TOP_CON0, 0x0000, 0, 0 },                    /* C20 normal paths, no sine (V) */
	{ B_PMIC, PMIC_AFE_UL_DL_CON0, 1, 1, 0 },                       /* C21 the codec's digital AFE on; common, the speaker sets it too (V) */
	{ B_PMIC, PMIC_MTKAIF_CFG0, 0x0000, 0, 0 },                     /* C22 MTKAIF TX format, protocol 1 (V) */
	{ B_PMIC, PMIC_AFE_AUD_PAD_TOP, 0x3100, 0xff00, 0 },            /* C23 aud_pad TX FIFOs on; the speaker owns the low byte (V: mainline AUD_PAD_TX_FIFO_NORMAL_PATH_ENABLE) */
	{ B_PMIC, PMIC_AFE_UL_SRC_CON0_H, 0x0000, 0, 0 },               /* C24 not DMIC (V) */
	{ B_PMIC, PMIC_AFE_UL_SRC_CON0_L, 0x0001, 0, 0 },               /* C25 UL SRC on (V) */
};

/* The codec's uplink power-down (TurnOnADcPowerACC(false), V): the uplink source off before the ADCs, the ADCs before the bias. */
static const struct step ul_codec_off_a[] = {
	{ B_PMIC, PMIC_AFE_UL_SRC_CON0_L, 0, 1, 0 },
	{ B_PMIC, PMIC_AFE_AUD_PAD_TOP, 0x3000, 0xff00, 0 },
	{ B_PMIC, PMIC_AFE_UL_DL_CON0, 0, 1, 0, USE_DL },               /* only if the speaker is down */
	{ B_TOPCON, 0, 0, 0, 0 },                                       /* computed with the microphone already gone from `users' */
};
static const struct step ul_codec_off_b[] = {
	{ B_PMIC, PMIC_AUDENC_ANA_CON0, 0, 0xf000, 0 },
	{ B_PMIC, PMIC_AUDENC_ANA_CON0, 0, 0x0fff, 0 },
	{ B_PMIC, PMIC_AUDENC_ANA_CON1, 0, 0xf000, 0 },
	{ B_PMIC, PMIC_AUDENC_ANA_CON1, 0, 0x0fff, 0 },
	{ B_PMIC, PMIC_AUDENC_ANA_CON8, 0x0000, 0, 0 },                 /* MICBIAS0 off */
	{ B_PMIC, PMIC_AUDENC_ANA_CON9, 0, 1, 0 },                      /* MICBIAS1 off (only on for AIN1, harmless otherwise) */
	{ B_PMIC, PMIC_AUDDEC_ANA_CON12, 0x0100, 0x2500, 0 },
	{ B_PMIC, PMIC_AUDDEC_ANA_CON12, 0, 0x2500, 0 },
	{ B_PMIC, PMIC_AUDENC_ANA_CON3, 0, 0, 0 },
	{ B_PMIC, PMIC_AUDDEC_ANA_CON11, 0, 0x0020, 0 },
	{ B_PMIC, PMIC_GPIO_MODE3_CLR, 0xffff, 0, 0 },                  /* set_capture_gpio(false) */
	{ B_PMIC, PMIC_GPIO_MODE3, 0x0000, 0, 0 },
	{ B_PMIC, PMIC_GPIO_DIR0, 0, 0xf000, 0 },
	{ B_PMIC, PMIC_AUD_TOP_CKPDN_CON0, 0x0066, 0x0066, 0, USE_DL }, /* the common parts, only if the speaker is down */
	{ B_PMIC, PMIC_AUDENC_ANA_CON6, 0, 1, 0, USE_DL },
	{ B_PMIC, PMIC_AUDDEC_ANA_CON11, 1u << 4, 1u << 4, 0, USE_DL },
	{ B_PMIC, PMIC_DCXO_CW14, 0, 0x2000, 0, USE_DL },
};

/*
 * The AFE's side, A1..A22 (mtk_capture_pcm_open/hw_params/prepare, set_adc_in, set_adc_enable; V). The
 * buffer's address and end (A4, A5) and the clearing of it (A6) are code (ul_afe_on()).
 */
static const struct step ul_afe_a[] = {
	{ B_AFE, AUDIO_TOP_CON0, 0, 1u << 24, 0 },                      /* A1 AudDrv_ADC_Clk_On: the aud_adc gate */
	{ B_AFE, AFE_MEMIF_HDALIGN, 0x7fff0000, 0x7fff0000, 0 },        /* A2 set_sram_mode normal, as DL1 (T) */
	{ B_AFE, AFE_MEMIF_MSB, 0, 1u << 29, 0 },                       /* A3 */
	{ B_AFE, AFE_MEMIF_MSB, 0, 1u << 28, 0 },                       /* A7 SetMemIfFormatReg: 16 bits */
	{ B_AFE, AFE_MEMIF_HDALIGN, 0, 1u << 5, 0 },                    /* A8 VUL isAlign = 0 */
	{ B_AFE, AFE_MEMIF_HD_MODE, 0, 3u << 10, 0 },                   /* A9 VUL isHD = 0 */
	{ B_AFE, AFE_CONN_24BIT, 0, 3u << 9, 0 },                       /* A10 O09/O10 are 16-bit */
	{ B_AFE, AFE_AUD_PAD_TOP_CFG, 0x00000031, 0, 0 },               /* A11 set_chip_adc_in: aud_pad_top fifo on, after the GPIO and the PMIC's MISO clock */
	{ B_AFE, AFE_ADDA_MTKAIF_CFG0, 0, 0, 0 },                       /* A12 reset MTKAIF_CFG0: no loopback, protocol 1 */
	{ B_AFE, AFE_ADDA_TOP_CON0, 0, 1, 0 },                          /* A13 the internal ADC */
	{ B_AFE, AFE_ADDA_IIR_COEF_02_01 + 0x0, 0x00000000, 0, 0 },     /* A14 the software IIR: a 35 Hz high-pass at 48 kHz (V; Open Mapple has the same) */
	{ B_AFE, AFE_ADDA_IIR_COEF_02_01 + 0x4, 0x00003fb8, 0, 0 },     /* A15 */
	{ B_AFE, AFE_ADDA_IIR_COEF_02_01 + 0x8, 0x3fb80000, 0, 0 },     /* A16 */
	{ B_AFE, AFE_ADDA_IIR_COEF_02_01 + 0xc, 0x3fb80000, 0, 0 },     /* A17 */
	{ B_AFE, AFE_ADDA_IIR_COEF_02_01 + 0x10, 0x0000c048, 0, 0 },    /* A18 */
	{ B_AFE, AFE_ADDA_UL_SRC_CON0, 0x00060400, 0, 0 },              /* A19 48 kHz (bits 19:17 = 3), IIR on (bit 10), UL_IIR_SW (V; Open Mapple's UL_SRC_CON0_48K) */
	{ B_AFE, AFE_ADDA_MTKAIF_RX_CFG0, 0, 1, 0 },                    /* A20 rxif data mode: analog mic */
	{ B_AFE, AFE_ADDA_UL_DL_CON0, 1, 1, 0 },                        /* A21 SetADDAEnable; common, the speaker sets it too */
	{ B_AFE, AFE_ADDA_UL_SRC_CON0, 1, 1, 0 },                       /* A22 UL SRC on: 0x00060401 */
};
/* A24..A29, after the pins */
static const struct step ul_afe_b[] = {
	{ B_AFE, AFE_CONN9, 1u << 3, 1u << 3, 0 },                      /* A24 I03 to O09 (SetAdcToVul) */
	{ B_AFE, AFE_CONN10, 1u << 4, 1u << 4, 0 },                     /* A25 I04 to O10 */
	{ B_AFE, AFE_DAC_CON1, 10u << 16, 0xfu << 16, 0 },              /* A26 VUL at 48 kHz (I2S_48K = 10) */
	{ B_AFE, AFE_DAC_CON1, 0, 1u << 27, 0 },                        /* A27 VUL stereo */
	{ B_AFE, AFE_DAC_CON0, 1u << 3, 1u << 3, 0 },                   /* A28 the VUL memory interface on */
	{ B_AFE, AFE_DAC_CON0, 1, 1, 0 },                               /* A29 the AFE on; common */
};
/* The AFE's uplink power-down (mtk_capture_alsa_stop, pcm_close, set_adc_enable(false); V) */
static const struct step ul_afe_off_a[] = {
	{ B_AFE, AFE_DAC_CON0, 0, 1u << 3, 0 },
	{ B_AFE, AFE_CONN9, 0, 1u << 3, 0 },
	{ B_AFE, AFE_CONN10, 0, 1u << 4, 0 },
	{ B_AFE, AFE_ADDA_UL_SRC_CON0, 0, 1, 0 },
	{ B_AFE, AFE_ADDA_UL_DL_CON0, 0, 1, 150, USE_DL },              /* only if the speaker is down */
	{ B_AFE, AUDIO_TOP_CON0, 1u << 24, 1u << 24, 0 },               /* the ADC clock gated */
};
static const struct step ul_afe_off_b[] = {
	{ B_AFE, AFE_DAC_CON0, 0, 1, 0, USE_DL },                       /* only if the speaker is down */
};

static inline uint32_t
rd(unsigned b, uint32_t reg)
{
	return *(volatile uint32_t *)(block[b] + reg);
}

static inline void
wr(unsigned b, uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(block[b] + reg) = value;
}

static uint32_t
now_ms(void)
{
	uint64_t ns = 0;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return (uint32_t)(ns / NSEC_PER_MSEC);
}

/*
 * The PMIC, through the wrapper's WACS2 channel as redwood_connsys.c and redwood_mtkkeys.c drive it:
 * one transaction at a time among all of them (redwood_pwrap_lock), and a change of some bits of a
 * register is one transaction.
 */
#define PWRAP_PHYS              0x1000d000u
#define PWRAP_WACS2_CMD         0xc20
#define PWRAP_WACS2_RDATA       0xc24
#define PWRAP_WACS2_VLDCLR      0xc28
#define PWRAP_FSM(r)            (((r) >> 16) & 7u)
#define PWRAP_IDLE              0
#define PWRAP_WFVLDCLR          6

static vm_offset_t pwrap;

static bool
pwrap_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 100000; spin++) {
		if (PWRAP_FSM(*(volatile uint32_t *)(pwrap + PWRAP_WACS2_RDATA)) == state) {
			return true;
		}
	}
	return false;
}

static bool
pmic_read_raw(uint32_t reg, uint32_t *value)
{
	if (!pwrap_wait(PWRAP_IDLE)) {
		return false;
	}
	*(volatile uint32_t *)(pwrap + PWRAP_WACS2_CMD) = (reg >> 1) << 16;
	if (!pwrap_wait(PWRAP_WFVLDCLR)) {
		return false;
	}
	*value = *(volatile uint32_t *)(pwrap + PWRAP_WACS2_RDATA) & 0xffff;
	*(volatile uint32_t *)(pwrap + PWRAP_WACS2_VLDCLR) = 1;
	return true;
}

static bool
pmic_write_raw(uint32_t reg, uint32_t value)
{
	if (!pwrap_wait(PWRAP_IDLE)) {
		return false;
	}
	*(volatile uint32_t *)(pwrap + PWRAP_WACS2_CMD) = (1u << 31) | (reg >> 1) << 16 | (value & 0xffff);
	return pwrap_wait(PWRAP_IDLE);
}

static bool
pmic_read(uint32_t reg, uint32_t *value)
{
	bool ok;

	redwood_pwrap_lock();
	ok = pmic_read_raw(reg, value);
	redwood_pwrap_unlock();
	return ok;
}

static bool
pmic_set(uint32_t reg, uint32_t value, uint32_t mask)
{
	uint32_t old = 0;
	bool ok = true;

	redwood_pwrap_lock();
	if (mask != 0) {
		ok = pmic_read_raw(reg, &old);
		value = (old & ~mask) | (value & mask);
	}
	ok = ok && pmic_write_raw(reg, value);
	redwood_pwrap_unlock();
	return ok;
}

/*
 * The pins (MT6765: four bits of function a pin, eight pins a register from 0x300; direction, output
 * and input in banks of 32), and I2C0 driven by hand on its two, the way redwood_hyn.c drives I2C4.
 */
#define PIN_SDA                 82
#define PIN_SCL                 83
#define AMP_ADDR                0x58

/*
 * The link to the PMIC: AUD_CLK_MOSI, AUD_DAT_MOSI0 and AUD_DAT_MOSI1 are function 1 of these pins
 * (mt6765-pinfunc.h), and no other pin has them. Android gives the pins that function while the DAC
 * interface runs and takes it away after (AudDrv_GPIO_Request, through pinctrl: no register trace has
 * it). Here they get it at power-up and what they had before at power-down.
 */
static const uint8_t link_pins[] = { 136, 138, 139 };
#define LINK_FUNCTION           1
static uint8_t link_before[sizeof(link_pins)];

static uint32_t
pin_mode(uint32_t pin)
{
	return (rd(B_GPIO, 0x300 + 0x10 * (pin / 8)) >> ((pin % 8) * 4)) & 0xf;
}

static void
pin_set_mode(uint32_t pin, uint32_t mode)
{
	uint32_t at = 0x300 + 0x10 * (pin / 8), shift = (pin % 8) * 4;

	wr(B_GPIO, at, (rd(B_GPIO, at) & ~(0xfu << shift)) | (mode << shift));
}

static inline bool
pin_get(uint32_t pin)
{
	return (rd(B_GPIO, 0x200 + 0x10 * (pin / 32)) >> (pin % 32)) & 1u;
}

/* open drain: released (an input, the pull-up takes it high) or driven low */
static void
line(uint32_t pin, bool high)
{
	if (high) {
		wr(B_GPIO, 0x008 + 0x10 * (pin / 32), 1u << (pin % 32));
	} else {
		wr(B_GPIO, 0x108 + 0x10 * (pin / 32), 1u << (pin % 32));
		wr(B_GPIO, 0x004 + 0x10 * (pin / 32), 1u << (pin % 32));
	}
	delay(3);
}

static void
scl_high(void)
{
	line(PIN_SCL, true);
	for (int i = 0; i < 200 && !pin_get(PIN_SCL); i++) {    /* the chip may hold it low */
		delay(5);
	}
}

static void
i2c_start(void)
{
	line(PIN_SDA, true);
	scl_high();
	line(PIN_SDA, false);
	line(PIN_SCL, false);
}

static void
i2c_stop(void)
{
	line(PIN_SDA, false);
	scl_high();
	line(PIN_SDA, true);
}

static bool
i2c_out(uint8_t byte)
{
	bool ack;

	for (int i = 7; i >= 0; i--) {
		line(PIN_SDA, (byte >> i) & 1);
		scl_high();
		line(PIN_SCL, false);
	}
	line(PIN_SDA, true);
	scl_high();
	ack = !pin_get(PIN_SDA);
	line(PIN_SCL, false);
	return ack;
}

static uint8_t
i2c_in(void)            /* one byte, the last: not acknowledged */
{
	uint8_t byte = 0;

	line(PIN_SDA, true);
	for (int i = 0; i < 8; i++) {
		scl_high();
		byte = (uint8_t)((byte << 1) | (pin_get(PIN_SDA) ? 1 : 0));
		line(PIN_SCL, false);
	}
	scl_high();
	line(PIN_SCL, false);
	return byte;
}

static bool
amp_write(uint8_t reg, uint8_t value)
{
	bool ok;

	i2c_start();
	ok = i2c_out(AMP_ADDR << 1) && i2c_out(reg) && i2c_out(value);
	i2c_stop();
	return ok;
}

static bool
amp_read(uint8_t reg, uint8_t *value)
{
	bool ok;

	i2c_start();
	ok = i2c_out(AMP_ADDR << 1) && i2c_out(reg);
	i2c_stop();
	if (!ok) {
		return false;
	}
	i2c_start();
	ok = i2c_out((AMP_ADDR << 1) | 1);
	if (ok) {
		*value = i2c_in();
	}
	i2c_stop();
	return ok;
}

static const char *const block_name[] = { "AFE", "infracfg", "topckgen", "GPIO", "PMIC", "amplifier", "PMIC" };

/*
 * The codec's AUDIO_TOP_CON0, the one register the speaker and the microphone fight over (the speaker's
 * 0x2b sets PDN_ADC and would silence a running microphone). Worked out after the change, from `users':
 *   neither  0x00ff, in its low byte only (as the speaker's power-down always wrote it)
 *   speaker  0x002b, whole (T: as traced)
 *   mic      0x8040, the vendor's 0x8000 / 0xdfbf applied to the idle 0x00ff (V)
 *   both     mic_top_both, 0x0000 (T: logs/mic.trace, Android writes 0 when the speaker starts under a recording; mtkmic_top_both=)
 */
static uint32_t mic_top_both = 0x0000;

static bool
top_con0(void)
{
	switch (users & USE_BOTH) {
	case USE_DL:
		return pmic_set(PMIC_AUDIO_TOP_CON0, 0x002b, 0);
	case USE_UL:
		return pmic_set(PMIC_AUDIO_TOP_CON0, 0x8040, 0);
	case USE_BOTH:
		return pmic_set(PMIC_AUDIO_TOP_CON0, mic_top_both, 0);
	default:
		return pmic_set(PMIC_AUDIO_TOP_CON0, 0x00ff, 0x00ff);
	}
}

/* One table, in order. Says what it is about to do; false at the first write the PMIC or the amplifier does not take. */
static bool
run(const char *what, const struct step *table, unsigned count)
{
	printf("mtkaudio: %s (%u writes)\n", what, count);
	for (unsigned i = 0; i < count; i++) {
		const struct step *s = &table[i];
		bool ok = true;

		if (s->shared & users) {
			continue;       /* the other path still uses it */
		}
		if (s->block == B_TOPCON) {
			ok = top_con0();
		} else if (s->block == B_PMIC) {
			ok = pmic_set(s->reg, s->value, s->mask);
		} else if (s->block == B_AMP) {
			ok = amp_write((uint8_t)s->reg, (uint8_t)s->value);
		} else if (s->mask != 0) {
			wr(s->block, s->reg, (rd(s->block, s->reg) & ~s->mask) | (s->value & s->mask));
		} else {
			wr(s->block, s->reg, s->value);
		}
		if (!ok) {
			printf("mtkaudio: %s: STOPPED at write %u of %u: the %s did not take %#x to register %#x (%s)\n", what, i + 1,
			    count, block_name[s->block], s->value, s->reg,
			    s->block == B_PMIC ? "the wrapper's WACS2 never came back to idle" : "no acknowledge on the bus");
			return false;
		}
		if (s->wait_us) {
			delay(s->wait_us);
		}
	}
	return true;
}

static uint32_t probe_skip;                     /* mtkaudio_skip0 */

/*
 * A table of "up 0/7": every write is said, with what the register held and what it holds after. Bit
 * `first` + i of mtkaudio_skip0 leaves write i out (the register is still read).
 */
static bool
run_probe(const char *what, const struct step *table, unsigned count, unsigned first)
{
	for (unsigned i = 0; i < count; i++) {
		const struct step *s = &table[i];
		uint32_t was = 0xdead, now = 0xdead;
		bool skip = (probe_skip >> (first + i)) & 1, ok = true;

		if (s->block == B_PMIC) {
			ok = pmic_read(s->reg, &was) && (skip || pmic_set(s->reg, s->value, s->mask)) && pmic_read(s->reg, &now);
		} else {
			was = rd(s->block, s->reg);
			if (!skip) {
				wr(s->block, s->reg, s->mask ? (was & ~s->mask) | (s->value & s->mask) : s->value);
			}
			now = rd(s->block, s->reg);
		}
		printf("mtkaudio: up 0/7, what the drivers set once at their probe, %s, write %u: %s register %#x was %#x, %s %#x%s\n",
		    what, first + i, block_name[s->block], s->reg, was, skip ? "LEFT OUT, is" : "now", now,
		    ok ? "" : " (the PMIC did NOT take it)");
		if (!ok) {
			return false;
		}
	}
	return true;
}

/*
 * Where the signal dies, as far as registers can say. Nothing in the sources says what the bits of
 * these monitors are (they are only named: codec/mt6357/mtk-auddrv-ana.h, mt6765/mtk-auddrv-afe.h, and
 * printed whole by the AFE driver's debug file), so they are not interpreted: each is read LOOKS times,
 * 50 ms apart, and what differs between two looks, or between a stage with a sine and the stage of
 * silence before it, is data moving there. The AFE's are read only once its clocks run.
 */
#define LOOKS                   4
static const struct {
	const char *name;
	uint32_t reg;
} pmic_monitors[] = {
	{ "AFUNC_AUD_MON0", PMIC_AFUNC_AUD_MON0 }, { "MTKAIF_MON0", PMIC_MTKAIF_MON0 }, { "MON1", PMIC_MTKAIF_MON0 + 2 },
	{ "MON2", PMIC_MTKAIF_MON0 + 4 }, { "MON3", PMIC_MTKAIF_MON0 + 6 }, { "FIFO_LOG_MON1", PMIC_MTKAIF_FIFO_LOG_MON1 },
	{ "PAD_TOP_MON", PMIC_AFE_AUD_PAD_TOP_MON }, { "PAD_TOP_MON1", PMIC_AFE_AUD_PAD_TOP_MON1 },
}, afe_monitors[] = {
	{ "AFE_DAC_MON", AFE_DAC_MON }, { "MTKAIF_MON0", AFE_ADDA_MTKAIF_MON0 }, { "MON1", AFE_ADDA_MTKAIF_MON1 },
	{ "DL_SDM_FIFO_MON", AFE_ADDA_DL_SDM_FIFO_MON }, { "DL_SRC_LCH_MON", AFE_ADDA_DL_SRC_LCH_MON },
	{ "DL_SDM_OUT_MON", AFE_ADDA_DL_SDM_OUT_MON },
};

static void
monitors(const char *when, bool afe)
{
	for (unsigned look = 1; look <= LOOKS; look++) {
		printf("mtkaudio: monitors, %s, look %u: the codec's", when, look);
		for (unsigned i = 0; i < COUNT(pmic_monitors); i++) {
			uint32_t value = 0xdead;

			pmic_read(pmic_monitors[i].reg, &value);
			printf(" %s %#x", pmic_monitors[i].name, value);
		}
		if (afe) {
			printf("; the AFE's");
			for (unsigned i = 0; i < COUNT(afe_monitors); i++) {
				printf(" %s %#x", afe_monitors[i].name, rd(B_AFE, afe_monitors[i].reg));
			}
		}
		printf("\n");
		delay_for_interval(50, NSEC_PER_MSEC);
	}
}

/* The amplifier's registers as Android's driver showed them while the Music profile was on (the reads of `cat` in logs/aud.trace, 172.103 s). */
static const uint8_t amp_playing[][2] = {
	{ 0x00, 0x76 }, { 0x01, 0x07 }, { 0x02, 0x1c }, { 0x03, 0x08 }, { 0x04, 0x05 }, { 0x05, 0x0c }, { 0x06, 0x07 }, { 0x07, 0x4e },
	{ 0x08, 0x06 }, { 0x09, 0x08 }, { 0x0a, 0x4b }, { 0x59, 0x4a }, { 0x60, 0xde }, { 0x61, 0xb3 }, { 0x62, 0x24 }, { 0x63, 0x09 },
	{ 0x64, 0x24 }, { 0x65, 0x15 }, { 0x66, 0x10 }, { 0x67, 0x23 }, { 0x68, 0x1b }, { 0x69, 0x5b }, { 0x70, 0x1c }, { 0x71, 0x00 },
	{ 0x72, 0xfe }, { 0x73, 0x4f }, { 0x74, 0x24 }, { 0x75, 0x02 }, { 0x76, 0x04 }, { 0x77, 0x00 }, { 0x78, 0x01 },
};

static void
amp_compare(void)
{
	unsigned differ = 0;

	for (unsigned i = 0; i < COUNT(amp_playing); i++) {
		uint8_t value = 0;
		bool read = amp_read(amp_playing[i][0], &value);

		if (!read || value != amp_playing[i][1]) {
			printf("mtkaudio: the amplifier's register %#04x %s %#04x; under Android it was %#04x\n", amp_playing[i][0],
			    read ? "is" : "was NOT READ, not", value, amp_playing[i][1]);
			differ++;
		}
	}
	printf("mtkaudio: the amplifier: %u of its %u registers read back as under Android, %u differ\n",
	    (unsigned)COUNT(amp_playing) - differ, (unsigned)COUNT(amp_playing), differ);
}

/*
 * The feeder.
 *
 * The buffer the hardware plays is the first 16 KB of the AFE's own sample memory, not DRAM: the
 * trace's AFE_DL1_BASE and AFE_DL1_END are 0x11221000 and 0x11224fff, inside audio_sram@11221000
 * (AllocateAudioSram succeeded in mtk_pcm_dl1_params). In its normal mode a word of it is one frame,
 * the left sample in the low half. The hardware reads it round and round at 48000 frames a second
 * whether or not anything new is in it, and says where it reads in AFE_DL1_CUR, as a physical address
 * (get_dlmem_frame_index in mtk-soc-afe-control.c: less the base, in bytes; 0 before the first read
 * stands for the base).
 *
 * Under Linux an interrupt every 1024 frames moves things along. There is none here: a thread looks
 * at the pointer every POLL_MS. What the hardware has read since the last look is free again; new
 * frames go in after the last ones written, never as far round as where the hardware reads. When
 * there is nothing to give it, silence is written to keep PAD_FRAMES ahead of it, so that it does not
 * play the last 85 ms again. If it has got ahead all the same (the thread was held up for longer than
 * that lead), what it played was old: an underrun is counted and writing begins again just ahead of it.
 * (Held up for longer than the whole buffer's 85 ms, a lap is not seen; the next looks put it right.)
 */
#define RING_FRAMES             16384           /* what iOS has written and the buffer has no room for yet: 0.34 s; a power of two */
#define POLL_MS                 5
#define PAD_FRAMES              480             /* 10 ms: two looks */
#define GUARD_FRAMES            64              /* never written closer than this behind where the hardware reads */
#define IDLE_MS                 1000            /* nothing written for this long: power down */
#define TONE_HZ_2COS            2143922726ll    /* 2 cos(2 pi 440/48000), Q30 */
#define TONE_HZ_SIN             3773            /* sin(2 pi 440/48000), Q16 */

static LCK_GRP_DECLARE(mtkaudio_lck_grp, "redwood-mtkaudio");
static LCK_MTX_DECLARE(mtkaudio_lock, &mtkaudio_lck_grp);

static volatile uint32_t *sram;                 /* the buffer, a frame a word */
static int16_t ring[RING_FRAMES * REDWOOD_AUDIO_CHANNELS];
static uint32_t ring_head, ring_tail;           /* in frames; play from head, add at tail */
static bool feed_wanted;                        /* the sink has frames and the thread should be playing them */
static bool feed_broken;                        /* a power-up failed: nothing more is taken */
static uint64_t feed_last_write;                /* mach_absolute_time() of the sink's latest call */
static uint32_t feed_gain = 32768;              /* of 32768: the volume */
static uint32_t volume_base = 96, volume_seen;  /* mtkaudio_below; the generation of iOS's setting last looked at */
static bool volume_known;

static uint32_t dl1_hw, dl1_write, dl1_queued;  /* where the hardware read at the last look; where we write next; frames between */
static uint32_t dl1_read, dl1_given, dl1_padded, dl1_underruns, dl1_bad;       /* since dl1_begin(): frames, frames, frames, times, pointers out of the buffer */

static uint32_t tone_left, tone_peak = 2000;    /* frames of the test tone still to make */
static int32_t tone_y1, tone_y2;                /* its last two samples, Q16 */

static uint32_t
dl1_position(void)
{
	uint32_t cur = rd(B_AFE, AFE_DL1_CUR);

	if (cur == 0) {
		return 0;
	}
	if (cur < SRAM_PHYS || cur >= SRAM_PHYS + DL1_BYTES) {
		dl1_bad++;
		return dl1_hw;
	}
	return (cur - SRAM_PHYS) / REDWOOD_AUDIO_FRAME;
}

static void
dl1_put(uint32_t frame)
{
	sram[dl1_write] = frame;
	dl1_write = (dl1_write + 1) % DL1_FRAMES;
	dl1_queued++;
}

/* With mtkaudio_lock held, and the buffer all zeros or stale: writing begins where the hardware reads. */
static void
dl1_begin(void)
{
	dl1_hw = dl1_write = dl1_position();
	dl1_queued = dl1_read = dl1_given = dl1_padded = dl1_underruns = dl1_bad = 0;
}

/* With mtkaudio_lock held: one look at the hardware's pointer (see above). */
static void
dl1_feed(void)
{
	uint32_t hw = dl1_position(), read = (hw - dl1_hw) % DL1_FRAMES, room;

	dl1_hw = hw;
	dl1_read += read;
	if (read > dl1_queued) {
		dl1_underruns++;
		dl1_write = hw;
		dl1_queued = 0;
	} else {
		dl1_queued -= read;
	}
	for (room = DL1_FRAMES - GUARD_FRAMES - dl1_queued; room != 0; room--) {
		int32_t left, right;

		if (tone_left != 0) {
			/* y[n] = 2 cos(w) y[n-1] - y[n-2] */
			int32_t y = (int32_t)((TONE_HZ_2COS * tone_y1) >> 30) - tone_y2;

			tone_y2 = tone_y1;
			tone_y1 = y;
			tone_left--;
			left = right = y >> 16;
		} else if (ring_head != ring_tail) {
			left = ring[2 * (ring_head % RING_FRAMES)] * (int32_t)feed_gain >> 15;
			right = ring[2 * (ring_head % RING_FRAMES) + 1] * (int32_t)feed_gain >> 15;
			ring_head++;
		} else {
			break;
		}
		dl1_put(((uint32_t)left & 0xffff) | ((uint32_t)right & 0xffff) << 16);
		dl1_given++;
	}
	while (dl1_queued < PAD_FRAMES) {
		dl1_put(0);
		dl1_padded++;
	}
}

/*
 * The volume, in the samples: iOS says how many eighths of a dB under the loudest (sys/redwood_audio.h),
 * the loudest is mtkaudio_below under full scale, and the sum scales what is fed. The codec's and the
 * amplifier's gains stay Android's. Not with mtkaudio_lock held: redwood_audio_volume() takes the
 * front end's lock, which the sink is called under.
 * ponytail: steps of 0.75 dB and no ramp between two settings; a table of 48 and a ramp if it is heard.
 */
static void
volume_look(void)
{
	static const uint16_t step[8] = { 32768, 30057, 27571, 25290, 23198, 21279, 19519, 17904 };    /* 10^(-0.75 i / 20), Q15 */
	uint32_t below = 0, generation = 0, steps, gain;
	bool muted = false;

	if (!redwood_audio_volume(&below, &muted, &generation)) {
		below = 0;
		muted = false;
	}
	if (volume_known && generation == volume_seen) {
		return;
	}
	steps = (volume_base + below) / 6;
	gain = (muted || steps / 8 > 15) ? 0 : (uint32_t)step[steps % 8] >> (steps / 8);
	printf("mtkaudio: the volume: %u.%03u dB under full scale%s (%u of 32768)\n", (volume_base + below) / 8,
	    (volume_base + below) % 8 * 125, muted ? ", muted" : "", gain);
	lck_mtx_lock(&mtkaudio_lock);
	feed_gain = gain;
	lck_mtx_unlock(&mtkaudio_lock);
	volume_seen = generation;
	volume_known = true;
}

/* The sink: see sys/redwood_audio.h. Called under the front end's lock, from the writer's thread: it only copies. */
static uint32_t
mtkaudio_sink(const int16_t *frames, uint32_t count)
{
	uint32_t room, taken;

	lck_mtx_lock(&mtkaudio_lock);
	if (feed_broken) {
		lck_mtx_unlock(&mtkaudio_lock);
		return 0;
	}
	room = RING_FRAMES - (ring_tail - ring_head);
	taken = count < room ? count : room;
	for (uint32_t i = 0; i < taken; i++, ring_tail++) {
		ring[2 * (ring_tail % RING_FRAMES)] = frames[2 * i];
		ring[2 * (ring_tail % RING_FRAMES) + 1] = frames[2 * i + 1];
	}
	feed_last_write = mach_absolute_time();
	if (!feed_wanted) {
		feed_wanted = true;
		wakeup(&feed_wanted);
	}
	lck_mtx_unlock(&mtkaudio_lock);
	return taken;
}

static bool afe_up;                             /* the AFE's clocks run (for either path): its registers may be touched */
static bool dl_afe;                             /* the speaker's part of the AFE is up: power_down() undoes it */
static uint32_t sgen_gain = 20;                 /* mtkaudio_sgen_gain: DL_GAIN_N_12DB */

/* Everything off, in the order Android went off: the AFE, then the codec and the amplifier. Safe on what is half up. */
static void
power_down(void)
{
	bool ok;

	users &= ~USE_DL;       /* before anything is done: the shared steps look at what is left */
	if (dl_afe) {
		bool mic_runs = (users & USE_UL) != 0;

		run("down 1/6: the AFE's memory interface and connections, mtk_pcm_dl1_stop", dl1_stop, COUNT(dl1_stop));
		run("down 2/6: the AFE's DAC interface and the AFE, mtk_soc_pcm_dl1_close", dl1_close, COUNT(dl1_close));
		for (int i = 0; !mic_runs && i < 1000 && (rd(B_AFE, AFE_DAC_MON) & 1); i++) {  /* at most 0.1 s; the source waits a second */
			delay(100);
		}
		for (unsigned i = 0; i < sizeof(link_pins); i++) {
			if (i == 0 && mic_runs) {
				continue;       /* pin 136 is the microphone's clock too: it restores it */
			}
			pin_set_mode(link_pins[i], link_before[i]);
		}
		printf("mtkaudio: the AFE %s (AFE_DAC_MON %#x); pins %u/%u/%u back to function %u/%u/%u\n",
		    (rd(B_AFE, AFE_DAC_MON) & 1) ? "has NOT stopped" : "has stopped", rd(B_AFE, AFE_DAC_MON), link_pins[0],
		    link_pins[1], link_pins[2], link_before[0], link_before[1], link_before[2]);
		if (!mic_runs) {
			run("down 3/6: the clocks and gates, AudDrv_Clk_Off", clocks_off, COUNT(clocks_off));
			afe_up = false;
		} else {
			printf("mtkaudio: down 3/6: the clocks and gates stay up: the microphone is recording\n");
		}
		dl_afe = false;
	}
	ok = run("down 4/6: the codec's line output and DAC, Speaker_Amp_Change(false)", codec_off, COUNT(codec_off));
	ok = run("down 5/6: the amplifier", amp_off, COUNT(amp_off)) && ok;
	ok = run("down 6/6: the codec's digital side and clocks, TurnOffDacPower", dac_off, COUNT(dac_off)) && ok;
	printf("mtkaudio: powered down%s\n", ok ? "" : " (not every write to the PMIC or the amplifier was taken)");
}

/* The rails (also the microphone's, when the speaker is not up). Said as found, then switched on. */
static void
rails_on(void)
{
	/*
	 * The rails Android has on from its start and so never writes while a sound plays (its regulators
	 * say "enabled" for each): VAUD28, the codec's analog supply, VAUX18, VIO28 and VLDO28. Bit 0 of each
	 * CON0 is the enable (Linux drivers/regulator/mt6357-regulator.c; include/linux/mfd/mt6357/registers.h).
	 * Said as found, then switched on.
	 */
	{
		static const struct { const char *name; uint32_t reg; } rails[] = {
			{ "VAUD28", 0x1930 }, { "VAUX18", 0x191c }, { "VIO28", 0x1944 }, { "VLDO28", 0x1b32 },
		};

		for (unsigned i = 0; i < COUNT(rails); i++) {
			uint32_t was = 0xdead, now = 0xdead;

			(void)pmic_read(rails[i].reg, &was);
			(void)pmic_set(rails[i].reg, 1, 1);
			(void)pmic_read(rails[i].reg, &now);
			printf("mtkaudio: rail %s: enable register %#x was %#x, now %#x\n", rails[i].name, rails[i].reg, was, now);
		}
		delay(2000);
	}
}

/* The codec and the amplifier: all of `tinymix Speaker_Amp_Switch On`. Nothing of the AFE is touched. */
static bool
power_up_analog(void)
{
	uint32_t a = 0xdead, b = 0xdead, c = 0xdead, d = 0xdead;
	uint8_t id = 0;
	bool answers;

	users |= USE_DL;
	rails_on();
	if (!run("up 1/7: the codec's clocks and its DAC's digital side, TurnOnDacPower", codec_dac_on, COUNT(codec_dac_on)) ||
	    !run_probe("the codec", codec_probe, COUNT(codec_probe), 0) ||
	    !run("up 2/7: the codec's charge pump, line output and DAC, the rest of Speaker_Amp_Change(true)", codec_spk_on,
	    COUNT(codec_spk_on))) {
		return false;
	}
	pmic_read(PMIC_AUDDEC_ANA_CON0, &a);
	pmic_read(PMIC_AFUNC_AUD_CON0, &b);
	pmic_read(PMIC_DCXO_CW14, &c);
	pmic_read(PMIC_AUDDEC_ANA_CON2, &d);
	printf("mtkaudio: the codec reads back AUDDEC_ANA_CON0 %#x (0x3009 in the capture), AFUNC_AUD_CON0 %#x (0xcba1), DCXO_CW14 %#x "
	    "(0xa2b5), AUDDEC_ANA_CON2 %#x (0x633)\n", a, b, c, d);

	printf("mtkaudio: I2C0 by hand: pins %u (data) and %u (clock) had function %u and %u\n", PIN_SDA, PIN_SCL, pin_mode(PIN_SDA),
	    pin_mode(PIN_SCL));
	pin_set_mode(PIN_SCL, 0);
	pin_set_mode(PIN_SDA, 0);
	line(PIN_SCL, true);
	line(PIN_SDA, true);
	answers = amp_read(0x00, &id);
	printf("mtkaudio: clock %d data %d; the amplifier %s at %#x, its register 0 is %#x (0x76 under Android)\n", pin_get(PIN_SCL),
	    pin_get(PIN_SDA), answers ? "answers" : "does NOT answer", AMP_ADDR, id);
	if (!run("up 3/7: the amplifier, Android's Music profile", amp_on, COUNT(amp_on))) {
		return false;
	}
	amp_compare();
	return run("up 3/7, its end: the codec's last, hp_pull_down(false)", codec_on_end, COUNT(codec_on_end));
}

/* The AFE: all of `tinyplay`'s open, hw_params, prepare and start. */
static void
power_up_afe(void)
{
	uint32_t before, after;

	dl_afe = true;
	if (!afe_up) {          /* (the microphone may have brought the AFE's clocks up already) */
		afe_up = true;
		run("up 4/7: the clocks and gates, AudDrv_Clk_On", clocks_on, COUNT(clocks_on));
		run_probe("the AFE", afe_probe, COUNT(afe_probe), 16);
		printf("mtkaudio: AUDIO_TOP_CON0 %#x (0xa0fd4078 in the capture); gates: infracfg %#x/%#x, topckgen %#x/%#x\n",
		    rd(B_AFE, AUDIO_TOP_CON0), rd(B_INFRA, INFRA_CG1_STA), rd(B_INFRA, INFRA_CG2_STA), rd(B_TOP, TOP_CLK_CFG_3),
		    rd(B_TOP, TOP_CLK_CFG_4));
	}

	run("up 5/7: the sample memory and where the buffer is, mtk_pcm_dl1_params", dl1_params, COUNT(dl1_params));
	for (uint32_t i = 0; i < DL1_FRAMES; i++) {
		sram[i] = 0;
	}
	sram[0] = 0x5a5aa5a5;
	before = sram[0];
	sram[0] = 0;
	printf("mtkaudio: the buffer: %#x to %#x, cleared; a word of it reads back %#x (0x5a5aa5a5 written)\n", rd(B_AFE, AFE_DL1_BASE),
	    rd(B_AFE, AFE_DL1_END), before);

	run("up 6/7: the formats, the DAC interface at 48 kHz, the AFE on, mtk_pcm_prepare", dl1_prepare, COUNT(dl1_prepare));
	for (unsigned i = 0; i < sizeof(link_pins); i++) {
		if (!(i == 0 && (users & USE_UL))) {    /* (pin 136: the microphone has saved what it had) */
			link_before[i] = (uint8_t)pin_mode(link_pins[i]);
		}
		pin_set_mode(link_pins[i], LINK_FUNCTION);
	}
	printf("mtkaudio: pins %u/%u/%u (the link to the PMIC) had function %u/%u/%u, now %u; pin 137 (its frame sync, AUD_SYNC_MOSI: "
	    "function 1) has function %u; AFE_I2S_CON1 %#x (0xa0b), AFE_ADDA_DL_SRC2_CON0 %#x (0x83001803)\n", link_pins[0],
	    link_pins[1], link_pins[2], link_before[0], link_before[1], link_before[2], LINK_FUNCTION, pin_mode(137),
	    rd(B_AFE, AFE_I2S_CON1), rd(B_AFE, AFE_ADDA_DL_SRC2_CON0));

	run("up 7/7: the connections, the rate, the memory interface started, mtk_pcm_dl1_start", dl1_start, COUNT(dl1_start));
	before = rd(B_AFE, AFE_DL1_CUR);
	delay(10000);
	after = rd(B_AFE, AFE_DL1_CUR);
	printf("mtkaudio: AFE_DAC_CON0 %#x (3), AFE_DAC_CON1 %#x; the hardware's pointer went from %#x to %#x in 10 ms: %s\n",
	    rd(B_AFE, AFE_DAC_CON0), rd(B_AFE, AFE_DAC_CON1), before, after,
	    before == after ? "IT DOES NOT MOVE: no clock reaches the memory interface, nothing will play" : "it reads (480 frames, "
	    "0x780, are 10 ms)");
}

/* ---- the microphone: boot-args, the power sequences, the polling, /dev/redwood-mic, the self-test ---- */

static uint32_t mic_level;                      /* mtkmic */
static uint32_t mic_ch;                         /* mtkmic_ch: 0 = L (AIN0), 1 = R (AIN2): what iOS gets */
static uint32_t mic_in_l = 1, mic_in_r = 3;     /* mtkmic_in_l/_r: preamp inputs, 1 = AIN0, 2 = AIN1 (and MICBIAS1), 3 = AIN2 */
static uint32_t mic_gain = 3;                   /* mtkmic_gain: PGA index, 6 dB steps (V: the driver's default 3 = 18 dB; Open Mapple records at 2) */
static uint32_t mic_mode = 1;                   /* mtkmic_mode: 0 ACC, 1 DCC (T: the r1's Android records in DCC, logs/mic.trace) */
static uint32_t mic_bias = 2;                   /* mtkmic_bias: MICBIAS0's VREF, bits 6:4 (2 = 1.9 V) */
static uint32_t mic_shift;                      /* mtkmic_shift: the kernel's digital gain, a saturating left shift */
static uint32_t mic_settle_ms = 800;            /* mtkmic_settle_ms: dropped after power-up (T: the DCC/IIR transient lasts ~0.7 s; 100 left a +14 dB start in the file) */
static uint32_t mic_vul = UL_SRAM_PHYS_DEFAULT; /* mtkmic_vul: G, where the uplink's buffer is in the AFE's SRAM */
static uint32_t mic_swap;                       /* mtkmic_swap=1: G, the left sample is in the HIGH half of a word (test T1 says) */
static uint32_t mic_pins140;                    /* mtkmic_pins140=1: G, pins 140 and 141 (CLK_MISO, SYNC_MISO) also function 1 (E5, T2 say) */
static uint32_t mic_dcc_off = 0x2061;           /* mtkmic_dcc_off: G, AFE_DCCLK_CFG0 when DCC is left */
static uint32_t mic_t12_peak = 500;             /* mtkmic_t12_peak: G, T1 and T2 pass with at least this peak on both channels */
static uint32_t mic_t4_db = 10;                 /* mtkmic_t4_db: G, T4 passes with the tone this many dB over the quiet before it, */
static uint32_t mic_t4_peak = 1000;             /* mtkmic_t4_peak: G, and at least this peak */
static uint32_t mic_t4_tone = 8000;             /* mtkmic_t4_tone: the tone's peak in T4, of 32767 (at most 8000) */
static uint32_t mic_t5;                         /* mtkmic_t5=1: also run T4 again with AIN1 and MICBIAS1 on the left channel */
static uint32_t mic_t4_gap = 150;               /* G: ms of the tone before T4 measures it (the air and the microphone's delay) */

static volatile uint32_t *vul;                  /* the uplink's buffer, a frame a word */
static const uint8_t ul_pins[] = { 142, 143, 140, 141 };        /* the first two: R1 (dtbo, aud_dat_miso_on); the others: mtkmic_pins140 */
static uint8_t ul_before[sizeof(ul_pins)];
static bool ul_afe_begun, ul_codec_begun;       /* what mic_down() has to undo */
static bool mic_on;                             /* the path is up and being read */
static bool mic_failed;                         /* three failures: off for the rest of the run */
static bool mic_selftest;                       /* the self-test owns the path: mic_poll() leaves the power alone */
static uint32_t mic_failures;
static uint64_t mic_retry_at;                   /* mach time: not before */
static uint32_t mic_rd;                         /* byte offset in the buffer read so far */
static uint32_t mic_settle_left;                /* frames still to drop */
static uint32_t mic_bad;                        /* pointers outside the buffer */
static volatile uint32_t ios_wants;             /* /dev/redwood-mic is open */
static char mic_summary[200];                   /* the self-test's result, said again by the quiesce */

struct mic_stats {
	uint32_t frames, peak[2];
	int64_t sum[2];
	uint64_t sq[2];
};
static struct mic_stats mic_win, mic_sec;       /* the self-test's window; the log's second */
static uint32_t mic_seconds;
static uint64_t mic_sec_start;                  /* ms */

static void
st_add(struct mic_stats *st, int32_t l, int32_t r)
{
	int32_t v[2] = { l, r };

	for (int c = 0; c < 2; c++) {
		uint32_t a = (uint32_t)(v[c] < 0 ? -v[c] : v[c]);

		st->peak[c] = a > st->peak[c] ? a : st->peak[c];
		st->sum[c] += v[c];
		st->sq[c] += (uint64_t)((int64_t)v[c] * v[c]);
	}
	st->frames++;
}

/* 20 log10(rms / 32768) in tenths of a dB, by halvings (6.02 dB each, near enough to tell loud from quiet); -999 for silence */
static int
st_db10(const struct mic_stats *st, int c)
{
	uint64_t mean_square = st->frames ? st->sq[c] / st->frames : 0;
	uint32_t rms = 0, r;
	int tenths = 0;

	for (uint32_t bit = 1u << 15; bit != 0; bit >>= 1) {
		if ((uint64_t)(rms | bit) * (rms | bit) <= mean_square) {
			rms |= bit;
		}
	}
	if (rms == 0) {
		return -999;
	}
	for (r = rms; r < 16384; r *= 2) {
		tenths -= 60;
	}
	return tenths - (int)((32768 - r) * 60 / 16384);
}

#define DB10(x)                 ((x) < 0 ? "-" : ""), ((x) < 0 ? -(x) : (x)) / 10, ((x) < 0 ? -(x) : (x)) % 10

static void
st_say(const char *what, const struct mic_stats *st)
{
	int l = st_db10(st, 0), r = st_db10(st, 1);

	printf("mtkmic: %s: %u frames; L peak %u, %s%d.%d dBFS, mean %lld; R peak %u, %s%d.%d dBFS, mean %lld\n", what, st->frames,
	    st->peak[0], DB10(l), st->frames ? st->sum[0] / (int64_t)st->frames : 0, st->peak[1], DB10(r),
	    st->frames ? st->sum[1] / (int64_t)st->frames : 0);
}

/* ---- /dev/redwood-mic: copied from redwood_mic.c (the mic=4 block), names prefixed ---- */
#define MIC_RING_FRAMES         16384           /* 341 ms; a power of two */
#define MIC_RING_KEEP           1920            /* 40 ms: how far behind the microphone a reader is kept */
#define MIC_RING_MOST           4800            /* 100 ms: more than this waiting, and the oldest is skipped */

static int16_t *mic_ring;
static volatile uint64_t mic_written;           /* frames put into the ring */
static uint64_t mic_read_at;                    /* frames taken from it (one reader: the audio server) */
static bool mic_primed;                         /* the reader is MIC_RING_KEEP behind; false after it ran dry */
static uint64_t mic_skipped, mic_dry;

static void
mic_ring_put(const int16_t *samples, uint32_t count)
{
	uint64_t at = mic_written;

	for (uint32_t i = 0; i < count; i++) {
		mic_ring[(at + i) % MIC_RING_FRAMES] = samples[i];
	}
	__atomic_store_n(&mic_written, at + count, __ATOMIC_RELEASE);
}

static int
mic_dev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint64_t written = __atomic_load_n(&mic_written, __ATOMIC_ACQUIRE), avail, want;
	int error = 0;

	if (mic_ring == NULL || uio_resid(uio) < 2) {
		return 0;
	}
	want = (uint64_t)uio_resid(uio) / 2;
	avail = written - mic_read_at;
	if (avail > MIC_RING_MOST) {
		mic_skipped += avail - MIC_RING_KEEP;
		mic_read_at = written - MIC_RING_KEEP;
		avail = MIC_RING_KEEP;
	}
	if (!mic_primed) {
		if (avail < MIC_RING_KEEP) {
			return 0;
		}
		mic_primed = true;
	}
	if (avail < want) {
		mic_dry++;
		mic_primed = false;
	}
	while (want > 0 && avail > 0 && error == 0) {
		uint64_t at = mic_read_at % MIC_RING_FRAMES, n = want < avail ? want : avail;

		n = n < MIC_RING_FRAMES - at ? n : MIC_RING_FRAMES - at;
		error = uiomove((caddr_t)&mic_ring[at], (int)(n * 2), uio);
		mic_read_at += n;
		want -= n;
		avail -= n;
	}
	return error;
}

static int
mic_dev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	if (!ios_wants) {       /* a new recording: nothing of an earlier one, the reader 40 ms behind again */
		mic_read_at = __atomic_load_n(&mic_written, __ATOMIC_ACQUIRE);
		mic_primed = false;
		mic_skipped = mic_dry = 0;
	}
	ios_wants = 1;
	wakeup(&feed_wanted);   /* the thread's idle wait */
	return 0;
}

/* (on the last close) */
static int
mic_dev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	ios_wants = 0;
	return 0;
}

static const struct cdevsw mic_cdevsw = {
	.d_open = mic_dev_open, .d_close = mic_dev_close, .d_read = mic_dev_read, .d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static void
mic_node_make(void)
{
	int major = cdevsw_add(-1, &mic_cdevsw);

	mic_ring = kalloc_data(MIC_RING_FRAMES * sizeof(int16_t), Z_WAITOK | Z_ZERO);
	if (mic_ring == NULL || major < 0 ||
	    devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-mic") == NULL) {
		printf("mtkmic: /dev/redwood-mic cannot be made\n");
		return;
	}
	printf("mtkmic: /dev/redwood-mic is made: mono 16-bit 48 kHz, the %s channel (%s), up while it is open\n",
	    mic_ch ? "right" : "left", mic_ch ? "AIN2" : "AIN0");
}

/* ---- the sequences ---- */

/* The uplink's own sine into the VUL memory interface (set_chip_sine_gen_enable, outputs O09/O10; 1/32 of full scale): T1 */
static const struct step ul_sgen_on[] = {
	{ B_AFE, AFE_SGEN_CON0, 0x04ac2ac1, 0, 0 },
	{ B_AFE, AFE_SGEN_CON0, 2u << 17, 7u << 17, 0 },
	{ B_AFE, AFE_SGEN_CON0, 2u << 5, 7u << 5, 0 },
	{ B_AFE, AFE_SGEN_CON2, 0x25, 0x3f, 0 },
};
static const struct step ul_sgen_off[] = {
	{ B_AFE, AFE_SGEN_CON0, 0, 0, 0 },
	{ B_AFE, AFE_SGEN_CON2, 0x3f, 0x3f, 0 },
};
/* The codec's uplink sine table (SineTable_UL2_Set; mainline "SGEN UL Enable" = AFE_TOP_CON0 bit 1): T2 */
static const struct step ul_pmic_sgen_on[] = {
	{ B_PMIC, PMIC_AFE_SGEN_CFG0, 0x0080, 0, 0 },
	{ B_PMIC, PMIC_AFE_SGEN_CFG1, 0x0101, 0, 0 },
	{ B_PMIC, PMIC_AFE_TOP_CON0, 2, 2, 0 },
};
static const struct step ul_pmic_sgen_off[] = {
	{ B_PMIC, PMIC_AFE_TOP_CON0, 0, 2, 0 },
	{ B_PMIC, PMIC_AFE_SGEN_CFG0, 0, 0, 0 },
	{ B_PMIC, PMIC_AFE_SGEN_CFG1, 0x0101, 0, 0 },
};

/* C11..C17 and the DCC variant's extras: they depend on the boot-args, so they are built here */
static bool
ul_codec_inputs(void)
{
	struct step t[24];
	unsigned n = 0;
	bool dcc = mic_mode != 0, ain1 = mic_in_l == 2 || mic_in_r == 2;
	uint32_t bias = mic_bias & 7, gain = mic_gain > 4 ? 4 : mic_gain;

#define T(reg, value, mask) (t[n++] = (struct step){ B_PMIC, (reg), (value), (mask), 0, 0 })
	T(PMIC_AUDENC_ANA_CON8, 0x0001 | bias << 4, 0);                 /* C11: MICBIAS0 on, VREF 1.9 V by default (T: 0x21) */
	if (ain1) {
		T(PMIC_AUDENC_ANA_CON9, 1, 1);                          /* C11b: MICBIAS1 */
	}
	/* T (logs/mic.trace, Android in DCC mode, 18 dB): gain and DCCEN for both, then per channel precharge, input and PGA, ADC, precharge off */
	T(PMIC_AUDENC_ANA_CON0, gain << 8 | (dcc ? 2 : 0), dcc ? 0x0702 : 0x0700);     /* C12: SetMicPGAGain, L (0x302) */
	T(PMIC_AUDENC_ANA_CON1, gain << 8 | (dcc ? 2 : 0), dcc ? 0x0702 : 0x0700);     /* C13: the same, R */
	for (int c = 0; c < 2; c++) {
		uint32_t con = c ? PMIC_AUDENC_ANA_CON1 : PMIC_AUDENC_ANA_CON0, sel = c ? mic_in_r : mic_in_l;

		sel = sel < 1 ? 1 : sel > 3 ? 3 : sel;
		if (dcc) {
			T(con, 0x0004, 0x0004);                         /* the DC precharge (0x306) */
			T(con, (sel << 6) | 0x01, 0x00c1);              /* C14/C16: the preamp's input and the PGA on (0x347, 0x3c7) */
		} else {
			T(con, (sel << 6) | 0x01, 0xf0ff);
		}
		T(con, 0x5000, 0xf000);                                 /* C15/C17: the ADC's input is the preamp, the ADC on (0x5347, 0x53c7) */
		if (dcc) {
			T(con, 0, 0x0004);                              /* the precharge off (0x5343, 0x53c3) */
		}
	}
#undef T
	return run("mic up: the inputs, the bias and the ADCs (C11..C17)", t, n);
}

static uint32_t
ul_position(void)
{
	uint32_t cur = rd(B_AFE, AFE_VUL_CUR);

	if (cur == 0) {
		return 0;
	}
	if (cur < mic_vul || cur >= mic_vul + UL_BYTES) {
		mic_bad++;
		return mic_rd;
	}
	return (cur - mic_vul) & ~15u;  /* 16-byte steps, as Open Mapple's */
}

/* A0, A4..A6, A1..A22, the pins, A24..A29, A30 */
static bool
ul_afe_on(void)
{
	uint32_t before, after;

	if (!afe_up) {          /* A0: the speaker's tables, if it is not up */
		afe_up = true;
		run("mic up: the clocks and gates, AudDrv_Clk_On", clocks_on, COUNT(clocks_on));
		run_probe("the AFE", afe_probe, COUNT(afe_probe), 16);
	}
	ul_afe_begun = true;
	wr(B_AFE, AFE_VUL_BASE, mic_vul);                       /* A4 */
	wr(B_AFE, AFE_VUL_END, mic_vul + UL_BYTES - 1);         /* A5 */
	for (uint32_t i = 0; i < UL_FRAMES; i++) {              /* A6 */
		vul[i] = 0;
	}
	if (!run("mic up: the AFE's uplink, ADC clock, formats, IIR and source (A1..A22)", ul_afe_a, COUNT(ul_afe_a))) {
		return false;
	}
	/* A23: AudDrv_GPIO_Request(true, ADDA_UL); pin 136 is the speaker's too */
	if (!(users & USE_DL)) {
		link_before[0] = (uint8_t)pin_mode(link_pins[0]);
	}
	pin_set_mode(link_pins[0], LINK_FUNCTION);
	for (unsigned i = 0; i < (mic_pins140 ? sizeof(ul_pins) : 2); i++) {
		ul_before[i] = (uint8_t)pin_mode(ul_pins[i]);
		pin_set_mode(ul_pins[i], LINK_FUNCTION);
	}
	run("mic up: the connections, the rate, the memory interface started (A24..A29)", ul_afe_b, COUNT(ul_afe_b));
	before = rd(B_AFE, AFE_VUL_CUR);
	delay(10000);
	after = rd(B_AFE, AFE_VUL_CUR);
	mic_rd = ul_position();
	printf("mtkmic: AFE_DAC_CON0 %#x, UL_SRC_CON0 %#x, pins %u/%u/%u/%u function %u/%u/%u/%u, AUDIO_TOP_CON0 %#x; the pointer went from %#x "
	    "to %#x in 10 ms: %s\n", rd(B_AFE, AFE_DAC_CON0), rd(B_AFE, AFE_ADDA_UL_SRC_CON0), ul_pins[0], ul_pins[1], ul_pins[2],
	    ul_pins[3], pin_mode(ul_pins[0]), pin_mode(ul_pins[1]), pin_mode(ul_pins[2]), pin_mode(ul_pins[3]),
	    rd(B_AFE, AUDIO_TOP_CON0), before, after,
	    before == after ? "IT DOES NOT MOVE: nothing will be heard" : "it writes (480 frames, 0x780 bytes, are 10 ms)");
	return before != after;
}

/* The microphone up. With codec false only the AFE's side (T1). On false the caller must mic_down(). */
static bool
mic_up(bool codec)
{
	users |= USE_UL;
	ul_afe_begun = ul_codec_begun = false;
	if (codec) {
		ul_codec_begun = true;
		if (!(users & USE_DL)) {
			rails_on();     /* C0 */
		}
		if (!run("mic up: the codec's common parts, ADC clocks, LCLDO (C1..C10)", ul_codec_a, COUNT(ul_codec_a)) ||
		    (mic_mode && !run("mic up: DCC's clock", ul_codec_dcc_clock, COUNT(ul_codec_dcc_clock))) ||
		    !ul_codec_inputs() ||
		    !run("mic up: the clocks, the digital side and the uplink source (C18..C25)", ul_codec_b, COUNT(ul_codec_b))) {
			return false;
		}
	}
	if (!ul_afe_on()) {
		return false;
	}
	mic_settle_left = mic_settle_ms * (REDWOOD_AUDIO_RATE / 1000);
	return true;
}

/* The microphone down: the AFE first, then the codec, the uplink's bit gone from `users' before either. Safe on what is half up. */
static void
mic_down(void)
{
	bool dl = (users & USE_DL) != 0;

	users &= ~USE_UL;
	mic_on = false;
	if (ul_afe_begun) {
		run("mic down: the uplink's connections, source and ADC clock", ul_afe_off_a, COUNT(ul_afe_off_a));
		for (unsigned i = 0; i < (mic_pins140 ? sizeof(ul_pins) : 2); i++) {
			pin_set_mode(ul_pins[i], ul_before[i]);
		}
		if (!dl) {
			pin_set_mode(link_pins[0], link_before[0]);
		}
		run("mic down: the AFE", ul_afe_off_b, COUNT(ul_afe_off_b));
		if (!dl) {
			for (int i = 0; i < 1000 && (rd(B_AFE, AFE_DAC_MON) & 1); i++) {
				delay(100);
			}
			run("mic down: the clocks and gates, AudDrv_Clk_Off", clocks_off, COUNT(clocks_off));
			afe_up = false;
		}
		ul_afe_begun = false;
	}
	if (ul_codec_begun) {
		run("mic down: the uplink source, the PAD and the codec's top control", ul_codec_off_a, COUNT(ul_codec_off_a));
		if (mic_mode) {         /* TurnOnADcPowerDCC(false): DCCEN, then the precharge */
			pmic_set(PMIC_AUDENC_ANA_CON0, 0, 0x0002);
			pmic_set(PMIC_AUDENC_ANA_CON1, 0, 0x0002);
			pmic_set(PMIC_AUDENC_ANA_CON0, 0, 0x0004);
			pmic_set(PMIC_AUDENC_ANA_CON1, 0, 0x0004);
		}
		run("mic down: the ADCs, then the bias, then the rest", ul_codec_off_b, COUNT(ul_codec_off_b));
		if (mic_mode) {
			pmic_set(PMIC_AFE_DCCLK_CFG0, mic_dcc_off, 0);
		}
		ul_codec_begun = false;
	}
}

static void
mic_second_say(void)
{
	int l = st_db10(&mic_sec, 0), r = st_db10(&mic_sec, 1);

	mic_seconds++;
	if (mic_seconds <= 3 || mic_seconds % 30 == 0) {
		printf("mtkmic: second %u: %u frames; L peak %u, %s%d.%d dBFS, mean %lld; R peak %u, %s%d.%d dBFS, mean %lld; iOS gets %s; "
		    "%u bad pointers\n", mic_seconds, mic_sec.frames, mic_sec.peak[0], DB10(l),
		    mic_sec.frames ? mic_sec.sum[0] / (int64_t)mic_sec.frames : 0, mic_sec.peak[1], DB10(r),
		    mic_sec.frames ? mic_sec.sum[1] / (int64_t)mic_sec.frames : 0, mic_ch ? "R" : "L", mic_bad);
	}
	memset(&mic_sec, 0, sizeof(mic_sec));
}

static void
mic_route(const int16_t *mono, int16_t *stereo, uint32_t n, bool route)
{
	if (n == 0 || !route) {
		return;
	}
	if (ios_wants && mic_ring != NULL) {
		mic_ring_put(mono, n);
	}
	if (redwood_audio_recording_mic()) {
		for (uint32_t j = 0; j < n; j++) {
			stereo[2 * j] = stereo[2 * j + 1] = mono[j];
		}
		redwood_audio_record_fed(stereo, n);
	}
}

/* The new frames of the buffer: into the statistics, and, with `route', into /dev/redwood-mic and the sound recording. */
static void
mic_read_words(bool route)
{
	uint32_t pos = ul_position(), frames = ((pos - mic_rd) % UL_BYTES) / 4, at = mic_rd / 4;
	int16_t mono[240], stereo[480];
	uint32_t n = 0;

	for (uint32_t i = 0; i < frames; i++, at = (at + 1) % UL_FRAMES) {
		uint32_t w = vul[at];
		int32_t l = (int16_t)(w & 0xffff), r = (int16_t)(w >> 16), s;

		if (mic_swap) {
			int32_t t = l;

			l = r;
			r = t;
		}
		if (mic_settle_left != 0) {
			mic_settle_left--;
			continue;
		}
		st_add(&mic_win, l, r);
		st_add(&mic_sec, l, r);
		s = (mic_ch ? r : l) * (1 << (mic_shift > 4 ? 4 : mic_shift));
		s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
		mono[n++] = (int16_t)s;
		if (n == 240) {
			mic_route(mono, stereo, n, route);
			n = 0;
		}
	}
	mic_route(mono, stereo, n, route);
	mic_rd = pos;
}

/* Called every 5 ms (every 20 ms while the path is down) from the one thread: up when the node is opened, down when it is closed. */
static void
mic_poll(void)
{
	bool want = ios_wants != 0;
	uint64_t ns = 0;

	if (mic_level < 2 || mic_selftest) {
		return;
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	if (want && !mic_on && !mic_failed && ns >= mic_retry_at) {
		printf("mtkmic: iOS records: the microphone up (channel %s)\n", mic_ch ? "R" : "L");
		mic_on = mic_up(true);
		if (!mic_on) {
			mic_down();
			mic_retry_at = ns + 1000ull * NSEC_PER_MSEC;
			if (++mic_failures >= 3) {
				mic_failed = true;
				printf("mtkmic: the power-up FAILED three times: the microphone stays off in this run\n");
			} else {
				printf("mtkmic: the power-up FAILED (%u of 3): tried again in a second\n", mic_failures);
			}
			return;
		}
		memset(&mic_sec, 0, sizeof(mic_sec));
		mic_seconds = 0;
		mic_sec_start = ns / NSEC_PER_MSEC;
	} else if (!want && mic_on) {
		mic_down();
		printf("mtkmic: iOS stopped: the microphone is down; %llu frames skipped as too late, %llu reads found too little\n",
		    mic_skipped, mic_dry);
	}
	if (mic_on) {
		mic_read_words(true);
		if (ns / NSEC_PER_MSEC - mic_sec_start >= 1000) {
			mic_sec_start += 1000;
			mic_second_say();
		}
	}
}

/*
 * Feed until there is nothing to play: for the sink, until iOS has written nothing for IDLE_MS and all
 * of it is in the buffer; for the test tone, until it is made and 100 ms more (the buffer's 85 ms)
 * have passed, or three seconds, whichever is first. Then a line about how it went.
 */
static void
feed_run(bool test)
{
	uint32_t start = now_ms(), took, tail = 100 / POLL_MS;
	uint32_t read, given, padded, underruns, bad, lost;

	lck_mtx_lock(&mtkaudio_lock);
	dl1_begin();
	lck_mtx_unlock(&mtkaudio_lock);
	for (uint32_t tick = 0;; tick++) {
		uint64_t idle_ns = 0;
		bool done;

		if (tick % (100 / POLL_MS) == 0) {
			volume_look();
		}
		lck_mtx_lock(&mtkaudio_lock);
		dl1_feed();
		absolutetime_to_nanoseconds(mach_absolute_time() - feed_last_write, &idle_ns);
		if (test) {
			done = (tone_left == 0 && tail-- == 0) || tick >= 3000 / POLL_MS;
		} else {
			done = ring_head == ring_tail && idle_ns > IDLE_MS * (uint64_t)NSEC_PER_MSEC;
			if (done) {
				feed_wanted = false;    /* under the lock: a write from now on wakes the thread again */
			}
		}
		lck_mtx_unlock(&mtkaudio_lock);
		if (done) {
			break;
		}
		mic_poll();     /* (nothing, unless mtkmic>=2) */
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
	took = now_ms() - start;
	lck_mtx_lock(&mtkaudio_lock);
	read = dl1_read;
	given = dl1_given;
	padded = dl1_padded;
	underruns = dl1_underruns;
	bad = dl1_bad;
	lost = tone_left;
	tone_left = 0;
	lck_mtx_unlock(&mtkaudio_lock);
	printf("mtkaudio: %s: %u frames given and %u of silence, in %u ms; the hardware read %u (%u a second; 48000 is right); "
	    "%u underruns, %u pointers outside the buffer", test ? "the test tone" : "played", given, padded, took, read,
	    took ? (uint32_t)((uint64_t)read * 1000 / took) : 0, underruns, bad);
	if (lost) {
		printf("; %u frames of the tone NOT played in three seconds", lost);
	}
	printf("\n");
}


/* ---- mtkmic=3: the self-test (docs/microphone-spec.md, step 6) ---- */

static void
mic_win_look(uint32_t ms)
{
	memset(&mic_win, 0, sizeof(mic_win));
	for (uint32_t t = 0; t < ms; t += POLL_MS) {
		mic_read_words(false);
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
	mic_read_words(false);
}

/* wait out the settling (the bias, the pop) without measuring */
static void
mic_settle_wait(void)
{
	for (uint32_t t = 0; t < 3000 && mic_settle_left != 0; t += POLL_MS) {
		mic_read_words(false);
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
}

/* the speaker's feeder and the microphone's reader, together, for `ms` */
static void
mic_duet(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += POLL_MS) {
		lck_mtx_lock(&mtkaudio_lock);
		dl1_feed();
		lck_mtx_unlock(&mtkaudio_lock);
		mic_read_words(false);
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
}

static bool
mic_both_loud(const struct mic_stats *st)
{
	return st->frames != 0 && st->peak[0] >= mic_t12_peak && st->peak[1] >= mic_t12_peak;
}

/* T4: the speaker plays the 440 Hz tone and the microphones should hear it. pass[0] = L, pass[1] = R. */
static void
mic_t4(const char *what, bool pass[2])
{
	struct mic_stats quiet;
	uint32_t peak = mic_t4_tone > 8000 ? 8000 : mic_t4_tone;

	pass[0] = pass[1] = false;
	if (!power_up_analog()) {
		power_down();
		printf("mtkmic: %s: the speaker would not come up\n", what);
		return;
	}
	power_up_afe();
	lck_mtx_lock(&mtkaudio_lock);
	dl1_begin();
	lck_mtx_unlock(&mtkaudio_lock);
	mic_duet(100);                  /* the speaker's power-up moves AUDIO_TOP_CON0 under the running microphone: let that go by */
	memset(&mic_win, 0, sizeof(mic_win));
	mic_duet(300);
	quiet = mic_win;
	st_say("T4 quiet, the speaker up", &quiet);
	lck_mtx_lock(&mtkaudio_lock);
	tone_y1 = 0;
	tone_y2 = -(int32_t)peak * TONE_HZ_SIN;
	tone_left = REDWOOD_AUDIO_RATE;
	lck_mtx_unlock(&mtkaudio_lock);
	mic_duet(mic_t4_gap);
	memset(&mic_win, 0, sizeof(mic_win));
	mic_duet(600);
	st_say("T4 the tone", &mic_win);
	for (int c = 0; c < 2; c++) {
		int delta = st_db10(&mic_win, c) - st_db10(&quiet, c);

		pass[c] = mic_win.frames != 0 && delta >= (int)mic_t4_db * 10 && mic_win.peak[c] >= mic_t4_peak;
		printf("mtkmic: %s, channel %s: the tone is %d dB (tenths) over the quiet, peak %u: %s (needs %u dB and a peak of %u)\n", what,
		    c ? "R" : "L", delta, mic_win.peak[c], pass[c] ? "PASS" : "FAIL", mic_t4_db, mic_t4_peak);
	}
	lck_mtx_lock(&mtkaudio_lock);
	tone_left = 0;
	lck_mtx_unlock(&mtkaudio_lock);
	power_down();                   /* the microphone stays up: the shared steps are left out */
}

static void
mic_selftest_run(void)
{
	bool ok, t1 = false, t2 = false, t3 = false, t4[2] = { false, false }, t5[2] = { false, false };
	bool ran5 = false;
	int l, r;

	mic_selftest = true;
	printf("mtkmic: the self-test: T1 the AFE's sine into the VUL memory, T2 the codec's sine over the link, T3 silence, T4 the speaker's "
	    "tone heard%s\n", mic_t5 ? ", T5 the same on AIN1" : "");

	/* T1: the codec left out */
	ok = mic_up(false);
	mic_settle_left = 0;
	if (ok) {
		run("T1: the AFE's sine on", ul_sgen_on, COUNT(ul_sgen_on));
		mic_win_look(500);
		run("T1: the AFE's sine off", ul_sgen_off, COUNT(ul_sgen_off));
		st_say("T1", &mic_win);
		t1 = mic_both_loud(&mic_win);
		if (!t1) {
			printf("mtkmic: T1 FAILED: the pointer or the word layout is wrong (L and R swapped? try mtkmic_swap=1; stuck? the buffer's place, "
			    "mtkmic_vul=)\n");
		}
	} else {
		printf("mtkmic: T1: the AFE's uplink did not come up\n");
	}
	mic_down();

	/* T2: everything, the codec's own sine */
	ok = mic_up(true);
	if (ok) {
		mic_on = true;
		run("T2: the codec's uplink sine on", ul_pmic_sgen_on, COUNT(ul_pmic_sgen_on));
		mic_settle_wait();
		mic_win_look(500);
		run("T2: the codec's uplink sine off", ul_pmic_sgen_off, COUNT(ul_pmic_sgen_off));
		st_say("T2", &mic_win);
		t2 = mic_both_loud(&mic_win);
		if (!t2) {
			printf("mtkmic: T2 FAILED%s: the link (pins 142/143, PMIC GPIO_MODE3, AUD_PAD_TOP, mtkmic_pins140=1?)\n", t1 ? "" : " as well");
		}

		/* T3: silence, the bias on */
		mic_win_look(1000);
		st_say("T3 the room", &mic_win);
		l = st_db10(&mic_win, 0);
		r = st_db10(&mic_win, 1);
		t3 = mic_win.frames != 0 && ((l > -900 && mic_win.sum[0] != 0) || (r > -900 && mic_win.sum[1] != 0));
		printf("mtkmic: T3: %s: a live microphone is not digital silence (L %s%d.%d dBFS, R %s%d.%d dBFS)\n", t3 ? "PASS" : "FAIL",
		    DB10(l), DB10(r));

		/* T4: the acoustic loopback, the speaker up beside it */
		mic_t4("T4", t4);

		/* T5: the same with AIN1 and MICBIAS1 on the left */
		if (mic_t5 && mic_in_l != 2) {
			uint32_t was = mic_in_l;

			ran5 = true;
			mic_down();
			mic_in_l = 2;
			ok = mic_up(true);
			if (ok) {
				mic_on = true;
				mic_settle_wait();
				mic_t4("T5 (AIN1 on L)", t5);
			}
			mic_in_l = was;
		}
	} else {
		printf("mtkmic: T2: the microphone did not come up (T3 and T4 are not run)\n");
	}
	mic_down();
	snprintf(mic_summary, sizeof(mic_summary), "mtkmic: SELFTEST T1 %s T2 %s T3 %s T4 L %s R %s%s%s%s",
	    t1 ? "PASS" : "FAIL", t2 ? "PASS" : "FAIL", t3 ? "PASS" : "FAIL", t4[0] ? "PASS" : "FAIL", t4[1] ? "PASS" : "FAIL",
	    ran5 ? " T5 L " : "", ran5 ? (t5[0] ? "PASS" : "FAIL") : "", ran5 ? (t5[1] ? " R PASS" : " R FAIL") : "");
	printf("%s\n", mic_summary);
	mic_selftest = false;
}

/* The registers mtkaudio=1 prints, with what Android leaves in them when nothing plays (from the trace), where the trace says. */
#define NOT_SEEN                0xffffffffu
static const struct {
	const char *name;
	uint32_t reg, idle;
} afe_dump[] = {
	{ "AUDIO_TOP_CON0", AUDIO_TOP_CON0, 0xa6fd407c },
	{ "AFE_DAC_CON0", AFE_DAC_CON0, 0 },
	{ "AFE_DAC_CON1", AFE_DAC_CON1, NOT_SEEN },
	{ "AFE_I2S_CON1", AFE_I2S_CON1, 0xa0a },
	{ "AFE_CONN3", AFE_CONN3, 0 },
	{ "AFE_CONN4", AFE_CONN4, 0 },
	{ "AFE_CONN28", AFE_CONN28, 0 },
	{ "AFE_CONN29", AFE_CONN29, 0 },
	{ "AFE_CONN_24BIT", AFE_CONN_24BIT, 0x0019e003 },
	{ "AFE_DL1_BASE", AFE_DL1_BASE, SRAM_PHYS },
	{ "AFE_DL1_CUR", AFE_DL1_CUR, NOT_SEEN },
	{ "AFE_DL1_END", AFE_DL1_END, SRAM_PHYS + DL1_BYTES - 1 },
	{ "AFE_MEMIF_MSB", AFE_MEMIF_MSB, NOT_SEEN },
	{ "AFE_MEMIF_HD_MODE", AFE_MEMIF_HD_MODE, 0 },
	{ "AFE_MEMIF_HDALIGN", AFE_MEMIF_HDALIGN, 0x7fff0000 },
	{ "AFE_ADDA_DL_SRC2_CON0", AFE_ADDA_DL_SRC2_CON0, 0x83001802 },
	{ "AFE_ADDA_DL_SRC2_CON1", AFE_ADDA_DL_SRC2_CON1, 0xf74f0000 },
	{ "AFE_ADDA_UL_DL_CON0", AFE_ADDA_UL_DL_CON0, 0 },
	{ "AFE_ADDA_PREDIS_CON0", AFE_ADDA_PREDIS_CON0, NOT_SEEN },
	{ "AFE_ADDA_PREDIS_CON1", AFE_ADDA_PREDIS_CON1, NOT_SEEN },
	{ "AFE_ADDA_DL_SDM_DCCOMP_CON", AFE_ADDA_DL_SDM_DCCOMP_CON, NOT_SEEN },
	{ "AFE_ADDA_MTKAIF_CFG0", AFE_ADDA_MTKAIF_CFG0, NOT_SEEN },
	{ "AFE_SGEN_CON0", AFE_SGEN_CON0, NOT_SEEN },
	{ "AFE_SGEN_CON2", AFE_SGEN_CON2, NOT_SEEN },
	{ "AFE_IRQ_MCU_CON0", AFE_IRQ_MCU_CON0, 0 },
	{ "AFE_DAC_MON", AFE_DAC_MON, NOT_SEEN },
	{ "AUDIO_TOP_CON1", AUDIO_TOP_CON1, NOT_SEEN },
	{ "AFE_ADDA_TOP_CON0", AFE_ADDA_TOP_CON0, NOT_SEEN },
	{ "AFE_ADDA_MTKAIF_TX_CFG1", AFE_ADDA_MTKAIF_TX_CFG1, NOT_SEEN },
	{ "AFE_ADDA_MTKAIF_MON0", AFE_ADDA_MTKAIF_MON0, NOT_SEEN },
	{ "AFE_ADDA_MTKAIF_MON1", AFE_ADDA_MTKAIF_MON1, NOT_SEEN },
	{ "AFE_AUD_PAD_TOP_CFG", AFE_AUD_PAD_TOP_CFG, NOT_SEEN },
}, pmic_dump[] = {
	{ "DCXO_CW14", PMIC_DCXO_CW14, 0x82b5 },
	{ "SMT_CON1", PMIC_SMT_CON1, 0x000f },
	{ "GPIO_MODE2", PMIC_GPIO_MODE2, NOT_SEEN },
	{ "AUD_TOP_CKPDN_CON0", PMIC_AUD_TOP_CKPDN_CON0, 0x0066 },
	{ "AUDNCP_CLKDIV_CON3", PMIC_AUDNCP_CLKDIV_CON3, 0x0001 },
	{ "AFE_UL_DL_CON0", PMIC_AFE_UL_DL_CON0, 0x0000 },
	{ "AFE_DL_SRC2_CON0_L", PMIC_AFE_DL_SRC2_CON0_L, 0x0000 },
	{ "AUDIO_TOP_CON0", PMIC_AUDIO_TOP_CON0, 0x00ff },
	{ "AFUNC_AUD_CON0", PMIC_AFUNC_AUD_CON0, 0xcba0 },
	{ "AFUNC_AUD_CON2", PMIC_AFUNC_AUD_CON2, 0x0000 },
	{ "AUDENC_ANA_CON6", PMIC_AUDENC_ANA_CON6, 0x00a0 },
	{ "AUDDEC_ANA_CON0", PMIC_AUDDEC_ANA_CON0, 0x3000 },
	{ "AUDDEC_ANA_CON2", PMIC_AUDDEC_ANA_CON2, 0x0233 },
	{ "AUDDEC_ANA_CON4", PMIC_AUDDEC_ANA_CON4, 0x0110 },
	{ "AUDDEC_ANA_CON6", PMIC_AUDDEC_ANA_CON6, 0x0201 },
	{ "AUDDEC_ANA_CON7", PMIC_AUDDEC_ANA_CON7, 0x00a8 },
	{ "AUDDEC_ANA_CON8", PMIC_AUDDEC_ANA_CON8, 0x0004 },
	{ "AUDDEC_ANA_CON10", PMIC_AUDDEC_ANA_CON10, 0x0155 },
	{ "AUDDEC_ANA_CON11", PMIC_AUDDEC_ANA_CON11, 0x0010 },
	{ "AUDDEC_ANA_CON12", PMIC_AUDDEC_ANA_CON12, 0x0000 },
	{ "AUDDEC_ANA_CON13", PMIC_AUDDEC_ANA_CON13, 0x0000 },
	{ "ZCD_CON0", PMIC_ZCD_CON0, 0x0000 },
	{ "ZCD_CON1", PMIC_ZCD_CON1, 0x0f9f },
	{ "AUDNCP_CLKDIV_CON0", PMIC_AUDNCP_CLKDIV_CON0, NOT_SEEN },
	{ "AUDNCP_CLKDIV_CON1", PMIC_AUDNCP_CLKDIV_CON1, NOT_SEEN },
	{ "AUDNCP_CLKDIV_CON2", PMIC_AUDNCP_CLKDIV_CON2, NOT_SEEN },
	{ "AUDNCP_CLKDIV_CON4", PMIC_AUDNCP_CLKDIV_CON4, NOT_SEEN },
	{ "AFE_TOP_CON0", PMIC_AFE_TOP_CON0, NOT_SEEN },
	{ "AFE_ADDA_MTKAIF_CFG0", PMIC_MTKAIF_CFG0, NOT_SEEN },
	{ "MTKAIF_RX_CFG0", PMIC_MTKAIF_RX_CFG0, NOT_SEEN },
	{ "MTKAIF_RX_CFG1", PMIC_MTKAIF_RX_CFG0 + 2, NOT_SEEN },
	{ "MTKAIF_RX_CFG2", PMIC_MTKAIF_RX_CFG0 + 4, NOT_SEEN },
	{ "MTKAIF_RX_CFG3", PMIC_MTKAIF_RX_CFG0 + 6, NOT_SEEN },
	{ "AFE_SGEN_CFG0", PMIC_AFE_SGEN_CFG0, NOT_SEEN },
	{ "AFE_SGEN_CFG1", PMIC_AFE_SGEN_CFG1, NOT_SEEN },
	{ "AFE_AUD_PAD_TOP", PMIC_AFE_AUD_PAD_TOP, NOT_SEEN },
	{ "AUDDEC_ANA_CON1", PMIC_AUDDEC_ANA_CON1, NOT_SEEN },
	{ "AUDDEC_ANA_CON3", PMIC_AUDDEC_ANA_CON3, NOT_SEEN },
	{ "AUDDEC_ANA_CON9", PMIC_AUDDEC_ANA_CON9, NOT_SEEN },
};

/* mtkmic=1: the uplink's registers (spec step 3); read only */
static const struct {
	const char *name;
	uint32_t reg;
} ul_afe_dump[] = {
	{ "AFE_ADDA_UL_SRC_CON0", AFE_ADDA_UL_SRC_CON0 }, { "AFE_ADDA_UL_SRC_CON1", AFE_ADDA_UL_SRC_CON1 },
	{ "AFE_VUL_BASE", AFE_VUL_BASE }, { "AFE_VUL_END", AFE_VUL_END }, { "AFE_VUL_CUR", AFE_VUL_CUR },
	{ "AFE_CONN9", AFE_CONN9 }, { "AFE_CONN10", AFE_CONN10 },
	{ "AFE_ADDA_MTKAIF_RX_CFG0", AFE_ADDA_MTKAIF_RX_CFG0 }, { "AFE_ADDA_MTKAIF_RX_CFG1", AFE_ADDA_MTKAIF_RX_CFG1 },
	{ "AFE_ADDA_MTKAIF_RX_CFG2", AFE_ADDA_MTKAIF_RX_CFG2 }, { "AFE_AUD_PAD_TOP_CFG", AFE_AUD_PAD_TOP_CFG },
	{ "AFE_ADDA_UL_SRC_MON0", AFE_ADDA_UL_SRC_MON0 }, { "AFE_ADDA_UL_SRC_MON1", AFE_ADDA_UL_SRC_MON1 },
}, ul_pmic_dump[] = {
	{ "AUDENC_ANA_CON0", PMIC_AUDENC_ANA_CON0 }, { "AUDENC_ANA_CON1", PMIC_AUDENC_ANA_CON1 },
	{ "AUDENC_ANA_CON3", PMIC_AUDENC_ANA_CON3 }, { "AUDENC_ANA_CON8", PMIC_AUDENC_ANA_CON8 },
	{ "AUDENC_ANA_CON9", PMIC_AUDENC_ANA_CON9 }, { "AUDENC_ANA_CON10", PMIC_AUDENC_ANA_CON10 },
	{ "AFE_UL_SRC_CON0_H", PMIC_AFE_UL_SRC_CON0_H }, { "AFE_UL_SRC_CON0_L", PMIC_AFE_UL_SRC_CON0_L },
	{ "GPIO_MODE3", PMIC_GPIO_MODE3 }, { "AFE_DCCLK_CFG0", PMIC_AFE_DCCLK_CFG0 }, { "AFE_DCCLK_CFG1", PMIC_AFE_DCCLK_CFG1 },
	{ "AFE_SGEN_CFG0", PMIC_AFE_SGEN_CFG0 }, { "AFE_SGEN_CFG1", PMIC_AFE_SGEN_CFG1 },
};

static void
dump_uplink(bool afe_gated)
{
	printf("mtkmic: pins 136..143 function:");
	for (uint32_t pin = 136; pin <= 143; pin++) {
		printf(" %u=%u", pin, pin_mode(pin));
	}
	printf("\n");
	if (!afe_gated) {
		for (unsigned i = 0; i < COUNT(ul_afe_dump); i++) {
			printf("mtkmic: AFE %#05x %-26s = %#010x\n", ul_afe_dump[i].reg, ul_afe_dump[i].name, rd(B_AFE, ul_afe_dump[i].reg));
		}
	}
	for (unsigned i = 0; i < COUNT(ul_pmic_dump); i++) {
		uint32_t value = 0;

		if (!pmic_read(ul_pmic_dump[i].reg, &value)) {
			printf("mtkmic: PMIC %#06x %s NOT READ\n", ul_pmic_dump[i].reg, ul_pmic_dump[i].name);
			break;
		}
		printf("mtkmic: PMIC %#06x %-18s = %#06x\n", ul_pmic_dump[i].reg, ul_pmic_dump[i].name, value);
	}
}

/* mtkaudio=1: nothing written. */
static void
dump(void)
{
	uint32_t cg1 = rd(B_INFRA, INFRA_CG1_STA), cg2 = rd(B_INFRA, INFRA_CG2_STA);
	uint32_t cfg3 = rd(B_TOP, TOP_CLK_CFG_3), cfg4 = rd(B_TOP, TOP_CLK_CFG_4);

	printf("mtkaudio: as the bootloader left it: infracfg MODULE_SW_CG_1 %#x (bit 25, the AFE's bus clock: %s), MODULE_SW_CG_2 %#x "
	    "(bit 4, its 26 MHz: %s)\n", cg1, (cg1 & (1u << 25)) ? "gated" : "runs", cg2, (cg2 & (1u << 4)) ? "gated" : "runs");
	printf("mtkaudio: topckgen CLK_CFG_3 %#x (audio_sel %u, %s), CLK_CFG_4 %#x (aud_intbus_sel %u, %s); Android gates all four "
	    "when nothing plays\n", cfg3, (cfg3 >> 24) & 3, (cfg3 & (1u << 31)) ? "gated" : "runs", cfg4, cfg4 & 3,
	    (cfg4 & (1u << 7)) ? "gated" : "runs");
	printf("mtkaudio: pins: %u/%u/%u (the link to the PMIC) function %u/%u/%u (1 carries it); %u/%u (I2C0) function %u/%u, "
	    "levels %d/%d\n", link_pins[0], link_pins[1], link_pins[2], pin_mode(link_pins[0]), pin_mode(link_pins[1]),
	    pin_mode(link_pins[2]), PIN_SDA, PIN_SCL, pin_mode(PIN_SDA), pin_mode(PIN_SCL), pin_get(PIN_SDA), pin_get(PIN_SCL));

	if ((cg1 & (1u << 25)) || (cfg4 & (1u << 7))) {
		/* Android's own read of a gated AFE came back as a bus violation (logs/aud.dmesg, 171.009893); here it may not come back */
		printf("mtkaudio: the AFE's bus clock is gated: its registers are NOT read (mtkaudio=2 opens the gates first)\n");
	} else {
		printf("mtkaudio: reading the AFE's registers at %#x\n", block_at[B_AFE].phys);
		for (unsigned i = 0; i < COUNT(afe_dump); i++) {
			printf("mtkaudio: AFE %#05x %-26s = %#010x", afe_dump[i].reg, afe_dump[i].name, rd(B_AFE, afe_dump[i].reg));
			if (afe_dump[i].idle != NOT_SEEN) {
				printf("   (Android, idle: %#x)", afe_dump[i].idle);
			}
			printf("\n");
		}
	}
	printf("mtkaudio: reading the codec's registers through the PMIC wrapper\n");
	for (unsigned i = 0; i < COUNT(pmic_dump); i++) {
		uint32_t value = 0;

		if (!pmic_read(pmic_dump[i].reg, &value)) {
			printf("mtkaudio: PMIC %#06x %s NOT READ: the wrapper's WACS2 does not answer; no more are tried\n", pmic_dump[i].reg,
			    pmic_dump[i].name);
			break;
		}
		printf("mtkaudio: PMIC %#06x %-18s = %#06x", pmic_dump[i].reg, pmic_dump[i].name, value);
		if (pmic_dump[i].idle != NOT_SEEN) {
			printf("   (Android, idle: %#x)", pmic_dump[i].idle);
		}
		printf("\n");
	}
	if (mic_level >= 1) {
		dump_uplink((cg1 & (1u << 25)) || (cfg4 & (1u << 7)));
	}
}

static void
mtkaudio_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t level = (uint32_t)(uintptr_t)parameter;

	for (unsigned b = 0; b < B_MAPPED; b++) {
		block[b] = ml_io_map(block_at[b].phys, block_at[b].size);
	}
	pwrap = ml_io_map(PWRAP_PHYS, 0x1000);
	sram = (volatile uint32_t *)ml_io_map(SRAM_PHYS, DL1_BYTES);
	if (mic_level >= 2) {
		vul = (volatile uint32_t *)ml_io_map(mic_vul, UL_BYTES);
	}
	dump();
	if (level < 2) {
		thread_terminate(current_thread());
	}

	if (level >= 3) {
		printf("mtkaudio: the test: the codec and the amplifier up, the AFE up, a tone, everything down\n");
		if (!power_up_analog()) {
			power_down();
			printf("mtkaudio: the test FAILED before a sound; the sink is not set\n");
			thread_terminate(current_thread());
		}
		if (level >= 5) {
			monitors("the codec up, the AFE not yet: nothing comes over the link", false);
			printf("mtkaudio: the codec's own sine table into its DAC for a second, the line output's gain at step %u (8 is "
			    "0 dB, a step is 1 dB down): heard, the codec's analog side and the amplifier work\n", sgen_gain);
			pmic_set(PMIC_ZCD_CON1, (sgen_gain << 7) | sgen_gain, 0x0f9f);
			run("the codec's sine table on", pmic_sgen_on, COUNT(pmic_sgen_on));
			monitors("the codec's sine table plays", false);
			delay_for_interval(1000 - LOOKS * 50, NSEC_PER_MSEC);
			run("the codec's sine table off", pmic_sgen_off, COUNT(pmic_sgen_off));
		}
		power_up_afe();
		if (level >= 4) {
			monitors("the link runs and carries silence", true);
			run("the AFE's sine generator into the DAC for a second, a thirty-second of full scale", sgen_on, COUNT(sgen_on));
			monitors("the link carries the AFE's sine", true);
			delay_for_interval(1000 - LOOKS * 50, NSEC_PER_MSEC);
			run("the sine generator off", sgen_off, COUNT(sgen_off));
		}
		printf("mtkaudio: one second of 440 Hz through the buffer, peak %u of 32767\n", tone_peak);
		lck_mtx_lock(&mtkaudio_lock);
		tone_y1 = 0;
		tone_y2 = -(int32_t)tone_peak * TONE_HZ_SIN;
		tone_left = REDWOOD_AUDIO_RATE;
		lck_mtx_unlock(&mtkaudio_lock);
		feed_run(true);
		power_down();
	}

	if (mic_level >= 3) {
		mic_selftest_run();
	}
	if (mic_level >= 2) {
		mic_node_make();
	}
	redwood_audio_set_sink("the r1's speaker (the AFE, the MT6357 and the AW87390, redwood_mtkaudio.c)", mtkaudio_sink);
	for (;;) {
		lck_mtx_lock(&mtkaudio_lock);
		while (!feed_wanted) {
			if (mic_level < 2) {
				msleep(&feed_wanted, &mtkaudio_lock, PZERO, "redwood-mtkaudio", NULL);
			} else if (mic_on) {
				lck_mtx_unlock(&mtkaudio_lock);
				mic_poll();
				delay_for_interval(POLL_MS, NSEC_PER_MSEC);
				lck_mtx_lock(&mtkaudio_lock);
			} else {
				struct timespec idle = { 0, 20 * NSEC_PER_MSEC };

				lck_mtx_unlock(&mtkaudio_lock);
				mic_poll();
				lck_mtx_lock(&mtkaudio_lock);
				if (!feed_wanted) {
					msleep(&feed_wanted, &mtkaudio_lock, PZERO, "redwood-mtkaudio", &idle);
				}
			}
		}
		lck_mtx_unlock(&mtkaudio_lock);
		printf("mtkaudio: iOS plays: powering up\n");
		if (!power_up_analog()) {
			lck_mtx_lock(&mtkaudio_lock);
			feed_broken = true;
			ring_head = ring_tail;
			lck_mtx_unlock(&mtkaudio_lock);
			power_down();
			printf("mtkaudio: the power-up FAILED: nothing more is taken from iOS in this run\n");
			thread_terminate(current_thread());
		}
		power_up_afe();
		feed_run(false);
		power_down();
	}
}

void
redwood_mtkaudio_init(void)
{
	uint32_t level = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("mtkaudio", &level, sizeof(level)) || level == 0) {
		return;
	}
	PE_parse_boot_argn("mtkaudio_below", &volume_base, sizeof(volume_base));
	PE_parse_boot_argn("mtkaudio_tone", &tone_peak, sizeof(tone_peak));
	PE_parse_boot_argn("mtkaudio_skip0", &probe_skip, sizeof(probe_skip));
	PE_parse_boot_argn("mtkaudio_sgen_gain", &sgen_gain, sizeof(sgen_gain));
	/* the microphone (docs/microphone-spec.md); with no mtkmic, none of this does anything */
	PE_parse_boot_argn("mtkmic", &mic_level, sizeof(mic_level));
	PE_parse_boot_argn("mtkmic_ch", &mic_ch, sizeof(mic_ch));
	PE_parse_boot_argn("mtkmic_in_l", &mic_in_l, sizeof(mic_in_l));
	PE_parse_boot_argn("mtkmic_in_r", &mic_in_r, sizeof(mic_in_r));
	PE_parse_boot_argn("mtkmic_gain", &mic_gain, sizeof(mic_gain));
	PE_parse_boot_argn("mtkmic_mode", &mic_mode, sizeof(mic_mode));
	PE_parse_boot_argn("mtkmic_bias", &mic_bias, sizeof(mic_bias));
	PE_parse_boot_argn("mtkmic_shift", &mic_shift, sizeof(mic_shift));
	PE_parse_boot_argn("mtkmic_settle_ms", &mic_settle_ms, sizeof(mic_settle_ms));
	PE_parse_boot_argn("mtkmic_vul", &mic_vul, sizeof(mic_vul));
	PE_parse_boot_argn("mtkmic_swap", &mic_swap, sizeof(mic_swap));
	PE_parse_boot_argn("mtkmic_pins140", &mic_pins140, sizeof(mic_pins140));
	PE_parse_boot_argn("mtkmic_dcc_off", &mic_dcc_off, sizeof(mic_dcc_off));
	PE_parse_boot_argn("mtkmic_top_both", &mic_top_both, sizeof(mic_top_both));
	PE_parse_boot_argn("mtkmic_t12_peak", &mic_t12_peak, sizeof(mic_t12_peak));
	PE_parse_boot_argn("mtkmic_t4_db", &mic_t4_db, sizeof(mic_t4_db));
	PE_parse_boot_argn("mtkmic_t4_peak", &mic_t4_peak, sizeof(mic_t4_peak));
	PE_parse_boot_argn("mtkmic_t4_tone", &mic_t4_tone, sizeof(mic_t4_tone));
	PE_parse_boot_argn("mtkmic_t4_gap", &mic_t4_gap, sizeof(mic_t4_gap));
	PE_parse_boot_argn("mtkmic_t5", &mic_t5, sizeof(mic_t5));
	mic_ch = mic_ch ? 1 : 0;
	mic_settle_ms = mic_settle_ms > 2000 ? 2000 : mic_settle_ms;
	if (mic_vul < SRAM_PHYS + DL1_BYTES || mic_vul + UL_BYTES > UL_SRAM_LAST || (mic_vul & 0xfff) != 0) {
		printf("mtkmic: mtkmic_vul=%#x is not page aligned inside the AFE's SRAM after DL1: %#x used\n", mic_vul,
		    UL_SRAM_PHYS_DEFAULT);
		mic_vul = UL_SRAM_PHYS_DEFAULT;
	}
	if (sgen_gain > 0x1f) {
		sgen_gain = 0x1f;       /* DL_GAIN_N_40DB */
	}
	if (tone_peak > 8000) {
		tone_peak = 8000;       /* a quarter of full scale: it is a test, and tone_y2 must fit */
	}
	if (kernel_thread_start(mtkaudio_thread, (void *)(uintptr_t)level, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("mtkaudio: cannot start its thread\n");
	}
}

/*
 * On the way back to Android (loader_return.c) and on a panic's: the microphone's bias must not stay on. No lock and no
 * waiting: the uplink source, both ADCs and both biases are written straight. Nothing if the microphone was never up.
 */
void
redwood_mtkaudio_quiesce(void)
{
	uint32_t con9 = 0;

	if (mic_summary[0] != 0) {
		printf("%s\n", mic_summary);   /* (the log keeps its beginning and its end: said again here) */
	}
	if (!(users & USE_UL) || pwrap == 0) {
		return;
	}
	(void)pmic_write_raw(PMIC_AFE_UL_SRC_CON0_L, 0);
	(void)pmic_write_raw(PMIC_AUDENC_ANA_CON0, 0);
	(void)pmic_write_raw(PMIC_AUDENC_ANA_CON1, 0);
	(void)pmic_write_raw(PMIC_AUDENC_ANA_CON8, 0);          /* MICBIAS0 off */
	if (pmic_read_raw(PMIC_AUDENC_ANA_CON9, &con9)) {
		(void)pmic_write_raw(PMIC_AUDENC_ANA_CON9, con9 & ~1u);  /* MICBIAS1 off */
	}
	printf("mtkmic: the run ends with the microphone up: its bias is switched off\n");
}
