// SPDX-License-Identifier: GPL-2.0
/*
 * a6l-net: tiny static network test tool for the V74 recovery (no ip/route/busybox there; toybox ping
 * only uses SOCK_DGRAM ICMP, blocked by ping_group_range). data3 agent, 25 Sep 2026.
 *
 *   a6l-net addr   IF A.B.C.D[/len]          set IPv4 address + netmask, bring IF up        (SIOCSIFADDR)
 *   a6l-net route  A.B.C.D[/len] IF [GW]     add a route via IF (host route for /32)        (SIOCADDRT)
 *   a6l-net ping   IF DST [count] [wait_s]   ICMP echo on a RAW socket bound to IF (root)   -> A6L_NET_PING_PASS|FAIL
 *   a6l-net dns    IF SERVER NAME [wait_s]   one UDP A query bound to IF                     -> A6L_NET_DNS_PASS|FAIL
 *   a6l-net http   IF IP [port] [host]       TCP connect bound to IF + HEAD /, first line    -> A6L_NET_HTTP_PASS|FAIL
 *   a6l-net stats  IF...                     rx/tx/drop counters from /sys/class/net
 *
 * Binding to the interface (SO_BINDTODEVICE) makes the kernel treat the destination as on-link when no
 * route exists (net/ipv4/route.c), so the test never touches the Wi-Fi/USB default routes.
 * Exit status: 0 pass, 1 fail, 2 usage/system error.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int parse_cidr(const char *s, struct in_addr *a, int *len)
{
	char buf[64];
	char *sl;

	snprintf(buf, sizeof(buf), "%s", s);
	sl = strchr(buf, '/');
	*len = 32;
	if (sl) {
		*sl = 0;
		*len = atoi(sl + 1);
		if (*len < 0 || *len > 32)
			return -1;
	}
	return inet_pton(AF_INET, buf, a) == 1 ? 0 : -1;
}

static uint32_t mask_of(int len)
{
	return len ? htonl(0xffffffffu << (32 - len)) : 0;
}

static int bind_dev(int fd, const char *ifname)
{
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname) + 1) < 0) {
		printf("A6L_NET SO_BINDTODEVICE %s: %s\n", ifname, strerror(errno));
		return -1;
	}
	return 0;
}

static int if_addr(const char *ifname, struct in_addr *out)
{
	struct ifreq ifr;
	int fd = socket(AF_INET, SOCK_DGRAM, 0), r;

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
	r = ioctl(fd, SIOCGIFADDR, &ifr);
	close(fd);
	if (r < 0)
		return -1;
	*out = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
	return 0;
}

static int cmd_addr(const char *ifname, const char *cidr)
{
	struct ifreq ifr;
	struct in_addr a;
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
	int len, fd;

	if (parse_cidr(cidr, &a, &len))
		return 2;
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
	sin->sin_family = AF_INET;
	sin->sin_addr = a;
	if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) {
		printf("A6L_NET_ADDR_FAIL %s %s: SIOCSIFADDR %s\n", ifname, cidr, strerror(errno));
		return 1;
	}
	sin->sin_addr.s_addr = mask_of(len);
	if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0)
		printf("A6L_NET netmask: %s (continuing)\n", strerror(errno));
	if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
		ifr.ifr_flags |= IFF_UP;
		ioctl(fd, SIOCSIFFLAGS, &ifr);
	}
	close(fd);
	printf("A6L_NET_ADDR_OK %s %s\n", ifname, cidr);
	return 0;
}

static int cmd_route(const char *cidr, const char *ifname, const char *gw)
{
	struct rtentry rt;
	struct in_addr a, g;
	char dev[IFNAMSIZ];
	int len, fd, r;

	if (parse_cidr(cidr, &a, &len))
		return 2;
	memset(&rt, 0, sizeof(rt));
	((struct sockaddr_in *)&rt.rt_dst)->sin_family = AF_INET;
	((struct sockaddr_in *)&rt.rt_dst)->sin_addr.s_addr = a.s_addr & mask_of(len);
	((struct sockaddr_in *)&rt.rt_genmask)->sin_family = AF_INET;
	((struct sockaddr_in *)&rt.rt_genmask)->sin_addr.s_addr = mask_of(len);
	rt.rt_flags = RTF_UP | (len == 32 ? RTF_HOST : 0);
	if (gw && *gw) {
		if (inet_pton(AF_INET, gw, &g) != 1)
			return 2;
		((struct sockaddr_in *)&rt.rt_gateway)->sin_family = AF_INET;
		((struct sockaddr_in *)&rt.rt_gateway)->sin_addr = g;
		rt.rt_flags |= RTF_GATEWAY;
	}
	snprintf(dev, sizeof(dev), "%s", ifname);
	rt.rt_dev = dev;
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	r = ioctl(fd, SIOCADDRT, &rt);
	close(fd);
	if (r < 0 && errno != EEXIST) {
		printf("A6L_NET_ROUTE_FAIL %s dev %s%s%s: %s\n", cidr, ifname, gw ? " via " : "", gw ? gw : "",
		       strerror(errno));
		return 1;
	}
	printf("A6L_NET_ROUTE_OK %s dev %s%s%s%s\n", cidr, ifname, gw ? " via " : "", gw ? gw : "",
	       r < 0 ? " (exists)" : "");
	return 0;
}

static uint16_t csum(const void *p, int n)
{
	const uint16_t *w = p;
	uint32_t s = 0;

	for (; n > 1; n -= 2)
		s += *w++;
	if (n)
		s += *(const uint8_t *)w;
	s = (s >> 16) + (s & 0xffff);
	s += s >> 16;
	return (uint16_t)~s;
}

static int cmd_ping(const char *ifname, const char *dst, int count, int wait_s)
{
	struct sockaddr_in to = { .sin_family = AF_INET }, from;
	struct in_addr src;
	uint16_t id = (uint16_t)getpid();
	int fd, got = 0, sent = 0, i;
	char sbuf[INET_ADDRSTRLEN] = "?";

	if (inet_pton(AF_INET, dst, &to.sin_addr) != 1)
		return 2;
	fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (fd < 0) {
		printf("A6L_NET_PING_FAIL socket(SOCK_RAW): %s (need root)\n", strerror(errno));
		return 2;
	}
	if (bind_dev(fd, ifname))
		return 2;
	if (!if_addr(ifname, &src)) {
		struct sockaddr_in b = { .sin_family = AF_INET, .sin_addr = src };

		inet_ntop(AF_INET, &src, sbuf, sizeof(sbuf));
		if (bind(fd, (struct sockaddr *)&b, sizeof(b)) < 0)
			printf("A6L_NET bind %s: %s\n", sbuf, strerror(errno));
	}
	printf("A6L_NET_PING %s -> %s via %s (raw ICMP, id 0x%04x)\n", sbuf, dst, ifname, id);
	for (i = 0; i < count; i++) {
		unsigned char pkt[64];
		struct icmphdr *ic = (struct icmphdr *)pkt;
		double t0, deadline;

		memset(pkt, 0, sizeof(pkt));
		ic->type = ICMP_ECHO;
		ic->un.echo.id = htons(id);
		ic->un.echo.sequence = htons(i + 1);
		memcpy(pkt + 8, "A6L-data3-ping", 14);
		ic->checksum = csum(pkt, sizeof(pkt));
		t0 = now_ms();
		if (sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
			printf("A6L_NET_PING seq %d sendto: %s\n", i + 1, strerror(errno));
			sleep(1);
			continue;
		}
		sent++;
		deadline = t0 + wait_s * 1000.0;
		for (;;) {
			struct pollfd pfd = { .fd = fd, .events = POLLIN };
			unsigned char rb[1500];
			socklen_t fl = sizeof(from);
			int left = (int)(deadline - now_ms()), n;
			struct iphdr *ip;
			struct icmphdr *rc;

			if (left <= 0) {
				printf("A6L_NET_PING seq %d: no reply in %d s\n", i + 1, wait_s);
				break;
			}
			if (poll(&pfd, 1, left) <= 0)
				continue;
			n = recvfrom(fd, rb, sizeof(rb), 0, (struct sockaddr *)&from, &fl);
			if (n < (int)sizeof(struct iphdr))
				continue;
			ip = (struct iphdr *)rb;
			if (n < ip->ihl * 4 + 8)
				continue;
			rc = (struct icmphdr *)(rb + ip->ihl * 4);
			if (rc->type == ICMP_ECHOREPLY && ntohs(rc->un.echo.id) == id &&
			    ntohs(rc->un.echo.sequence) == i + 1) {
				char fb[INET_ADDRSTRLEN];

				inet_ntop(AF_INET, &from.sin_addr, fb, sizeof(fb));
				printf("A6L_NET_PING reply from %s seq %d ttl %d time %.1f ms\n", fb, i + 1, ip->ttl,
				       now_ms() - t0);
				got++;
				break;
			} else if (rc->type != ICMP_ECHO) {
				char fb[INET_ADDRSTRLEN];

				inet_ntop(AF_INET, &from.sin_addr, fb, sizeof(fb));
				printf("A6L_NET_PING icmp type %d code %d from %s\n", rc->type, rc->code, fb);
			}
		}
		if (i + 1 < count)
			usleep(500000);
	}
	close(fd);
	printf("A6L_NET_PING_%s %s %d/%d replies (sent %d)\n", got ? "PASS" : "FAIL", dst, got, count, sent);
	return got ? 0 : 1;
}

static int cmd_dns(const char *ifname, const char *server, const char *name, int wait_s)
{
	unsigned char q[512], r[1500];
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(53) };
	struct pollfd pfd;
	int fd, n, qn = 12, an, i, off, found = 0;
	const char *p = name;
	uint16_t id = (uint16_t)(getpid() ^ 0xa6);

	if (inet_pton(AF_INET, server, &to.sin_addr) != 1)
		return 2;
	memset(q, 0, sizeof(q));
	q[0] = id >> 8; q[1] = id & 0xff; q[2] = 0x01; /* RD */ q[5] = 1; /* QDCOUNT */
	while (*p) {
		const char *dot = strchr(p, '.');
		int l = dot ? (int)(dot - p) : (int)strlen(p);

		if (l <= 0 || l > 63 || qn + l + 6 > (int)sizeof(q))
			return 2;
		q[qn++] = l;
		memcpy(q + qn, p, l);
		qn += l;
		p += l + (dot ? 1 : 0);
	}
	q[qn++] = 0;
	q[qn++] = 0; q[qn++] = 1; /* A */
	q[qn++] = 0; q[qn++] = 1; /* IN */
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0 || bind_dev(fd, ifname))
		return 2;
	if (sendto(fd, q, qn, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
		printf("A6L_NET_DNS_FAIL sendto %s: %s\n", server, strerror(errno));
		return 1;
	}
	pfd.fd = fd; pfd.events = POLLIN;
	if (poll(&pfd, 1, wait_s * 1000) <= 0) {
		printf("A6L_NET_DNS_FAIL no answer from %s in %d s\n", server, wait_s);
		return 1;
	}
	n = recv(fd, r, sizeof(r), 0);
	close(fd);
	if (n < 12 || r[0] != q[0] || r[1] != q[1]) {
		printf("A6L_NET_DNS_FAIL bad reply (%d bytes)\n", n);
		return 1;
	}
	an = (r[6] << 8) | r[7];
	off = qn; /* reply echoes the question */
	printf("A6L_NET_DNS reply rcode %d answers %d (%d bytes)\n", r[3] & 0xf, an, n);
	for (i = 0; i < an && off + 12 <= n; i++) {
		int type, rdlen;

		if ((r[off] & 0xc0) == 0xc0) {
			off += 2;
		} else {
			while (off < n && r[off])
				off += r[off] + 1;
			off++;
		}
		if (off + 10 > n)
			break;
		type = (r[off] << 8) | r[off + 1];
		rdlen = (r[off + 8] << 8) | r[off + 9];
		off += 10;
		if (type == 1 && rdlen == 4 && off + 4 <= n) {
			printf("A6L_NET_DNS %s A %u.%u.%u.%u\n", name, r[off], r[off + 1], r[off + 2], r[off + 3]);
			found++;
		}
		off += rdlen;
	}
	printf("A6L_NET_DNS_%s %s via %s (%d A records)\n", found ? "PASS" : "FAIL", name, server, found);
	return found ? 0 : 1;
}

