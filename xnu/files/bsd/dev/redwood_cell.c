/*
 * Stage 3b (boot-arg r1_md_if=1, see redwood_md.c): pdp_ip0, the modem's bearer as a network interface.
 *
 * What kind of interface: family IFNET_FAMILY_CELLULAR, type IFT_CELLULAR (iOS's own name and type for it), point-to-point, no ARP, no
 * link-layer header at all: every packet the stack hands to output is a bare IPv4 packet and goes to CLDMA TX queue 0 (redwood_cldma.c);
 * every packet the modem delivers is a bare IP packet too. This kernel has no plumber for the cellular family (the real ones live in the
 * baseband driver), so one is registered here, copied from utun's: it attaches the protocol with an input and a pre_output and nothing else.
 * Received packets go to the stack with proto_input() (as bsd/net/if_ipsec.c's non-nexus path does), not through ifnet_input(): there is
 * no demux to run and no header to strip.
 *
 * Mbuf ownership: output() always frees the mbuf it is given (the data is copied into the CLDMA slot first); input() allocates its own
 * mbuf, and either proto_input() takes it or it is freed here. Nothing is kept.
 *
 * Teardown: the interface is never detached (ifnet_detach is asynchronous and the kernel is about to power the modem down anyway): it is
 * brought DOWN, its default routes and address removed, and left attached with cell_up false, so output and input drop everything.
 *
 * Locking: cell_lock (a mutex) covers the counters and cell_up only, held for a few instructions, never across cldma_tx(), the stack or a
 * log line. Output may run on any thread; input and tick run on the modem thread.
 */
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <net/bpf.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
#include <net/kpi_protocol.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <sys/kpi_mbuf.h>
#include <sys/kpi_socket.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern void redwood_md_say(bool must, const char *format, ...) __printflike(2, 3);
extern bool cldma_tx(const uint8_t *ip, uint32_t len, uint32_t ms);
#define SAYM(...) redwood_md_say(true, __VA_ARGS__)

#define CELL_MAX_PKT    1500u
#define CELL_NAME       "pdp_ip0"

static LCK_GRP_DECLARE(cell_grp, "redwood-cell");
static LCK_MTX_DECLARE(cell_lock, &cell_grp);

static ifnet_t cell_ifp;
static volatile bool cell_up;                   /* packets pass: set when the interface is configured, cleared first thing in stop */
static bool cell_stopped;
static volatile uint32_t cell_now;              /* the modem thread's clock (ms), for cldma_tx's slot timestamps */
static uint8_t c_ip[4], c_peer[4], c_dns[4];
static uint32_t cap_pkts, cap_bytes, next_tick, tx_logged;
static struct {
	uint32_t tx_p, tx_b, rx_p, rx_b, drops, tx_v6, rx_v6, tx_busy, cap_hit;
	uint32_t txp[4], rxp[4];                /* tcp, udp, icmp, other */
	uint32_t tx_dns;                        /* UDP to port 53 */
} cs;

static char cell_dns_str[20];
static int cell_done_flag;
SYSCTL_DECL(_kern_redwood);
SYSCTL_STRING(_kern_redwood, OID_AUTO, cell_dns, CTLFLAG_RD | CTLFLAG_LOCKED, cell_dns_str, sizeof(cell_dns_str), "the cellular bearer's DNS server (r1_md_if=1), empty until the interface is up");
SYSCTL_INT(_kern_redwood, OID_AUTO, cell_done, CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, &cell_done_flag, 0, "set to 1 by cell_test when it is finished: the data phase ends (r1_md_if=1)");

static inline uint32_t
pclass(uint8_t proto)
{
	return proto == IPPROTO_TCP ? 0 : (proto == IPPROTO_UDP ? 1 : (proto == IPPROTO_ICMP ? 2 : 3));
}

/* ---- the interface ---- */

static errno_t
cell_demux(__unused ifnet_t ifp, mbuf_t data, __unused char *frame_header, protocol_family_t *protocol)
{
	while (data != NULL && mbuf_len(data) < 1) {
		data = mbuf_next(data);
	}
	if (data == NULL) {
		return ENOENT;
	}
	switch (((uint8_t *)mbuf_data(data))[0] >> 4) {
	case 4:
		*protocol = PF_INET;
		break;
	case 6:
		*protocol = PF_INET6;
		break;
	default:
		*protocol = PF_UNSPEC;
		break;
	}
	return 0;
}

static errno_t
cell_add_proto(__unused ifnet_t ifp, protocol_family_t protocol, __unused const struct ifnet_demux_desc *demux, __unused u_int32_t count)
{
	return protocol == PF_INET || protocol == PF_INET6 ? 0 : ENOPROTOOPT;
}

