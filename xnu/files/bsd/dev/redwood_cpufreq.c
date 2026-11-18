/*
 * The clock speed of the processor cores the kernel runs on, on the POCO X5 Pro (redwood).
 * Only with boot-arg cpufreq=1: the addresses are this phone's.
 *
 * The boot loader leaves each group of cores at a modest frequency and expects the operating
 * system to ask for more. On this SoC that is one register write: the hardware ("EPSS") has
 * a table of frequencies per group and switches the clock source and the supply voltage by
 * itself when told which row is wanted. There is no reason for us ever to want less than the
 * top row, for every group that has a core of ours in it (one core, unless boot-arg cpus=N
 * says more).
 *
 * Register layout: Linux drivers/cpufreq/qcom-cpufreq-hw.c ("epss" variant).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern void redwood_tl(const char *format, ...) __printflike(1, 2);
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);

/*
 * The loader's log as well: this runs early, and the kernel's own log of a run keeps only its end. With
 * interrupts off -- on the way back to the loader (redwood_ddr_quiesce()) -- the loader's log only: the
 * kernel's console may not be written there, and printf was a panic ("Console I/O from interrupt-disabled
 * context") at the end of every run once the memory vote was taken away on the way back.
 */
static void
tell(const char *format, ...) __printflike(1, 2);

static void
tell(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
}

#define EPSS_PHYS               0x18591000UL    /* three groups ("domains") of 0x1000: small cores, big, biggest */
#define EPSS_DOMAIN_SIZE        0x1000UL
#define EPSS_DOMAINS            3
#define EPSS_FREQ_TABLE         0x100           /* a word per row: [31:30] clock source, [7:0] multiple of 19.2 MHz */
#define EPSS_TABLE_ROWS         40
#define EPSS_PERF_STATE         0x320           /* the row that is wanted */
#define XO_KHZ                  19200
#define ALTERNATE_KHZ           300000          /* source 0: a fixed 300 MHz */

/* How fast a simple loop runs, in thousands of iterations per millisecond. */
static uint32_t
loop_speed(void)
{
	volatile uint64_t sink = 0;
	uint64_t start = mach_absolute_time(), nanos;

	for (uint32_t i = 0; i < 2000000; i++) {
		sink += i;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	return nanos ? (uint32_t)(2000000ULL * 1000 / nanos) : 0;
}

/* The group of the core with that number in its MPIDR (the device tree's "reg": the number is the second field). */
static uint32_t
domain_of(uint32_t phys_id)
{
	uint32_t core = (phys_id >> 8) & 0xff;

	return core < 4 ? 0 : core < 7 ? 1 : 2;
}

static void
raise_domain(uint32_t domain, uint32_t cores)
{
	uint32_t top = 0, top_khz = 0, was;
	vm_offset_t base;

	base = ml_io_map(EPSS_PHYS + domain * EPSS_DOMAIN_SIZE, EPSS_DOMAIN_SIZE);
	for (uint32_t row = 0; row < EPSS_TABLE_ROWS; row++) {
		uint32_t entry = *(volatile uint32_t *)(base + EPSS_FREQ_TABLE + 4 * row);
		uint32_t khz = (entry >> 30) ? XO_KHZ * (entry & 0xff) : ALTERNATE_KHZ;

		if (row > 0 && khz <= top_khz) {
			break;                  /* the table ends by repeating its last row */
		}
		top = row;
		top_khz = khz;
	}
	was = *(volatile uint32_t *)(base + EPSS_PERF_STATE);
	*(volatile uint32_t *)(base + EPSS_PERF_STATE) = top;
	delay_for_interval(20, NSEC_PER_MSEC);
	tell("cpufreq: group %u (%u of our cores): row %u -> %u of the frequency table, %u MHz\n", domain, cores, was, top,
	    top_khz / 1000);
}

/*
 * Boot-arg membench=1: how fast this processor reads, writes and copies memory, in MB a second
 * (32 MB at a time, far more than any cache): what the memory's clock allows a processor, which
 * nobody here asks to be raised.
 */
static void
memory_bench(void)
{
	const vm_size_t size = 32u << 20;
	vm_offset_t a = 0, b = 0;
	uint64_t began, ns[3] = { 0, 0, 0 };
	volatile uint64_t sum = 0;

	if (kmem_alloc(kernel_map, &a, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &b, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		printf("membench: no memory\n");
		return;
	}
	memset((void *)a, 1, size);
	memset((void *)b, 2, size);
	for (int round = 0; round < 4; round++) {
		uint64_t t;

		began = mach_absolute_time();
		memset((void *)a, round, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[0] += t;
		began = mach_absolute_time();
		for (const uint64_t *p = (const uint64_t *)b, *end = (const uint64_t *)(b + size); p < end; p += 8) {
			sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[1] += t;
		began = mach_absolute_time();
		memcpy((void *)b, (const void *)a, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[2] += t;
	}
	tell("membench: on core %llu: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n",
	    (__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff, 4ull * (size >> 20) * 1000000000ull / ns[0],
	    4ull * (size >> 20) * 1000000000ull / ns[1], 4ull * (size >> 20) * 1000000000ull / ns[2]);
	kmem_free(kernel_map, a, size);
	kmem_free(kernel_map, b, size);
}

/*
 * Boot-arg membench=2: the same on every core of ours at once (a thread each, started together, 16 MB
 * at a time), 2 s after this starts, so that all the cores are up: the sum of what they get is what
 * the memory gives at the clock it has been left at.
 */
#define BENCH_THREADS   8
static struct {
	volatile uint32_t ready, go, done;
	uint64_t ns[BENCH_THREADS][3];
	uint32_t core[BENCH_THREADS];
} bench;

static void
bench_thread(void *arg, wait_result_t wr __unused)
{
	const vm_size_t size = 16u << 20;
	uintptr_t index = (uintptr_t)arg;
	vm_offset_t a = 0, b = 0;
	volatile uint64_t sum = 0;

	if (kmem_alloc(kernel_map, &a, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &b, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		os_atomic_inc(&bench.ready, relaxed);
		os_atomic_inc(&bench.done, relaxed);
		return;
	}
	memset((void *)a, 1, size);
	memset((void *)b, 2, size);
	os_atomic_inc(&bench.ready, relaxed);
	while (!os_atomic_load(&bench.go, acquire)) {
	}
	bench.core[index] = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	for (int round = 0; round < 4; round++) {
		uint64_t began, t;

		began = mach_absolute_time();
		memset((void *)a, round, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][0] += t;
		began = mach_absolute_time();
		for (const uint64_t *p = (const uint64_t *)b, *end = (const uint64_t *)(b + size); p < end; p += 8) {
			sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][1] += t;
		began = mach_absolute_time();
		memcpy((void *)b, (const void *)a, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][2] += t;
	}
	os_atomic_inc(&bench.done, release);
	kmem_free(kernel_map, a, size);
	kmem_free(kernel_map, b, size);
}

extern void redwood_host_keep_add(const char *format, ...) __printflike(1, 2);

static void
bench_all(void *arg __unused, wait_result_t wr __unused)
{
	const uint32_t threads = MIN(ml_get_topology_info()->num_cpus, BENCH_THREADS);
	const uint64_t mb = 4 * 16;                     /* per thread and test */
	uint64_t total[3] = { 0, 0, 0 };

	delay_for_interval(2000, NSEC_PER_MSEC);
	for (uintptr_t i = 0; i < threads; i++) {
		thread_t thread;

		if (kernel_thread_start(bench_thread, (void *)i, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			os_atomic_inc(&bench.ready, relaxed);
			os_atomic_inc(&bench.done, relaxed);
		}
	}
	while (os_atomic_load(&bench.ready, relaxed) < threads) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	os_atomic_store(&bench.go, 1, release);
	while (os_atomic_load(&bench.done, acquire) < threads) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	for (uint32_t i = 0; i < threads; i++) {
		tell("membench: all at once: core %u: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n", bench.core[i],
		    bench.ns[i][0] ? mb * 1000000000ull / bench.ns[i][0] : 0, bench.ns[i][1] ? mb * 1000000000ull / bench.ns[i][1] : 0,
		    bench.ns[i][2] ? mb * 1000000000ull / bench.ns[i][2] : 0);
		for (int k = 0; k < 3; k++) {
			total[k] += bench.ns[i][k] ? mb * 1000000000ull / bench.ns[i][k] : 0;
		}
	}
	tell("membench: all %u at once, together: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n", threads, total[0],
	    total[1], total[2]);
	/* the boot log's head is lost to the host: say it again to the kept log, every 10 s for two minutes */
	for (int n = 0; n < 12; n++) {
		redwood_host_keep_add("membench: all %u at once, together: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n",
		    threads, total[0], total[1], total[2]);
		delay_for_interval(10000, NSEC_PER_MSEC);
	}
}

/*
 * Boot-arg cpuload=MS: every MS milliseconds, how busy each of our cores was meanwhile, in
 * percent, to the loader's log (tools/frames.py shows it per gesture):
 *   tl: u at US 7:NN 6:NN ... 1:NN      the core's number (as in its MPIDR), then its share
 * of the time it was not idle (interrupts included).
 */
static uint32_t load_every_ms;

static void
load_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t idle_before[MAX_CPUS] = { 0 }, then = mach_absolute_time();

	for (;;) {
		char line[192];
		int at;
		uint64_t now, us;

		delay_for_interval(load_every_ms, NSEC_PER_MSEC);
		now = mach_absolute_time();
		absolutetime_to_nanoseconds(now, &us);
		at = snprintf(line, sizeof(line), "tl: u at %llu", us / 1000);
		for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
			uint64_t idle = 0, busy;

			if (!ml_cpu_idle_time((int)i, &idle)) {
				continue;
			}
			busy = (now - then) > (idle - idle_before[i]) ? (now - then) - (idle - idle_before[i]) : 0;
			idle_before[i] = idle;
			if (at < (int)sizeof(line) - 12) {
				at += snprintf(line + at, sizeof(line) - at, " %u:%llu", (topology->cpus[i].phys_id >> 8) & 0xff,
				    100 * busy / (now - then));
			}
		}
		then = now;
		redwood_tl("%s\n", line);
	}
}

static void
load_start(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("cpuload", &load_every_ms, sizeof(load_every_ms)) || load_every_ms == 0) {
		return;
	}
	if (kernel_thread_start(load_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * Boot-arg ddr=1: the memory's bus clock managers ("BCM") in the command database the boot loader
 * left, with what is needed to vote for bandwidth on them -- only read. (membench=2 says the cores
 * together get no more from memory than one alone: 12 GB/s; nobody here votes for more.)
 */
#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define CMD_DB_BCM              5               /* the kind of resource in a slave's id: bus clock managers */

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};
struct cmd_db_bcm {                             /* a BCM's auxiliary data (Linux: struct bcm_db) */
	uint32_t unit;
	uint16_t width;
	uint8_t vcd, reserved;
};

/* (the same condition as the cores' clocks: the addresses are this phone's) */
static bool
cpufreq_wanted(void)
{
	uint32_t on = 0;

	return PE_parse_boot_argn("cpufreq", &on, sizeof(on)) && on != 0;
}

static void
ddr_look(void)
{
	/* (memory, not registers: mapped so that reads of any size and alignment are allowed) */
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		tell("ddr: no command database\n");
		return;
	}
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id != CMD_DB_BCM || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			struct cmd_db_bcm aux = { 0 };
			char name[9] = { 0 };

			memcpy(name, entry[i].id, 8);
			if (entry[i].length >= sizeof(aux) && slave->data_offset + entry[i].offset + sizeof(aux) < CMD_DB_SIZE - 0x40) {
				memcpy(&aux, db->data + slave->data_offset + entry[i].offset, sizeof(aux));
			}
			tell("ddr: BCM %-8s at %#x: unit %u, width %u, vcd %u (aux %u bytes)\n", name, entry[i].address, aux.unit,
			    aux.width, aux.vcd, entry[i].length);
		}
	}
}

/*
 * ... and what the boot loader's firmware left in the application processor's RPMh command slots
 * ("DRV 2"; Linux: rpmh-rsc.c): 2 active, 3 sleep, 3 wake slot sets and a control one, of 16 commands
 * each (sc7280's qcom,tcs-config): every command with an address, and which of them go to the BCMs
 * above -- the bandwidth it asked for, and that is in force whenever the processors are awake.
 */
#define RSC_PHYS                0x18220000UL
#define RSC_SIZE                0x10000UL
#define RSC_TCS(n, reg)         (0xd00 + 0x2a0 * (n) + (reg))
#define RSC_TCS_COUNT           9
#define RSC_CMDS                16

static void
ddr_slots(void)
{
	vm_offset_t rsc = ml_io_map(RSC_PHYS, RSC_SIZE);

	for (unsigned t = 0; t < RSC_TCS_COUNT; t++) {
		uint32_t enabled = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x1c));

		for (unsigned c = 0; c < RSC_CMDS; c++) {
			uint32_t msgid = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x30 + 20 * c));
			uint32_t address = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x34 + 20 * c));
			uint32_t data = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x38 + 20 * c));

			if (address == 0 && data == 0) {
				continue;
			}
			tell("ddr: slot set %u (%s) command %u%s: message %#x, address %#x, data %#x%s\n", t,
			    t < 2 ? "active" : t < 5 ? "sleep" : t < 8 ? "wake" : "control", c, enabled & (1u << c) ? " (on)" : "",
			    msgid, address, data, (address >> 16 & 0xf) == CMD_DB_BCM ? " <- a BCM" : "");
		}
	}
}

