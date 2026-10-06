// SPDX-License-Identifier: Apache-2.0
// Fixed allocation, immutable boot head and cyclic tail. Each slot is separately
// checksummed: an interrupted overwrite is discarded, never presented as a valid
// old/new mixed record. Decode with tools/decode-a6l-log-ring.py.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { HEADER = 32, SLOT_HEADER = 24, CHUNK = 32768, HEAD = 4, TAIL = 16,
       SLOTS = HEAD + TAIL, SLOT = SLOT_HEADER + CHUNK,
       FILE_SIZE = HEADER + SLOTS * SLOT };
static volatile sig_atomic_t stopping;
static void stop(int sig) { (void)sig; stopping = 1; }
static void put32(unsigned char *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i));
}
static void put64(unsigned char *p, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8 * i));
}
static uint32_t get32(const unsigned char *p) {
    uint32_t v = 0; for (unsigned i = 0; i < 4; ++i) v |= (uint32_t)p[i] << (8 * i); return v;
}
static uint64_t get64(const unsigned char *p) {
    uint64_t v = 0; for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i); return v;
}
static uint32_t crc32(const unsigned char *p, size_t n, uint32_t c) {
    while (n--) {
        c ^= *p++;
        for (unsigned i = 0; i < 8; ++i) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return c;
}
static int io_all(int fd, void *buf, size_t n, off_t off, int writing) {
    unsigned char *p = buf;
    while (n) {
        ssize_t r = writing ? pwrite(fd, p, n, off) : pread(fd, p, n, off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { if (!r) errno = EIO; return -1; }
        p += r; n -= (size_t)r; off += r;
    }
    return 0;
}
static unsigned slot_for(uint64_t seq) {
    return seq < HEAD ? (unsigned)seq : HEAD + (unsigned)((seq - HEAD) % TAIL);
}
static int append(int fd, uint64_t seq, unsigned char *data, size_t n) {
    unsigned char h[SLOT_HEADER] = {0};
    put64(h, seq); put32(h + 8, (uint32_t)n);
    uint32_t crc = crc32(data, n, crc32(h, 12, 0xffffffffu)) ^ 0xffffffffu;
    put32(h + 12, crc); memcpy(h + 16, "A6LSLOT1", 8);
    off_t off = HEADER + (off_t)slot_for(seq) * SLOT;
    if (io_all(fd, data, n, off + SLOT_HEADER, 1)) return -1;
    return io_all(fd, h, sizeof h, off, 1);
}
static int64_t milliseconds(void) {
    struct timespec t; if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: a6l-log-ring FILE.ring < stream\n"); return 2; }
    int fd = open(argv[1], O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { perror("ring open"); return 1; }
    struct stat st;
    unsigned char header[HEADER] = {0}, expected[HEADER] = {0}, data[CHUNK], h[SLOT_HEADER];
    memcpy(expected, "A6LRING1", 8); put32(expected + 8, CHUNK);
    put32(expected + 12, HEAD); put32(expected + 16, TAIL);
    uint64_t seq = 0;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { fprintf(stderr, "ring requires regular file\n"); goto fail; }
    if (st.st_size) {
        if (st.st_size != FILE_SIZE || io_all(fd, header, sizeof header, 0, 0) ||
            memcmp(header, expected, sizeof header)) { fprintf(stderr, "ring format/size mismatch\n"); goto fail; }
        for (unsigned i = 0; i < SLOTS; ++i) {
            off_t off = HEADER + (off_t)i * SLOT;
            if (io_all(fd, h, sizeof h, off, 0)) goto fail;
            if (memcmp(h + 16, "A6LSLOT1", 8)) continue;
            uint64_t s = get64(h); uint32_t n = get32(h + 8);
            if (!n || n > CHUNK || s == UINT64_MAX || slot_for(s) != i) continue;
            if (io_all(fd, data, n, off + SLOT_HEADER, 0)) goto fail;
            uint32_t crc = crc32(data, n, crc32(h, 12, 0xffffffffu)) ^ 0xffffffffu;
            if (crc == get32(h + 12) && s >= seq) seq = s + 1;
        }
    } else {
        // Reserve every data block before consuming stdin: fail visibly and early
        // if Android's other metadata users have consumed the available space.
        int r = posix_fallocate(fd, 0, FILE_SIZE);
        if (r) { errno = r; perror("ring reserve"); goto fail; }
        if (io_all(fd, expected, sizeof expected, 0, 1) || fsync(fd)) goto fail;
    }
    struct sigaction sa = {0}; sa.sa_handler = stop; sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) || sigaction(SIGINT, &sa, NULL)) goto fail;
    size_t used = 0; int64_t deadline = milliseconds() + 1000;
    while (!stopping) {
        struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };
        int64_t now = milliseconds();
        int timeout = used ? (deadline <= now ? 0 : (int)(deadline - now)) : -1;
        int r = poll(&p, 1, timeout);
        if (r < 0) { if (errno == EINTR) continue; goto fail; }
        if (r && (p.revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(STDIN_FILENO, data + used, CHUNK - used);
            if (n < 0) { if (errno == EINTR) continue; goto fail; }
            if (!n) break;
            if (!used) deadline = milliseconds() + 1000;
            used += (size_t)n;
        } else if (r && (p.revents & (POLLERR | POLLNVAL))) { errno = EIO; goto fail; }
        if (used && (used == CHUNK || milliseconds() >= deadline)) {
            if (seq == UINT64_MAX || append(fd, seq++, data, used)) goto fail;
            used = 0;
        }
    }
    if (used && (seq == UINT64_MAX || append(fd, seq, data, used))) goto fail;
    if (fsync(fd)) goto fail;
    return close(fd) ? 1 : 0;
fail:
    perror("a6l-log-ring"); close(fd); return 1;
}