static errno_t
cell_del_proto(__unused ifnet_t ifp, __unused protocol_family_t protocol)
{
	return 0;
}

/* the dlil input path (not the one used here, but a protocol needs an input): same as the direct one */
static errno_t
cell_proto_input(ifnet_t ifp, protocol_family_t protocol, mbuf_t m, __unused char *frame_header)
{
	mbuf_pkthdr_setrcvif(m, ifp);
	if (proto_input(protocol, m) != 0) {
		mbuf_freem(m);
		ifnet_stat_increment_in(ifp, 0, 0, 1);
	}
	return 0;
}

static errno_t
cell_proto_pre_output(__unused ifnet_t ifp, protocol_family_t protocol, __unused mbuf_t *packet, __unused const struct sockaddr *dest, __unused void *route,
    char *frame_type, __unused char *link_layer_dest)
{
	*(protocol_family_t *)(void *)frame_type = protocol;
	return 0;
}

static errno_t
cell_attach_proto(ifnet_t ifp, protocol_family_t protocol)
{
	struct ifnet_attach_proto_param proto;
	errno_t error;

	bzero(&proto, sizeof(proto));
	proto.input = cell_proto_input;
	proto.pre_output = cell_proto_pre_output;
	error = ifnet_attach_protocol(ifp, protocol, &proto);
	return error == EEXIST ? 0 : error;
}

static errno_t
cell_ioctl(ifnet_t ifp, unsigned long command, void *data)
{
	struct ifreq *request = data;
	errno_t error = 0;

	switch (command) {
	case SIOCSIFADDR:
		ifnet_set_flags(ifp, IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);
		break;
	case SIOCSIFFLAGS:
		ifnet_set_flags(ifp, (ifnet_flags(ifp) & IFF_UP) ? IFF_RUNNING : 0, IFF_RUNNING);
		break;
	case SIOCGIFDEVMTU:
		request->ifr_devmtu.ifdm_current = ifnet_mtu(ifp);
		request->ifr_devmtu.ifdm_max = CELL_MAX_PKT;
		request->ifr_devmtu.ifdm_min = IF_MINMTU;
		break;
	case SIOCSIFMTU:
		error = request->ifr_mtu > CELL_MAX_PKT || request->ifr_mtu < IF_MINMTU ? EINVAL : ifnet_set_mtu(ifp, request->ifr_mtu);
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}
	return error;
}

/* The stack has a packet to send: a bare IP packet. Always consumes the mbuf. */
static errno_t
cell_output(ifnet_t ifp, mbuf_t m)
{
	uint8_t pkt[CELL_MAX_PKT];
	size_t len = mbuf_pkthdr_len(m);
	bool sent = false, v6 = false, capped = false;
	uint32_t ihl = 0, dport = 0;

	if (cell_up && len >= 20 && len <= sizeof(pkt) && mbuf_copydata(m, 0, len, pkt) == 0) {
		if ((pkt[0] >> 4) == 4) {
			lck_mtx_lock(&cell_lock);
			capped = cs.tx_p >= cap_pkts || cs.tx_b + len > cap_bytes;
			if (capped) {
				cs.cap_hit |= 1;
			}
			lck_mtx_unlock(&cell_lock);
			if (!capped) {
				sent = cldma_tx(pkt, (uint32_t)len, cell_now);
			}
		} else {
			v6 = (pkt[0] >> 4) == 6;
		}
	}
	lck_mtx_lock(&cell_lock);
	if (sent) {
		ihl = (uint32_t)(pkt[0] & 15) * 4;
		cs.tx_p++;
		cs.tx_b += (uint32_t)len;
		cs.txp[pclass(pkt[9])]++;
		if (pkt[9] == IPPROTO_UDP && ihl + 4 <= len) {
			dport = (uint32_t)(pkt[ihl + 2] << 8 | pkt[ihl + 3]);
			cs.tx_dns += dport == 53;
		} else if (pkt[9] == IPPROTO_TCP && ihl + 4 <= len) {
			dport = (uint32_t)(pkt[ihl + 2] << 8 | pkt[ihl + 3]);
		}
	} else {
		cs.drops++;
		cs.tx_v6 += v6;
		cs.tx_busy += !capped && !v6 && cell_up;
	}
	lck_mtx_unlock(&cell_lock);
	if (sent) {
		bpf_tap_out(ifp, DLT_RAW, m, NULL, 0);
		ifnet_stat_increment_out(ifp, 1, (uint32_t)len, 0);
		if (tx_logged < 16) {   /* what else talks, once a default route exists: protocol, port, destination (two octets unless it is the DNS server) */
			tx_logged++;
			if (!memcmp(pkt + 16, c_dns, 4)) {
				SAYM("r1cell: tx #%u proto %u dport %u len %zu to %u.%u.%u.%u (DNS)", tx_logged, pkt[9], dport, len, pkt[16], pkt[17], pkt[18], pkt[19]);
			} else {
				SAYM("r1cell: tx #%u proto %u dport %u len %zu to %u.%u.x.x", tx_logged, pkt[9], dport, len, pkt[16], pkt[17]);
			}
		}
	} else {
		ifnet_stat_increment_out(ifp, 0, 0, 1);
	}
	mbuf_freem(m);
	return 0;
}

