/*
 * The Infinix Hot 20's battery as iOS reads it. Boot-arg mtkbatt=1; mtkbatt=2 says every reading in the log, every 2 s
 * instead of every 10; mtkbatt=3 takes the percentage from the voltage alone (the way without Android's number).
 *
 * The percentage is Android's own, carried on by the PMIC's counter of charge:
 *  - Android's gauge is a program, and it leaves the percentage it shows in the PMIC's clock, bits 14:8 of RTC_AL_MTH (0x5aa):
 *    0xe401 while Android said 100, 0xe301 while it said 99 (10 November 2026, read from Android through pmic_access).
 *  - The MT6358's fuel gauge counts the charge through the phone's 5 mOhm (R_FG_VALUE of its device tree) in FGADC_CAR_CON0/1
 *    (0xd14, 0xd16), and Android does not start the counter again: it went on from XNU's last number after run 764. Read after
 *    a latch (FGADC_CON1, 0xd0a: bit 0 asks, bit 15 says latched, bit 3 lets go) with the current, FGADC_CUR_CON0 (0xd8a):
 *    16 bits with a sign, 250 mV / 65536 a count over 5 mOhm = 0.763 mA. Run 764 saw -240..-437 mA on a computer's port
 *    (Android says about the same there, -357 mA), the voltage lower the more negative it was, and the counter falling:
 *    less than 0 is out of the battery. The counter fell 92546 in the 90.8 s that the current summed to 9.94 mAh:
 *    0.1074 uAh a count. The cell is 5132 mAh (the 25 degree table of the phone's profile), 51 mAh a point.
 *  - So: the percentage in the clock when the kernel starts, plus the counter's change since. Not written back to the clock:
 *    Android's gauge makes its own on its next start.
 * Checked against the voltage, and without Android's number (0, or over 100) it is the voltage alone:
 *  - The MT6358's BATADC, channel 0 of its AUXADC. A conversion is asked for (bit 0 of AUXADC_RQST0, 0x1108) and its result
 *    is AUXADC_ADC0 (0x1088: 15 bits, bit 15 "ready"). Without the request the register keeps the last conversion somebody
 *    made: run 754 read 26788 counts at every look for a minute, which was Android's last. A count is 1.8 V x 3 / 32768 =
 *    0.16479 mV: 26775 counts while Android's own gauge said 4414 mV.
 *  - The cell at rest by a curve of a 4.45 V Li-ion cell, checked at ten points against the phone's profile: 4345 mV is 90%,
 *    3885 mV 50%, 3690 mV 10%. Under a load the voltage is brought back by the current over 0.28 Ohm, which is what run
 *    764's own pairs give (4306 mV at -240 mA, 4254 mV at -422 mA) and more than the profile's 0.13: it is rough, 4276 mV
 *    at -357 mA is 94% by it where Android says 99. If the two ways are more than 30 points apart the voltage is believed.
 * A charger on the port: RGS_CHRDET, bit 5 of CHR_CON0 (0xa88), the name Android's PMIC header gives it; 0x62 with the cable
 * in. It has not been seen at 0 yet (the cable is how the phone is run). "Charging" is said only while the current goes in.
 * Not done: the charger itself (RT9471 on I2C5: nothing sets or feeds it here), the temperature (25 degrees is said).
 * One transaction at a time on the wrapper: redwood_pwrap_lock() is the camera's and CONNSYS's too.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define PWRAP_BASE              0x1000d000ULL
#define PWRAP_WACS2_CMD         0xc20
#define PWRAP_WACS2_RDATA       0xc24
#define PWRAP_WACS2_VLDCLR      0xc28
#define PWRAP_FSM(r)            (((r) >> 16) & 7u)

#define MT6358_RTC_AL_MTH       0x05aa          /* bits 14:8: the percentage Android's gauge shows */
#define MT6358_CHR_CON0         0x0a88
#define CHR_CON0_CHRDET         (1u << 5)
#define MT6358_FGADC_CON0       0x0d08
#define MT6358_FGADC_CON1       0x0d0a
#define FGADC_CON1_READ         (1u << 0)
#define FGADC_CON1_CLEAR        (1u << 3)
#define FGADC_CON1_LATCHED      (1u << 15)
#define MT6358_FGADC_CAR_CON0   0x0d14
#define MT6358_FGADC_CAR_CON1   0x0d16
#define MT6358_FGADC_CUR_CON0   0x0d8a
#define MT6358_AUXADC_ADC0      0x1088          /* BATADC */
#define MT6358_AUXADC_RQST0     0x1108