/*
 * The memory's bandwidth (on by default; boot-arg ddr=0: not): a vote through RPMh -- the most there
 * is -- on the bus clock managers of the memory controller (MC0: the memory's clock) and of the
 * shared hub in front of it with the system cache (SH0), as Linux's interconnect driver casts for
 * a busy GPU (icc-bcm-voter.c: a BCM command is commit, valid, the average and the peak, 14 bits each,
 * in the BCM's units; the BCM takes the highest of its levels the vote reaches), and the memory kept
 * on for this processor (ACV, bit 3, which the boot loader's firmware had set already, the same).
 * All cores together got 12.4 GB/s writing without it (membench=2: what one core gets alone), 14.8
 * with it; frames over 16.7 ms 28% instead of 34-36%. ddr_vote=N: N instead of the most (16383).
 * When the kernel goes back to the loader (redwood_ddr_quiesce(), on every way back, a panic's too)
 * the vote goes down to ddr_rest=N (default 1024). What the firmware had voted cannot be read back,
 * but what the cores get from memory can be measured (membench=2, writing): with any vote from 4
 * to 4096 12.0 to 12.4 GB/s, as before the first vote (12.4); with the most, 15.4; with none at all
 * ("no request", valid clear: what Linux sends for nothing) 0.2 -- the memory at its floor, and the
 * loader's USB transfers at 8 MB/s instead of 30. So the firmware had a vote of its own in our name,
 * of that middle range, and one from the middle of it is left.
 */
#define BCM_COMMIT              (1u << 30)
#define BCM_VALID               (1u << 29)
#define BCM_VOTE(x, y)          (BCM_COMMIT | BCM_VALID | ((uint32_t)(x) & 0x3fff) << 14 | ((uint32_t)(y) & 0x3fff))
#define TCS_REG(reg)            (ddr_rsc + 0xd00 + (reg))       /* command slot set 0, an "active" one */
#define BCM_ACV_APPS            8
#define DDR_REST                1024            /* the vote left for the loader (ddr_rest=N); see redwood_ddr_quiesce() */

static vm_offset_t ddr_rsc;
static uint32_t ddr_mc0, ddr_sh0, ddr_acv;
static bool ddr_voted;

static uint32_t
cmd_db_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id != CMD_DB_BCM || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

/* Up to three writes through command slot set 0, and the wait until RPMh has done them (as bsd/dev/redwood_goodix.c) */
static bool
ddr_rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = *(volatile uint32_t *)TCS_REG(0x00);
	bool done = false;

	for (int i = 0; *(volatile uint32_t *)TCS_REG(0x18) == 0; i++) {
		if (i > 2000) {
			tell("ddr: RPMh: the command slots stay busy\n");
			return false;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 24);
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 16);
	*(volatile uint32_t *)TCS_REG(0x08) = 1;
	for (unsigned i = 0; i < count; i++) {
		*(volatile uint32_t *)TCS_REG(0x30 + 20 * i) = 0x10008 | 0x100;        /* a 4-byte write, with a response */
		*(volatile uint32_t *)TCS_REG(0x34 + 20 * i) = address[i];
		*(volatile uint32_t *)TCS_REG(0x38 + 20 * i) = data[i];
	}
	*(volatile uint32_t *)TCS_REG(0x10) = (1u << count) - 1;
	*(volatile uint32_t *)TCS_REG(0x1c) = (1u << count) - 1;
	*(volatile uint32_t *)TCS_REG(0x00) = irq_enable | 1;
	*(volatile uint32_t *)TCS_REG(0x14) = 1u << 16;
	*(volatile uint32_t *)TCS_REG(0x14) = 1u << 16 | 1u << 24;
	for (int i = 0; i < 2000 && !done; i++) {
		done = (*(volatile uint32_t *)TCS_REG(0x04) & 1) != 0;
		if (!done) {
			delay_for_interval(100, NSEC_PER_USEC);
		}
	}
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 24);
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 16);
	*(volatile uint32_t *)TCS_REG(0x1c) = 0;
	*(volatile uint32_t *)TCS_REG(0x10) = 0;
	*(volatile uint32_t *)TCS_REG(0x08) = 1;
	*(volatile uint32_t *)TCS_REG(0x00) = irq_enable;
	if (!done) {
		tell("ddr: RPMh: no answer to the vote\n");
	}
	return done;
}

static bool
ddr_vote(uint32_t vote)
{
	uint32_t address[3] = { ddr_mc0, ddr_sh0, ddr_acv };
	uint32_t data[3] = { BCM_VOTE(vote, vote), BCM_VOTE(vote, vote), BCM_VOTE(0, BCM_ACV_APPS) };

	return ddr_rpmh_write(address, data, 3);
}

static void
ddr_raise(void)
{
	uint32_t vote = 0x3fff;

	PE_parse_boot_argn("ddr_vote", &vote, sizeof(vote));
	ddr_mc0 = cmd_db_address("MC0");
	ddr_sh0 = cmd_db_address("SH0");
	ddr_acv = cmd_db_address("ACV");
	if (ddr_mc0 == 0 || ddr_sh0 == 0 || ddr_acv == 0 || vote == 0 || vote > 0x3fff) {
		tell("ddr: no vote (MC0 %#x, SH0 %#x, ACV %#x, vote %u)\n", ddr_mc0, ddr_sh0, ddr_acv, vote);
		return;
	}
	ddr_rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	ddr_voted = ddr_vote(vote);
	tell("ddr: memory bandwidth vote %u on MC0 (%#x) and SH0 (%#x)%s\n", vote, ddr_mc0, ddr_sh0, ddr_voted ? "" : ": FAILED");
}