/* A packet from the modem (redwood_md.c cell_rx, modem thread): the bare IP packet, still in its CLDMA buffer. */
void
cell_if_input(const uint8_t *ip, uint32_t len)
{
	mbuf_t m = NULL;
	unsigned chunks = 1;
	bool drop = false, v6 = false, capped = false;
	uint32_t pc;

	if (!cell_up) {
		return;
	}
	lck_mtx_lock(&cell_lock);
	if (len < 20 || len > CELL_MAX_PKT || (ip[0] >> 4) != 4) {
		v6 = len >= 20 && (ip[0] >> 4) == 6;
		cs.rx_v6 += v6;
		drop = true;
	} else if (cs.rx_p >= cap_pkts || cs.rx_b + len > cap_bytes) {
		cs.cap_hit |= 2;
		capped = drop = true;
	} else {
		pc = pclass(ip[9]);
		cs.rx_p++;
		cs.rx_b += len;
		cs.rxp[pc]++;
	}
	cs.drops += drop;
	lck_mtx_unlock(&cell_lock);
	if (drop) {
		(void)capped;
		return;
	}
	if (mbuf_allocpacket(MBUF_DONTWAIT, len, &chunks, &m) != 0 || m == NULL) {
		lck_mtx_lock(&cell_lock);
		cs.drops++;
		lck_mtx_unlock(&cell_lock);
		return;
	}
	if (mbuf_copyback(m, 0, len, ip, MBUF_DONTWAIT) != 0) {
		mbuf_freem(m);
		lck_mtx_lock(&cell_lock);
		cs.drops++;
		lck_mtx_unlock(&cell_lock);
		return;
	}
	mbuf_pkthdr_setrcvif(m, cell_ifp);
	bpf_tap_in(cell_ifp, DLT_RAW, m, NULL, 0);
	if (proto_input(PF_INET, m) != 0) {     /* not taken: ours to free */
		mbuf_freem(m);
		ifnet_stat_increment_in(cell_ifp, 0, 0, 1);
		lck_mtx_lock(&cell_lock);
		cs.drops++;
		lck_mtx_unlock(&cell_lock);
	} else {
		ifnet_stat_increment_in(cell_ifp, 1, len, 0);
	}
}

/* ---- configuring it, the way a user-space configurator would (ioctls on a kernel socket, a route) ---- */

static void
fill_sin(struct sockaddr_in *s, const uint8_t a[4])
{
	bzero(s, sizeof(*s));
	s->sin_len = sizeof(*s);
	s->sin_family = AF_INET;
	memcpy(&s->sin_addr, a, 4);
}

static errno_t
cell_flags(socket_t so, bool up)
{
	struct ifreq ifr;

	bzero(&ifr, sizeof(ifr));
	strlcpy(ifr.ifr_name, CELL_NAME, sizeof(ifr.ifr_name));
	ifr.ifr_flags = (short)(up ? (ifnet_flags(cell_ifp) | IFF_UP) : (ifnet_flags(cell_ifp) & ~IFF_UP));
	return sock_ioctl(so, SIOCSIFFLAGS, &ifr);
}

static int
cell_route(bool add, bool scoped)
{
	static const uint8_t zero[4];
	struct sockaddr_in dst, gw, mask;
	int flags = RTF_UP | RTF_GATEWAY | RTF_STATIC;

	fill_sin(&dst, zero);
	fill_sin(&gw, c_peer);
	fill_sin(&mask, zero);
	if (scoped) {
		return rtrequest_scoped(add ? RTM_ADD : RTM_DELETE, (struct sockaddr *)&dst, (struct sockaddr *)&gw, (struct sockaddr *)&mask, flags | RTF_IFSCOPE, NULL, ifnet_index(cell_ifp));
	}
	return rtrequest(add ? RTM_ADD : RTM_DELETE, (struct sockaddr *)&dst, (struct sockaddr *)&gw, (struct sockaddr *)&mask, flags, NULL);
}

