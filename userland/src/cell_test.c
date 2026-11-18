/*
 * cell_test [ifarg WORD]: Stage 3b's test of pdp_ip0, the modem's bearer as a network interface (xnu/files/bsd/dev/redwood_cell.c).
 * Started from rc.jobs in the background; ends at once unless the kernel's arguments have WORD (r1_md_if=1) or r1_md=9 (the shorthand
 * that includes it). Every line goes to the kept log ("!" + kern.redwood.say) with the prefix "cell_test:". One pass, no retries:
 *   (a) wait up to 150 s for pdp_ip0 to have an IPv4 address and the kernel to publish the DNS server (kern.redwood.cell_dns);
 *       log the interface, the address (two octets) and the default route's interface
 *   (b) one UDP DNS query for example.com to that server, from a socket bound to pdp_ip0 (IP_BOUND_IF)
 *   (c) TCP to the first A record, port 80, bound the same way, GET /, the status line, bytes, time to first byte, total time
 *   (d) getaddrinfo("example.com"): would an ordinary app resolve?
 *   (e) the RESULT line; then kern.redwood.cell_done=1 tells the kernel the data phase can end.
 * About 2 KB of traffic in all. Nothing listens.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define IFNAME "pdp_ip0"

static void
say(const char *fmt, ...)
{
	char text[170], line[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	snprintf(line, sizeof(line), "!cell_test: %s", text);
	printf("cell_test: %s\n", text);
	fflush(stdout);
	sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
}

static double
now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static void
finish_flag(void)
{
	int one = 1;

	sysctlbyname("kern.redwood.cell_done", NULL, NULL, &one, sizeof(one));
}

static volatile int g_have_if;

static void
on_alarm(int sig)
{
	(void)sig;
	say("RESULT if %s dns fail http fail resolver fail bytes 0 (watchdog: 75 s)", g_have_if ? "ok" : "missing");
	finish_flag();
	_exit(0);
}

static int
bootarg_has(const char *word)
{
	char args[1024] = "", *at;
	size_t size = sizeof(args) - 1, n = strlen(word);

	sysctlbyname("kern.bootargs", args, &size, NULL, 0);
	for (at = args; (at = strstr(at, word)) != NULL; at += n) {
		if ((at == args || at[-1] == ' ') && (at[n] == 0 || at[n] == ' ')) {
			return 1;
		}
	}
	return 0;
}

/* the interface's IPv4 address, if it has one */
static int
if_addr(struct in_addr *out)
{
	struct ifaddrs *list, *a;
	int found = 0;

	if (getifaddrs(&list) != 0) {
		return 0;
	}
	for (a = list; a; a = a->ifa_next) {
		if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && !strcmp(a->ifa_name, IFNAME)) {
			*out = ((struct sockaddr_in *)(void *)a->ifa_addr)->sin_addr;
			found = 1;
		}
	}
	freeifaddrs(list);
	return found;
}

/* the iOS SDK has no net/route.h: the pieces of it that the NET_RT_DUMP reader needs (Darwin's layout, 92 bytes) */
struct rt_metrics {
	uint32_t rmx_locks, rmx_mtu, rmx_hopcount;
	int32_t rmx_expire;
	uint32_t rmx_recvpipe, rmx_sendpipe, rmx_ssthresh, rmx_rtt, rmx_rttvar, rmx_pksent, rmx_state, rmx_filler[3];
};
struct rt_msghdr {
	unsigned short rtm_msglen;
	unsigned char rtm_version, rtm_type;
	unsigned short rtm_index;
	int rtm_flags, rtm_addrs, rtm_pid, rtm_seq, rtm_errno, rtm_use;
	uint32_t rtm_inits;
	struct rt_metrics rtm_rmx;
};
#define RTF_UP          0x1
#define RTF_IFSCOPE     0x1000000
#define RTAX_DST        0
#define RTAX_NETMASK    2
#define RTAX_MAX        8
#define RT_ROUND(n) (((n) + 3) & ~3)