/* On the way back to the loader (osfmk/arm64/loader_return.c: loader_return(), whatever the reason; the other processors are stopped) */
void redwood_ddr_quiesce(void);

void
redwood_ddr_quiesce(void)
{
	uint32_t address[2] = { ddr_mc0, ddr_sh0 };
	uint32_t rest = DDR_REST, data[2];
	bool done;

	if (!ddr_voted) {
		return;
	}
	ddr_voted = false;
	PE_parse_boot_argn("ddr_rest", &rest, sizeof(rest));
	rest = MIN(rest, 0x3fff);
	data[0] = data[1] = rest != 0 ? BCM_VOTE(rest, rest) : BCM_COMMIT;     /* 0: no request */
	done = ddr_rpmh_write(address, data, 2);
	tell("ddr: memory bandwidth vote %u for the loader: %s\n", rest, done ? "done" : "FAILED");
}

/*
 * The Rabbit r1 (MT6765), not the POCO: boot-args r1_clk=1 (what the clocks, rails, DRAM level and board
 * temperature are, said; reads only), r1_cpufreq=MHz (cpu0-3) and r1_cpufreq1=MHz (cpu4-7) (one of the stock
 * kernel's own frequencies, with its voltage), r1_thermal=C (default 58: the board's NTC at which what
 * r1_cpufreq raised is put back) and r1_dram=3200. Never cpufreq= on this phone: that is the POCO's, above.
 * Sources: haretic's mediatek-cpufreq.c, clk-mt6765.c, clk-pll.c, mt6357-regulator.c, mtk-dvfsrc.c,
 * mt6577_auxadc.c and ntc.c, and the device tree's tables (mt6765-rabbit-r1.dts, cluster0_opp, cluster1_opp).
 */
extern void redwood_keep(const char *format, ...) __printflike(1, 2);
extern void redwood_host_keep_add(const char *format, ...) __printflike(1, 2);
#define SAY redwood_host_keep_add

#define R1_TOP_PHYS     0x10000000UL    /* topckgen */
#define R1_INFRA_PHYS   0x10001000UL    /* infracfg_ao */
#define R1_APMIXED_PHYS 0x1000c000UL
#define R1_PWRAP_PHYS   0x1000d000UL
#define R1_DVFSRC_PHYS  0x10012000UL
#define R1_MCUCFG_PHYS  0x10200000UL
#define R1_AUXADC_PHYS  0x11001000UL
#define R1_CLK_CFG_1    0x050           /* topckgen: mfg_sel in [1:0] */
#define R1_CLK_CFG_5    0x090           /* topckgen: spm_sel gated by bit 31 */
#define R1_TOP_MISC     0x104           /* topckgen: bit 4 turns arm_div_pll1 (MAINPLL for the cores) on */
#define R1_MAINPLL      0x240           /* apmixed CON1 of the PLLs; ARMPLL_L (cpu0-3) 0x220, ARMPLL (cpu4-7) 0x210, CCIPLL 0x230, MFGPLL 0x250 */
#define R1_DV_SW_REQ    0x04
#define R1_DV_SW_REQ2   0x08
#define R1_DV_LEVEL     0xdc
#define R1_OPPS         16

static const uint32_t r1_pll[2] = { 0x220, 0x210 };
static const uint32_t r1_mux[2] = { 0x7a0, 0x7a4 };     /* mcucfg: [10:9] 0 26 MHz, 1 the cluster's PLL, 2 MAINPLL, 3 UNIVPLL/2 */
static vm_offset_t r1_top, r1_infra, r1_apm, r1_pwrap, r1_dv, r1_mcu, r1_adc;
static bool r1_swapped;                                 /* TOP2_ELR0 bit 15: the two VSRAM banks are swapped */
static uint32_t r1_cur_vproc, r1_cur_vsram;             /* the rail as last read back */
static uint32_t r1_lk_con1[2], r1_lk_vproc, r1_lk_vsram, r1_lk_top;
static bool r1_raised[2], r1_latched;
static const struct r1_opp *r1_row[2];                  /* the r1_cpufreq rows that were raised: the governor's high point */
static bool r1_gov_on, r1_gov_off;                      /* governor running (r1_gov=1); screen off forces low */
static bool r1_quiet;                                   /* the governor's switches: no progress lines, no loop_speed() (under r1_mtx) */
#define QSAY(...) do { if (!r1_quiet) { SAY(__VA_ARGS__); } } while (0)
static LCK_GRP_DECLARE(r1_grp, "r1-clocks");
static LCK_MTX_DECLARE(r1_mtx, &r1_grp);       /* the thermal thread's drop and the one before the return to the loader do not overlap */
static int r1_trip = 58;
static uint32_t r1_test_s;                      /* r1_thermal_test=SECS: the drop is taken once, that long after the raise, as if the trip were reached */
static uint32_t r1_sw_req_old;
static bool r1_sw_req_saved;
static bool r1_dram_seen;
static void r1_dram_now(void);

/* (frequency, Vproc, Vsram, PLL CON1 = postdiv << 24 | pcw): the non-turbo rows of cluster0_opp and cluster1_opp, Vsram = Vproc + 100 mV within 850..1200 mV */
struct r1_opp {
	uint16_t mhz;
	uint32_t vproc, vsram, con1;
};
static const struct r1_opp r1_opp[2][R1_OPPS] = {
	{
		{ 900, 812500, 912500, 0x01114ec4 }, { 979, 812500, 912500, 0x0112d3b1 }, { 1085, 812500, 912500, 0x0114dd89 },
		{ 1218, 812500, 912500, 0x01176c4e }, { 1351, 812500, 912500, 0x0119fb13 }, { 1484, 812500, 912500, 0x011c89d8 },
		{ 1617, 812500, 912500, 0x000f8c4e }, { 1750, 850000, 950000, 0x0010d3b1 }, { 1803, 893750, 993750, 0x00115627 },
		{ 1856, 937500, 1037500, 0x0011d89d }, { 1909, 975000, 1075000, 0x00125b13 }, { 1962, 1018750, 1118750, 0x0012dd89 },
		{ 2015, 1056250, 1156250, 0x00136000 }, { 2068, 1100000, 1200000, 0x0013e276 }, { 2130, 1150000, 1200000, 0x00147b13 },
		{ 2200, 1200000, 1200000, 0x00152762 },
	}, {
		{ 400, 812500, 912500, 0x020f6276 }, { 501, 812500, 912500, 0x021344ec }, { 542, 812500, 912500, 0x0214d89d },
		{ 643, 812500, 912500, 0x0218bb13 }, { 745, 812500, 912500, 0x021ca762 }, { 846, 812500, 912500, 0x011044ec },
		{ 948, 812500, 912500, 0x01123b13 }, { 1050, 850000, 950000, 0x0114313b }, { 1114, 893750, 993750, 0x01156c4e },
		{ 1179, 937500, 1037500, 0x0116ac4e }, { 1244, 975000, 1075000, 0x0117ec4e }, { 1309, 1018750, 1118750, 0x01192c4e },
		{ 1374, 1056250, 1156250, 0x011a6c4e }, { 1439, 1100000, 1200000, 0x011bac4e }, { 1515, 1150000, 1200000, 0x000e913b },
		{ 1600, 1200000, 1200000, 0x000f6276 },
	}
};

static inline uint32_t
r1rd(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
r1wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(base + at) = value;
	__asm__ volatile ("dsb sy");
}

/* 26 MHz * pcw / 2^14 >> postdiv: clk-pll.c's __mtk_pll_recalc_rate with 22 bits of which 8 whole */
static uint32_t
r1_pll_khz(uint32_t con1)
{
	return (uint32_t)(((26000ULL * (con1 & 0x3fffff)) >> 14) >> ((con1 >> 24) & 7));
}

/* ---- the PMIC through the wrapper (redwood_mtkkeys.c's, the MT6765 has the same WACS2 offsets: pwrap mt6765_regs) ---- */

#define PWRAP_WACS2_CMD     0xc20
#define PWRAP_WACS2_RDATA   0xc24
#define PWRAP_WACS2_VLDCLR  0xc28
#define PWRAP_FSM(r)        (((r) >> 16) & 7u)
#define PWRAP_FSM_IDLE      0
#define PWRAP_FSM_WFVLDCLR  6

static bool
pwrap_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 100000; spin++) {
		if (PWRAP_FSM(r1rd(r1_pwrap, PWRAP_WACS2_RDATA)) == state) {
			return true;
		}
	}
	return false;
}

static bool
pmic_read(uint32_t reg, uint32_t *value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (pwrap_wait(PWRAP_FSM_IDLE)) {
		r1wr(r1_pwrap, PWRAP_WACS2_CMD, (reg >> 1) << 16);
		if (pwrap_wait(PWRAP_FSM_WFVLDCLR)) {
			*value = r1rd(r1_pwrap, PWRAP_WACS2_RDATA) & 0xffff;
			r1wr(r1_pwrap, PWRAP_WACS2_VLDCLR, 1);
			ok = true;
		}
	}
	redwood_pwrap_unlock();
	return ok;
}