#define COUNT_NV                164795          /* 1.8 V x 3 / 32768, in nV a count */
#define FG_COUNT_NA             762939          /* 250 mV / 65536 over 5 mOhm, in nA a count */
#define CAR_COUNT_PAH           107425          /* run 764: 9.94 mAh by the current while the counter fell 92546, in pAh a count */
#define CELL_UAH                5132000         /* the phone's profile at 25 degrees */
#define CELL_MOHM               280
#define FULL_MV                 4410
#define APART_HUNDREDTHS        3000

static volatile uint32_t *pwrap;
/* the MT6358's by default; the MT6357 (r1_batt=1) moves these (r1batt_thread) */
static uint32_t adc_rqst = MT6358_AUXADC_RQST0, adc_reg = MT6358_AUXADC_ADC0, car_reg = MT6358_FGADC_CAR_CON0;
static uint16_t adc_bit = 1;

static bool
pwrap_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 200000; spin++) {
		if (PWRAP_FSM(pwrap[PWRAP_WACS2_RDATA / 4]) == state) {
			return true;
		}
	}
	return false;
}

static bool
pmic_read(uint32_t reg, uint16_t *value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (pwrap_wait(0)) {
		pwrap[PWRAP_WACS2_CMD / 4] = (reg >> 1) << 16;
		if (pwrap_wait(6)) {
			*value = (uint16_t)pwrap[PWRAP_WACS2_RDATA / 4];
			pwrap[PWRAP_WACS2_VLDCLR / 4] = 1;
			ok = true;
		}
	}
	redwood_pwrap_unlock();
	return ok;
}

static bool
pmic_write(uint32_t reg, uint16_t value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (pwrap_wait(0)) {
		pwrap[PWRAP_WACS2_CMD / 4] = (1u << 31) | ((reg >> 1) << 16) | value;
		ok = pwrap_wait(0);
	}
	redwood_pwrap_unlock();
	return ok;
}

