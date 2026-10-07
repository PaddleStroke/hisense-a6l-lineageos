// SPDX-License-Identifier: Apache-2.0
// a6l-krec: synchronous kernel-log recorder on a raw eMMC partition (restart hang, 7 Oct 2026,
// firmware/extracted/restart-hang-20261007/README.md). Every batch of /dev/kmsg records is on flash (O_DIRECT|O_DSYNC,
// i.e. FUA/flush) before the next read, so a hard hang loses at most the records the kernel printed after the last
// write completed (typically < 2 ms of log). Decode with tools/a6l-krec-decode.py (pulled from recovery by
// tools/a6l-krec-pull.sh).
//
// Partition layout (reserve2, GPT 57, 1 GiB, all zero in stock, unused by stock and by this ROM):
//   [0, 4 KiB)            superblock: magic "A6LKREC1", geometry, boot counter (CRC32)
//   slot i (0..15)        at 1 MiB + i * 16 MiB; one slot per recorder run (boot or shutdown), slot = run % 16
//     block 0             slot header: magic "A6LKSLT1", run number, mode, boot_id, cmdline, start/end state (CRC32)
//     blocks 1..4095      data blocks: "A6LB" header (run, block seq, used bytes, kmsg seq range, CRC32) + text
// The current data block is rewritten in place as it fills; a full block is never rewritten. Only reserve2 is ever
// written: the device must resolve to a partition whose sysfs PARTNAME is "reserve2" (--test skips this check, host
// loop-device tests only) and the superblock area must be all zero (first use) or carry our magic.
//
// Stops: /dev/a6l-krec.stop appears (init writes it at sys.boot_completed=1; content = seconds to keep recording,
// default 60), the slot is full (16 MiB), the write budget is spent (96 MiB incl. rewrites) or uptime > 900 s
// (boot mode) / never (shutdown mode, it records until the reset or until eMMC writes fail).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BS 4096u
#define SB_AREA (1u << 20)
#define NSLOT 16u
#define SLOT_BYTES (16u << 20)
#define SLOT_BLOCKS (SLOT_BYTES / BS)
#define DHDR 48u
#define PAYLOAD (BS - DHDR)
#define BUDGET (96ull << 20)
#define VERSION 1u