static bool
pmic_write(uint32_t reg, uint32_t value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (pwrap_wait(PWRAP_FSM_IDLE)) {
		r1wr(r1_pwrap, PWRAP_WACS2_CMD, (1u << 31) | (reg >> 1) << 16 | (value & 0xffff));
		ok = pwrap_wait(PWRAP_FSM_IDLE);
	}
	redwood_pwrap_unlock();
	return ok;
}

/* MT6357: volts = 518750 + 6250 * step (mt6357-regulator.c, buck_volt_range1) */
#define R1_UV(step)     (518750u + 6250u * (step))
#define R1_STEP(uv)     (((uv) - 518750u) / 6250u)
#define VPROC_ELR0      0x14aa  /* the selector, [6:0] */
#define VPROC_DBG0      0x14a2  /* what it is at, [6:0] */
#define VCORE_DBG0      0x1522
#define TOP2_ELR0       0x15a
#define VSRAM_NORMAL    0x19fa  /* LDO_VSRAM_CON0 [6:0], readback LDO_VSRAM_PROC_DBG0 0x19cc [14:8] */
#define VSRAM_SWAPPED   0x19fc  /* CON1, readback LDO_VSRAM_OTHERS_DBG0 0x19ea */

static bool
r1_vproc_get(uint32_t *uv)
{
	uint32_t v;

	if (!pmic_read(VPROC_DBG0, &v)) {
		return false;
	}
	*uv = R1_UV(v & 0x7f);
	return true;
}

static bool
r1_vcore_get(uint32_t *uv)
{
	uint32_t v;

	if (!pmic_read(VCORE_DBG0, &v)) {
		return false;
	}
	*uv = R1_UV(v & 0x7f);
	return true;
}

static bool
r1_vsram_get(uint32_t *uv)
{
	uint32_t v;

	if (!pmic_read(r1_swapped ? 0x19ea : 0x19cc, &v)) {
		return false;
	}
	*uv = R1_UV((v >> 8) & 0x7f);
	return true;
}

/*
 * One rail to one voltage: its selector's [6:0] read-modify-written, then its readback polled (10 us steps,
 * 1 ms) until it says the same, then 100 us more (Linux has no settle time; this is ours, a guess). The
 * rail as it then reads is kept in r1_cur_*.
 */
static bool
r1_rail_set(bool sram, uint32_t uv)
{
	uint32_t sel = sram ? (r1_swapped ? VSRAM_SWAPPED : VSRAM_NORMAL) : VPROC_ELR0, v, got = 0;
	uint32_t *cur = sram ? &r1_cur_vsram : &r1_cur_vproc;
	bool seen = false;

	if (pmic_read(sel, &v) && pmic_write(sel, (v & ~0x7fu) | R1_STEP(uv))) {
		for (int i = 0; i < 100; i++) {
			seen = sram ? r1_vsram_get(&got) : r1_vproc_get(&got);
			if (seen && got == uv) {
				break;
			}
			delay(10);
		}
	}
	if (seen) {
		*cur = got;
	}
	if (!seen || got != uv) {
		SAY("r1cpu: ABORT %s reads step %u want %u", sram ? "Vsram" : "Vproc", seen ? R1_STEP(got) : 0, R1_STEP(uv));
		return false;
	}
	delay(100);
	QSAY("r1cpu: %s %u uV (step %u)", sram ? "Vsram" : "Vproc", got, R1_STEP(got));
	return true;
}

/*
 * mediatek-cpufreq.c's mtk_cpufreq_voltage_tracking, Vsram and Vproc kept within 100..200 mV of each other:
 * up, Vsram first; down, Vproc first. On a rail that does not read back what it was told: the pair this
 * step began from is written back, in the reverse order.
 */
static bool
r1_track(uint32_t new_vproc)
{
	uint32_t new_vsram = MIN(MAX(new_vproc + 100000, 850000), 1200000);
	int retry = 3 * ((1200000 + 99999) / 100000);

	do {
		uint32_t pv = r1_cur_vproc, ps = r1_cur_vsram, vproc, vsram;

		if (pv <= new_vproc) {
			vsram = MIN(MAX(pv + 200000, 850000), new_vsram);
			if (!r1_rail_set(true, vsram)) {
				(void)r1_rail_set(true, ps);
				return false;
			}
			vproc = (vsram == 1200000 || new_vsram == 850000) ? new_vproc : vsram - 100000;
			if (!r1_rail_set(false, vproc)) {
				(void)r1_rail_set(false, pv);
				(void)r1_rail_set(true, ps);
				return false;
			}
		} else {
			vproc = MAX(new_vproc, ps - 200000);
			if (!r1_rail_set(false, vproc)) {
				(void)r1_rail_set(false, pv);
				return false;
			}
			vsram = vproc == new_vproc ? new_vsram : MAX(new_vsram, vproc + 100000);
			if (!r1_rail_set(true, vsram)) {
				(void)r1_rail_set(true, ps);
				(void)r1_rail_set(false, pv);
				return false;
			}
		}
		if (--retry < 0) {
			SAY("r1cpu: ABORT over the loop count, Vproc %u Vsram %u", r1_cur_vproc, r1_cur_vsram);
			return false;
		}
	} while (r1_cur_vproc != new_vproc || r1_cur_vsram != new_vsram);
	return true;
}

/* The table, checked before anything uses it: each CON1 within 1 MHz of its row, each voltage a whole step, Vsram the rule's. */
static bool
r1_table_ok(void)
{
	for (uint32_t cl = 0; cl < 2; cl++) {
		for (uint32_t i = 0; i < R1_OPPS; i++) {
			const struct r1_opp *o = &r1_opp[cl][i];
			uint32_t khz = r1_pll_khz(o->con1), want = MIN(MAX(o->vproc + 100000, 850000), 1200000);
			const char *bad = NULL;

			if (khz + 1000 < o->mhz * 1000u || khz > o->mhz * 1000u + 1000) {
				bad = "CON1 does not decode to its MHz";
			} else if (o->vproc < 518750u || o->vsram < 518750u || R1_UV(R1_STEP(o->vproc)) != o->vproc ||
			    R1_UV(R1_STEP(o->vsram)) != o->vsram) {
				bad = "voltage is not a whole step";
			} else if (o->vsram != want || o->vproc > 1200000 || (i > 0 && o->mhz <= o[-1].mhz)) {
				bad = "Vsram, Vproc or order";
			}
			if (bad != NULL) {
				SAY("r1cpu: table row MP%u %u MHz: %s", cl, o->mhz, bad);
				return false;
			}
		}
	}
	SAY("r1cpu: table ok");
	return true;
}

/* The first row at or above that frequency (the OPP "ceiling"), the top one if there is none. */
static const struct r1_opp *
r1_ceil(uint32_t cl, uint32_t mhz)
{
	for (uint32_t i = 0; i < R1_OPPS; i++) {
		if (r1_opp[cl][i].mhz >= mhz) {
			return &r1_opp[cl][i];
		}
	}
	return &r1_opp[cl][R1_OPPS - 1];
}

/* What the rail has to be for the cluster's `own`: the highest of that, the other cluster's live frequency's row, and the CCI's 812500. */
static uint32_t
r1_need(uint32_t cl, uint32_t own)
{
	uint32_t o = 1 - cl, sel = (r1rd(r1_mcu, r1_mux[o]) >> 9) & 3;
	uint32_t mhz = sel == 2 ? r1_pll_khz(r1rd(r1_apm, R1_MAINPLL)) / 1000 : sel == 3 ? 0xffff : r1_pll_khz(r1rd(r1_apm, r1_pll[o])) / 1000;

	return MAX(MAX(own, r1_ceil(o, mhz)->vproc), 812500);
}

static bool
r1_mux_set(uint32_t cl, uint32_t sel)
{
	r1wr(r1_mcu, r1_mux[cl], (r1rd(r1_mcu, r1_mux[cl]) & ~(3u << 9)) | sel << 9);
	return ((r1rd(r1_mcu, r1_mux[cl]) >> 9) & 3) == sel;
}

/* clk-pll.c's mtk_pll_set_rate_regs: postdiv and pcw in one write, then the CHG bit, then 20 us; true if it reads back */
static bool
r1_pll_set(uint32_t cl, uint32_t value)
{
	uint32_t v = r1rd(r1_apm, r1_pll[cl]);

	r1wr(r1_apm, r1_pll[cl], (v & ~(7u << 24) & ~0x3fffffu) | value);
	r1wr(r1_apm, r1_pll[cl], r1rd(r1_apm, r1_pll[cl]) | 1u << 31);
	delay(20);
	return (r1rd(r1_apm, r1_pll[cl]) & 0x073fffffu) == value;
}

/* After a failure: the rail back to LK's, but not below what the other cluster, if it was raised, needs. */
static void
r1_rail_back(uint32_t cl)
{
	(void)r1_track(r1_raised[1 - cl] ? r1_need(cl, r1_lk_vproc) : r1_lk_vproc);
}

/*
 * One cluster to a row, in the order of mtk_cpufreq_set_target: the rail up to what the row, the intermediate
 * clock (MAINPLL, whose row's voltage is the "intermediate voltage") and the other cluster need; the intermediate
 * clock on; the cluster's mux to MAINPLL; its PLL retuned; the mux back to its PLL; the rail down if it can be.
 */
