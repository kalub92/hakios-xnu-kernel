/*
 * celld: the cellular bearer's face in iOS (docs/cellular-stage4.md 3.2 and 4). No CommCenter here, so this daemon makes configd's network
 * service for pdp_ip0 out of the kernel's status sysctls (redwood_md.c: kern.redwood.cell_state / cell_addr / cell_dns / cell_mtu / cell_gen,
 * redwood_cell.c): while the state is data_up, State:/Network/Service/<id>/{IPv4,DNS} and State:/Network/Interface/pdp_ip0/Link are in
 * the dynamic store, with PrimaryRank "Last" (Wi-Fi wins whenever it has an address); when it is not, they are removed. IPMonitor then installs
 * the default route and the DNS configuration itself (the kernel adds no default route in service mode).
 * Also the daily data allowance (4d): /private/var/redwood/cell-usage = "YYYY-MM-DD bytes"; at start kern.redwood.cell_budget_kb is lowered to
 * what is left of today's allowance (boot-arg r1_md_daily_mb, default 100); the file is rewritten at every MB and when the bearer goes down.
 *
 * Started from rc.jobs in the background; ends at once unless the boot-args have r1_md=10, r1_md=11 or r1_md_iffake=1. Lines: kept log "celld:".
 * ponytail: polls once a second (no sysctl notification exists), State only (no Setup: entries) -- add them if IPMonitor ignores the service.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

typedef const struct __SCDynamicStore *SCDynamicStoreRef;
extern SCDynamicStoreRef SCDynamicStoreCreate(CFAllocatorRef allocator, CFStringRef name, void *callout, void *context);
extern Boolean SCDynamicStoreSetValue(SCDynamicStoreRef store, CFStringRef key, CFPropertyListRef value);
extern Boolean SCDynamicStoreRemoveValue(SCDynamicStoreRef store, CFStringRef key);

#define SVC     "5C3E1B7A-0D1F-4C6E-9A52-7B1E4F9D2A60"
#define K_IPV4  "State:/Network/Service/" SVC "/IPv4"
#define K_DNS   "State:/Network/Service/" SVC "/DNS"
#define K_SVC   "State:/Network/Service/" SVC "/Service"
#define K_LINK  "State:/Network/Interface/pdp_ip0/Link"
#define USAGE   "/private/var/redwood/cell-usage"

static void
say(const char *fmt, ...)
{
	char text[170], line[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	snprintf(line, sizeof(line), "!celld: %s", text);
	printf("celld: %s\n", text);
	fflush(stdout);
	sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
	snprintf(line, sizeof(line), "celld: %s", text);
	sysctlbyname("kern.redwood.host_keep", NULL, NULL, line, strlen(line));
}

static CFStringRef
cfs(const char *s)
{
	return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

static CFArrayRef
one(const char *s)
{
	CFStringRef v = cfs(s);
	return CFArrayCreate(NULL, (const void **)&v, 1, &kCFTypeArrayCallBacks);
}

static void
set(SCDynamicStoreRef store, const char *key, const char *const *k, CFTypeRef *v, int n)
{
	CFStringRef kk[8];
	CFTypeRef vv[8];
	CFDictionaryRef d;

	for (int i = 0; i < n; i++) {
		kk[i] = cfs(k[i]);
		vv[i] = v[i];
	}
	d = CFDictionaryCreate(NULL, (const void **)kk, (const void **)vv, n, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (!SCDynamicStoreSetValue(store, cfs(key), d)) {
		say("SCDynamicStoreSetValue %s failed", key);
	}
}

static void
publish(SCDynamicStoreRef store, const char *addr, const char *dns, int mtu)
{
	/* the bearer's peer is its first DNS server (redwood_cell.c): the router of the point-to-point link */
	CFTypeRef yes = kCFBooleanTrue, name = cfs("pdp_ip0"), rank = cfs("Last");
	{
		const char *k[] = { "Addresses", "DestAddresses", "Router", "SubnetMasks", "InterfaceName", "PrimaryRank" };
		CFTypeRef v[] = { one(addr), one(dns), cfs(dns), one("255.255.255.255"), name, rank };
		set(store, K_IPV4, k, v, 6);
	}
	{
		const char *k[] = { "ServerAddresses", "InterfaceName" };
		CFTypeRef v[] = { one(dns), name };
		set(store, K_DNS, k, v, 2);
	}
	{
		const char *k[] = { "PrimaryRank" };
		set(store, K_SVC, k, &rank, 1);
	}
	{
		const char *k[] = { "Active" };
		set(store, K_LINK, k, &yes, 1);
	}
	say("published %s: %s dns %s mtu %d, rank Last", SVC, "pdp_ip0", dns, mtu);
}

