/*
 * logto: with boot-arg logto=IP:PORT, send the kernel's log (kern.msgbuf: it has the kept lines too, said again
 * every minute) as UDP datagrams of at most 1200 bytes to that address, once en0 has an IPv4 address (looked at
 * every 5 s). Without the boot-arg it ends at once. Never blocks the boot: rc.jobs starts it with "&".
 * (kern.redwood.host_keep is write-only; only its length can be read, so it is not sent.)
 * Mac side: tools/logrecv.sh PORT.
 */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>

static int
en0_has_address(void)
{
	struct ifaddrs *list, *i;
	int found = 0;

	if (getifaddrs(&list) != 0) {
		return 0;
	}
	for (i = list; i != NULL; i = i->ifa_next) {
		if (strcmp(i->ifa_name, "en0") == 0 && i->ifa_addr != NULL && i->ifa_addr->sa_family == AF_INET &&
		    ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr != 0) {
			found = 1;
		}
	}
	freeifaddrs(list);
	return found;
}

int
main(void)
{
	char args[1024] = "", *at, host[64];
	size_t size = sizeof(args) - 1, cap = 0, have, tail_len = 0;
	struct sockaddr_in to = { .sin_len = sizeof(to), .sin_family = AF_INET };
	char *buf, tail[64];
	int s;

	sysctlbyname("kern.bootargs", args, &size, NULL, 0);
	if ((at = strstr(args, "logto=")) == NULL || (at != args && at[-1] != ' ')) {
		return 0;
	}
	at += 6;
	if (sscanf(at, "%63[0-9.]:%hu", host, &to.sin_port) != 2 || inet_pton(AF_INET, host, &to.sin_addr) != 1) {
		return 1;
	}
	to.sin_port = htons(to.sin_port);
	while (!en0_has_address()) {
		sleep(5);
	}
	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) < 0 || sysctlbyname("kern.msgbuf", NULL, &cap, NULL, 0) != 0 ||
	    (buf = malloc(cap + 64)) == NULL) {
		return 1;
	}
	for (;;) {
		size_t start = 0;

		have = cap;
		if (sysctlbyname("kern.msgbuf", buf, &have, NULL, 0) == 0) {
			if (tail_len != 0) {
				for (size_t p = have >= tail_len ? have - tail_len + 1 : 0; p-- > 0;) {    /* the last place the old end is seen */
					if (memcmp(buf + p, tail, tail_len) == 0) {
						start = p + tail_len;
						break;
					}
				}
			}
			for (size_t p = start; p < have; p += 1200) {
				sendto(s, buf + p, have - p < 1200 ? have - p : 1200, 0, (struct sockaddr *)&to, sizeof(to));
				usleep(2000);
			}
			if (have > start) {
				tail_len = have < sizeof(tail) ? have : sizeof(tail);
				memcpy(tail, buf + have - tail_len, tail_len);
			}
		}
		usleep(500000);
	}
}