static bool
r1_switch(uint32_t cl, uint32_t to_mhz, uint32_t to_uv, uint32_t to_con1)
{
	const char *name = cl ? "MP1" : "MP0", *pll = cl ? "ARMPLL" : "ARMPLL_L";
	uint32_t was = r1_pll_khz(r1rd(r1_apm, r1_pll[cl])) / 1000, main_mhz = r1_pll_khz(r1rd(r1_apm, R1_MAINPLL)) / 1000;
	uint32_t inter = r1_ceil(cl, main_mhz)->vproc, up = r1_need(cl, MAX(to_uv, inter)), down = r1_need(cl, to_uv);
	uint32_t before = r1_quiet ? 0 : loop_speed(), pv = r1_cur_vproc, ps = r1_cur_vsram;
	uint32_t up_vsram = MIN(MAX(up + 100000, 850000), 1200000);

	QSAY("r1cpu: %s %u -> %u: rail %u/%u -> %u/%u (steps %u/%u -> %u/%u)", name, was, to_mhz, pv, ps, up, up_vsram,
	    R1_STEP(pv), R1_STEP(ps), R1_STEP(up), R1_STEP(up_vsram));
	if (pv <= up && !r1_track(up)) {
		r1_rail_back(cl);
		return false;
	}
	r1wr(r1_top, R1_TOP_MISC, r1rd(r1_top, R1_TOP_MISC) | 1u << 4);
	if (!(r1rd(r1_top, R1_TOP_MISC) & (1u << 4))) {
		SAY("r1cpu: %s: the intermediate clock did not come on (TOP+0x104 %#x)", name, r1rd(r1_top, R1_TOP_MISC));
		r1_rail_back(cl);
		return false;
	}
	if (!r1_mux_set(cl, 2)) {
		SAY("r1cpu: %s: the mux did not go to MAINPLL", name);
		goto back_on_pll;
	}
	QSAY("r1cpu: %s on MAINPLL %u", name, main_mhz);
	if (!r1_pll_set(cl, to_con1)) {
		SAY("r1cpu: %s: %s CON1 reads %#x, not %#x: LK's back", name, pll, r1rd(r1_apm, r1_pll[cl]), to_con1);
		(void)r1_pll_set(cl, r1_lk_con1[cl] & 0x073fffffu);
		goto back_on_pll;
	}
	QSAY("r1cpu: %s CON1 %#x", pll, r1rd(r1_apm, r1_pll[cl]));
	if (!r1_mux_set(cl, 1)) {
		(void)r1_mux_set(cl, 2);
		SAY("r1cpu: %s: the mux did not come back to %s: on MAINPLL, the rail kept at the intermediate voltage", name, pll);
		(void)r1_track(r1_need(cl, inter));
		return false;
	}
	if (down < r1_cur_vproc) {
		if (!r1_track(down)) {
			SAY("r1cpu: %s: the rail did not come down (Vproc %u Vsram %u): left", name, r1_cur_vproc, r1_cur_vsram);
		}
	}
	QSAY("r1cpu: %s back on %s: %u MHz, loop %u -> %u, Vproc %u Vsram %u", name, pll, r1_pll_khz(r1rd(r1_apm, r1_pll[cl])) / 1000,
	    before, loop_speed(), r1_cur_vproc, r1_cur_vsram);
	return true;

back_on_pll:
	if (r1_mux_set(cl, 1)) {
		r1_rail_back(cl);
	} else {
		SAY("r1cpu: %s: the mux would not go back to %s: the rail is left where it is (Vproc %u Vsram %u)", name, pll, r1_cur_vproc,
		    r1_cur_vsram);
	}
	return false;
}

/* ---- the board's NTC on AUXADC channel 0: 390 kOhm to 1.8 V over the thermistor to ground (mt6765-rabbit-r1.dts ap_ntc) ---- */

static const struct {
	int16_t c;
	uint32_t ohm;
} r1_ntc[] = {                  /* ntc.c ncpXXwf104 */
	{ -40, 4397119 }, { -35, 3088599 }, { -30, 2197225 }, { -25, 1581881 }, { -20, 1151037 }, { -15, 846579 },
	{ -10, 628988 }, { -5, 471632 }, { 0, 357012 }, { 5, 272500 }, { 10, 209710 }, { 15, 162651 }, { 20, 127080 },
	{ 25, 100000 }, { 30, 79222 }, { 35, 63167 }, { 40, 50677 }, { 45, 40904 }, { 50, 33195 }, { 55, 27091 },
	{ 60, 22224 }, { 65, 18323 }, { 70, 15184 }, { 75, 12635 }, { 80, 10566 }, { 85, 8873 }, { 90, 7481 },
	{ 95, 6337 }, { 100, 5384 }, { 105, 4594 }, { 110, 3934 }, { 115, 3380 }, { 120, 2916 }, { 125, 2522 },
};

/* What the last read saw, for r1_show: the clock gate, CON0/CON1/MISC/DAT0 before and after the trigger, and the poll that timed out (1 the first, 2 the second). */
static struct {
	uint32_t gate, before[4], after[4], timeout;
} r1_adc_diag;

static void
r1_adc_regs(uint32_t *v)
{
	v[0] = r1rd(r1_adc, 0x00);
	v[1] = r1rd(r1_adc, 0x04);
	v[2] = r1rd(r1_adc, 0x94);
	v[3] = r1rd(r1_adc, 0x14);
}

static const char *r1_rule = "-";      /* how the last accepted reading was accepted: rdy (bit 12 seen) or 2x (two conversions agree) */

static bool
r1_ntc_temp(uint32_t raw, int *temp)
{
	uint64_t uv = (uint64_t)raw * 1500000 / 4096, ohm;

	if (uv == 0 || uv >= 1800000) {
		return false;
	}
	ohm = 390000 * uv / (1800000 - uv);
	for (unsigned i = 0; i + 1 < sizeof(r1_ntc) / sizeof(r1_ntc[0]); i++) {
		if (ohm <= r1_ntc[i].ohm && ohm >= r1_ntc[i + 1].ohm) {
			*temp = r1_ntc[i].c + (int)(5 * (r1_ntc[i].ohm - ohm) / (r1_ntc[i].ohm - r1_ntc[i + 1].ohm));
			return true;
		}
	}
	return false;
}

/* One conversion of channel 0 as mt6577_auxadc_read does it up to the trigger; the caller waits and reads DAT0. */
static bool
r1_adc_trigger(void)
{
	r1wr(r1_adc, 0x04, r1rd(r1_adc, 0x04) & ~1u);
	for (int i = 0; i < 1000 && (r1rd(r1_adc, 0x14) & (1u << 12)); i++) {
		delay(10);
	}
	if (r1rd(r1_adc, 0x14) & (1u << 12)) {
		return false;
	}
	r1wr(r1_adc, 0x04, r1rd(r1_adc, 0x04) | 1u);
	return true;
}

/*
 * The raw 12-bit reading, and the temperature it means if the resistance is in the table's range (mt6577_auxadc.c's
 * read). If bit 12 (ready) never shows -- it did not, on this chip, though DAT0 held a value -- two conversions 2 ms
 * apart, each read 2 ms after its trigger: accepted only if both are nonzero, within 8 counts, and 15..70 C.
 */
static bool
r1_ntc_read(uint32_t *raw, int *temp)
{
	uint32_t v[2];

	static bool powered;

	r1_adc_diag.gate = r1rd(r1_infra, 0x94);
	r1_adc_diag.timeout = 0;
	if (r1rd(r1_infra, 0x94) & (1u << 10)) {        /* CLK_IFR_AUXADC gated: ungated through the CLR register */
		r1wr(r1_infra, 0x8c, 1u << 10);
	}
	if (!powered) {                                 /* mt6577_auxadc.c probe: MISC (+0x94) bit 14 set, then 1 ms, before any conversion */
		r1wr(r1_adc, 0x94, r1rd(r1_adc, 0x94) | (1u << 14));
		delay(1000);
		powered = true;
	}
	*raw = 0;
	r1_adc_regs(r1_adc_diag.before);
	if (!r1_adc_trigger()) {
		r1_adc_diag.timeout = 1;
		return false;
	}
	delay(20);
	/* bit 12 is set by 25 us after the trigger and gone by 200 us: polled without a pause, and the value taken from the read that saw it */
	for (int i = 0; i < 400; i++) {
		uint32_t d = r1rd(r1_adc, 0x14);

		if (d & (1u << 12)) {
			r1_adc_regs(r1_adc_diag.after);
			*raw = d & 0xfff;
			r1_rule = "rdy";
			return r1_ntc_temp(*raw, temp);
		}
	}
	r1_adc_regs(r1_adc_diag.after);
	r1_adc_diag.timeout = 2;
	for (int i = 0; i < 2; i++) {
		if (!r1_adc_trigger()) {
			return false;
		}
		delay(2000);
		v[i] = r1rd(r1_adc, 0x14) & 0xfff;
	}
	*raw = v[1];
	if (v[0] == 0 || v[1] == 0 || v[0] + 8 < v[1] || v[1] + 8 < v[0] || !r1_ntc_temp((v[0] + v[1]) / 2, temp) || *temp < 15 || *temp > 70) {
		return false;
	}
	r1_rule = "2x";
	return true;
}