static void
withdraw(SCDynamicStoreRef store)
{
	SCDynamicStoreRemoveValue(store, cfs(K_IPV4));
	SCDynamicStoreRemoveValue(store, cfs(K_DNS));
	SCDynamicStoreRemoveValue(store, cfs(K_SVC));
	SCDynamicStoreRemoveValue(store, cfs(K_LINK));
	say("withdrawn");
}

static int
sysint(const char *name)
{
	int v = 0;
	size_t n = sizeof(v);

	return sysctlbyname(name, &v, &n, NULL, 0) == 0 ? v : -1;
}

static void
sysstr(const char *name, char *out, size_t size)
{
	size_t n = size - 1;

	out[0] = 0;
	if (sysctlbyname(name, out, &n, NULL, 0) == 0) {
		out[n < size ? n : size - 1] = 0;
	}
}

static void
today(char *out, size_t size)
{
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	strftime(out, size, "%Y-%m-%d", &tm);
}

int
main(void)
{
	char args[1024], state[32] = "", addr[24], dns[24], day[16], fday[16];
	uint64_t used0 = 0, bytes = 0, written = 0;
	unsigned long long fb = 0;
	int gen = -1, mtu, daily_mb = 100, kb, left_kb, published = 0;
	SCDynamicStoreRef store;
	const char *p;
	FILE *f;

	sysstr("kern.bootargs", args, sizeof(args));
	if (!strstr(args, "r1_md=10") && !strstr(args, "r1_md=11") && !strstr(args, "r1_md_iffake=1")) {
		return 0;
	}
	if ((p = strstr(args, "r1_md_daily_mb=")) != NULL) {
		daily_mb = atoi(p + 15);
	}
	store = SCDynamicStoreCreate(NULL, CFSTR("celld"), NULL, NULL);
	if (store == NULL) {
		say("no dynamic store (configd not running?)");
		return 1;
	}
	/* the daily allowance */
	today(day, sizeof(day));
	if ((f = fopen(USAGE, "r")) != NULL) {
		if (fscanf(f, "%15s %llu", fday, &fb) == 2 && !strcmp(fday, day)) {
			used0 = fb;
		}
		fclose(f);
	}
	left_kb = (int)(daily_mb * 1024 - (long long)(used0 / 1024));
	left_kb = left_kb < 0 ? 0 : left_kb;
	kb = sysint("kern.redwood.cell_budget_kb");
	if (kb < 0 || left_kb < kb) {
		sysctlbyname("kern.redwood.cell_budget_kb", NULL, NULL, &left_kb, sizeof(left_kb));
	}
	say("started, state %s, allowance today %d MB, used %llu B, kernel budget %d -> %d KB", sysint("kern.redwood.cell_gen") < 0 ? "none" : "ok", daily_mb,
	    (unsigned long long)used0, kb, left_kb < kb || kb < 0 ? left_kb : kb);
	mkdir("/private/var/redwood", 0755);
	for (;;) {
		int g = sysint("kern.redwood.cell_gen"), up;
		size_t n = sizeof(bytes);

		sysstr("kern.redwood.cell_state", state, sizeof(state));
		sysstr("kern.redwood.cell_addr", addr, sizeof(addr));
		sysstr("kern.redwood.cell_dns", dns, sizeof(dns));
		mtu = sysint("kern.redwood.cell_mtu");
		up = !strcmp(state, "data_up") && addr[0] && dns[0];
		if (up && (!published || g != gen)) {
			publish(store, addr, dns, mtu);
			published = 1;
		} else if (!up && published) {
			withdraw(store);
			published = 0;
		}
		gen = g;
		if (sysctlbyname("kern.redwood.cell_bytes", &bytes, &n, NULL, 0) == 0 && (bytes - written >= 1048576 || (!up && bytes != written))) {
			written = bytes;
			if ((f = fopen(USAGE, "w")) != NULL) {
				fprintf(f, "%s %llu\n", day, (unsigned long long)(used0 + bytes));
				fclose(f);
				say("usage %s %llu bytes (state %s)", day, (unsigned long long)(used0 + bytes), state);
			}
		}
		sleep(1);
	}
}
