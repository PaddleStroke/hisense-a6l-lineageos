// SPDX-License-Identifier: Apache-2.0
/* A6L: step the TPS65185 e-paper PMIC power-up by hand and print its registers at every step (attended bring-up).
 * The kernel tps65185 driver must NOT be bound (it owns the GPIOs). gpio42/gpio56 are already high via DT regulators.
 * Sequence = stock tps65185_active_mode: VIN(gpio2)=1 -> WAKEUP(gpio80)=1 -> registers -> PWRUP(gpio35)=1 -> poll PG for
 * <ms> -> registers -> PWRUP=0 -> WAKEUP=0 -> VIN=0. VCOM_CTRL(gpio3) is never raised: VCOM stays off, no image change.
 * usage: a6l_tps65185_step /dev/gpiochipN /dev/i2c-N [rails_ms=300] [--no-rails] */
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
static int i2c, lines, pg;
static void ms(int n) { struct timespec t = {n / 1000, (n % 1000) * 1000000L}; nanosleep(&t, 0); }
static int rd(uint8_t reg) {
    uint8_t v = 0; struct i2c_msg m[2] = {{0x68, 0, 1, &reg}, {0x68, I2C_M_RD, 1, &v}}; struct i2c_rdwr_ioctl_data d = {m, 2};
    return ioctl(i2c, I2C_RDWR, &d) < 0 ? -1 : v;
}
static void set(unsigned bit, int val) { struct gpio_v2_line_values v = {.bits = val ? 1ull << bit : 0, .mask = 1ull << bit}; if (ioctl(lines, GPIO_V2_LINE_SET_VALUES_IOCTL, &v)) perror("gpio set"); }
static int pgood(void) { struct gpio_v2_line_values v = {.mask = 1}; ioctl(pg, GPIO_V2_LINE_GET_VALUES_IOCTL, &v); return (int)(v.bits & 1); }
static void dump(const char *tag) {
    static const char *n[] = {"TMST", "ENABLE", "VADJ", "VCOM1", "VCOM2", "INT_EN1", "INT_EN2", "INT1", "INT2", "UPSEQ0", "UPSEQ1", "DWNSEQ0", "DWNSEQ1", "TMST1", "TMST2", "PG", "REVID"};
    printf("A6L_TPS %-10s pgood_gpio=%d", tag, pgood()); for (int r = 0; r <= 0x10; r++) { int v = rd(r); if (v < 0) printf(" %s=NAK", n[r]); else printf(" %s=%02x", n[r], v); } printf("\n");
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--stockseq")) {   /* stock tps65185_active_mode writes UPSEQ0 = 0xE1 (VEE before VNEG; chip default 0xE4) */
        i2c = open(argv[2], O_RDWR); if (i2c < 0) { perror("i2c"); return 5; }
        uint8_t w[2] = {0x09, 0xe1}; struct i2c_msg m = {0x68, 0, 2, w}; struct i2c_rdwr_ioctl_data d = {&m, 1};
        if (ioctl(i2c, I2C_RDWR, &d) < 0) { perror("A6L_TPS_FAIL upseq write"); return 6; }
        printf("A6L_TPS_UPSEQ0=%02x\n", rd(9)); return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "--vcom")) {   /* a6l_tps65185_step --vcom /dev/i2c-N <mV 0..5110>: driver may be bound (I2C_RDWR) */
        int mv = atoi(argv[3]); if (mv < 0 || mv > 5110) return 2; i2c = open(argv[2], O_RDWR); if (i2c < 0) { perror("i2c"); return 5; }
        uint8_t w1[2] = {0x03, (uint8_t)((mv / 10) & 0xff)}, w2[2] = {0x04, (uint8_t)(((mv / 10) >> 8) & 1)};
        struct i2c_msg m1 = {0x68, 0, 2, w1}, m2 = {0x68, 0, 2, w2}; struct i2c_rdwr_ioctl_data d1 = {&m1, 1}, d2 = {&m2, 1};
        if (ioctl(i2c, I2C_RDWR, &d2) < 0 || ioctl(i2c, I2C_RDWR, &d1) < 0) { perror("A6L_TPS_FAIL vcom write"); return 6; }
        printf("A6L_TPS_VCOM set=%dmV VCOM1=%02x VCOM2=%02x\n", mv, rd(3), rd(4)); return 0;
    }
    if (argc < 3) return 2;
    int rails_ms = argc > 3 && argv[3][0] != '-' ? atoi(argv[3]) : 300, rails = 1;
    for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--no-rails")) rails = 0;
    if (rails_ms < 0 || rails_ms > 2000) return 2;
    int chip = open(argv[1], O_RDONLY); if (chip < 0) { perror("gpiochip"); return 3; }
    struct gpiochip_info ci; if (ioctl(chip, GPIO_GET_CHIPINFO_IOCTL, &ci) || ci.lines < 100) { fprintf(stderr, "not the TLMM chip\n"); return 3; }
    struct gpio_v2_line_request rq; memset(&rq, 0, sizeof rq);   /* index: 0=vin(gpio2) 1=wakeup(80) 2=pwrup(35) 3=vcom_ctrl(3, held low) */
    rq.offsets[0] = 2; rq.offsets[1] = 80; rq.offsets[2] = 35; rq.offsets[3] = 3; rq.num_lines = 4; rq.config.flags = GPIO_V2_LINE_FLAG_OUTPUT; strcpy(rq.consumer, "a6l-tps-step");
    if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &rq)) { perror("A6L_TPS_FAIL gpio request (driver still bound?)"); return 4; } lines = rq.fd;
    struct gpio_v2_line_request ri; memset(&ri, 0, sizeof ri); ri.offsets[0] = 0; ri.num_lines = 1; ri.config.flags = GPIO_V2_LINE_FLAG_INPUT | (getenv("A6L_PG_PULLUP") ? GPIO_V2_LINE_FLAG_BIAS_PULL_UP : 0); strcpy(ri.consumer, "a6l-tps-pg");
    if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &ri)) { perror("A6L_TPS_FAIL pgood request"); return 4; } pg = ri.fd;
    i2c = open(argv[2], O_RDWR); if (i2c < 0) { perror("i2c"); return 5; }
    set(0, 1); ms(20); dump("vin");
    set(1, 1); ms(10); dump("wakeup"); ms(50); dump("wakeup+50");
    if (rails) {
        set(2, 1);
        for (int t = 0; t < rails_ms; t += 10) { ms(10); int p = rd(0x0f), g = pgood(); if (t % 50 == 40 || g) printf("A6L_TPS t=%dms PG=%02x gpio=%d INT1=%02x INT2=%02x\n", t + 10, p, g, rd(7), rd(8)); if (g) { ms(50); break; } }
        dump("rails");
        set(2, 0); ms(100); dump("pwrdown");
    }
    set(1, 0); ms(20); set(0, 0);
    printf("A6L_TPS_DONE\n"); return 0;
}