/* r1_clk=1, once: four conversions, DAT0 whole at 25 us, 200 us, 2 ms and 10 ms after each trigger, and DAT1..3 (is the thermistor on another channel?) */
static void
r1_adc_probe(void)
{
	uint32_t d[4];

	for (int c = 0; c < 4; c++) {
		if (!r1_adc_trigger()) {
			SAY("r1adc: conv %d: the previous conversion's ready bit would not clear", c);
			continue;
		}
		delay(25);
		d[0] = r1rd(r1_adc, 0x14);
		delay(175);
		d[1] = r1rd(r1_adc, 0x14);
		delay(1800);
		d[2] = r1rd(r1_adc, 0x14);
		delay(8000);
		d[3] = r1rd(r1_adc, 0x14);
		SAY("r1adc: conv %d: DAT0 %#x %#x %#x %#x (25 us, 200 us, 2 ms, 10 ms after the trigger)", c, d[0], d[1], d[2], d[3]);
	}
	SAY("r1adc: DAT1 %#x DAT2 %#x DAT3 %#x", r1rd(r1_adc, 0x18), r1rd(r1_adc, 0x1c), r1rd(r1_adc, 0x20));
}

/* Everything raised put back to LK's clocks, then the rail to LK's (what each switch leaves is at least what the CCI needs). */
static void
r1_restore(void)
{
	for (uint32_t cl = 2; cl-- > 0;) {
		if (r1_raised[cl] && r1_switch(cl, r1_pll_khz(r1_lk_con1[cl]) / 1000, r1_lk_vproc, r1_lk_con1[cl] & 0x073fffffu)) {
			r1_raised[cl] = false;
		}
	}
	if (!r1_raised[0] && !r1_raised[1] && r1_cur_vproc > r1_lk_vproc) {
		(void)r1_track(r1_lk_vproc);
	}
}

static void
r1_drop(int temp)
{
	r1_restore();
	SAY("r1therm: %d C >= %d: back to LK clocks: MP0 %u MHz, Vproc %u", temp, r1_trip, r1_pll_khz(r1rd(r1_apm, r1_pll[0])) / 1000,
	    r1_cur_vproc);
}

static volatile int r1_temp_last = -273;

/* r1_md=7 gate 10: the thermal thread's last good NTC reading */
bool
redwood_r1_temp(int *celsius)
{
	*celsius = r1_temp_last;
	return r1_temp_last > -273;
}

static void
r1_thermal_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint32_t raw = 0, fails = 0;
	int temp = 0;

	for (uint32_t second = 0;; second++) {
		bool ok;

		delay_for_interval(1000, NSEC_PER_MSEC);
		ok = r1_ntc_read(&raw, &temp);
		fails = ok ? 0 : fails + 1;
		if (ok) {
			r1_temp_last = temp;
		}
		if (second % 10 == 0) {
			if (r1_dram_seen) {
				lck_mtx_lock(&r1_mtx);  /* VCORE is read over pwrap, as the governor's switches do */
				r1_dram_now();
				lck_mtx_unlock(&r1_mtx);
			}
			if (ok) {
				redwood_keep("r1therm: NTC %d C (raw %u, %s)", temp, raw, r1_rule);
			} else {
				redwood_keep("r1therm: NTC not read (raw %u)", raw);
			}
		}
		/* ponytail: latched drop, no re-raise or hysteresis; add a 52/58 C step governor (dts 165-175) if sustained load matters */
		lck_mtx_lock(&r1_mtx);
		if (!r1_latched && (r1_raised[0] || r1_raised[1]) && r1_test_s != 0 && second >= r1_test_s) {
			SAY("r1therm: test drop after %u s", second);
			r1_latched = true;
			r1_drop(temp);
		} else if (!r1_latched && (r1_raised[0] || r1_raised[1]) && ((ok && temp >= r1_trip) || fails >= 3)) {
			r1_latched = true;
			r1_drop(temp);
		}
		lck_mtx_unlock(&r1_mtx);
	}
}

