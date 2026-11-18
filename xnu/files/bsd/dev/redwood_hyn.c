/*
 * The touch screen of the rabbit r1 (MT6765): a Hynitron mutual-capacitance controller (Android's
 * "hyn_ts", protocol of the CST3xx family) at address 0x1a on I2C4, as a source of touches for
 * /dev/redwood-touch. Only with boot-arg touchscreen=3: every address below belongs to this device.
 *
 * The chip keeps its firmware in its own flash, so nothing is uploaded: reset it and read.
 *   pins     GPIO105 is the clock and 106 the data of I2C4, driven here by hand as GPIOs (the way
 *            redwood_mtkcam.c drives the camera's bus); GPIO174 is the reset, active low; GPIO0 is
 *            the interrupt line
 *   chip     write d0 00, read the report; byte 6 is 0xab in a valid one, byte 5 the count of
 *            fingers; a finger is five bytes (the first at 0, the second at 7):
 *            id << 4 | state (6 = down), x high, y high, x low << 4 | y low, pressure.
 *            Then write d0 00 ab to say it was read.
 * No interrupt is used: a thread reads the report 60 times a second (boot-arg touch_poll_us).
 *
 * Read from the device under Android (the i2c tracepoints, /proc/interrupts, the device tree) and
 * from Linux drivers/input/touchscreen/hynitron_cstxxx.c and drivers/pinctrl/mediatek/pinctrl-mt6765.c.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern void thread_terminate_self(void);

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define GPIO_PHYS       0x10005000
#define GPIO_SIZE       0x1000
#define PIN_IRQ         0
#define PIN_SCL         bus_scl
#define PIN_SDA         bus_sda
#define PIN_RESET       174
#define HYN_ADDR        0x1a
#define PANEL_W         480
#define PANEL_H         640
#define MAX_FINGERS     2
#define REPORT_BYTES    12      /* two fingers; the chip offers 28 */

static vm_offset_t gpio;
/*
 * Which bus the bit-banger is on (default: the touch screen's I2C4). buttons=3 (the wheel, I2C6) and r1_bl=1 (the backlight, I2C5)
 * borrow it, one transaction group at a time: bus_take() sets the pins, bus_give() lets go.
 */
static uint32_t bus_scl = 105, bus_sda = 106;
static volatile int bus_busy;

static void
bus_take(uint32_t scl, uint32_t sda)
{
	while (!__sync_bool_compare_and_swap(&bus_busy, 0, 1)) {
		delay(20);
	}
	bus_scl = scl;
	bus_sda = sda;
}

static void
bus_give(void)
{
	__sync_lock_release(&bus_busy);
}

static inline uint32_t
rd(uint32_t at)
{
	return *(volatile uint32_t *)(gpio + at);
}

static inline void
wr(uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(gpio + at) = value;
}

static void
pin_gpio_mode(uint32_t pin)     /* function 0, four bits a pin, eight pins a register */
{
	uint32_t at = 0x300 + 0x10 * (pin / 8);

	wr(at, rd(at) & ~(0xfu << ((pin % 8) * 4)));
}

static inline bool
pin_get(uint32_t pin)
{
	return (rd(0x200 + 0x10 * (pin / 32)) >> (pin % 32)) & 1u;
}

static void
pin_out(uint32_t pin, bool high)
{
	wr((high ? 0x104 : 0x108) + 0x10 * (pin / 32), 1u << (pin % 32));
	wr(0x004 + 0x10 * (pin / 32), 1u << (pin % 32));
}