static uint32_t crctab[256];
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crctab[i] = c;
    }
}
static uint32_t crc32b(const unsigned char *p, size_t n) {
    uint32_t c = 0xffffffffu;
    while (n--) c = crctab[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}
static void put16(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static void put64(unsigned char *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static uint32_t get32(const unsigned char *p) {
    uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i); return v;
}
static uint64_t get64(const unsigned char *p) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v;
}

static int dev = -1, kfd = -1;
static unsigned char *blk, *hdr;            // aligned 4 KiB buffers: current data block, slot header
static uint64_t run, slot_off, written, kseq_first, kseq_last, last_kseq_seen;
static uint32_t bseq = 1, used, nrec;
static int have_rec;
static char mode[16] = "boot";

static void klog(const char *fmt, const char *a, unsigned long long b) {
    char m[256];
    int n = snprintf(m, sizeof m, "<6>a6l_krec: ");
    snprintf(m + n, sizeof m - (size_t)n, fmt, a, b);
    int f = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (f >= 0) { if (write(f, m, strlen(m)) < 0) {} close(f); }
}
static double uptime(void) {
    struct timespec t; clock_gettime(CLOCK_BOOTTIME, &t); return (double)t.tv_sec + t.tv_nsec / 1e9;
}
static int pw(const void *buf, uint64_t off) {
    ssize_t r = pwrite(dev, buf, BS, (off_t)off);
    if (r != (ssize_t)BS) return -1;
    written += BS;
    return 0;
}
static int write_block(void) {
    memcpy(blk, "A6LB", 4);
    put64(blk + 8, run); put32(blk + 16, bseq); put16(blk + 20, used); put16(blk + 22, 0);
    put32(blk + 24, nrec); put64(blk + 28, kseq_first); put64(blk + 36, kseq_last); put32(blk + 44, 0);
    put32(blk + 4, 0);
    put32(blk + 4, crc32b(blk, DHDR + used));
    return pw(blk, slot_off + (uint64_t)bseq * BS);
}
static void new_block(void) {
    memset(blk, 0, BS); used = 0; nrec = 0; have_rec = 0; kseq_first = kseq_last = 0;
}
// slot header: state 1 = running, 2 = stopped cleanly; reason text
static int write_header(uint32_t state, const char *reason) {
    // header layout: 0 magic, 8 crc, 12 version, 16 run, 24 mode[16], 40 state, 44 last block seq, 48 last kmsg seq,
    // 56 bytes written, 64 uptime us at this write, 72 start wall time, 80 start uptime us, 128 boot_id, 256 reason,
    // 512 cmdline
    put32(hdr + 40, state); put32(hdr + 44, bseq); put64(hdr + 48, last_kseq_seen);
    put64(hdr + 56, written); put64(hdr + 64, (uint64_t)(uptime() * 1e6));
    memset(hdr + 256, 0, 64); snprintf((char *)hdr + 256, 64, "%s", reason);
    put32(hdr + 8, 0); put32(hdr + 8, crc32b(hdr, BS));
    return pw(hdr, slot_off);
}

// append one text line (already formatted) to the current block, flushing full blocks
static int append(const char *s, size_t n, uint64_t kseq) {
    if (n > PAYLOAD) n = PAYLOAD;
    if (used + n > PAYLOAD) {
        if (write_block()) return -1;
        if (++bseq >= SLOT_BLOCKS) return 1;
        new_block();
    }
    memcpy(blk + DHDR + used, s, n); used += (uint32_t)n; nrec++;
    if (!have_rec) { kseq_first = kseq; have_rec = 1; }
    kseq_last = kseq;
    return 0;
}

// /dev/kmsg record "pri,seq,ts_us,flags[,..];text\n[ KEY=val\n...]" -> "<pri>[  ts] text\n"
static int record(const char *r, ssize_t n) {
    char line[PAYLOAD + 64];
    unsigned long long pri = 0, seq = 0, ts = 0;
    const char *semi = memchr(r, ';', (size_t)n);
    if (!semi || sscanf(r, "%llu,%llu,%llu", &pri, &seq, &ts) != 3) return 0;
    if (last_kseq_seen && seq > last_kseq_seen + 1) {
        int g = snprintf(line, sizeof line, "<4>[ a6l_krec ] *** %llu kmsg records lost (overwritten) ***\n",
                         seq - last_kseq_seen - 1);
        int e = append(line, (size_t)g, seq);
        if (e) return e;
    }
    last_kseq_seen = seq;
    const char *t = semi + 1, *end = memchr(t, '\n', (size_t)(r + n - t));
    if (!end) end = r + n;
    size_t tl = (size_t)(end - t);
    int h = snprintf(line, sizeof line, "<%llu>[%5llu.%06llu] ", pri & 7, ts / 1000000, ts % 1000000);
    if (tl > sizeof line - (size_t)h - 2) tl = sizeof line - (size_t)h - 2;
    memcpy(line + h, t, tl); line[h + tl] = '\n';
    return append(line, (size_t)h + tl + 1, seq);
}

static int check_target(const char *path, int test) {
    char rp[4096], ue[4096], sys[4200];
    if (!realpath(path, rp)) { klog("cannot resolve %s (%llu)", path, (unsigned long long)errno); return -1; }
    if (!test) {
        const char *b = strrchr(rp, '/'); b = b ? b + 1 : rp;
        snprintf(sys, sizeof sys, "/sys/class/block/%s/uevent", b);
        int f = open(sys, O_RDONLY | O_CLOEXEC);
        ssize_t n = f >= 0 ? read(f, ue, sizeof ue - 1) : -1;
        if (f >= 0) close(f);
        if (n <= 0) { klog("no sysfs uevent for %s (%llu)", rp, 0); return -1; }
        ue[n] = 0;
        if (!strstr(ue, "PARTNAME=reserve2\n")) { klog("REFUSED: %s is not PARTNAME=reserve2 (%llu)", rp, 0); return -1; }
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *path = "/dev/block/by-name/reserve2";
    int test = 0, seek_end = 0;
    double max_up = 900;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) snprintf(mode, sizeof mode, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--dev") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--test")) test = 1;
        else if (!strcmp(argv[i], "--seek-end")) seek_end = 1;
        else if (!strcmp(argv[i], "--max-uptime") && i + 1 < argc) max_up = atof(argv[++i]);
        else { fprintf(stderr, "usage: %s [--mode boot|shutdown] [--dev PATH] [--test] [--seek-end] [--max-uptime S]\n", argv[0]); return 2; }
    }
    int shutdown_mode = !strcmp(mode, "shutdown");
    if (shutdown_mode) max_up = 1e12;
    signal(SIGTERM, SIG_IGN); signal(SIGHUP, SIG_IGN); signal(SIGINT, test ? SIG_DFL : SIG_IGN); signal(SIGPIPE, SIG_IGN);
    crc_init();
    // second-stage ueventd creates /dev/block/by-name during coldboot; wait for it (bounded, 10 s)
    for (int i = 0; i < 100 && access(path, F_OK); i++) usleep(100000);
    if (check_target(path, test)) return 3;
    dev = open(path, O_RDWR | O_DIRECT | O_DSYNC | O_CLOEXEC);
    if (dev < 0) { klog("open %s failed (errno %llu)", path, (unsigned long long)errno); return 3; }
    uint64_t size = 0;
    if (ioctl(dev, BLKGETSIZE64, &size) || size < SB_AREA + (uint64_t)NSLOT * SLOT_BYTES) {
        struct stat st;   // regular file (host tests)
        if (!test || fstat(dev, &st) || (uint64_t)st.st_size < SB_AREA + (uint64_t)NSLOT * SLOT_BYTES) {
            klog("REFUSED: %s too small (%llu bytes)", path, (unsigned long long)size); return 3;
        }
    }
    if (posix_memalign((void **)&blk, BS, BS) || posix_memalign((void **)&hdr, BS, BS)) return 4;
    mlockall(MCL_CURRENT | MCL_FUTURE);
    struct sched_param sp = { .sched_priority = 10 };
    sched_setscheduler(0, SCHED_FIFO, &sp);

    // superblock: format on first use (all zero), refuse anything foreign, bump the run counter
    flock(dev, LOCK_EX);
    unsigned char *sb = hdr;
    if (pread(dev, sb, BS, 0) != (ssize_t)BS) { klog("superblock read failed (%s %llu)", path, 0); return 5; }
    if (memcmp(sb, "A6LKREC1", 8)) {
        for (unsigned i = 0; i < BS; i++) if (sb[i]) { klog("REFUSED: %s superblock not empty and not ours (%llu)", path, 0); return 5; }
        run = 0;
    } else {
        uint32_t c = get32(sb + 12); put32(sb + 12, 0);
        (void)c;
        run = get64(sb + 32);   // counter kept even if a torn write broke the CRC
    }
    run++;
    memset(sb, 0, BS);
    memcpy(sb, "A6LKREC1", 8); put32(sb + 8, VERSION);
    put32(sb + 16, BS); put32(sb + 20, SB_AREA); put32(sb + 24, NSLOT); put32(sb + 28, SLOT_BYTES); put64(sb + 32, run);
    snprintf((char *)sb + 64, 192, "A6L kmsg recorder ring on reserve2; decoder: firmware/extracted/restart-hang-20261007/krec/tools\n");
    put32(sb + 12, crc32b(sb, BS));
    if (pw(sb, 0)) { klog("superblock write failed (%s %llu)", path, (unsigned long long)errno); return 5; }
    flock(dev, LOCK_UN);

    slot_off = SB_AREA + (uint64_t)((run - 1) % NSLOT) * SLOT_BYTES;
    // slot header
    memset(hdr, 0, BS);
    memcpy(hdr, "A6LKSLT1", 8); put32(hdr + 12, VERSION); put64(hdr + 16, run);
    snprintf((char *)hdr + 24, 16, "%s", mode);
    { int f = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC); if (f >= 0) { if (read(f, hdr + 128, 36) < 0) {} close(f); } }
    put64(hdr + 72, (uint64_t)time(NULL)); put64(hdr + 80, (uint64_t)(uptime() * 1e6));
    { int f = open("/proc/cmdline", O_RDONLY | O_CLOEXEC); if (f >= 0) { if (read(f, hdr + 512, 2048) < 0) {} close(f); } }
    // zero the first data block of this slot so a stale block 1 from 16 runs ago is not taken as ours (run checked too)
    new_block();
    kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (kfd < 0) { klog("cannot open /dev/kmsg (%s %llu)", path, (unsigned long long)errno); return 6; }
    if (shutdown_mode || seek_end) lseek(kfd, 0, SEEK_END);
    if (write_header(1, "running") || write_block()) { klog("first write failed (%s %llu)", path, (unsigned long long)errno); return 7; }
    { char m[96]; snprintf(m, sizeof m, "run %llu slot %llu mode %s", (unsigned long long)run,
                           (unsigned long long)((run - 1) % NSLOT), mode);
      klog("recording %s to reserve2, first write synced (%llu)", m, 0);
      int f = open("/dev/a6l-krec.ready", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
      if (f >= 0) { if (write(f, m, strlen(m)) < 0) {} close(f); } }

    char rec[8192];
    const char *why = "?";
    double stop_at = 0;
    for (;;) {
        int dirty = 0, e = 0;
        for (;;) {
            ssize_t n = read(kfd, rec, sizeof rec - 1);
            if (n < 0 && errno == EPIPE) continue;           // overwritten; the seq gap is reported
            if (n <= 0) break;
            if ((e = record(rec, n))) break;
            dirty = 1;
        }
        if (e < 0) { why = "eMMC write failed"; break; }
        if (e > 0) { why = "slot full"; break; }
        if (dirty && write_block()) { why = "eMMC write failed"; break; }
        if (written > BUDGET) { why = "write budget spent"; break; }
        double now = uptime();
        if (now > max_up) { why = "max uptime"; break; }
        if (!stop_at && !shutdown_mode) {
            int f = open("/dev/a6l-krec.stop", O_RDONLY | O_CLOEXEC);
            if (f >= 0) {
                char b[32] = {0}; ssize_t n = read(f, b, sizeof b - 1); close(f);
                for (char *q = b; *q; q++) if (*q == '\n' || *q == '\r') *q = 0;
                double d = n > 0 ? atof(b) : 60; if (d <= 0) d = 60;
                stop_at = now + d;
                klog("stop requested, recording %s more seconds (%llu)", n > 0 ? b : "60", 0);
            }
        }
        if (stop_at && now >= stop_at) { why = "stop file (boot_completed + delay)"; break; }
        struct pollfd p = { .fd = kfd, .events = POLLIN };
        poll(&p, 1, 1000);
    }
    if (strcmp(why, "eMMC write failed")) {
        if (used) write_block();
        write_header(2, why);
    }
    klog("stopped: %s, %llu bytes written", why, (unsigned long long)written);
    return 0;
}
