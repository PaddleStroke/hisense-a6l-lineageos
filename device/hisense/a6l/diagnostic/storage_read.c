/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded, fixed-range direct reads. Never mounts or writes block devices. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifdef A6L_STORAGE_HOST_TEST
#include "storage_read_fixture.h"
#else
#define READ_DEVICE "/dev/mmcblk1"
#define SYS_BLOCK "/sys/class/block/mmcblk1"
#endif

struct read_region {
    const char *name;
    uint64_t offset, bytes;
    const char *sha256;
};
#include "storage_read_ranges.h"

int a6l_storage_hash_selftest(void)
{
    static const unsigned char expected[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    unsigned char digest[32];
    return SHA256((const unsigned char *)"abc", 3, digest) &&
           memcmp(digest, expected, sizeof(digest)) == 0 ? 0 : -1;
}

static int read_attribute(const char *path, char *buffer, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buffer, size - 1);
    int saved = errno;
    close(fd);
    errno = saved;
    if (n <= 0 || n == (ssize_t)size - 1)
        return -1;
    while (n > 0 && (buffer[n - 1] == '\n' || buffer[n - 1] == '\r'))
        --n;
    buffer[n] = 0;
    return 0;
}

int a6l_storage_verify(void (*emit)(const char *, ...))
{
    int fd = -1, result = 1;
    void *buffer = NULL;
    struct utsname kernel;
    struct stat st;
    char value[256], resolved[PATH_MAX];
    uint64_t bytes = 0, total = 0;
    int sector = 0;
    struct timespec start, end;
    alarm(25); /* Only this forked reader exits on timeout; PID1 keeps logging. */
    emit("A6L_STORAGE_READ_BEGIN passes=2 direct=1 timeout=25\n");
    if (a6l_storage_hash_selftest()) {
        emit("A6L_STORAGE_READ_FAIL gate=hash-selftest\n");
        goto out;
    }
    if (uname(&kernel) || strcmp(kernel.release, "7.2.3-a6l-probe+") ||
        strcmp(kernel.machine, "aarch64")) {
        emit("A6L_STORAGE_READ_FAIL gate=kernel\n");
        goto out;
    }
    if (read_attribute(SYS_BLOCK "/device/name", value, sizeof(value)) ||
        strcmp(value, "hDEaP3")) {
        emit("A6L_STORAGE_READ_FAIL gate=product\n");
        goto out;
    }
    if (read_attribute(SYS_BLOCK "/device/manfid", value, sizeof(value)) ||
        strcmp(value, "0x000090")) {
        emit("A6L_STORAGE_READ_FAIL gate=manufacturer\n");
        goto out;
    }
    if (read_attribute(SYS_BLOCK "/dev", value, sizeof(value)) || strcmp(value, "179:0") ||
        !realpath(SYS_BLOCK "/device", resolved) ||
        !strstr(resolved, "/c0c4000.mmc/") || !strstr(resolved, "/mmc1:0001")) {
        emit("A6L_STORAGE_READ_FAIL gate=controller\n");
        goto out;
    }
    /* No fallback to buffered I/O or a different device. */
    fd = open(READ_DEVICE, O_RDONLY | O_DIRECT | O_CLOEXEC | O_NOFOLLOW | O_EXCL);
    if (fd < 0) {
        emit("A6L_STORAGE_READ_FAIL gate=open errno=%d\n", errno);
        goto out;
    }
    if (fstat(fd, &st)) {
        emit("A6L_STORAGE_READ_FAIL gate=stat errno=%d\n", errno);
        goto out;
    }
#ifdef A6L_STORAGE_HOST_TEST
    /* Host fixtures are sparse regular files; production never accepts them. */
    if (!S_ISREG(st.st_mode))
        goto out;
    bytes = st.st_size;
    sector = 512;
#else
    if (!S_ISBLK(st.st_mode) || major(st.st_rdev) != 179 || minor(st.st_rdev) != 0 ||
        ioctl(fd, BLKGETSIZE64, &bytes) || ioctl(fd, BLKSSZGET, &sector)) {
        emit("A6L_STORAGE_READ_FAIL gate=block-geometry errno=%d\n", errno);
        goto out;
    }
#endif
    if (bytes != A6L_READ_DISK_BYTES || sector != 512) {
        emit("A6L_STORAGE_READ_FAIL gate=capacity\n");
        goto out;
    }
    if (posix_memalign(&buffer, 4096, 131072)) {
        emit("A6L_STORAGE_READ_FAIL gate=allocation\n");
        goto out;
    }
    clock_gettime(CLOCK_MONOTONIC, &start);
    emit("A6L_STORAGE_READ_IDENTITY_OK product=hDEaP3 bytes=%llu sector=%d\n",
         (unsigned long long)bytes, sector);
    for (unsigned int pass = 0; pass < 2; ++pass) {
        const size_t chunk = pass == 0 ? 131072 : 65536;
        for (size_t i = 0; i < sizeof(read_regions) / sizeof(read_regions[0]); ++i) {
            const struct read_region *r = &read_regions[i];
            SHA256_CTX hash;
            unsigned char digest[SHA256_DIGEST_LENGTH];
            char hex[SHA256_DIGEST_LENGTH * 2 + 1];
            if (r->offset % 512 || r->bytes % 512 || !r->bytes ||
                r->offset > bytes || r->bytes > bytes - r->offset || !SHA256_Init(&hash))
                goto out;
            emit("A6L_STORAGE_READ_REGION_BEGIN pass=%u name=%s bytes=%llu chunk=%zu\n",
                 pass + 1, r->name, (unsigned long long)r->bytes, chunk);
            for (uint64_t done = 0; done < r->bytes;) {
                size_t wanted = r->bytes - done < chunk ? (size_t)(r->bytes - done) : chunk;
                ssize_t n = pread(fd, buffer, wanted, (off_t)(r->offset + done));
                if (n != (ssize_t)wanted) {
                    emit("A6L_STORAGE_READ_FAIL gate=read pass=%u name=%s offset=%llu got=%lld errno=%d\n",
                         pass + 1, r->name, (unsigned long long)(r->offset + done),
                         (long long)n, n < 0 ? errno : 0);
                    goto out;
                }
                if (!SHA256_Update(&hash, buffer, wanted))
                    goto out;
                done += wanted;
                total += wanted;
            }
            if (!SHA256_Final(digest, &hash))
                goto out;
            for (size_t k = 0; k < sizeof(digest); ++k)
                snprintf(hex + k * 2, 3, "%02x", digest[k]);
            int match = strcmp(hex, r->sha256) == 0;
            emit("A6L_STORAGE_READ_HASH pass=%u name=%s sha256=%s match=%d\n",
                 pass + 1, r->name, hex, match);
            if (!match)
                goto out;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    emit("A6L_STORAGE_READ_PASS regions=5 passes=2 bytes=%llu duration_ms=%lld\n",
         (unsigned long long)total,
         (long long)(end.tv_sec - start.tv_sec) * 1000 +
         (long long)(end.tv_nsec - start.tv_nsec) / 1000000);
    result = 0;
out:
    if (fd >= 0)
        close(fd);
    free(buffer);
    alarm(0);
    if (result)
        emit("A6L_STORAGE_READ_FAILED\n");
    return result;
}
