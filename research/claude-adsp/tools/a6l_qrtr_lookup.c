// SPDX-License-Identifier: GPL-2.0-only
/*
 * a6l_qrtr_lookup: minimal, read-only QRTR service enumerator for the A6L
 * diagnostic RAM userspace (no libqrtr, no dependencies).
 *
 * Mirrors the control-port lookup used by linux-msm/qrtr `qrtr-lookup`
 * (https://github.com/linux-msm/qrtr, src/lookup.c): send QRTR_TYPE_NEW_LOOKUP
 * (service 0 / instance 0 = everything) to the local node's control port and
 * print every QRTR_TYPE_NEW_SERVER answer until the empty terminator or a
 * timeout. It never sends anything to a remote service, so it cannot start
 * audio, sensors or the modem.
 *
 * Kernel ABI: include/uapi/linux/qrtr.h (AF_QIPCRTR = 42).
 * Requires qrtr.ko (and qrtr-smd.ko for the ADSP "IPCRTR" glink channel).
 *
 * Build (coordinator, WSL, same static AArch64 toolchain as a6l_haptic_probe):
 *   aarch64-linux-gnu-gcc -static -O2 -Wall -o a6l_qrtr_lookup a6l_qrtr_lookup.c
 * Syntax-checked here only with: clang --target=aarch64-linux-gnu -fsyntax-only
 *
 * Usage: a6l_qrtr_lookup [timeout_ms]     (default 2000)
 * Output: one line per service: "node <n> port <p> service <s> version <v> instance <i>"
 * Exit: 0 printed >= 1 service, 2 none, 1 error.
 */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif
#define QRTR_NODE_BCAST 0xffffffffu
#define QRTR_PORT_CTRL  0xfffffffeu

enum { QRTR_TYPE_NEW_SERVER = 4, QRTR_TYPE_DEL_SERVER = 5, QRTR_TYPE_NEW_LOOKUP = 10 };

struct sockaddr_qrtr { unsigned short sq_family; uint32_t sq_node; uint32_t sq_port; };
struct qrtr_ctrl_pkt {
	uint32_t cmd;
	union {
		struct { uint32_t service, instance, node, port; } server;
		struct { uint32_t node, port; } client;
	};
} __attribute__((packed));

int main(int argc, char **argv)
{
	int timeout_ms = argc > 1 ? atoi(argv[1]) : 2000;
	struct sockaddr_qrtr sq = { AF_QIPCRTR, 0, 0 }, from;
	struct qrtr_ctrl_pkt pkt;
	socklen_t sl = sizeof(sq);
	int fd, n = 0;

	fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (fd < 0) { fprintf(stderr, "socket(AF_QIPCRTR): %s (qrtr.ko loaded?)\n", strerror(errno)); return 1; }
	if (getsockname(fd, (struct sockaddr *)&sq, &sl)) { perror("getsockname"); return 1; }
	printf("local node %u port %u\n", sq.sq_node, sq.sq_port);

	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = QRTR_TYPE_NEW_LOOKUP;	/* service 0, instance 0: all */
	sq.sq_port = QRTR_PORT_CTRL;	/* local control port */
	if (sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&sq, sizeof(sq)) < 0) { perror("sendto ctrl"); return 1; }

	for (;;) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		int r = poll(&pfd, 1, timeout_ms);
		if (r <= 0) { if (r < 0) perror("poll"); else fprintf(stderr, "lookup timeout after %d ms\n", timeout_ms); break; }
		sl = sizeof(from);
		r = recvfrom(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &sl);
		if (r < 0) { perror("recvfrom"); break; }
		if ((size_t)r < sizeof(uint32_t)) continue;
		if (pkt.cmd == QRTR_TYPE_NEW_SERVER) {
			if (!pkt.server.service && !pkt.server.instance && !pkt.server.node && !pkt.server.port)
				break;	/* end of lookup list */
			printf("node %u port %u service %u version %u instance %u\n",
			       pkt.server.node, pkt.server.port, pkt.server.service,
			       pkt.server.instance & 0xff, pkt.server.instance >> 8);
			n++;
		}
	}
	close(fd);
	return n ? 0 : 2;
}