static int cmd_http(const char *ifname, const char *ip, int port, const char *host)
{
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port) };
	struct pollfd pfd;
	char req[256], rsp[512];
	int fd, fl, n, err = 0;
	socklen_t el = sizeof(err);
	double t0 = now_ms();

	if (inet_pton(AF_INET, ip, &to.sin_addr) != 1)
		return 2;
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0 || bind_dev(fd, ifname))
		return 2;
	fl = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	if (connect(fd, (struct sockaddr *)&to, sizeof(to)) < 0 && errno != EINPROGRESS) {
		printf("A6L_NET_HTTP_FAIL connect %s:%d: %s\n", ip, port, strerror(errno));
		return 1;
	}
	pfd.fd = fd; pfd.events = POLLOUT;
	if (poll(&pfd, 1, 8000) <= 0) {
		printf("A6L_NET_HTTP_FAIL connect %s:%d: timeout (no SYN-ACK)\n", ip, port);
		return 1;
	}
	getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
	if (err) {
		printf("A6L_NET_HTTP_FAIL connect %s:%d: %s\n", ip, port, strerror(err));
		return 1;
	}
	printf("A6L_NET_HTTP connected %s:%d in %.0f ms\n", ip, port, now_ms() - t0);
	fcntl(fd, F_SETFL, fl);
	n = snprintf(req, sizeof(req), "HEAD / HTTP/1.0\r\nHost: %s\r\nUser-Agent: a6l-net\r\n\r\n", host);
	if (send(fd, req, n, 0) != n) {
		printf("A6L_NET_HTTP_FAIL send: %s\n", strerror(errno));
		return 1;
	}
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 8000) <= 0 || (n = recv(fd, rsp, sizeof(rsp) - 1, 0)) <= 0) {
		printf("A6L_NET_HTTP_FAIL no response\n");
		return 1;
	}
	rsp[n] = 0;
	rsp[strcspn(rsp, "\r\n")] = 0;
	close(fd);
	printf("A6L_NET_HTTP_PASS %s:%d \"%s\"\n", ip, port, rsp);
	return 0;
}

