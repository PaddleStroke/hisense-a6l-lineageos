// SPDX-License-Identifier: Apache-2.0
/* a6l_lease_probe — checks the patched drm_hwcomposer lease server (eink3). NDK static; also for the realinit VM with a
 * second virtio-gpu output. usage: a6l_lease_probe <connector-id> [socket-name=a6l.hwc.lease] [--modeset SECONDS]
 * Prints the reply, the objects visible on the lessee fd, and with --modeset lights the connector with a grey dumb
 * buffer for N seconds (then closes the fd = lease ends). Exit 0 = lease granted (and modeset ok if asked). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <connector-id> [socket] [--modeset S]\n", argv[0]); return 2; }
    const char *name = argc > 2 && argv[2][0] != '-' ? argv[2] : "a6l.hwc.lease"; int hold = 0;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--modeset") && i + 1 < argc) hold = atoi(argv[i + 1]);
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0); struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
    size_t nl = strlen(name); memcpy(a.sun_path + 1, name, nl);
    if (connect(s, (struct sockaddr *)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nl))) { printf("A6L_LEASE_FAIL connect @%s: %s\n", name, strerror(errno)); return 1; }
    char req[48]; int n = snprintf(req, sizeof req, "LEASE %s", argv[1]); send(s, req, (size_t)n, 0);
    char rep[256] = {0}; union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } cb; memset(&cb, 0, sizeof cb);
    struct iovec iov = {rep, sizeof rep - 1}; struct msghdr m = {0}; m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = cb.b; m.msg_controllen = sizeof cb.b;
    struct pollfd p = {s, POLLIN, 0}; int fd = -1;
    if (poll(&p, 1, 3000) != 1 || recvmsg(s, &m, 0) <= 0) { printf("A6L_LEASE_FAIL no reply\n"); return 1; }
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) if (c->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(c), sizeof fd);
    printf("A6L_LEASE_REPLY %s fd=%d\n", rep, fd);
    if (strncmp(rep, "OK ", 3) || fd < 0) return 1;
    drmModeRes *r = drmModeGetResources(fd);
    printf("A6L_LEASE_LESSEE crtcs=%d connectors=%d master=%d\n", r ? r->count_crtcs : -1, r ? r->count_connectors : -1, drmIsMaster(fd));
    if (!r || r->count_crtcs != 1 || r->count_connectors != 1) { printf("A6L_LEASE_FAIL lessee should see exactly 1 crtc + 1 connector\n"); return 1; }
    if (hold > 0) {
        drmModeConnector *k = drmModeGetConnector(fd, r->connectors[0]); if (!k || !k->count_modes) { printf("A6L_LEASE_FAIL no mode\n"); return 1; }
        struct drm_mode_create_dumb c = {.width = k->modes[0].hdisplay, .height = k->modes[0].vdisplay, .bpp = 32}; uint32_t fb = 0;
        if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) { printf("A6L_LEASE_FAIL dumb: %s\n", strerror(errno)); return 1; }
        uint32_t hs[4] = {c.handle}, ps[4] = {c.pitch}, os[4] = {0};
        if (drmModeAddFB2(fd, c.width, c.height, DRM_FORMAT_XRGB8888, hs, ps, os, &fb, 0)) { printf("A6L_LEASE_FAIL addfb: %s\n", strerror(errno)); return 1; }
        struct drm_mode_map_dumb md = {.handle = c.handle}; drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md);
        void *px = mmap(0, c.size, PROT_WRITE, MAP_SHARED, fd, md.offset); if (px != MAP_FAILED) memset(px, 0x80, c.size);
        uint32_t cid = k->connector_id;
        if (drmModeSetCrtc(fd, r->crtcs[0], fb, 0, 0, &cid, 1, &k->modes[0])) { printf("A6L_LEASE_FAIL setcrtc: %s\n", strerror(errno)); return 1; }
        printf("A6L_LEASE_MODESET_OK %s %ux%u for %d s\n", k->modes[0].name, c.width, c.height, hold); fflush(stdout);
        sleep((unsigned)hold); drmModeSetCrtc(fd, r->crtcs[0], 0, 0, 0, NULL, 0, NULL);
    }
    printf("A6L_LEASE_PASS\n"); return 0;
}