/* Creates pdp_ip0, gives it the bearer's /32 with `peer` as its point-to-point peer, adds the default routes, publishes the DNS server (sysctl
 * kern.redwood.cell_dns) and lets packets through. false + why on any failure (what was made stays, down). */
bool
cell_if_attach(const uint8_t ip[4], const uint8_t peer[4], const uint8_t dns[4], uint32_t mtu, uint32_t pkts, uint32_t kb, char *why, size_t whylen)
{
	struct ifnet_init_eparams init = {
		.ver = IFNET_INIT_CURRENT_VERSION,
		.len = sizeof(init),
		.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO,
		.uniqueid = "redwood-pdp_ip0",
		.uniqueid_len = 15,
		.name = "pdp_ip",
		.unit = 0,
		.family = IFNET_FAMILY_CELLULAR,
		.type = IFT_CELLULAR,
		.output = cell_output,
		.demux = cell_demux,
		.add_proto = cell_add_proto,
		.del_proto = cell_del_proto,
		.ioctl = cell_ioctl,
	};
	struct in_aliasreq ra;
	struct ifreq ifr;
	socket_t so = NULL;
	ifnet_t ifp = NULL;
	errno_t e;
	int r1, r2;

	if (cell_ifp != NULL) {
		snprintf(why, whylen, "already attached");
		return false;
	}
	memcpy(c_ip, ip, 4);
	memcpy(c_peer, peer, 4);
	memcpy(c_dns, dns, 4);
	cap_pkts = pkts;
	cap_bytes = kb * 1000;
	if (mtu < 576 || mtu > CELL_MAX_PKT) {
		mtu = 1430;
	}
	e = proto_register_plumber(PF_INET, IFNET_FAMILY_CELLULAR, cell_attach_proto, NULL);
	if (e != 0 && e != EEXIST) {
		snprintf(why, whylen, "plumber %d", e);
		return false;
	}
	e = ifnet_allocate_extended(&init, &ifp);
	if (e != 0) {
		snprintf(why, whylen, "allocate %d", e);
		return false;
	}
	ifnet_set_capabilities_enabled(ifp, 0, -1);
	ifnet_set_baudrate(ifp, 10 * 1000 * 1000);
	ifnet_set_mtu(ifp, mtu);
	ifnet_set_flags(ifp, IFF_POINTOPOINT | IFF_NOARP, 0xffff);
	ifnet_set_offload(ifp, 0);
	e = ifnet_attach(ifp, NULL);
	if (e != 0) {
		ifnet_release(ifp);
		snprintf(why, whylen, "attach %d", e);
		return false;
	}
	bpfattach(ifp, DLT_RAW, 0);
	cell_ifp = ifp;
	SAYM("r1cell: ifnet %s attached: family cellular, type IFT_CELLULAR, raw IP, mtu %u, caps %u pkts %u KB per direction", CELL_NAME, mtu, pkts, kb);
	e = sock_socket(AF_INET, SOCK_DGRAM, 0, NULL, NULL, &so);
	if (e != 0) {
		snprintf(why, whylen, "socket %d", e);
		return false;
	}
	e = cell_flags(so, true);
	if (e != 0) {
		snprintf(why, whylen, "up %d", e);
		sock_close(so);
		return false;
	}
	bzero(&ifr, sizeof(ifr));
	strlcpy(ifr.ifr_name, CELL_NAME, sizeof(ifr.ifr_name));
	e = sock_ioctl(so, SIOCPROTOATTACH, &ifr);
	if (e != 0 && e != EEXIST) {    /* (SIOCAIFADDR below plumbs the protocol itself too: not fatal here) */
		SAYM("r1cell: SIOCPROTOATTACH returned %d", e);
	}
	bzero(&ra, sizeof(ra));
	strlcpy(ra.ifra_name, CELL_NAME, sizeof(ra.ifra_name));
	fill_sin(&ra.ifra_addr, ip);
	fill_sin(&ra.ifra_dstaddr, peer);
	fill_sin(&ra.ifra_mask, (const uint8_t[4]){ 255, 255, 255, 255 });
	e = sock_ioctl(so, SIOCAIFADDR, &ra);
	sock_close(so);
	if (e != 0) {
		snprintf(why, whylen, "address %d", e);
		return false;
	}
	r1 = cell_route(true, false);
	r2 = cell_route(true, true);
	SAYM("r1cell: address %u.%u.x.x/32 peer %u.%u.%u.%u set, default route: unscoped %d scoped %d (0 = ok)", ip[0], ip[1], peer[0], peer[1], peer[2], peer[3], r1, r2);
	if (r1 != 0 && r2 != 0) {
		snprintf(why, whylen, "no default route %d/%d", r1, r2);
		return false;
	}
	snprintf(cell_dns_str, sizeof(cell_dns_str), "%u.%u.%u.%u", dns[0], dns[1], dns[2], dns[3]);
	lck_mtx_lock(&cell_lock);
	cell_up = true;
	lck_mtx_unlock(&cell_lock);
	next_tick = cell_now + 10000;
	SAYM("r1cell: if up: %s %u.%u.x.x, DNS %s published as kern.redwood.cell_dns", CELL_NAME, ip[0], ip[1], cell_dns_str);
	return true;
}

