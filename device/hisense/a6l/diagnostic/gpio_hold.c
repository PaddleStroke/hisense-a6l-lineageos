/* a6l_gpio_hold <gpiochip> <seconds> <line>=<0|1> ... : drive TLMM lines for a bounded time, then drive them low and release.
 * Attended diagnostic helper (e.g. e-ink enables gpio42/gpio56 while probing the TPS65185). */
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 4) return 2;
    int chip = open(argv[1], O_RDONLY); if (chip < 0) { perror("chip"); return 3; }
    struct gpio_v2_line_request rq; memset(&rq, 0, sizeof rq); struct gpio_v2_line_values v = {0, 0};
    for (int i = 3; i < argc && rq.num_lines < 8; i++) { unsigned l, val; if (sscanf(argv[i], "%u=%u", &l, &val) != 2) return 2; rq.offsets[rq.num_lines] = l; if (val) v.bits |= 1ull << rq.num_lines; v.mask |= 1ull << rq.num_lines; rq.num_lines++; }
    rq.config.flags = GPIO_V2_LINE_FLAG_OUTPUT; strcpy(rq.consumer, "a6l-gpio-hold");
    if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &rq)) { perror("request"); return 4; }
    if (ioctl(rq.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v)) { perror("set"); return 5; }
    printf("A6L_GPIO_HOLD set mask=%llx bits=%llx for %ss\n", (unsigned long long)v.mask, (unsigned long long)v.bits, argv[2]); fflush(stdout);
    sleep((unsigned)atoi(argv[2]));
    v.bits = 0; ioctl(rq.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v); close(rq.fd); printf("A6L_GPIO_HOLD released\n"); return 0;
}
