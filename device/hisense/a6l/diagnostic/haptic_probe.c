/* One bounded test for the pinned upstream spmi_haptics driver; not a HAL.
 * Its strong_magnitude >> 8 is scaled as a percentage, NOT divided by 255.
 * 8192 -> 32 -> 1229 mV requested, rounded to 1276 mV (stock limit 3200).
 * A normal 50% FF value would already saturate this driver at 3596 mV.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

enum { STRENGTH = 8192, LENGTH_MS = 100 };
static int event(int fd, unsigned short code, int value) {
    struct input_event e = { .type = EV_FF, .code = code, .value = value };
    return write(fd, &e, sizeof(e)) == sizeof(e) ? 0 : -1;
}
int main(int argc, char **argv) {
    unsigned mv = ((3596-116)*(STRENGTH >> 8))/100+116;
    unsigned rounded_mv = ((mv+58)/116)*116;
    if (argc == 2 && !strcmp(argv[1], "--self-test")) {
        if (mv != 1229 || rounded_mv != 1276 || rounded_mv > 3200 || LENGTH_MS > 100) return 1;
        puts("A6L_HAPTIC_BOUNDS_PASS strong=8192 pulse_ms=100 rounded_mv=1276");
        return 0;
    }
    if (argc != 3 || strcmp(argv[1], "--pulse") || strncmp(argv[2], "/dev/input/event", 16)) {
        fputs("Usage: a6l_haptic_probe --self-test | --pulse /dev/input/eventN\n", stderr);
        return 2;
    }
    struct utsname u;
    if (uname(&u) || strcmp(u.release, "7.2.3-a6l-probe+")) {
        fputs("Refusing unreviewed kernel\n", stderr); return 2;
    }
    int fd = open(argv[2], O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { perror("open"); return 1; }
    char name[128] = {0}; struct stat st;
    if (fstat(fd, &st) || !S_ISCHR(st.st_mode) || ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0 || strcmp(name, "spmi_haptics")) {
        fputs("Refusing non-SPMI haptics device\n", stderr); close(fd); return 2;
    }
    struct ff_effect effect = { .type = FF_RUMBLE, .id = -1 };
    effect.u.rumble.strong_magnitude = STRENGTH;
    effect.replay.length = LENGTH_MS;
    if (ioctl(fd, EVIOCSFF, &effect) < 0) { perror("EVIOCSFF"); close(fd); return 1; }
    int rc = event(fd, effect.id, 1);
    /* The kernel effect timer stops after 100 ms even if this process dies. */
    if (!rc) {
        struct timespec left = { .tv_sec = 0, .tv_nsec = 150000000 };
        while (nanosleep(&left, &left) < 0 && errno == EINTR) {}
    }
    if (event(fd, effect.id, 0)) rc = -1;
    if (ioctl(fd, EVIOCRMFF, effect.id) < 0) rc = -1;
    close(fd);
    puts(rc ? "A6L_HAPTIC_TEST_ERROR" : "A6L_HAPTIC_COMMANDS_PASS physical_confirmation_required=1");
    return rc ? 1 : 0;
}