static void
r1_thermal_start(void)
{
	thread_t thread;

	if (kernel_thread_start(r1_thermal_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* SMC 0xC2000221 (MTK_SIP_SPM_FIRMWARE_STATUS): -1 not set up, 0 none, 1 loaded, 2 running (psci_smp.c's psci_call) */
static int64_t
sip_call(uint64_t function)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = 0;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	return (int64_t)x0;
}

/* DVFSRC LEVEL [31:16] is one-hot; the bit's place is {0,1,3,4,8,9} for levels 0..5: that level, or -1 if it is none of them. */
static int
r1_level(uint32_t level_reg)
{
	static const uint8_t bits[6] = { 0, 1, 3, 4, 8, 9 };

	for (int i = 0; i < 6; i++) {
		if ((level_reg >> 16) == 1u << bits[i]) {
			return i;
		}
	}
	return -1;
}

static const uint8_t r1_lv_vcore[6] = { 0, 1, 2, 2, 3, 3 };
static const uint8_t r1_lv_dram[6] = { 0, 0, 0, 1, 1, 2 };
static const uint32_t r1_dram_mts[3] = { 1534, 2392, 3200 };

/* r1_clk=1: all of it read, and said (kept whole for the host: a line holds 199 characters, so three of them). */
static void
r1_show(void)
{
	uint32_t m0 = (r1rd(r1_mcu, r1_mux[0]) >> 9) & 3, m1 = (r1rd(r1_mcu, r1_mux[1]) >> 9) & 3, bus = (r1rd(r1_mcu, 0x7c0) >> 9) & 3;
	uint32_t c0 = r1rd(r1_apm, r1_pll[0]), c1 = r1rd(r1_apm, r1_pll[1]), mfg = r1rd(r1_apm, 0x250);
	uint32_t vproc = 0, vsram = 0, vcore = 0, raw = 0, level = 0, req = 0, req2 = 0, loop;
	int temp = 0, lvl = -1;
	bool gated = (r1rd(r1_top, R1_CLK_CFG_5) & (1u << 31)) != 0, ntc;
	int64_t spmfw = sip_call(0xC2000221);

	(void)r1_vproc_get(&vproc);
	(void)r1_vsram_get(&vsram);
	(void)r1_vcore_get(&vcore);
	if (!gated) {                   /* the DVFSRC is read only with spm_sel's clock running */
		level = r1rd(r1_dv, R1_DV_LEVEL);
		req = r1rd(r1_dv, R1_DV_SW_REQ);
		req2 = r1rd(r1_dv, R1_DV_SW_REQ2);
		lvl = r1_level(level);
	}
	r1_adc_probe();
	ntc = r1_ntc_read(&raw, &temp);
	loop = loop_speed();
	SAY("r1adc: infracfg+0x94 %#x (bit 10 gate) before CON0 %#x CON1 %#x MISC %#x DAT0 %#x | after CON0 %#x CON1 %#x MISC %#x DAT0 %#x | timed out: %u",
	    r1_adc_diag.gate, r1_adc_diag.before[0], r1_adc_diag.before[1], r1_adc_diag.before[2], r1_adc_diag.before[3],
	    r1_adc_diag.after[0], r1_adc_diag.after[1], r1_adc_diag.after[2], r1_adc_diag.after[3], r1_adc_diag.timeout);
	SAY("r1clk: MP0 sel %u ARMPLL_L CON1 %#x %u MHz | MP1 sel %u ARMPLL CON1 %#x %u MHz | BUS sel %u CCIPLL %u MHz | MAINPLL %u MHz",
	    m0, c0, r1_pll_khz(c0) / 1000, m1, c1, r1_pll_khz(c1) / 1000, bus, r1_pll_khz(r1rd(r1_apm, 0x230)) / 1000,
	    r1_pll_khz(r1rd(r1_apm, R1_MAINPLL)) / 1000);
	SAY("r1clk: Vproc %u uV | VSRAM_PROC(%s bank) %u uV | VCORE %u uV | mfg_sel %u MFGPLL CON1 %#x %u MHz", vproc,
	    r1_swapped ? "swapped" : "normal", vsram, vcore, r1rd(r1_top, R1_CLK_CFG_1) & 3, mfg, r1_pll_khz(mfg) / 1000);
	SAY("r1clk: DVFSRC LEVEL %#x lvl %d (vcore %d, dram %u MT/s) SW_REQ %#x SW_REQ2 %#x spm_sel %s | SPMFW %ld | NTC raw %u %d C (%s) | loop %u",
	    level, lvl, lvl >= 0 ? r1_lv_vcore[lvl] : -1, lvl >= 0 ? r1_dram_mts[r1_lv_dram[lvl]] : 0, req, req2,
	    gated ? "GATED" : "ungated", (long)spmfw, raw, temp, ntc ? r1_rule : "not valid", loop);
}

/*
 * r1_dram=3200: DVFSRC level 5 asked for (DRAM 3200, VCORE 0.8 V) by one write of SW_REQ, if the SPM firmware
 * is running and the DVFSRC shows a level; it is not started here (that is the SPM entry and INIT/START SMCs,
 * a global change nothing could undo). SW_REQ goes back on the way to the loader (redwood_r1_dram_restore()).
 */
static void
r1_dram(uint32_t mts)
{
	int64_t spmfw = sip_call(0xC2000221);
	uint32_t old, level = 0;
	int lvl = -1;
	bool done = false;

	if (mts != 3200) {
		SAY("r1dram: %u refused: only 3200", mts);
		return;
	}
	if (!(r1rd(r1_top, R1_CLK_CFG_5) & (1u << 31)) && spmfw == 2) {
		lvl = r1_level(r1rd(r1_dv, R1_DV_LEVEL));
	}
	if (lvl < 0) {
		SAY("r1dram: DVFSRC not started (SPMFW %ld): needs SPM entry + INIT/START, not done", (long)spmfw);
		return;
	}
	old = r1rd(r1_dv, R1_DV_SW_REQ);
	r1_sw_req_old = old;
	r1_sw_req_saved = true;
	r1wr(r1_dv, R1_DV_SW_REQ, (old & ~0xfu) | 2 | 3 << 2);
	for (int i = 0; i < 1000 && !done; i++) {
		level = r1rd(r1_dv, R1_DV_LEVEL);
		done = (level >> 16) == 1u << 9;
		if (!done) {
			delay(10);
		}
	}
	lvl = r1_level(level);
	SAY("r1dram: SW_REQ %#x -> %#x, LEVEL %#x = %u MT/s%s", old, r1rd(r1_dv, R1_DV_SW_REQ), level,
	    lvl >= 0 ? r1_dram_mts[r1_lv_dram[lvl]] : 0, done ? "" : " (TIMEOUT: SW_REQ put back)");
	if (!done) {
		r1wr(r1_dv, R1_DV_SW_REQ, old);
		r1_sw_req_saved = false;
	}
}

/*
 * r1_dram=1 (stage A, read only; also done for every other r1_dram value): which DRAM rate and shuffle the DRAMC
 * runs, and the DVFSRC/SPM state. docs/dram-dvfsrc-spec.md section 1 (vendor mtk_dramc.c decode).
 */
#define R1_DRAMC_AO     0x1022a000UL
#define R1_DDRPHY_AO    0x10228000UL
#define R1_SPM_PHYS     0x10006000UL
static vm_offset_t r1_dramc, r1_ddrphy, r1_spm;

static uint32_t
r1_dram_rate(uint32_t shu, uint32_t idx, uint32_t *pcw_o, uint32_t *pre_o, uint32_t *pos_o, uint32_t *ck4_o)
{
	uint32_t pcw = r1rd(r1_ddrphy, (idx ? 0xd94 : 0xd9c) + 0x500 * shu) >> 16;
	uint32_t pp = r1rd(r1_ddrphy, (idx ? 0xda0 : 0xda8) + 0x500 * shu);
	uint32_t pre = (pp >> 18) & 3, pos = pp & 7, ck4 = (r1rd(r1_ddrphy, 0xd18 + 0x500 * shu) >> 27) & 1;

	*pcw_o = pcw;
	*pre_o = pre;
	*pos_o = pos;
	*ck4_o = ck4;
	return (((52u >> pre) * (pcw >> 8)) >> pos) >> ck4;
}

static void
r1_dram_now(void)
{
	uint32_t shu, idx, pcw, pre, pos, ck4, mts, a[3], ctrl = 0, level = 0, req = 0, req2 = 0, rsv9, vcore = 0, d;
	int64_t spmfw = sip_call(0xC2000221);

	if (r1_dramc == 0) {
		r1_dramc = ml_io_map(R1_DRAMC_AO, 0x2000);
		r1_ddrphy = ml_io_map(R1_DDRPHY_AO, 0x2000);
		r1_spm = ml_io_map(R1_SPM_PHYS, 0x1000);
	}
	shu = (r1rd(r1_dramc, 0xe4) >> 1) & 3;
	idx = r1rd(r1_ddrphy, 0x510) >> 31;
	mts = r1_dram_rate(shu < 3 ? shu : 0, idx, &pcw, &pre, &pos, &ck4);
	if (!(r1rd(r1_top, R1_CLK_CFG_5) & (1u << 31))) {
		ctrl = r1rd(r1_dv, 0x00);
		level = r1rd(r1_dv, R1_DV_LEVEL);
		req = r1rd(r1_dv, R1_DV_SW_REQ);
		req2 = r1rd(r1_dv, R1_DV_SW_REQ2);
	}
	rsv9 = r1rd(r1_spm, 0x658);
	(void)r1_vcore_get(&vcore);
	SAY("r1dram: now SHU%u pll%u pcw %#x pre %u pos %u ck4 %u = %u MT/s | CTRL %#x LEVEL %#x SW_REQ %#x SW_REQ2 %#x RSV9 %#x | SPMFW %ld VCORE %u uV%s",
	    shu, idx, pcw, pre, pos, ck4, mts, ctrl, level, req, req2, rsv9, (long)spmfw, vcore,
	    (r1rd(r1_top, R1_CLK_CFG_5) & (1u << 31)) ? " (DVFSRC gated)" : "");
	for (uint32_t i = 0; i < 3; i++) {
		a[i] = r1_dram_rate(i, idx, &d, &d, &d, &d);
	}
	SAY("r1dram: shuffles SHU0 %u SHU1 %u SHU2 %u MT/s", a[0], a[1], a[2]);
}

/* On the way back to the loader (loader_return.c): the memory request taken away again. An MMIO write only: no wait, no pwrap, no log. */
void redwood_r1_dram_restore(void);
void
redwood_r1_dram_restore(void)
{
	if (r1_sw_req_saved) {
		r1_sw_req_saved = false;
		r1wr(r1_dv, R1_DV_SW_REQ, r1_sw_req_old);
	}
}

/*
 * Where a run ends in order (loader_return_timer(), five fingers: next to redwood_adreno_stop(), from a thread that may
 * wait, so the PMIC wrapper and the delays work): what r1_cpufreq raised is put back, so that the loader and Android
 * do not get raised clocks and voltages. Nothing raised: nothing done. The thermal guard's drop is the same one and
 * the lock keeps them apart. A panic, a watchdog reset or any return that does not pass here is not covered.
 */
void redwood_r1_stop(void);
void
redwood_r1_stop(void)
{
	lck_mtx_lock(&r1_mtx);
	if (!r1_latched && (r1_raised[0] || r1_raised[1] || r1_gov_on)) {   /* latched first: the governor does nothing from here */
		r1_latched = true;
		r1_restore();
		SAY("r1cpu: back to LK clocks before return: MP0 %u MHz MP1 %u MHz Vproc %u Vsram %u", r1_pll_khz(r1rd(r1_apm, r1_pll[0])) / 1000,
		    r1_pll_khz(r1rd(r1_apm, r1_pll[1])) / 1000, r1_cur_vproc, r1_cur_vsram);
	}
	lck_mtx_unlock(&r1_mtx);
}

/*
 * r1_cpufreq / r1_cpufreq1: refused unless the frequency is a row of the table and everything reads as LK left it
 * (Vproc 0.800-0.8125 V, the clusters at 1500 and 920 MHz on their own PLLs, VSRAM 50-250 mV above Vproc, MAINPLL
 * 800-1300 MHz, the board cool), so that the voltages above are known to be steps from where the rails are.
 */
static void
r1_cpufreq(const uint32_t mhz[2], bool table_ok)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	const struct r1_opp *row[2] = { NULL, NULL };
	uint32_t vproc = 0, vsram = 0, raw = 0, main_mhz = r1_pll_khz(r1rd(r1_apm, R1_MAINPLL)) / 1000, big = 0;
	const char *why = NULL;
	int temp = 0;

	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		big += ((topology->cpus[i].phys_id >> 8) & 0xff) != 0;
	}
	if (!table_ok) {
		SAY("r1cpu: refused: the table check failed");
		return;
	}
	for (uint32_t cl = 0; cl < 2; cl++) {
		if (mhz[cl] == 0) {
			continue;
		}
		for (uint32_t i = 0; i < R1_OPPS; i++) {
			if (r1_opp[cl][i].mhz == mhz[cl]) {
				row[cl] = &r1_opp[cl][i];
			}
		}
		if (row[cl] == NULL || (cl == 1 && big == 0)) {
			SAY("r1cpu: MP%u %u MHz refused: %s", cl, mhz[cl], row[cl] == NULL ? "not a non-turbo OPP of the table" : "no cores there (cpus must be 5 or more)");
			return;
		}
	}
	if (!r1_vproc_get(&vproc) || !r1_vsram_get(&vsram)) {
		why = "the PMIC did not answer";
	} else if (vproc < R1_UV(45) || vproc > R1_UV(47)) {
		why = "Vproc is not 0.800-0.8125 V";
	} else if (vsram < vproc + 50000 || vsram > vproc + 250000) {
		why = "VSRAM_PROC is not 50-250 mV above Vproc";
	} else if (main_mhz < 800 || main_mhz > 1300) {
		why = "MAINPLL is not 800-1300 MHz";
	} else if (!r1_ntc_read(&raw, &temp) || temp >= r1_trip) {
		why = "the board is not read, or not below the trip";
	}
	for (uint32_t cl = 0; why == NULL && cl < 2; cl++) {
		uint32_t lk = cl ? 920 : 1500, now = r1_pll_khz(r1rd(r1_apm, r1_pll[cl])) / 1000;

		if (row[cl] != NULL && (now + 10 < lk || now > lk + 10 || ((r1rd(r1_mcu, r1_mux[cl]) >> 9) & 3) != 1)) {
			why = cl ? "MP1 is not at 920 MHz on ARMPLL" : "MP0 is not at 1500 MHz on ARMPLL_L";
		}
	}
	if (why != NULL) {
		SAY("r1cpu: left as LK left it: %s (Vproc %u, VSRAM %u, MAINPLL %u MHz, NTC %d C, trip %d)", why, vproc, vsram, main_mhz, temp, r1_trip);
		return;
	}
	r1_lk_vproc = r1_cur_vproc = vproc;
	r1_lk_vsram = r1_cur_vsram = vsram;
	r1_lk_con1[0] = r1rd(r1_apm, r1_pll[0]);
	r1_lk_con1[1] = r1rd(r1_apm, r1_pll[1]);
	r1_lk_top = r1rd(r1_top, R1_TOP_MISC);
	SAY("r1cpu: LK's: Vproc %u Vsram %u ARMPLL_L CON1 %#x ARMPLL CON1 %#x mux %u/%u TOP+0x104 %#x, NTC %d C", vproc, vsram, r1_lk_con1[0],
	    r1_lk_con1[1], (r1rd(r1_mcu, r1_mux[0]) >> 9) & 3, (r1rd(r1_mcu, r1_mux[1]) >> 9) & 3, r1_lk_top, temp);
	for (uint32_t cl = 0; cl < 2; cl++) {
		if (row[cl] != NULL) {
			r1_raised[cl] = r1_switch(cl, row[cl]->mhz, row[cl]->vproc, row[cl]->con1);
			r1_row[cl] = r1_raised[cl] ? row[cl] : NULL;
		}
	}
}

/*
 * r1_gov=1 (with r1_cpufreq=): the governor. Low = LK's clocks (r1_restore), high = the r1_cpufreq rows; r1_raised[0] is
 * the high flag, so the thermal drop and the stop mean what they did. Every 50 ms the busy share of each core (as
 * load_thread): up at once if any is 70% or more, down after 10 samples in a row with all under 30%, 200 ms at least
 * between switches. Nothing while r1_latched (the thermal drop, the end of the run). Screen off: low.
 */
static uint32_t r1_gov_ups, r1_gov_downs, r1_gov_last_us;

void redwood_r1_gov_screen(bool off);
void
redwood_r1_gov_screen(bool off)
{
	r1_gov_off = off;
}

static void
r1_gov_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t idle_before[MAX_CPUS] = { 0 }, then = mach_absolute_time(), started = then, high_ns = 0, switched = 0;
	uint32_t low_run = 0, samples = 0, n_hi = 0, n_mid = 0, n_lo = 0, longest_low = 0;
	bool dead = false;

	for (;;) {
		uint64_t now, ns;
		uint32_t top = 0;
		bool high, go_up, go_down;

		delay_for_interval(50, NSEC_PER_MSEC);
		now = mach_absolute_time();
		for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
			uint64_t idle = 0, busy;

			if (!ml_cpu_idle_time((int)i, &idle)) {
				continue;
			}
			busy = (now - then) > (idle - idle_before[i]) ? (now - then) - (idle - idle_before[i]) : 0;
			idle_before[i] = idle;
			top = MAX(top, (uint32_t)(100 * busy / (now - then)));
		}
		high = r1_raised[0];
		if (high) {
			high_ns += now - then;
		}
		then = now;
		low_run = top < 30 ? low_run + 1 : 0;
		longest_low = MAX(longest_low, low_run);
		if (top >= 70) {
			n_hi++;
		} else if (top >= 30) {
			n_mid++;
		} else {
			n_lo++;
		}
		if (++samples % 200 == 0) {
			absolutetime_to_nanoseconds(now - started, &ns);
			redwood_keep("r1gov: up %u down %u, %u%% of time high, last switch %u us; samples busiest core >=70 %u, 30-69 %u, <30 %u, longest low run %u",
			    r1_gov_ups, r1_gov_downs, ns ? (uint32_t)(100 * high_ns / (now - started)) : 0, r1_gov_last_us, n_hi, n_mid, n_lo, longest_low);
		}
		if (dead || r1_latched) {
			continue;
		}
		absolutetime_to_nanoseconds(now - switched, &ns);
		go_up = !high && !r1_gov_off && top >= 70 && (switched == 0 || ns >= 200 * NSEC_PER_MSEC);
		go_down = high && (r1_gov_off || low_run >= 10) && (switched == 0 || ns >= 200 * NSEC_PER_MSEC);
		if (!go_up && !go_down) {
			continue;
		}
		lck_mtx_lock(&r1_mtx);
		if (!r1_latched) {
			r1_quiet = true;
			if (go_up) {
				for (uint32_t cl = 0; cl < 2; cl++) {
					if (r1_row[cl] != NULL && !r1_raised[cl]) {
						r1_raised[cl] = r1_switch(cl, r1_row[cl]->mhz, r1_row[cl]->vproc, r1_row[cl]->con1);
						dead |= !r1_raised[cl];
					}
				}
				r1_gov_ups++;
			} else {
				r1_restore();
				r1_gov_downs++;
			}
			r1_quiet = false;
		}
		lck_mtx_unlock(&r1_mtx);
		switched = mach_absolute_time();
		absolutetime_to_nanoseconds(switched - now, &ns);
		r1_gov_last_us = (uint32_t)(ns / 1000);
		low_run = 0;
		if (dead) {
			SAY("r1gov: a switch up failed: the governor stops (clocks as the switch left them)");
		}
	}
}