/* the register's bits (mask) at `want`, looked at every millisecond for `ms` */
static bool
pmic_until(uint32_t reg, uint16_t mask, uint16_t want, unsigned ms, uint16_t *last)
{
	for (unsigned t = 0; t <= ms; t++) {
		if (pmic_read(reg, last) && (*last & mask) == want) {
			return true;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	return false;
}

/* one conversion of the battery's voltage, asked for now: counts, or -1 with the reason said once */
static int32_t
batadc_convert(void)
{
	static bool said;
	uint16_t raw = 0;

	if (pmic_write(adc_rqst, adc_bit)) {
		/* the last conversion's "ready" may still be up: the converter is given its time before the look */
		delay_for_interval(3, NSEC_PER_MSEC);
		if (pmic_until(adc_reg, 0x8000, 0x8000, 50, &raw)) {
			return raw & 0x7fff;
		}
	}
	if (!said) {
		said = true;
		printf("mtkbatt: no conversion of BATADC in 50 ms after the request (ADC0 %#x): no reading is given\n", raw);
	}
	return -1;
}

/*
 * The fuel gauge's current and its counter of charge, latched together. False, with the reason said once, if the gauge does
 * not latch (it is Android that switched it on; nothing here does).
 */
static bool
gauge_latch(int16_t *current, uint32_t *charge)
{
	static bool said;
	uint16_t con1 = 0, got = 0, low = 0, high = 0, cur = 0;
	bool ok;

	if (!pmic_read(MT6358_FGADC_CON1, &con1)) {
		return false;
	}
	con1 &= (uint16_t)~(FGADC_CON1_READ | FGADC_CON1_CLEAR | FGADC_CON1_LATCHED);
	ok = pmic_write(MT6358_FGADC_CON1, con1 | FGADC_CON1_READ) &&
	    pmic_until(MT6358_FGADC_CON1, FGADC_CON1_LATCHED, FGADC_CON1_LATCHED, 100, &got) &&
	    pmic_read(MT6358_FGADC_CUR_CON0, &cur) && pmic_read(car_reg, &low) && pmic_read(car_reg + 2, &high);
	/* let go, whatever happened */
	(void)pmic_write(MT6358_FGADC_CON1, con1 | FGADC_CON1_CLEAR);
	(void)pmic_until(MT6358_FGADC_CON1, FGADC_CON1_LATCHED, 0, 100, &got);
	(void)pmic_write(MT6358_FGADC_CON1, con1);
	if (!ok) {
		if (!said) {
			uint16_t con0 = 0;

			said = true;
			(void)pmic_read(MT6358_FGADC_CON0, &con0);
			printf("mtkbatt: the fuel gauge does not latch (FGADC_CON0 %#x, CON1 %#x): no current\n", con0, got);
		}
		return false;
	}
	*current = (int16_t)cur;
	*charge = ((uint32_t)high << 16) | low;
	return true;
}

/* the cell's charge at rest by its voltage, in hundredths of a percent: (mV, hundredths) */
static int32_t
percent_of(int32_t mv)
{
	static const struct { int32_t mv, hundredths; } curve[] = {
		{ 3400, 0 }, { 3620, 500 }, { 3690, 1000 }, { 3745, 2000 }, { 3785, 3000 }, { 3825, 4000 }, { 3885, 5000 },
		{ 3985, 6000 }, { 4100, 7000 }, { 4225, 8000 }, { 4345, 9000 }, { 4395, 9700 }, { FULL_MV, 10000 },
	};

	if (mv <= curve[0].mv) {
		return 0;
	}
	for (unsigned i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
		if (mv <= curve[i].mv) {
			return curve[i - 1].hundredths + (mv - curve[i - 1].mv) * (curve[i].hundredths - curve[i - 1].hundredths) / (curve[i].mv - curve[i - 1].mv);
		}
	}
	return 10000;
}

static void
mtkbatt_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t level = 1;
	uint32_t looks = 0;
	int32_t kept_mv = 0;            /* the cell's voltage at rest, smoothed over four looks */
	int32_t kept_ma = 0;            /* the current, the same way: "charging" does not flicker with it */
	int32_t start_hundredths = -1;  /* Android's percentage, or -1 */
	uint32_t start_charge = 0;
	uint16_t rtc = 0;
	bool counted = false, apart_said = false;

	PE_parse_boot_argn("mtkbatt", &level, sizeof(level));
	pwrap = (volatile uint32_t *)ml_io_map(PWRAP_BASE, 0x1000);
	if (pmic_read(MT6358_RTC_AL_MTH, &rtc) && ((rtc >> 8) & 0x7f) >= 1 && ((rtc >> 8) & 0x7f) <= 100) {
		start_hundredths = (int32_t)((rtc >> 8) & 0x7f) * 100;
	}
	delay_for_interval(20000, NSEC_PER_MSEC);
	for (;; delay_for_interval(level == 2 ? 2000 : 10000, NSEC_PER_MSEC)) {
		struct redwood_battery_state b = { .size = sizeof(b) };
		int32_t sum = 0, good = 0, low = 0x7fff, high = 0, mv, rest_mv, ma = 0, by_voltage, by_charge = -1;
		uint16_t chr = 0;
		int16_t gauge = 0;
		uint32_t charge = 0;
		bool charger, current;

		for (unsigned i = 0; i < 4; i++) {
			int32_t counts = batadc_convert();

			if (counts >= 0) {
				sum += counts;
				good++;
				low = counts < low ? counts : low;
				high = counts > high ? counts : high;
			}
		}
		if (good == 0) {
			continue;       /* nothing is said to iOS on a look that read nothing: it keeps what it had */
		}
		mv = (int32_t)((int64_t)(sum / good) * COUNT_NV / 1000000);
		if (mv < 2500 || mv > 4700) {
			printf("mtkbatt: BATADC %d counts would be %d mV: not a cell's voltage, not given\n", sum / good, mv);
			continue;
		}
		charger = pmic_read(MT6358_CHR_CON0, &chr) && (chr & CHR_CON0_CHRDET);
		current = gauge_latch(&gauge, &charge);
		if (current) {
			ma = (int32_t)((int64_t)gauge * FG_COUNT_NA / 1000000);
		}
		rest_mv = mv - ma * CELL_MOHM / 1000;
		kept_mv = kept_mv == 0 ? rest_mv : (kept_mv * 3 + rest_mv) / 4;
		kept_ma = (kept_ma * 3 + ma) / 4;
		by_voltage = percent_of(kept_mv);
		if (current && start_hundredths >= 0 && level != 3) {
			if (!counted) {
				counted = true;
				start_charge = charge;
				printf("mtkbatt: Android left %d%% (RTC_AL_MTH %#x); the charge is counted from %#x\n", start_hundredths / 100, rtc, charge);
			}
			/* hundredths of a percent: counts x pAh / (uAh x 100) */
			by_charge = start_hundredths + (int32_t)((int64_t)(int32_t)(charge - start_charge) * CAR_COUNT_PAH / ((int64_t)CELL_UAH * 100));
			by_charge = by_charge < 0 ? 0 : by_charge > 10000 ? 10000 : by_charge;
			if (by_charge - by_voltage > APART_HUNDREDTHS || by_voltage - by_charge > APART_HUNDREDTHS) {
				if (!apart_said) {
					apart_said = true;
					printf("mtkbatt: %d%% by the charge and %d%% by the voltage (%d mV at rest): the voltage is believed from here\n",
					    by_charge / 100, by_voltage / 100, kept_mv);
				}
				start_hundredths = -1;
				by_charge = -1;
			}
		} else if (!counted && !apart_said && looks == 0) {
			printf("mtkbatt: the percentage is by the voltage alone (RTC_AL_MTH %#x, %s, mtkbatt=%u)\n", rtc,
			    current ? "the gauge answers" : "no gauge", level);
		}
		b.percent_hundredths = by_charge >= 0 ? by_charge : by_voltage;
		b.voltage_uv = mv * 1000;
		b.current_ua = current ? ma * 1000 : 0;
		b.temperature_hundredths = 2500;        /* not read: 25 degrees */
		b.present = 1;
		b.external = charger;
		/* Linux's numbers: 1 charging, 2 discharging, 3 not charging, 4 full */
		if (!charger) {
			b.status = 2;
		} else if (b.percent_hundredths >= 9950) {
			b.status = 4;
		} else if (current && kept_ma <= 0) {
			b.status = 3;
		} else {
			b.status = 1;
		}
		redwood_battery_report(&b);
		if (level >= 2 || (looks % 30) == 0) {
			printf("mtkbatt: %d.%02d%% (%s), BATADC %d counts (%d..%d of %d) = %d mV, charger %s (CHR_CON0 %#x), status %d",
			    b.percent_hundredths / 100, b.percent_hundredths % 100, by_charge >= 0 ? "by the charge" : "by the voltage",
			    sum / good, low, high, good, mv, charger ? "in" : "out", chr, b.status);
			if (current) {
				printf("; gauge %d counts = %d mA, charge counter %#x, at rest %d mV = %d.%02d%% by the voltage\n", gauge, ma, charge,
				    kept_mv, by_voltage / 100, by_voltage % 100);
			} else {
				printf("; no current\n");
			}
		}
		looks++;
	}
}

/*
 * The rabbit r1's battery (boot-arg r1_batt=1): an MT6357, 1000 mAh, 4.4 V, 10 mOhm shunt. Registers from the Linux port's
 * mt6357-gauge.c / auxadc6357.c / registers.h; none of it has been read on this hardware, so every raw value is logged ("r1batt:").
 *  - HWCID 0x8, 0x57xx for an MT6357. Anything else: nothing is touched.
 *  - Voltage: the gauge's ISENSE channel (the device tree: "stock battery voltage is ISENSE, not BATADC"), RQST0 0x110e bit 1,
 *    result ADC1 0x108a, ratio 3 over 1 of 1.8 V / 32768.
 *  - Current: FGADC_CUR_CON0 0xd8a, ones' complement, 3.14331 x count in 0.1 mA over 10 mOhm, negative out of the cell.
 *    Counter: CAR_CON0/1 0xd12/0xd14 (bits 30:11 of the pair, sign 31). Both only if FGADC_CON0 bit 0 says the gauge is on.
 *  - Percentage: the voltage's own curve (percent_of), unloaded. Charger: CHR_CON0 0xb88 bit 5 is a guess; the raw register is logged.
 */
static void
r1batt_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint16_t id = 0, con0 = 0;
	int32_t kept_mv = 0, r1_mv_off = 65, shown = -1;   /* +65 mV: ISENSE read 4326-4336 where Android showed 4392-4398 */

	/* no efuse gain/offset in the MT6357 ADC reference (auxadc6357.c: fixed 1800 mV vref), so the default is the +65 mV measured against Android's reading: r1_batt_mv_off= (mV, may be negative) */
	PE_parse_boot_argn("r1_batt_mv_off", &r1_mv_off, sizeof(r1_mv_off));
	adc_rqst = 0x110e;
	adc_reg = 0x108a;
	car_reg = 0x0d12;
	adc_bit = 2;
	pwrap = (volatile uint32_t *)ml_io_map(PWRAP_BASE, 0x1000);
	if (!pmic_read(0x0008, &id) || (id & 0xff00) != 0x5700) {
		printf("r1batt: HWCID %#x is not an MT6357 (0x57xx), or the PMIC does not answer: left alone\n", id);
		return;
	}
	(void)pmic_read(0x0d08, &con0);
	printf("r1batt: HWCID %#x; FGADC_CON0 %#x: gauge %s; mV offset %d\n", id, con0, (con0 & 1) ? "on" : "off, no current", r1_mv_off);
	delay_for_interval(20000, NSEC_PER_MSEC);
	for (unsigned looks = 0;; looks++, delay_for_interval(10000, NSEC_PER_MSEC)) {
		struct redwood_battery_state b = { .size = sizeof(b) };
		int32_t sum = 0, good = 0, mv, ma = 0, percent;
		uint16_t chr = 0;
		int16_t gauge = 0;
		uint32_t charge = 0;
		bool charger, current = false;

		for (unsigned i = 0; i < 4; i++) {
			int32_t counts = batadc_convert();

			if (counts >= 0) {
				sum += counts;
				good++;
			}
		}
		if (good == 0) {
			continue;
		}
		mv = (int32_t)((int64_t)(sum / good) * COUNT_NV / 1000000);
		if (mv < 2500 || mv > 4700) {
			printf("r1batt: ISENSE %d counts would be %d mV: not a cell's voltage, not given\n", sum / good, mv);
			continue;
		}
		(void)pmic_read(0x0b88, &chr);
		charger = chr & CHR_CON0_CHRDET;
		(void)pmic_read(0x0d08, &con0);
		if (con0 & 1) {
			current = gauge_latch(&gauge, &charge);
		}
		if (current) {
			int32_t mag = (gauge & 0x8000) ? 0xffff - (uint16_t)gauge : (uint16_t)gauge;

			ma = (int32_t)((int64_t)mag * 314331 / 1000000);        /* 0.1 mA x 3.14331, then to mA */
			ma = (gauge & 0x8000) ? -ma : ma;
		}
		mv += r1_mv_off;
		kept_mv = kept_mv == 0 ? mv : (kept_mv * 3 + mv) / 4;
		/* a 4.4 V cell rests at about 4.35 V full: stretch the curve above 3985 mV so 4350 reads as FULL_MV */
		percent = percent_of(kept_mv > 3985 ? 3985 + (kept_mv - 3985) * (FULL_MV - 3985) / (4350 - 3985) : kept_mv);
		/* on the charger, full and nearly no current: 100 % (the cell is topped up; its voltage sags under the charger's taper) */
		bool terminated = charger && kept_mv >= 4380 && current && ma > -100 && ma < 100;

		if (terminated) {
			percent = 10000;
		}
		/* low-pass: after the first reading at most 1 % per 30 s, never up while discharging, never down while charging */
		if (shown < 0 || terminated) {
			shown = percent;
		} else if (looks % 3 == 0 && percent != shown && (percent > shown ? charger : !charger)) {
			shown += percent > shown ? (percent - shown < 100 ? percent - shown : 100) : -(shown - percent < 100 ? shown - percent : 100);
		}
		percent = shown;
		b.percent_hundredths = percent;
		b.voltage_uv = mv * 1000;
		b.current_ua = ma * 1000;
		b.temperature_hundredths = 2500;
		b.present = 1;
		b.external = charger;
		b.status = !charger ? 2 : percent >= 9950 ? 4 : (current && ma <= 0) ? 3 : 1;
		redwood_battery_report(&b);
		if (looks < 6 || (looks % 6) == 0) {
			printf("r1batt: HWCID %#x %d.%02d%%, ISENSE %d counts = %d mV (offset %d), CHR_CON0 %#x (charger %s), FGADC_CON0 %#x, status %d; current %#x = %d mA, counter %#x%s\n",
			    id, percent / 100, percent % 100, sum / good, mv, r1_mv_off, chr, charger ? "in" : "out", con0, b.status,
			    (uint16_t)gauge, ma, charge, current ? "" : " (no current)");
		}
	}
}