/* the default routes in the kernel's table (NET_RT_DUMP): "pdp_ip0" or other names, scoped ones marked */
static void
default_routes(char *out, size_t size)
{
	int mib[6] = { CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_DUMP, 0 };
	size_t len = 0;
	char *buf, *p;
	int n = 0;

	out[0] = 0;
	if (sysctl(mib, 6, NULL, &len, NULL, 0) != 0 || len == 0 || !(buf = malloc(len))) {
		snprintf(out, size, "(route table unreadable)");
		return;
	}
	if (sysctl(mib, 6, buf, &len, NULL, 0) != 0) {
		free(buf);
		snprintf(out, size, "(route table unreadable)");
		return;
	}
	for (p = buf; p + sizeof(struct rt_msghdr) <= buf + len;) {
		struct rt_msghdr *rtm = (struct rt_msghdr *)(void *)p;
		char *sa = (char *)(rtm + 1);
		struct sockaddr_in *dst = NULL, *mask = NULL;
		int bit;

		if (rtm->rtm_msglen < sizeof(*rtm)) {
			break;
		}
		for (bit = 0; bit < RTAX_MAX && sa < p + rtm->rtm_msglen; bit++) {
			struct sockaddr *s = (struct sockaddr *)(void *)sa;

			if (!(rtm->rtm_addrs & (1 << bit))) {
				continue;
			}
			if (bit == RTAX_DST) {
				dst = (struct sockaddr_in *)(void *)s;
			} else if (bit == RTAX_NETMASK) {
				mask = (struct sockaddr_in *)(void *)s;
			}
			sa += s->sa_len ? RT_ROUND(s->sa_len) : 4;
		}
		if (dst && dst->sin_family == AF_INET && dst->sin_addr.s_addr == 0 && (!mask || mask->sin_len <= 8 || mask->sin_addr.s_addr == 0) && (rtm->rtm_flags & RTF_UP) && n < 4) {
			char name[IF_NAMESIZE] = "?";
			size_t used = strlen(out);

			if_indextoname(rtm->rtm_index, name);
			snprintf(out + used, size - used, "%s%s%s", used ? ", " : "", name, (rtm->rtm_flags & RTF_IFSCOPE) ? " (scoped)" : "");
			n++;
		}
		p += rtm->rtm_msglen;
	}
	if (!n) {
		snprintf(out, size, "none");
	}
	free(buf);
}