/* open drain: released (an input, the pull-up takes it high) or driven low */
static void
line(uint32_t pin, bool high)
{
	if (high) {
		wr(0x008 + 0x10 * (pin / 32), 1u << (pin % 32));
	} else {
		pin_out(pin, false);
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
i2c_in(bool ack)
{
	uint8_t byte = 0;

	line(PIN_SDA, true);
	for (int i = 0; i < 8; i++) {
		scl_high();
		byte = (uint8_t)((byte << 1) | (pin_get(PIN_SDA) ? 1 : 0));
		line(PIN_SCL, false);
	}
	line(PIN_SDA, !ack);
	scl_high();
	line(PIN_SCL, false);
	return byte;
}

static bool
hyn_write(const uint8_t *bytes, uint32_t length)
{
	bool ok;

	i2c_start();
	ok = i2c_out(HYN_ADDR << 1);
	for (uint32_t i = 0; ok && i < length; i++) {
		ok = i2c_out(bytes[i]);
	}
	i2c_stop();
	return ok;
}

static bool
hyn_read(uint16_t reg, uint8_t *bytes, uint32_t length)
{
	const uint8_t at[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
	bool ok;

	if (!hyn_write(at, sizeof(at))) {
		return false;
	}
	i2c_start();
	ok = i2c_out((HYN_ADDR << 1) | 1);
	for (uint32_t i = 0; ok && i < length; i++) {
		bytes[i] = i2c_in(i + 1 < length);
	}
	i2c_stop();
	return ok;
}

static bool down[16];
static uint32_t last_x[16], last_y[16];
static unsigned reports, touches, misses;

static void
report(const uint8_t *b)
{
	bool seen[16] = { false };
	uint32_t count = b[5] & 0x7f;

	if (count > MAX_FINGERS) {
		count = MAX_FINGERS;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *f = b + (i ? i * 5 + 2 : 0);
		uint32_t which = f[0] >> 4;
		uint32_t x = ((uint32_t)f[1] << 4) | (f[3] >> 4), y = ((uint32_t)f[2] << 4) | (f[3] & 0xf);

		if ((f[0] & 0xf) != 6) {
			continue;               /* lifted */
		}
		x = x >= PANEL_W ? PANEL_W - 1 : x;
		y = y >= PANEL_H ? PANEL_H - 1 : y;
		last_x[which] = x * 65535 / (PANEL_W - 1);
		last_y[which] = y * 65535 / (PANEL_H - 1);
		seen[which] = true;
		touches++;
		redwood_touch_post(down[which] ? REDWOOD_TOUCH_MOVE : REDWOOD_TOUCH_DOWN, which, last_x[which], last_y[which]);
		if (!down[which]) {
			printf("hyn: finger %u down at %u, %u (%02x %02x %02x %02x %02x %02x %02x)\n", which, x, y,
			    b[0], b[1], b[2], b[3], b[4], b[5], b[6]);
		}
		down[which] = true;
	}
	for (uint32_t which = 0; which < 16; which++) {
		if (down[which] && !seen[which]) {
			redwood_touch_post(REDWOOD_TOUCH_UP, which, last_x[which], last_y[which]);
			down[which] = false;
		}
	}
	reports++;
}

static void
hyn_thread(__unused void *parameter, __unused wait_result_t result)
{
	static const uint8_t done[3] = { 0xd0, 0x00, 0xab };
	uint8_t b[REPORT_BYTES];
	uint32_t poll_us = 16000;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	if (poll_us < 1000 || poll_us > 100000) {
		poll_us = 16000;
	}
	/* hyn_poll_us=4000..33000: the sleep after each read (16000 = 55 Hz drifts against the 58.8 Hz panel; a read takes ~0.4 ms of bit-banging) */
	if (PE_parse_boot_argn("hyn_poll_us", &poll_us, sizeof(poll_us))) {
		poll_us = poll_us < 4000 ? 4000 : poll_us > 33000 ? 33000 : poll_us;
	}
	gpio = ml_io_map(GPIO_PHYS, GPIO_SIZE);
	bus_take(105, 106);
	pin_gpio_mode(PIN_SCL);
	pin_gpio_mode(PIN_SDA);
	pin_gpio_mode(PIN_RESET);
	line(PIN_SCL, true);
	line(PIN_SDA, true);
	pin_out(PIN_RESET, false);
	delay(10000);
	pin_out(PIN_RESET, true);
	delay(150000);
	printf("hyn: reset done; clock %d data %d interrupt %d; the chip %s at 0x%x\n", pin_get(PIN_SCL), pin_get(PIN_SDA),
	    pin_get(PIN_IRQ), hyn_read(0xd040, b, 6) ? "answers" : "does NOT answer", HYN_ADDR);
	printf("hyn: d040 = %02x %02x %02x %02x %02x %02x\n", b[0], b[1], b[2], b[3], b[4], b[5]);
	bus_give();

	for (unsigned tick = 1;; tick++) {
		bus_take(105, 106);
		if (!hyn_read(0xd000, b, sizeof(b))) {
			misses++;
		} else if (b[6] == 0xab) {
			report(b);
			hyn_write(done, sizeof(done));
		}
		bus_give();
		if (tick % (30 * (1000000 / poll_us)) == 0) {
			printf("hyn: so far %u reports, %u finger positions, %u reads not answered; interrupt line %d; "
			    "last read %02x %02x %02x %02x %02x %02x %02x\n", reports, touches, misses, pin_get(PIN_IRQ),
			    b[0], b[1], b[2], b[3], b[4], b[5], b[6]);
		}
		delay(poll_us);
	}
}

/*
 * The rest of the r1's hardware, on the same bit-banger. Each probe reads an ID first, says what it read, and does nothing if it
 * is not what the Linux port (haretic's mt6765-rabbit-r1) leads one to expect. Everything is off unless its boot-arg is given.
 *
 * buttons=3 ("r1keys:"):
 *   side button  keypad KP_MEM1 (0x10010004) bit 1, clear while down: the lock button. Read only; the boot loader's watcher reads it too.
 *   wheel        OCH1970 Hall sensor, I2C6 (GPIO161 clock, 162 data) address 0x0d, reset GPIO173 low 10 ms. Registers as och1970.c:
 *                WIA 0x00, CNTL2 0x21 (standby, then continuous 100 Hz), X/Y/Z at 0x17 (8 bytes: status, X, Y, Z, big endian).
 *                The angle is atan2(X, Z); every 20 degrees is one volume step, a growing angle being down. Read every 10 ms while
 *                turning and 33 ms at rest; a part step left over after 400 ms of stillness is dropped.
 * r1_bl=1 ("r1bl:"): MT6370 backlight, I2C5 (GPIO48 clock, 49 data) address 0x34. DEV_INFO 0x00; as mt6370-backlight.c (its
 *   0x1A0/0x1A4/0x1A5 are bank 1 of the driver's regmap, so 0xA0/0xA4/0xA5 on the bus): DIM2 bits 2:0 = (b - 1) & 7, DIM1 = (b - 1) >> 3,
 *   then bit 6 of EN. The level is not touched at boot, only read. sysctl kern.redwood.backlight, 0 to 512 (the device tree's max).
 */
#define KPD_PHYS        0x10010000
#define KP_MEM1         0x0004
#define OCH_ADDR        0x0d
#define PIN_WHEEL_RESET 173
#define WHEEL_STEP_DEG  20
#define BL_ADDR         0x34
#define BL_MAX          512

/* register `reg` of the device at `addr` on the bus that bus_take() chose: written, or read for `length` bytes after a repeated start */
static bool
reg_write(uint8_t addr, uint8_t reg, uint8_t value)
{
	bool ok;

	i2c_start();
	ok = i2c_out(addr << 1) && i2c_out(reg) && i2c_out(value);
	i2c_stop();
	return ok;
}

static bool
reg_read(uint8_t addr, uint8_t reg, uint8_t *bytes, uint32_t length)
{
	bool ok;

	i2c_start();
	ok = i2c_out(addr << 1) && i2c_out(reg);
	if (ok) {
		i2c_start();
		ok = i2c_out((addr << 1) | 1);
	}
	for (uint32_t i = 0; ok && i < length; i++) {
		bytes[i] = i2c_in(i + 1 < length);
	}
	i2c_stop();
	return ok;
}

static uint32_t
now_ms(void)
{
	uint64_t t, ns;

	clock_get_uptime(&t);
	absolutetime_to_nanoseconds(t, &ns);
	return (uint32_t)(ns / 1000000);
}

/*
 * atan2(s, c) in whole degrees, 0..359, or -1 without a field: the smaller over the larger as a Q16 ratio x, then
 * atan(x) = 45 x + 15.625 x (1 - x) degrees (0.2 degrees off at worst: a step is 20).
 */
static int
och_angle(int s, int c)
{
	uint64_t as = s < 0 ? -s : s, ac = c < 0 ? -c : c, big = as > ac ? as : ac, x;
	int a;

	if (big == 0) {
		return -1;
	}
	x = ((as > ac ? ac : as) << 16) / big;
	a = (int)((45 * x + (x * (65536 - x) >> 16) * 125 / 8 + 32768) >> 16);
	a = as > ac ? 90 - a : a;
	a = c < 0 ? 180 - a : a;
	a = s < 0 ? 360 - a : a;
	return a % 360;
}

static void
r1keys_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint8_t wia = 0, cntl2 = 0, wia0, cntl2_0, b[8] = { 0 };
	uint32_t mem, moved = 0, polls = 0;
	int base = 0, last = 0;
	bool wheel = false, have = false, fast = false, down = false, seen = false, ok;

	gpio = ml_io_map(GPIO_PHYS, GPIO_SIZE);
	vm_offset_t kpd = ml_io_map(KPD_PHYS, 0x1000);

	mem = *(volatile uint32_t *)(kpd + KP_MEM1);
	printf("r1keys: keypad KP_MEM1 %#x (side button = bit 1, clear while down)\n", mem);
	bus_take(161, 162);
	pin_gpio_mode(PIN_SCL);
	pin_gpio_mode(PIN_SDA);
	pin_gpio_mode(PIN_WHEEL_RESET);
	line(PIN_SCL, true);
	line(PIN_SDA, true);
	pin_out(PIN_WHEEL_RESET, false);
	delay(10000);
	pin_out(PIN_WHEEL_RESET, true);
	delay(5000);
	ok = reg_read(OCH_ADDR, 0x00, &wia, 1) && reg_read(OCH_ADDR, 0x21, &cntl2, 1);
	printf("r1keys: wheel reset done; clock %d data %d; %s: WIA %#x, CNTL2 %#x\n", pin_get(PIN_SCL), pin_get(PIN_SDA),
	    ok ? "answers" : "does NOT answer at 0x0d", wia, cntl2);
	if (ok && wia != 0x00 && wia != 0xff) {
		cntl2 = (cntl2 & 0xc0) | 0x20;  /* keep the top bits, high range */
		wheel = reg_write(OCH_ADDR, 0x21, cntl2) && reg_write(OCH_ADDR, 0x21, cntl2 | 0x0e);
		ok = reg_read(OCH_ADDR, 0x21, &wia, 1);
		printf("r1keys: wheel %s; CNTL2 written %#x, reads back %#x\n", wheel ? "in continuous mode" : "mode write NOT acknowledged",
		    cntl2 | 0x0e, ok ? wia : 0xffff);
	} else {
		printf("r1keys: wheel not started: WIA is not a chip's\n");
	}
	bus_give();
	wia0 = wia;
	cntl2_0 = cntl2;

	for (;; delay(wheel && fast ? 10000 : wheel ? 33000 : 20000), polls++) {
		bool now;

		/* the side button: the same twice in a row, or it is a bounce */
		mem = *(volatile uint32_t *)(kpd + KP_MEM1);
		now = !(mem & 2);
		if (now == seen && now != down) {
			down = now;
			redwood_touch_post(down ? REDWOOD_BUTTON_DOWN : REDWOOD_BUTTON_UP, REDWOOD_BUTTON_LOCK, 0, 0);
			printf("r1keys: side button %s (KP_MEM1 %#x)\n", down ? "down" : "up", mem);
		}
		seen = now;
		if (!wheel) {
			continue;
		}
		bus_take(161, 162);
		ok = reg_read(OCH_ADDR, 0x17, b, sizeof(b));
		bus_give();
		if (!ok) {
			continue;
		}
		int x = (int16_t)(b[2] << 8 | b[3]), z = (int16_t)(b[6] << 8 | b[7]), deg = och_angle(x, z), t = (int)now_ms();

		if (polls < 5 || polls % 600 == 0) {
			printf("r1keys: (WIA %#x CNTL2 %#x started %d; KP_MEM1 %#x) wheel %02x %02x %02x %02x %02x %02x %02x %02x: X %d Z %d = %d deg\n", wia0, cntl2_0, wheel, mem, b[0], b[1], b[2], b[3], b[4],
			    b[5], b[6], b[7], x, z, deg);
		}
		if (deg < 0) {
			continue;
		}
		if (!have) {
			have = true;
			base = last = deg;
			moved = t;
			continue;
		}
		int delta = deg - base, steps, d;

		delta = delta > 180 ? delta - 360 : delta <= -180 ? delta + 360 : delta;
		steps = delta / WHEEL_STEP_DEG;
		if (steps) {
			base = (base + steps * WHEEL_STEP_DEG + 360) % 360;
			steps = steps > 8 ? 8 : steps < -8 ? -8 : steps;        /* short of half a turn a poll */
			printf("r1keys: wheel %d deg, %d step(s) %s\n", deg, steps < 0 ? -steps : steps, steps > 0 ? "down" : "up");
			for (int i = 0; i < (steps < 0 ? -steps : steps); i++) {
				uint32_t which = steps > 0 ? REDWOOD_BUTTON_VOLUME_DOWN : REDWOOD_BUTTON_VOLUME_UP;

				redwood_touch_post(REDWOOD_BUTTON_DOWN, which, 0, 0);
				redwood_touch_post(REDWOOD_BUTTON_UP, which, 0, 0);
			}
		}
		d = deg > last ? deg - last : last - deg;
		if ((d > 180 ? 360 - d : d) >= 2) {
			moved = (uint32_t)t;
			last = deg;
		} else if (delta % WHEEL_STEP_DEG && (uint32_t)t - moved > 400) {
			base = deg;     /* still: a part step is dropped, the grid moves to here */
		}
		fast = (uint32_t)t - moved < 1000;
	}
}

static bool bl_ready;

/* the backlight as the registers say: 0 off, else 1..2048; -1 if the chip does not answer */
static int
bl_get(void)
{
	uint8_t en = 0, dim[2] = { 0 };
	bool ok;

	bus_take(48, 49);
	ok = reg_read(BL_ADDR, 0xa0, &en, 1) && reg_read(BL_ADDR, 0xa4, &dim[0], 1) && reg_read(BL_ADDR, 0xa5, &dim[1], 1);
	bus_give();
	return !ok ? -1 : (en & 0x40) ? (dim[1] << 3 | (dim[0] & 7)) + 1 : 0;
}

static bool
bl_set(uint32_t b)
{
	uint8_t en = 0, dim2 = 0;
	bool ok;

	bus_take(48, 49);
	ok = reg_read(BL_ADDR, 0xa0, &en, 1) && reg_read(BL_ADDR, 0xa4, &dim2, 1);
	if (ok && b) {
		/* DIM1 latches both fields; DIM2's reserved bits are kept */
		ok = reg_write(BL_ADDR, 0xa4, (dim2 & ~7u) | ((b - 1) & 7)) && reg_write(BL_ADDR, 0xa5, (uint8_t)((b - 1) >> 3)) &&
		    reg_write(BL_ADDR, 0xa0, en | 0x40);
	} else if (ok) {
		ok = reg_write(BL_ADDR, 0xa0, en & ~0x40u);
	}
	bus_give();
	return ok;
}

/* what the registers hold, for the log: EN, BSTCTRL, PWM, DIM2, DIM1 */
static void
bl_regs(uint8_t r[5])
{
	static const uint8_t reg[5] = { 0xa0, 0xa1, 0xa2, 0xa4, 0xa5 };

	bus_take(48, 49);
	for (int i = 0; i < 5; i++) {
		if (!reg_read(BL_ADDR, reg[i], &r[i], 1)) {
			r[i] = 0xee;
		}
	}
	bus_give();
}

extern void redwood_r1_gov_screen(bool off);      /* redwood_cpufreq.c: the governor goes low while the screen is off */

static int
bl_sysctl(struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value, error;

	if (!bl_ready || (value = bl_get()) < 0) {
		return ENXIO;
	}
	value = value > BL_MAX ? BL_MAX : value;
	error = sysctl_handle_int(oidp, &value, 0, req);
	if (error || !req->newptr) {
		return error;
	}
	if (value < 0 || value > BL_MAX) {
		return EINVAL;
	}
	static uint32_t writes;
	uint8_t was[5], now[5];
	bool ok;

	bl_regs(was);
	ok = bl_set((uint32_t)value);
	bl_regs(now);
	/* the first 24 writes, then every 16th */
	if (writes < 24 || writes % 16 == 0) {
		printf("r1bl: write #%u level %d %s: EN/BST/PWM/DIM2/DIM1 %02x %02x %02x %02x %02x -> %02x %02x %02x %02x %02x, reads back %d\n",
		    writes, value, ok ? "ok" : "FAILED", was[0], was[1], was[2], was[3], was[4], now[0], now[1], now[2], now[3], now[4], bl_get());
	}
	writes++;
	redwood_r1_gov_screen(value == 0);
	return ok ? 0 : EIO;
}

SYSCTL_DECL(_kern_redwood);
SYSCTL_PROC(_kern_redwood, OID_AUTO, backlight, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, 0, 0, bl_sysctl, "I",
    "the r1's backlight, 0 (off) to 512; only with boot-arg r1_bl=1");

static void
r1bl_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint8_t info = 0, en = 0, dim2 = 0, dim1 = 0;
	uint32_t mode = 0;
	int boot_level = 0;
	bool ok;

	gpio = ml_io_map(GPIO_PHYS, GPIO_SIZE);
	bus_take(48, 49);
	pin_gpio_mode(PIN_SCL);
	pin_gpio_mode(PIN_SDA);
	line(PIN_SCL, true);
	line(PIN_SDA, true);
	ok = reg_read(BL_ADDR, 0x00, &info, 1);
	printf("r1bl: clock %d data %d; MT6370 at 0x%x %s: DEV_INFO %#x\n", pin_get(PIN_SCL), pin_get(PIN_SDA), BL_ADDR,
	    ok ? "answers" : "does NOT answer", info);
	/* vendor id in bits 7:4; 0x9 and 0xb are the MT6372's 14-bit dimming, which these formulas are not for */
	if (ok && info != 0x00 && info != 0xff && (info >> 4) != 0x9 && (info >> 4) != 0xb) {
		ok = reg_read(BL_ADDR, 0xa0, &en, 1) && reg_read(BL_ADDR, 0xa4, &dim2, 1) && reg_read(BL_ADDR, 0xa5, &dim1, 1);
		printf("r1bl: EN(0xa0) %#x DIM2(0xa4) %#x DIM1(0xa5) %#x: %s, level %d of 2048 (left as it is)\n", en, dim2, dim1,
		    ok ? "read" : "READ FAILED", (en & 0x40) ? (dim1 << 3 | (dim2 & 7)) + 1 : 0);
		bl_ready = ok;
		/*
		 * The reference driver clears PWM_EN (bit 7) and PWM_HYS_EN (bit 2) of 0xA2 (the device tree has no mediatek,bled-pwm-enable):
		 * with PWM_EN left set by the boot loader the brightness follows the SoC's PWM pin and the I2C level is ignored or scaled.
		 */
		if (ok) {
			uint8_t pwm = 0, bst = 0, after = 0;

			if (reg_read(BL_ADDR, 0xa1, &bst, 1) && reg_read(BL_ADDR, 0xa2, &pwm, 1) && (pwm & 0x84)) {
				reg_write(BL_ADDR, 0xa2, pwm & ~0x84u);
			}
			reg_read(BL_ADDR, 0xa2, &after, 1);
			printf("r1bl: BSTCTRL(0xa1) %#x PWM(0xa2) %#x -> %#x (PWM_EN and PWM_HYS_EN cleared as mt6370-backlight.c does)\n", bst, pwm, after);
			boot_level = (en & 0x40) ? (dim1 << 3 | (dim2 & 7)) + 1 : 0;
		}
	} else {
		printf("r1bl: not the MT6370 the Linux port expects: left alone\n");
	}
	bus_give();
	if (bl_ready && PE_parse_boot_argn("r1_bl", &mode, sizeof(mode)) && mode == 3) {
		/* r1_bl=3: level 16 held for 30 s after the probe, then the boot level back (steadier to look at than the pulse) */
		printf("r1bl: self-test, 16 held for 30 s\n");
		printf("r1bl: hold 16 %s\n", bl_set(16) ? "ok" : "FAILED");
		delay_for_interval(30000, 1000 * NSEC_PER_USEC);
		printf("r1bl: hold over, back to %d %s\n", boot_level, bl_set(boot_level) ? "ok" : "FAILED");
	}
	if (bl_ready && PE_parse_boot_argn("r1_bl", &mode, sizeof(mode)) && mode == 2) {
		/* self-test, visible without a log: 20 s of 32 / 511 every 2 s, then the boot level back */
		char sum[160] = "";
		size_t n = 0;

		printf("r1bl: self-test, 32 and 511 alternating for 20 s\n");
		for (int i = 0; i < 10; i++) {
			delay_for_interval(2000, 1000 * NSEC_PER_USEC);
			bool ok = bl_set(i % 2 ? 511 : 32);
			int back = bl_get();

			printf("r1bl: self-test level %d %s, reads back %d\n", i % 2 ? 511 : 32, ok ? "ok" : "FAILED", back);
			n += snprintf(sum + n, sizeof(sum) - n, " %d%s>%d", i % 2 ? 511 : 32, ok ? "" : "!", back);
		}
		printf("r1bl: self-test over, back to %d %s\n", boot_level, bl_set(boot_level) ? "ok" : "FAILED");
		/* (the lines above fall into the log's dropped middle: say the summary again, 12 times 20 s apart) */
		for (int i = 0; i < 12; i++) {
			delay_for_interval(20000, 1000 * NSEC_PER_USEC);
			printf("r1bl: self-test summary (set>read-back):%s; after: %d\n", sum, bl_get());
		}
	}
	thread_terminate_self();
}

/*
 * r1_imu=1 ("r1imu:"): ICM-42607P accelerometer on bit-banged SPI mode 0 (GPIO13 MISO, 14 CS, 15 MOSI, 16 CLK), no I2C bus.
 * r1_imu_map= (default 123): which chip axis (1-3) feeds x, y, z; r1_imu_neg= (default 0): bit 0-2 negates x, y, z.
 */
int32_t redwood_r1_accel[3];
uint32_t redwood_r1_accel_count;

static uint8_t
imu_byte(uint8_t out)
{
	uint8_t in = 0;

	for (int bit = 7; bit >= 0; bit--) {
		pin_out(15, (out >> bit) & 1);
		delay(1);
		pin_out(16, true);
		in = (uint8_t)(in << 1 | pin_get(13));
		delay(1);
		pin_out(16, false);
	}
	return in;
}

static void
imu_xfer(uint8_t reg, bool read, uint8_t *data, int len)
{
	pin_out(14, false);
	imu_byte(read ? reg | 0x80 : reg);
	for (int i = 0; i < len; i++) {
		data[i] = imu_byte(read ? 0 : data[i]);
	}
	pin_out(14, true);
	delay(2);
}

static uint8_t
imu_rd(uint8_t reg)
{
	uint8_t v;

	imu_xfer(reg, true, &v, 1);
	return v;
}

static void
imu_wr(uint8_t reg, uint8_t v)
{
	imu_xfer(reg, false, &v, 1);
}

static void
r1imu_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t map = 123, neg = 0, polls = 0, m1, m2, id;

	PE_parse_boot_argn("r1_imu_map", &map, sizeof(map));
	PE_parse_boot_argn("r1_imu_neg", &neg, sizeof(neg));
	gpio = ml_io_map(GPIO_PHYS, GPIO_SIZE);
	m1 = rd(0x310);
	m2 = rd(0x320);
	for (uint32_t pin = 13; pin <= 16; pin++) {
		pin_gpio_mode(pin);
	}
	pin_out(14, true);
	pin_out(16, false);
	pin_out(15, false);
	wr(0x008, 1u << 13);                    /* MISO an input */
	printf("r1imu: GPIO13-16 were modes %u %u %u %u; now GPIO\n", (m1 >> 20) & 15, (m1 >> 24) & 15, (m1 >> 28) & 15, m2 & 15);
	imu_wr(0x02, 0x10);                     /* soft reset */
	delay(2000);
	id = imu_rd(0x75);
	if (id != 0x60) {
		printf("r1imu: WHOAMI %#x, not 0x60: left alone\n", id);
		wr(0x310, m1);
		wr(0x320, m2);
		thread_terminate_self();
	}
	printf("r1imu: WHOAMI %#x: ICM-42607P\n", id);
	imu_wr(0x21, 0x4A);                     /* +-4 g, 50 Hz */
	imu_wr(0x1F, 0x03);                     /* accel low-noise */
	delay(20000);
	printf("r1imu: PWR_MGMT0 %#x ACCEL_CONFIG0 %#x (want 0x3 0x4a)\n", imu_rd(0x1F), imu_rd(0x21));

	for (;; delay(40000), polls++) {
		uint8_t b[6];
		int32_t raw[3], mm[3];

		imu_xfer(0x0B, true, b, 6);
		for (int i = 0; i < 3; i++) {
			raw[i] = (int16_t)(b[2 * i] << 8 | b[2 * i + 1]);
		}
		for (int i = 0; i < 3; i++) {
			uint32_t axis = (i == 0 ? map / 100 : i == 1 ? map / 10 % 10 : map % 10);

			mm[i] = raw[axis >= 1 && axis <= 3 ? axis - 1 : i] * 9807 / 8192;
			redwood_r1_accel[i] = (neg >> i) & 1 ? -mm[i] : mm[i];
		}
		redwood_r1_accel_count++;
		if (polls % 250 == 249) {
			printf("r1imu: x %d y %d z %d mm/s^2 (raw %d %d %d), %u samples\n", redwood_r1_accel[0], redwood_r1_accel[1],
			    redwood_r1_accel[2], raw[0], raw[1], raw[2], redwood_r1_accel_count);
		}
	}
}

void
redwood_hyn_init(void)
{
	uint32_t which = 0;
	thread_t thread;

	if (PE_parse_boot_argn("buttons", &which, sizeof(which)) && which == 3 &&
	    kernel_thread_start(r1keys_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	if (PE_parse_boot_argn("r1_bl", &which, sizeof(which)) && (which >= 1 && which <= 3) &&
	    kernel_thread_start(r1bl_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	if (PE_parse_boot_argn("r1_imu", &which, sizeof(which)) && which == 1 &&
	    kernel_thread_start(r1imu_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	which = 0;

	/* 3 is the r1; 1 and 2 are the other phones' controllers */
	if (!PE_parse_boot_argn("touchscreen", &which, sizeof(which)) || which != 3) {
		return;
	}
	if (kernel_thread_start(hyn_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("hyn: cannot start the thread that reads the touch screen\n");
	}
}