/*
 * RTC (boot-arg r1_rtc, GenericARMPE.cpp): the MT6357's time counter, base 0x588. BBPU (0x588) is only read
 * (CBUSY bit 6); never written here, nor PDN1/PDN2 (0x5b4/0x5b6), AL_* (0x5a0-0x5ae), or base+0x50/0x52.
 * The log lines are stashed until redwood_mtkbatt_init (findBootEpoch runs before the log is kept).
 */
extern void redwood_host_keep_add(const char *format, ...) __printflike(1, 2);
#define RTC_BBPU        0x588
#define RTC_WRTGR       0x5c2
#define RTC_TC(i)       (0x592 + 2 * (i))       /* SEC MIN HOU DOM DOW MTH YEA */
static const uint16_t rtc_mask[7] = { 0x3f, 0x3f, 0x1f, 0x1f, 0x7, 0xf, 0x7f };
static uint32_t rtc_base = 1968;
static char rtc_stash[6][200];
static uint32_t rtc_stashed;
static bool rtc_flushed;

void redwood_rtc_say(const char *format, ...) __printflike(1, 2);
void
redwood_rtc_say(const char *format, ...)
{
	char line[200];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (rtc_flushed) {
		redwood_host_keep_add("%s", line);
	} else if (rtc_stashed < 6) {
		strlcpy(rtc_stash[rtc_stashed++], line, sizeof(rtc_stash[0]));
	}
}