static int
bound_socket(int type, unsigned idx)
{
	int fd = socket(AF_INET, type, 0);

	if (fd < 0) {
		return -1;
	}
	if (setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof(idx)) != 0) {
		say("IP_BOUND_IF failed: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static int
dns_skip(const unsigned char *m, int ml, int *pos)
{
	int q = *pos;

	for (int guard = 0; guard < 64; guard++) {
		if (q >= ml) {
			return 0;
		}
		if (m[q] == 0) {
			*pos = q + 1;
			return 1;
		}
		if ((m[q] & 0xc0) == 0xc0) {
			*pos = q + 2;
			return *pos <= ml;
		}
		if (m[q] & 0xc0) {
			return 0;
		}
		q += 1 + m[q];
	}
	return 0;
}

/* one query, one answer: rcode (-1 = none), answers, first A in `a` */
static int
dns_query(unsigned idx, struct in_addr server, int *rcode, int *answers, struct in_addr *a, double *rtt)
{
	static const unsigned char q[] = { 0x5a, 0x17, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0,
		7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1 };
	unsigned char r[512];
	struct sockaddr_in to;
	struct timeval tv = { 5, 0 };
	int fd = bound_socket(SOCK_DGRAM, idx), n, pos, qd, an;
	double t0;

	*rcode = -1;
	*answers = 0;
	a->s_addr = 0;
	if (fd < 0) {
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	memset(&to, 0, sizeof(to));
	to.sin_len = sizeof(to);
	to.sin_family = AF_INET;
	to.sin_port = htons(53);
	to.sin_addr = server;
	t0 = now_ms();
	if (sendto(fd, q, sizeof(q), 0, (struct sockaddr *)&to, sizeof(to)) != (ssize_t)sizeof(q)) {
		say("dns: sendto failed: %s", strerror(errno));
		close(fd);
		return -1;
	}
	n = (int)recv(fd, r, sizeof(r), 0);
	*rtt = now_ms() - t0;
	close(fd);
	if (n < 12 || r[0] != 0x5a || r[1] != 0x17) {
		return -1;
	}
	*rcode = r[3] & 15;
	qd = r[4] << 8 | r[5];
	an = r[6] << 8 | r[7];
	*answers = an;
	pos = 12;
	for (int i = 0; i < qd && i < 4; i++) {
		if (!dns_skip(r, n, &pos)) {
			return 0;
		}
		pos += 4;
	}
	for (int i = 0; i < an && i < 16 && pos < n; i++) {
		int type, rdl;

		if (!dns_skip(r, n, &pos) || pos + 10 > n) {
			break;
		}
		type = r[pos] << 8 | r[pos + 1];
		rdl = r[pos + 8] << 8 | r[pos + 9];
		if (pos + 10 + rdl > n) {
			break;
		}
		if (type == 1 && rdl == 4 && !a->s_addr) {
			memcpy(&a->s_addr, r + pos + 10, 4);
		}
		pos += 10 + rdl;
	}
	return 0;
}

/* GET / from `to` on port 80: status line, bytes, ttfb, total. Returns the status code, or -1 */
static int
http_get(unsigned idx, struct in_addr to, int *bytes, double *ttfb, double *total, char *status, size_t ssize)
{
	static const char req[] = "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n";
	struct sockaddr_in sa;
	char buf[2048];
	int fd = bound_socket(SOCK_STREAM, idx), code = -1;
	double t0 = now_ms(), deadline;
	fd_set set;
	struct timeval tv;

	*bytes = 0;
	*ttfb = *total = -1;
	status[0] = 0;
	if (fd < 0) {
		return -1;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(80);
	sa.sin_addr = to;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 && errno != EINPROGRESS) {
		snprintf(status, ssize, "connect: %s", strerror(errno));
		close(fd);
		return -1;
	}
	FD_ZERO(&set);
	FD_SET(fd, &set);
	tv.tv_sec = 10;
	tv.tv_usec = 0;
	if (select(fd + 1, NULL, &set, NULL, &tv) <= 0) {
		snprintf(status, ssize, "connect timeout");
		close(fd);
		return -1;
	}
	{
		int err = 0;
		socklen_t el = sizeof(err);

		getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
		if (err) {
			snprintf(status, ssize, "connect: %s", strerror(err));
			close(fd);
			return -1;
		}
	}
	say("http: connected in %.0f ms", now_ms() - t0);
	if (send(fd, req, sizeof(req) - 1, 0) != (ssize_t)sizeof(req) - 1) {
		snprintf(status, ssize, "send: %s", strerror(errno));
		close(fd);
		return -1;
	}
	deadline = now_ms() + 10000;
	for (;;) {
		ssize_t n;
		double left = deadline - now_ms();

		if (left <= 0 || *bytes >= 16384) {
			break;
		}
		FD_ZERO(&set);
		FD_SET(fd, &set);
		tv.tv_sec = (time_t)(left / 1000);
		tv.tv_usec = (suseconds_t)((left - tv.tv_sec * 1000) * 1000);
		if (select(fd + 1, &set, NULL, NULL, &tv) <= 0) {
			break;
		}
		n = recv(fd, buf, sizeof(buf), 0);
		if (n <= 0) {
			break;      /* closed (Connection: close) */
		}
		if (*bytes == 0) {
			char *e;

			*ttfb = now_ms() - t0;
			buf[n < (ssize_t)sizeof(buf) ? n : (ssize_t)sizeof(buf) - 1] = 0;
			e = strpbrk(buf, "\r\n");
			if (e) {
				*e = 0;
			}
			for (char *c = buf; *c; c++) {
				if (!isprint((unsigned char)*c)) {
					*c = '?';
				}
			}
			snprintf(status, ssize, "%.60s", buf);
			if (!strncmp(buf, "HTTP/", 5) && strchr(buf, ' ')) {
				code = atoi(strchr(buf, ' '));
			}
		}
		*bytes += (int)n;
	}
	*total = now_ms() - t0;
	close(fd);
	return code;
}

/* getaddrinfo in a thread, so that a resolver with no server cannot hold the test beyond 10 s */
static struct {
	int done, rc;
	double ms;
	pthread_mutex_t m;
	pthread_cond_t c;
} gai = { 0, 0, 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER };

static void *
gai_thread(void *arg)
{
	struct addrinfo hints, *res = NULL;
	double t0 = now_ms();
	int rc;

	(void)arg;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	rc = getaddrinfo("example.com", "80", &hints, &res);
	pthread_mutex_lock(&gai.m);
	gai.rc = rc;
	gai.ms = now_ms() - t0;
	gai.done = 1;
	pthread_cond_signal(&gai.c);
	pthread_mutex_unlock(&gai.m);
	if (res) {
		freeaddrinfo(res);
	}
	return NULL;
}

int
main(int argc, char **argv)
{
	struct in_addr ip = { 0 }, dns = { 0 }, a = { 0 };
	char dnsbuf[32] = "", routes[120], status[80] = "", ipm[24];
	unsigned idx;
	int have_if = 0, dns_ok = 0, http = -1, res_ok = 0, bytes = 0, rcode, answers;
	double rtt = 0, ttfb, total, t0;
	pthread_t th;

	if (argc >= 3 && !strcmp(argv[1], "ifarg") && !bootarg_has(argv[2]) && !bootarg_has("r1_md=9")) {
		return 0;
	}
	signal(SIGPIPE, SIG_IGN);
	signal(SIGALRM, on_alarm);
	/* (a) wait for the interface; the whole test is over by ~40 s after it appears, the watchdog fires 75 s after that */
	say("started: waiting up to 150 s for %s", IFNAME);
	for (t0 = now_ms(); now_ms() - t0 < 150000; usleep(500000)) {
		size_t len = sizeof(dnsbuf) - 1;

		if (if_addr(&ip) && ip.s_addr) {
			dnsbuf[0] = 0;
			sysctlbyname("kern.redwood.cell_dns", dnsbuf, &len, NULL, 0);
			if (dnsbuf[0] && inet_pton(AF_INET, dnsbuf, &dns) == 1) {
				have_if = 1;
				break;
			}
		}
	}
	g_have_if = have_if;
	alarm(75);
	if (!have_if) {
		say("%s never got an IPv4 address (and a published DNS server) in 150 s", IFNAME);
		goto summary;
	}
	{
		unsigned char *o = (unsigned char *)&ip;

		snprintf(ipm, sizeof(ipm), "%u.%u.x.x", o[0], o[1]);
	}
	idx = if_nametoindex(IFNAME);
	default_routes(routes, sizeof(routes));
	say("(a) %s index %u address %s, DNS %s, default route(s): %s (up after %.0f s)", IFNAME, idx, ipm, dnsbuf, routes, (now_ms() - t0) / 1000);
	/* (b) */
	if (dns_query(idx, dns, &rcode, &answers, &a, &rtt) == 0 && rcode >= 0) {
		dns_ok = rcode == 0 && answers > 0 && a.s_addr;
		say("(b) dns example.com via %s: rcode %d answers %d first A %s rtt %.0f ms", dnsbuf, rcode, answers, a.s_addr ? inet_ntoa(a) : "-", rtt);
	} else {
		say("(b) dns example.com via %s: no answer in 5 s (rtt %.0f ms)", dnsbuf, rtt);
	}
	/* (c) */
	if (dns_ok) {
		http = http_get(idx, a, &bytes, &ttfb, &total, status, sizeof(status));
		say("(c) http %s:80: status \"%s\" (%d) bytes %d first byte %.0f ms total %.0f ms", inet_ntoa(a), status, http, bytes, ttfb, total);
	} else {
		say("(c) http skipped: no A record");
	}
	/* (d) */
	if (pthread_create(&th, NULL, gai_thread, NULL) == 0) {
		struct timespec ts;
		struct timeval tv;

		gettimeofday(&tv, NULL);
		ts.tv_sec = tv.tv_sec + 10;
		ts.tv_nsec = tv.tv_usec * 1000;
		pthread_mutex_lock(&gai.m);
		while (!gai.done && pthread_cond_timedwait(&gai.c, &gai.m, &ts) == 0) {
		}
		if (gai.done) {
			res_ok = gai.rc == 0;
			say("(d) getaddrinfo example.com: %s (rc %d) %.0f ms", gai.rc == 0 ? "ok" : gai_strerror(gai.rc), gai.rc, gai.ms);
		} else {
			say("(d) getaddrinfo example.com: no answer in 10 s");
		}
		pthread_mutex_unlock(&gai.m);
	} else {
		say("(d) getaddrinfo: thread failed");
	}
summary:
	if (http >= 0) {
		snprintf(status, sizeof(status), "%d", http);
	} else {
		snprintf(status, sizeof(status), "fail");
	}
	say("RESULT if %s dns %s http %s resolver %s bytes %d", have_if ? "ok" : "missing", dns_ok ? "ok" : "fail", status, res_ok ? "ok" : "fail", bytes);
	finish_flag();
	return 0;
}