static long rd(const char *ifname, const char *f)
{
	char p[128], b[32] = "";
	int fd, n;

	snprintf(p, sizeof(p), "/sys/class/net/%s/statistics/%s", ifname, f);
	fd = open(p, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, b, sizeof(b) - 1);
	close(fd);
	b[n > 0 ? n : 0] = 0;
	return atol(b);
}

static int cmd_stats(int argc, char **argv)
{
	for (int i = 0; i < argc; i++)
		printf("A6L_NET_STATS %s rx %ld tx %ld rx_drop %ld tx_drop %ld rx_err %ld tx_err %ld rx_bytes %ld tx_bytes %ld\n",
		       argv[i], rd(argv[i], "rx_packets"), rd(argv[i], "tx_packets"), rd(argv[i], "rx_dropped"),
		       rd(argv[i], "tx_dropped"), rd(argv[i], "rx_errors"), rd(argv[i], "tx_errors"),
		       rd(argv[i], "rx_bytes"), rd(argv[i], "tx_bytes"));
	return 0;
}

static int usage(void)
{
	fprintf(stderr, "usage: a6l-net addr IF A.B.C.D[/len] | route A.B.C.D[/len] IF [GW] | ping IF DST [count] [wait]\n"
			"       | dns IF SERVER NAME [wait] | http IF IP [port] [host] | stats IF...\n");
	return 2;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2)
		return usage();
	if (!strcmp(argv[1], "addr") && argc == 4)
		return cmd_addr(argv[2], argv[3]);
	if (!strcmp(argv[1], "route") && (argc == 4 || argc == 5))
		return cmd_route(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
	if (!strcmp(argv[1], "ping") && argc >= 4)
		return cmd_ping(argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 4, argc > 5 ? atoi(argv[5]) : 3);
	if (!strcmp(argv[1], "dns") && argc >= 5)
		return cmd_dns(argv[2], argv[3], argv[4], argc > 5 ? atoi(argv[5]) : 5);
	if (!strcmp(argv[1], "http") && argc >= 4)
		return cmd_http(argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 80, argc > 5 ? argv[5] : argv[3]);
	if (!strcmp(argv[1], "stats") && argc >= 3)
		return cmd_stats(argc - 2, argv + 2);
	return usage();
}