static bool
rtc_pwrap(void)
{
	if (pwrap == NULL) {
		pwrap = (volatile uint32_t *)ml_io_map(PWRAP_BASE, 0x1000);
	}
	return pwrap != NULL;
}

static uint64_t
days_from_civil(uint32_t y, uint32_t m, uint32_t d)
{
	y -= m <= 2;
	uint32_t era = y / 400, yoe = y - era * 400;
	uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return (uint64_t)era * 146097 + doe - 719468;
}

static bool
rtc_regs(uint16_t v[7], uint16_t *bbpu)
{
	for (int i = 0; i < 7; i++) {
		if (!pmic_read(RTC_TC(i), &v[i])) {
			return false;
		}
	}
	return pmic_read(RTC_BBPU, bbpu);
}

bool
redwood_rtc_read(uint64_t *epoch)
{
	uint16_t v[7], bbpu = 0, sec2;

	if (!rtc_pwrap() || !rtc_regs(v, &bbpu)) {
		redwood_rtc_say("r1rtc: the PMIC does not answer");
		return false;
	}
	if (pmic_read(RTC_TC(0), &sec2) && sec2 < v[0] && !rtc_regs(v, &bbpu)) {  /* a carry between the reads */
		return false;
	}
	redwood_rtc_say("r1rtc: raw SEC %u MIN %u HOU %u DOM %u DOW %u MTH %#x YEA %u BBPU %#x",
	    v[0], v[1], v[2], v[3], v[4], v[5], v[6], bbpu);
	uint32_t year = (v[6] >= 56 && v[6] <= 99) ? 1968 + v[6] : (v[6] >= 124 && v[6] <= 127) ? 1900 + v[6] : 0;
	uint32_t mon = v[5] & 0xf;
	if (year < 2024 || year > 2099 || mon < 1 || mon > 12 || v[3] < 1 || v[3] > 31 || (v[2] & 0x1f) > 23 ||
	    (v[1] & 0x3f) > 59 || (v[0] & 0x3f) > 59) {
		redwood_rtc_say("r1rtc: not a time of day: not used");
		return false;
	}
	rtc_base = year - v[6];
	*epoch = days_from_civil(year, mon, v[3] & 0x1f) * 86400 + (v[2] & 0x1f) * 3600 + (v[1] & 0x3f) * 60 + (v[0] & 0x3f);
	redwood_rtc_say("r1rtc: %04u-%02u-%02u %02u:%02u:%02u UTC (base %u) = %llu", year, mon, v[3] & 0x1f, v[2] & 0x1f,
	    v[1] & 0x3f, v[0] & 0x3f, rtc_base, *epoch);
	return true;
}

