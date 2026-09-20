/* a6l_epd_nor_read — READ-ONLY full backup of the e-ink panel SPI NOR through spidev.
 * NOT YET RUN ON THE PHONE. Requires the a6l-eink-flash-read overlay (chosen marker "read1").
 *
 * Safety by construction: the only opcodes this program can emit are in ALLOWED[]
 * (0x9F JEDEC ID, 0x05 read status, 0x5A SFDP, 0x03 read). There is no code path that sends
 * write-enable, status-write, erase or program. Flash power = TLMM gpio42 (stock epd_pwr_on),
 * requested through the GPIO chardev, driven high only while reading and driven low before release.
 *
 * usage: a6l_epd_nor_read <spidev> <gpiochip> <outfile> [--dry-run]
 * Output: two full passes must match byte-for-byte, otherwise nothing is kept.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#define EPD_PWR_ON_LINE 42
#define CHUNK 4096u
static const uint8_t ALLOWED[] = {0x9f, 0x05, 0x5a, 0x03};
static int spi = -1;
static int xfer(const uint8_t *tx, size_t txn, uint8_t *rx, size_t rxn) {
    int ok = 0; for (size_t i = 0; i < sizeof ALLOWED; i++) ok |= tx[0] == ALLOWED[i];
    if (!ok) { fprintf(stderr, "A6L_EPD_NOR_REFUSED opcode=0x%02x\n", tx[0]); abort(); }
    struct spi_ioc_transfer t[2]; memset(t, 0, sizeof t);
    t[0].tx_buf = (uintptr_t)tx; t[0].len = (uint32_t)txn;
    t[1].rx_buf = (uintptr_t)rx; t[1].len = (uint32_t)rxn;
    return ioctl(spi, SPI_IOC_MESSAGE(2), t) < 0 ? -errno : 0;
}
static int read_pass(uint8_t *dst, uint32_t size) {
    for (uint32_t a = 0; a < size; a += CHUNK) {
        uint8_t cmd[4] = {0x03, (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a};
        uint32_t n = size - a < CHUNK ? size - a : CHUNK;
        int r = xfer(cmd, 4, dst + a, n); if (r) return r;
    }
    return 0;
}
static uint32_t fnv(const uint8_t *p, size_t n) { uint32_t h = 2166136261u; while (n--) { h ^= *p++; h *= 16777619u; } return h; }
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <spidev> <gpiochip> <outfile> [--dry-run]\n", argv[0]); return 2; }
    int dry = argc > 4 && !strcmp(argv[4], "--dry-run"), rc = 1, line = -1; uint8_t *a = NULL, *b = NULL;
    int chip = open(argv[2], O_RDONLY | O_CLOEXEC); if (chip < 0) { perror("gpiochip"); return 3; }
    struct gpiochip_info ci; if (ioctl(chip, GPIO_GET_CHIPINFO_IOCTL, &ci) || ci.lines < 100) { fprintf(stderr, "A6L_EPD_NOR_FAIL not the TLMM gpiochip (lines=%u)\n", ci.lines); return 3; }
    printf("A6L_EPD_NOR_GPIOCHIP label=%s lines=%u\n", ci.label, ci.lines);
    if (dry) { printf("A6L_EPD_NOR_DRY_RUN no gpio or spi traffic\n"); return 0; }
    struct gpio_v2_line_request rq; memset(&rq, 0, sizeof rq);
    rq.offsets[0] = EPD_PWR_ON_LINE; rq.num_lines = 1; rq.config.flags = GPIO_V2_LINE_FLAG_OUTPUT; strcpy(rq.consumer, "a6l-epd-nor-read");
    if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &rq)) { perror("A6L_EPD_NOR_FAIL gpio42 request"); return 4; }
    line = rq.fd;
    struct gpio_v2_line_values v = {.bits = 1, .mask = 1};
    if (ioctl(line, GPIO_V2_LINE_SET_VALUES_IOCTL, &v)) { perror("gpio high"); goto out; }
    usleep(50000);   /* stock waits ~10 ms */
    spi = open(argv[1], O_RDWR | O_CLOEXEC); if (spi < 0) { perror("spidev"); goto out; }
    uint8_t mode = SPI_MODE_0, bits = 8; uint32_t hz = 4000000;
    if (ioctl(spi, SPI_IOC_WR_MODE, &mode) || ioctl(spi, SPI_IOC_WR_BITS_PER_WORD, &bits) || ioctl(spi, SPI_IOC_WR_MAX_SPEED_HZ, &hz)) { perror("spi setup"); goto out; }
    uint8_t id[3][3], op = 0x9f;
    for (int i = 0; i < 3; i++) if (xfer(&op, 1, id[i], 3)) { perror("jedec id"); goto out; }
    printf("A6L_EPD_NOR_JEDEC %02x %02x %02x\n", id[0][0], id[0][1], id[0][2]);
    if (memcmp(id[0], id[1], 3) || memcmp(id[0], id[2], 3)) { printf("A6L_EPD_NOR_FAIL unstable id\n"); goto out; }
    if ((id[0][0] == 0xff && id[0][1] == 0xff) || (id[0][0] == 0 && id[0][1] == 0)) { printf("A6L_EPD_NOR_FAIL no device answered (power/pins?)\n"); goto out; }
    if (id[0][2] < 0x10 || id[0][2] > 0x18) { printf("A6L_EPD_NOR_FAIL implausible capacity code 0x%02x\n", id[0][2]); goto out; }
    uint32_t size = 1u << id[0][2];
    uint8_t sr, rdsr = 0x05; if (!xfer(&rdsr, 1, &sr, 1)) printf("A6L_EPD_NOR_STATUS 0x%02x\n", sr);
    uint8_t sfdp_cmd[5] = {0x5a, 0, 0, 0, 0}, sfdp[16]; if (!xfer(sfdp_cmd, 5, sfdp, 16)) { printf("A6L_EPD_NOR_SFDP"); for (int i = 0; i < 16; i++) printf(" %02x", sfdp[i]); printf("\n"); }
    a = malloc(size); b = malloc(size); if (!a || !b) goto out;
    if (read_pass(a, size) || read_pass(b, size)) { perror("A6L_EPD_NOR_FAIL read"); goto out; }
    if (memcmp(a, b, size)) { printf("A6L_EPD_NOR_FAIL passes differ\n"); goto out; }
    /* the stock window (0x70080) and the VCOM block the stock kernel parses */
    printf("A6L_EPD_NOR_READ bytes=%u fnv_all=%08x fnv_stock_window=%08x\n", size, fnv(a, size), size >= 0x70080 ? fnv(a, 0x70080) : 0);
    if (size > 0x70014) printf("A6L_EPD_NOR_VCOM_DIGITS %02x %02x %02x -> %d mV\n", a[0x70011], a[0x70013], a[0x70014], a[0x70011] * 1000 + a[0x70013] * 100 + a[0x70014] * 10);
    int o = open(argv[3], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0400); if (o < 0) { perror("outfile"); goto out; }
    if (write(o, a, size) != (ssize_t)size || fsync(o)) { perror("write"); close(o); unlink(argv[3]); goto out; }
    close(o); rc = 0; printf("A6L_EPD_NOR_PASS file=%s\n", argv[3]);
out:
    if (spi >= 0) close(spi);
    if (line >= 0) { v.bits = 0; ioctl(line, GPIO_V2_LINE_SET_VALUES_IOCTL, &v); close(line); }
    close(chip); free(a); free(b);
    if (rc) printf("A6L_EPD_NOR_RESULT FAIL\n");
    return rc;
}
