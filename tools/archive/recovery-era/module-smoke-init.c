/* Diskless emulator harness, never packaged into a phone image. */
static long call(long n, long a, long b, long c, long d, long e)
{
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
    return x0;
}
static void say(long fd, const char *s)
{
    long n = 0;
    while (s[n]) ++n;
    call(64, fd, (long)s, n, 0, 0);
}
void _start(void)
{
    call(40, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0);
    long console = call(56, -100, (long)"/dev/console", 1, 0, 0);
    long fd = call(56, -100, (long)"/sdhci-msm.ko", 0, 0, 0);
    long result = fd < 0 ? fd : call(273, fd, (long)"", 0, 0, 0);
    say(console, result == 0 ? "A6L_MODULE_SMOKE_PASS\n" : "A6L_MODULE_SMOKE_FAIL\n");
    if (fd >= 0) call(57, fd, 0, 0, 0, 0);
    for (;;) {
        long delay[2] = {1, 0};
        call(101, (long)delay, 0, 0, 0, 0);
    }
}