bool
redwood_rtc_write(uint64_t epoch)
{
	uint64_t days = epoch / 86400, rem = epoch % 86400;
	uint16_t want[7], old, got, bbpu = 0;
	clock_sec_t s0, s1;
	clock_usec_t u0, u1;
	bool trig, mismatch = false;
	uint32_t z = (uint32_t)days + 719468, era = z / 146097, doe = z - era * 146097;
	uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	uint32_t mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
	uint32_t y = yoe + era * 400 + (m <= 2);

	if (y < 2024 || y - rtc_base > (rtc_base == 1968 ? 99u : 127u) || !rtc_pwrap()) {
		redwood_rtc_say("r1rtc: set %llu: year %u does not fit (base %u): not written", epoch, y, rtc_base);
		return false;
	}
	want[0] = rem % 60; want[1] = rem / 60 % 60; want[2] = rem / 3600; want[3] = d;
	want[4] = (days + 4) % 7 + 1; want[5] = m; want[6] = y - rtc_base;
	for (int i = 0; i < 7; i++) {
		if (!pmic_read(RTC_TC(i), &old) || !pmic_write(RTC_TC(i), (old & ~rtc_mask[i]) | want[i])) {
			redwood_rtc_say("r1rtc: set %llu: register %d not written", epoch, i);
			return false;
		}
	}
	trig = pmic_write(RTC_WRTGR, 1);
	clock_get_system_microtime(&s0, &u0);
	do {
		delay_for_interval(10, NSEC_PER_USEC);
		clock_get_system_microtime(&s1, &u1);
	} while ((!pmic_read(RTC_BBPU, &bbpu) || (bbpu & (1u << 6))) && (s1 - s0) * 1000000ULL + u1 - u0 < 1000000);
	for (int i = 0; i < 7; i++) {
		mismatch |= !pmic_read(RTC_TC(i), &got) || (got & rtc_mask[i]) != want[i];
	}
	redwood_rtc_say("r1rtc: set %llu: trigger %s, CBUSY clear after %u us, read-back %s", epoch, trig ? "ok" : "timeout",
	    (uint32_t)((s1 - s0) * 1000000ULL + u1 - u0), mismatch ? "MISMATCH" : "ok");
	return !mismatch;
}

void
redwood_mtkbatt_init(void)
{
	uint32_t level = 0;
	thread_t thread;

	rtc_flushed = true;
	for (uint32_t i = 0; i < rtc_stashed; i++) {
		redwood_host_keep_add("%s", rtc_stash[i]);
	}

	if (PE_parse_boot_argn("r1_batt", &level, sizeof(level)) && level == 1) {
		if (kernel_thread_start(r1batt_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
		return;
	}
	if (!PE_parse_boot_argn("mtkbatt", &level, sizeof(level)) || level < 1) {
		return;
	}
	if (kernel_thread_start(mtkbatt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
