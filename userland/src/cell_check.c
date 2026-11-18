/*
 * cell_check: the radio-off tests of Stage 4c/4d (docs/cellular-stage4.md 7), with the kernel's fake bearer (boot-arg r1_md_iffake=1) and celld.
 *   usage: cell_check STEP...      every line goes to the kept log ("cell_check:")
 *   wait S                  sleep S seconds
 *   fake 1|0                bring the fake bearer up / down (kern.redwood.cell_fake)
 *   state NAME S            wait up to S s for kern.redwood.cell_state == NAME
 *   dump                    configd's store: State:/Network/Global/IPv4 and the service's IPv4, DNS, Service and the pdp_ip0 Link
 *   ifstat                  pdp_ip0 and en0: packets/bytes in and out, flags, SIOCGIFEXPENSIVE / SIOCGIFCONSTRAINED
 *   udp ADDR PORT N SIZE US N unbound UDP datagrams of SIZE bytes to ADDR:PORT, US microseconds apart
 *   gai NAME                getaddrinfo (mDNSResponder; the answer does not matter, where the query goes does)
 *   usage                   the daily usage file
 */
#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>

typedef const struct __SCDynamicStore *SCDynamicStoreRef;
extern SCDynamicStoreRef SCDynamicStoreCreate(CFAllocatorRef allocator, CFStringRef name, void *callout, void *context);
extern CFPropertyListRef SCDynamicStoreCopyValue(SCDynamicStoreRef store, CFStringRef key);

#define SVC "5C3E1B7A-0D1F-4C6E-9A52-7B1E4F9D2A60"

static void
say(const char *fmt, ...)
{
	char text[400], line[420];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	for (char *c = text; *c; c++) {
		if (*c == '\n' || *c == '\t') {
			*c = ' ';
		}
	}
	printf("cell_check: %s\n", text);
	fflush(stdout);
	snprintf(line, sizeof(line), "!cell_check: %s", text);
	sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
	snprintf(line, sizeof(line), "cell_check: %s", text);
	sysctlbyname("kern.redwood.host_keep", NULL, NULL, line, strlen(line));
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
store_key(SCDynamicStoreRef store, const char *key)
{
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFPropertyListRef v = store ? SCDynamicStoreCopyValue(store, k) : NULL;
	char text[300] = "(absent)";

	if (v != NULL) {
		CFStringRef d = CFCopyDescription(v);
		CFStringGetCString(d, text, sizeof(text), kCFStringEncodingUTF8);
	}
	say("store %s = %s", key + (strncmp(key, "State:/Network/", 15) ? 0 : 15), text);
}

static void
dump(void)
{
	SCDynamicStoreRef store = SCDynamicStoreCreate(NULL, CFSTR("cell_check"), NULL, NULL);

	store_key(store, "State:/Network/Global/IPv4");
	store_key(store, "State:/Network/Service/" SVC "/IPv4");
	store_key(store, "State:/Network/Service/" SVC "/DNS");
	store_key(store, "State:/Network/Service/" SVC "/Service");
	store_key(store, "State:/Network/Interface/pdp_ip0/Link");
}

static void
ifstat(void)
{
	struct ifaddrs *list, *a;
	static const char *const names[] = { "pdp_ip0", "en0" };

	if (getifaddrs(&list) != 0) {
		return;
	}
	for (int i = 0; i < 2; i++) {
		for (a = list; a; a = a->ifa_next) {
			if (a->ifa_addr && a->ifa_addr->sa_family == AF_LINK && !strcmp(a->ifa_name, names[i]) && a->ifa_data) {
				struct if_data *d = a->ifa_data;
				int s = socket(AF_INET, SOCK_DGRAM, 0), ex = -1, co = -1;
				struct ifreq r;

				if (s >= 0) {
					memset(&r, 0, sizeof(r));
					strlcpy(r.ifr_name, names[i], sizeof(r.ifr_name));
					if (ioctl(s, _IOWR('i', 160, struct ifreq), &r) == 0) {
						ex = *(int *)(void *)&r.ifr_ifru;
					}
					memset(&r, 0, sizeof(r));
					strlcpy(r.ifr_name, names[i], sizeof(r.ifr_name));
					if (ioctl(s, _IOWR('i', 204, struct ifreq), &r) == 0) {
						co = *(int *)(void *)&r.ifr_ifru;
					}
					close(s);
				}
				say("ifstat %s: flags %#x type %u tx %u pkts %u B, rx %u pkts %u B, expensive %d constrained %d", names[i], a->ifa_flags, d->ifi_type, d->ifi_opackets,
				    d->ifi_obytes, d->ifi_ipackets, d->ifi_ibytes, ex, co);
			}
		}
	}
	freeifaddrs(list);
}

int
main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "wait") && i + 1 < argc) {
			sleep((unsigned)atoi(argv[++i]));
		} else if (!strcmp(argv[i], "fake") && i + 1 < argc) {
			int v = atoi(argv[++i]), e = sysctlbyname("kern.redwood.cell_fake", NULL, NULL, &v, sizeof(v));
			say("fake %d: %s", v, e ? "REFUSED (no r1_md_iffake=1, or the allowance is used up)" : "ok");
		} else if (!strcmp(argv[i], "state") && i + 2 < argc) {
			char st[32] = "";
			int t = atoi(argv[i + 2]);

			for (int n = 0; n < t * 10; n++) {
				sysstr("kern.redwood.cell_state", st, sizeof(st));
				if (!strcmp(st, argv[i + 1])) {
					break;
				}
				usleep(100000);
			}
			say("state %s (wanted %s)", st, argv[i + 1]);
			i += 2;
		} else if (!strcmp(argv[i], "dump")) {
			dump();
		} else if (!strcmp(argv[i], "ifstat")) {
			ifstat();
		} else if (!strcmp(argv[i], "udp") && i + 5 < argc) {
			struct sockaddr_in to = { .sin_len = sizeof(to), .sin_family = AF_INET };
			int s = socket(AF_INET, SOCK_DGRAM, 0), n = atoi(argv[i + 3]), size = atoi(argv[i + 4]), us = atoi(argv[i + 5]), sent = 0, err = 0;
			char *buf = calloc(1, 1500);

			inet_pton(AF_INET, argv[i + 1], &to.sin_addr);
			to.sin_port = htons((uint16_t)atoi(argv[i + 2]));
			for (int k = 0; k < n; k++) {
				if (sendto(s, buf, (size_t)(size > 1400 ? 1400 : size), 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
					err = errno;
				} else {
					sent++;
				}
				usleep((useconds_t)us);
			}
			say("udp %s:%s: %d of %d sent, last errno %d", argv[i + 1], argv[i + 2], sent, n, err);
			close(s);
			free(buf);
			i += 5;
		} else if (!strcmp(argv[i], "gai") && i + 1 < argc) {
			struct addrinfo *res = NULL, hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
			int e = getaddrinfo(argv[++i], NULL, &hints, &res);

			say("gai %s: %s (%d)", argv[i], e ? gai_strerror(e) : "answered", e);
		} else if (!strcmp(argv[i], "usage")) {
			char text[80] = "(none)";
			FILE *f = fopen("/private/var/redwood/cell-usage", "r");

			if (f) {
				fgets(text, sizeof(text), f);
				fclose(f);
			}
			say("usage file: %s", text);
		}
	}
	return 0;
}