/* Everything of the r1: false (and nothing touched) unless one of its boot-args is there. */
static bool
r1_init(void)
{
	uint32_t clk = 0, mhz[2] = { 0, 0 }, dram = 0, trip = 58, bench = 0, gov = 0;
	bool table_ok = false;

	(void)PE_parse_boot_argn("r1_clk", &clk, sizeof(clk));
	(void)PE_parse_boot_argn("r1_cpufreq", &mhz[0], sizeof(mhz[0]));
	(void)PE_parse_boot_argn("r1_cpufreq1", &mhz[1], sizeof(mhz[1]));
	(void)PE_parse_boot_argn("r1_dram", &dram, sizeof(dram));
	if (clk != 1 && mhz[0] == 0 && mhz[1] == 0 && dram == 0) {
		return false;
	}
	if (PE_parse_boot_argn("r1_thermal", &trip, sizeof(trip))) {
		r1_trip = (int)trip;
	}
	(void)PE_parse_boot_argn("r1_thermal_test", &r1_test_s, sizeof(r1_test_s));
	r1_top = ml_io_map(R1_TOP_PHYS, 0x1000);
	r1_infra = ml_io_map(R1_INFRA_PHYS, 0x1000);
	r1_apm = ml_io_map(R1_APMIXED_PHYS, 0x1000);
	r1_pwrap = ml_io_map(R1_PWRAP_PHYS, 0x1000);
	r1_dv = ml_io_map(R1_DVFSRC_PHYS, 0x1000);
	r1_mcu = ml_io_map(R1_MCUCFG_PHYS, 0x1000);
	r1_adc = ml_io_map(R1_AUXADC_PHYS, 0x1000);
	if (clk == 1 || mhz[0] != 0 || mhz[1] != 0) {
		uint32_t top2 = 0;

		if (pmic_read(TOP2_ELR0, &top2)) {
			r1_swapped = (top2 & (1u << 15)) != 0;
		}
		table_ok = r1_table_ok();
	}
	if (clk == 1) {
		r1_show();
	}
	if (mhz[0] != 0 || mhz[1] != 0) {
		r1_cpufreq(mhz, table_ok);
	}
	if (dram != 0) {
		r1_dram_now();
		r1_dram_seen = true;
		if (dram != 1) {
			r1_dram(dram);
		}
	}
	if (clk == 1 || mhz[0] != 0 || mhz[1] != 0 || dram == 1) {
		r1_thermal_start();
	}
	if (PE_parse_boot_argn("r1_gov", &gov, sizeof(gov)) && gov == 1 && r1_row[0] != NULL) {
		thread_t thread;

		r1_gov_on = true;
		if (kernel_thread_start(r1_gov_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
		SAY("r1gov: on: low = LK's clocks, high = r1_cpufreq");
	}
	if (PE_parse_boot_argn("membench", &bench, sizeof(bench)) && bench == 1) {
		memory_bench();
	} else if (bench == 2) {
		thread_t thread;

		if (kernel_thread_start(bench_all, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
	return true;
}

void
redwood_cpufreq_init(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t enabled = 0, cores[EPSS_DOMAINS] = { 0 }, before, here;

	load_start();
	enabled = 2;
	PE_parse_boot_argn("ddr", &enabled, sizeof(enabled));
	if (enabled == 1) {
		ddr_look();
		ddr_slots();
	} else if (enabled == 2 && cpufreq_wanted()) {
		ddr_raise();
	}
	if (r1_init()) {
		return;
	}
	enabled = 0;
	if (!PE_parse_boot_argn("cpufreq", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		cores[domain_of(topology->cpus[i].phys_id)]++;
	}
	/* (whichever core this thread happens to be on, and stays on or not: it only goes to the log) */
	here = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	before = loop_speed();
	for (uint32_t domain = 0; domain < EPSS_DOMAINS; domain++) {
		if (cores[domain] != 0) {
			raise_domain(domain, cores[domain]);
		}
	}
	printf("cpufreq: on core %u a loop ran at %u, now at %u\n", here, before, loop_speed());
	if (PE_parse_boot_argn("membench", &enabled, sizeof(enabled)) && enabled == 1) {
		memory_bench();
	} else if (enabled == 2) {
		thread_t thread;

		if (kernel_thread_start(bench_all, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
}
