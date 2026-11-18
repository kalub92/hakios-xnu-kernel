/*
 * r1camdump: the camera receiver's registers out of Android, WHILE THE CAMERA APP SHOWS ITS PREVIEW (the blocks are clocked
 * and powered only then). /dev/mem does not exist on the r1; the vendor's ISP driver maps these very blocks for its HAL
 * (drivers/misc/mediatek/cameraisp/src/mt6765/camera_isp.c, ISP_mmap: an offset that is one of its block addresses, a
 * length within the block), and this asks for the same mappings, read-only, and prints every word that is not zero:
 *
 *   SENINF   0x1a040000 +0x8000   every block (the camera's port is CSI2 = SENINF5, +0x4000): its timing (+0x4a08 the settle), the lane
 *                                 order (+0x4824), the muxes
 *   MIPI RX  0x11c10000 +0x6000   csi0a .. csi2b, the analog blocks (the camera's: +0x4000, +0x5000)
 *   CAM_A    0x1a004000 +0x1000   the ISP's first pass (its sensor interface at +0x500 says the pixel mode Android uses)
 *   CAMSV_0, CAMSV_1 0x1a050000, 0x1a051000   idle under Android: their reset values
 *
 * Nothing is written. Built without a C library:  tools/android/build.sh   (or, with the repo's zig:
 *   xnu-deps/host/zig/zig cc -target aarch64-linux-musl -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static
 *       -fno-lto -O1 -Wl,-e,_start -o tools/android/r1camdump tools/android/r1camdump.c)
 * Run:  adb push tools/android/r1camdump /data/local/tmp/ ; open the camera app ;
 *       adb shell su -c /data/local/tmp/r1camdump > logs/camera/r1camdump.txt
 */
typedef unsigned long u64;
typedef unsigned int u32;

#define AT_FDCWD (-100)

static long
sys(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
	__asm__ volatile ("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
	return x0;
}

static char out[4096];
static long out_n;

static void
flush(void)
{
	sys(64, 1, (long)out, out_n, 0, 0, 0);          /* write */
	out_n = 0;
}

static void
say(const char *s)
{
	while (*s) {
		if (out_n == sizeof(out)) {
			flush();
		}
		out[out_n++] = *s++;
	}
}

static void
say_hex(u64 v, int digits)
{
	char b[20];

	b[digits] = 0;
	for (int i = digits - 1; i >= 0; i--, v >>= 4) {
		b[i] = "0123456789abcdef"[v & 15];
	}
	say("0x");
	say(b);
}

static const struct {
	const char *name;
	u64 base, size;
} blocks[] = {
	{ "SENINF", 0x1a040000, 0x8000 }, { "MIPIRX", 0x11c10000, 0x6000 }, { "CAM_A", 0x1a004000, 0x1000 },
	{ "CAMSV_0", 0x1a050000, 0x1000 }, { "CAMSV_1", 0x1a051000, 0x1000 },
};

__attribute__((used)) int
main(void)
{
	long fd = sys(56, AT_FDCWD, (long)"/dev/camera-isp", 2 /* O_RDWR */, 0, 0, 0);  /* openat */

	if (fd < 0) {
		say("r1camdump: cannot open /dev/camera-isp: errno ");
		say_hex((u64)-fd, 2);
		say(" (run it as root: su -c)\n");
		flush();
		return 1;
	}
	for (unsigned b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++) {
		/* mmap(NULL, size, PROT_READ, MAP_SHARED, fd, base) */
		long m = sys(222, 0, (long)blocks[b].size, 1, 1, fd, (long)blocks[b].base);
		unsigned shown = 0;

		say(blocks[b].name);
		say(" ");
		say_hex(blocks[b].base, 8);
		if (m < 0 && m > -4096) {
			say(": mmap errno ");
			say_hex((u64)-m, 2);
			say("\n");
			continue;
		}
		say(":\n");
		for (u64 off = 0; off < blocks[b].size; off += 4) {
			u32 v = *(volatile u32 *)(m + (long)off);

			if (v) {
				say(blocks[b].name);
				say(" +");
				say_hex(off, 4);
				say(" = ");
				say_hex(v, 8);
				say("\n");
				shown++;
			}
		}
		say(blocks[b].name);
		say(": ");
		say_hex(shown, 4);
		say(" words not zero\n");
		sys(215, m, (long)blocks[b].size, 0, 0, 0, 0);  /* munmap */
	}
	flush();
	sys(57, fd, 0, 0, 0, 0, 0);                             /* close */
	return 0;
}

/* the entry point: argc and argv on the stack, as the kernel leaves them */
__attribute__((naked, noreturn)) void
_start(void)
{
	__asm__ volatile (
		"bl main\n"
		"mov x8, #93\n"         /* exit */
		"svc 0\n");
}
