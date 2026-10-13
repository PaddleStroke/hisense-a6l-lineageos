/* Read the fixed region exposed by the captured A6L epd_spi_read handler.
 * Standalone AArch64 Linux executable; no libc, ioctls or device writes.
 * The captured driver ignores count and copies 0x70080 bytes, then returns the
 * copy_to_user residual (zero on success). A normal small-buffer reader is
 * unsuitable. This program provides the full buffer and performs one read.
 */
#if !defined(__aarch64__)
#error Build only for AArch64 Linux
#endif

typedef unsigned long usize;
enum { REGION_SIZE = 0x70080 };
static unsigned char region[REGION_SIZE];

static long syscall3(long number, long a, long b, long c) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

static void diagnostic(const char *text, usize length) {
    syscall3(64, 2, (long)text, (long)length); /* write(stderr) */
}

__attribute__((noreturn)) static void finish(long status) {
    syscall3(93, status, 0, 0); /* exit */
    __builtin_unreachable();
}

__attribute__((noreturn)) void _start(void) {
    for (usize i = 0; i < REGION_SIZE; ++i)
        region[i] = 0xa5;

    /* openat(AT_FDCWD, fixed path, O_RDONLY). No O_CREAT or writable access. */
    long fd = syscall3(56, -100, (long)"/dev/epd_flash", 0);
    if (fd < 0) {
        static const char message[] = "Cannot open /dev/epd_flash read-only; no read attempted.\n";
        diagnostic(message, sizeof(message) - 1);
        finish(10);
    }
    long result = syscall3(63, fd, (long)region, REGION_SIZE); /* read */
    syscall3(57, fd, 0, 0); /* close */
    if (result != 0) {
        static const char message[] = "Unexpected driver read result; no payload emitted.\n";
        diagnostic(message, sizeof(message) - 1);
        finish(11);
    }
    usize changed = 0;
    for (usize i = 0; i < REGION_SIZE; ++i)
        changed += region[i] != 0xa5;
    if (!changed) {
        static const char message[] = "Read left the buffer untouched; no payload emitted.\n";
        diagnostic(message, sizeof(message) - 1);
        finish(12);
    }
    usize offset = 0;
    while (offset < REGION_SIZE) {
        long n = syscall3(64, 1, (long)(region + offset), REGION_SIZE - offset);
        if (n <= 0) finish(13);
        offset += (usize)n;
    }
    static const char message[] = "Fixed region copied; zero return matches the captured driver contract.\n";
    diagnostic(message, sizeof(message) - 1);
    finish(0);
}