static void
cell_counters(const char *tag)
{
	uint32_t a[9], tp[4], rp[4], dns;

	lck_mtx_lock(&cell_lock);
	a[0] = cs.tx_p; a[1] = cs.tx_b; a[2] = cs.rx_p; a[3] = cs.rx_b; a[4] = cs.drops; a[5] = cs.tx_v6; a[6] = cs.rx_v6; a[7] = cs.tx_busy; a[8] = cs.cap_hit;
	memcpy(tp, cs.txp, sizeof(tp));
	memcpy(rp, cs.rxp, sizeof(rp));
	dns = cs.tx_dns;
	lck_mtx_unlock(&cell_lock);
	SAYM("r1cell: %s: tx %u/%u rx %u/%u drops %u", tag, a[0], a[1], a[2], a[3], a[4]);
	SAYM("r1cell: protos tx tcp %u udp %u icmp %u other %u (dns %u) rx tcp %u udp %u icmp %u other %u; v6 dropped tx %u rx %u; tx ring busy %u; capped %#x", tp[0], tp[1], tp[2], tp[3], dns,
	    rp[0], rp[1], rp[2], rp[3], a[5], a[6], a[7], a[8]);
}

/* every modem-thread pass: the clock for the TX slots, the 10 s counters line; true once when a traffic cap was hit */
bool
cell_if_tick(uint32_t rel)
{
	static bool said_cap;
	bool hit;

	cell_now = rel;
	if (!cell_up) {
		return false;
	}
	if (rel >= next_tick) {
		next_tick = rel + 10000;
		cell_counters("if up");
	}
	lck_mtx_lock(&cell_lock);
	hit = cs.cap_hit != 0 && !said_cap;
	said_cap |= hit;
	lck_mtx_unlock(&cell_lock);
	return hit;
}

bool
cell_if_active(void)
{
	return cell_up;
}

bool
cell_if_done(void)
{
	return cell_done_flag != 0;
}

bool
cell_if_exists(void)
{
	return cell_ifp != NULL;
}

void
cell_if_stats(uint32_t *txp, uint32_t *txb, uint32_t *rxp, uint32_t *rxb, uint32_t *drops, bool *caphit)
{
	lck_mtx_lock(&cell_lock);
	*txp = cs.tx_p;
	*txb = cs.tx_b;
	*rxp = cs.rx_p;
	*rxb = cs.rx_b;
	*drops = cs.drops;
	*caphit = cs.cap_hit != 0;
	lck_mtx_unlock(&cell_lock);
}

/* Down before the off sequence (and in md_power_off): packets stop first, then the routes and the address go and the interface is marked down.
 * Not detached (see the top). Safe to call more than once. */
void
cell_if_stop(const char *why)
{
	socket_t so = NULL;
	struct in_aliasreq ra;
	errno_t e1 = 0, e2 = 0;
	int r1, r2;

	if (cell_ifp == NULL) {
		return;
	}
	lck_mtx_lock(&cell_lock);
	cell_up = false;
	if (cell_stopped) {
		lck_mtx_unlock(&cell_lock);
		return;
	}
	cell_stopped = true;
	lck_mtx_unlock(&cell_lock);
	cell_counters("if final");
	r1 = cell_route(false, false);
	r2 = cell_route(false, true);
	if (sock_socket(AF_INET, SOCK_DGRAM, 0, NULL, NULL, &so) == 0) {
		bzero(&ra, sizeof(ra));
		strlcpy(ra.ifra_name, CELL_NAME, sizeof(ra.ifra_name));
		fill_sin(&ra.ifra_addr, c_ip);
		e1 = sock_ioctl(so, SIOCDIFADDR, &ra);
		e2 = cell_flags(so, false);
		sock_close(so);
	}
	SAYM("r1cell: if down (%s): routes removed %d/%d, address %d, flags %d; %s stays attached, permanently down (no detach)", why, r1, r2, e1, e2, CELL_NAME);
}
