// SPDX-License-Identifier: GPL-2.0-only
/* Diagnostic only: text in the captured boot LCD's reserved RAM buffer.
 * No display-controller, PMIC, storage, or e-ink register accesses.
 * Mapping lifecycle follows drivers/firmware/efi/earlycon.c.
 */
#include <linux/console.h>
#include <linux/font.h>
#include <linux/io.h>
#include <linux/libfdt.h>
#include <linux/of_fdt.h>
#include <linux/serial_core.h>
#include <asm/early_ioremap.h>

#define A6L_FB_BASE 0x9d400000UL
#define A6L_FB_RESERVED 0x23ff000UL
#define A6L_FB_WIDTH 1080U
#define A6L_FB_HEIGHT 2340U
/* Assumed XBL layout, shared by other SDM660 boot framebuffers. */
#define A6L_FB_STRIDE (A6L_FB_WIDTH * 4U)
#define A6L_FB_BYTES (A6L_FB_STRIDE * A6L_FB_HEIGHT)
#define CELL_W 16U
#define CELL_H 32U

static struct console *a6l_console;
static void *a6l_fb;
static unsigned int a6l_x, a6l_y;
static bool a6l_paging_ready;

static __ref void *a6l_map_line(unsigned int y)
{
	if (y >= A6L_FB_HEIGHT)
		return NULL;
	if (a6l_fb)
		return a6l_fb + y * A6L_FB_STRIDE;
	if (a6l_paging_ready)
		return NULL;
	return early_memremap_prot(A6L_FB_BASE + y * A6L_FB_STRIDE,
			A6L_FB_STRIDE, pgprot_val(pgprot_writecombine(PAGE_KERNEL)));
}

static __ref void a6l_unmap_line(void *line)
{
	if (!a6l_fb)
		early_memunmap(line, A6L_FB_STRIDE);
}

static void a6l_write(struct console *con, const char *text, unsigned int count)
{
	/* Circular text rows avoid scrolling/copying the entire 10 MB screen. */
	while (count) {
		unsigned int n = 0, h, i, bit;
		unsigned int room = (A6L_FB_WIDTH - a6l_x) / CELL_W;

		while (n < count && n < room && text[n] != '\n')
			n++;
		for (h = 0; h < CELL_H; h++) {
			u32 *line = a6l_map_line(a6l_y + h);

			if (!line)
				return;
			if (!a6l_x)
				memset32(line, 0xff000000, A6L_FB_WIDTH);
			for (i = 0; i < n; i++) {
				unsigned char c = text[i];
				u8 row;

				if (c < 32 || c > 126)
					c = ' ';
				row = font_vga_8x16.data[c * 16 + h / 2];
				for (bit = 0; bit < 8; bit++) {
					u32 pixel = row & BIT(7 - bit) ? 0xffffffff : 0xff000000;
					unsigned int x = a6l_x + i * CELL_W + bit * 2;

					line[x] = pixel;
					line[x + 1] = pixel;
				}
			}
			wmb();
			a6l_unmap_line(line);
		}
		text += n;
		count -= n;
		a6l_x += n * CELL_W;
		if (count && *text == '\n') {
			text++;
			count--;
			a6l_x = A6L_FB_WIDTH;
		}
		if (a6l_x + CELL_W > A6L_FB_WIDTH) {
			a6l_x = 0;
			a6l_y += CELL_H;
			if (a6l_y + CELL_H > A6L_FB_HEIGHT)
				a6l_y = 0;
		}
	}
}

static int __init a6l_remap(void)
{
	a6l_paging_ready = true;
	if (!a6l_console || !console_is_registered(a6l_console))
		return 0;
	a6l_fb = memremap(A6L_FB_BASE, A6L_FB_BYTES, MEMREMAP_WC);
	return a6l_fb ? 0 : -ENOMEM;
}
early_initcall(a6l_remap);

static int __init a6l_earlycon_setup(struct earlycon_device *dev, const char *opt)
{
	const fdt32_t *reg;
	int node, len;

	if (!initial_boot_params ||
	    !of_flat_dt_is_compatible(of_get_flat_dt_root(), "hisense,hlte730t"))
		return -ENODEV;
	node = fdt_path_offset(initial_boot_params,
		"/reserved-memory/framebuffer@9d400000");
	if (node < 0)
		return -ENODEV;
	reg = fdt_getprop(initial_boot_params, node, "reg", &len);
	if (!reg || len != 16 || fdt32_to_cpu(reg[0]) ||
	    fdt32_to_cpu(reg[1]) != A6L_FB_BASE || fdt32_to_cpu(reg[2]) ||
	    fdt32_to_cpu(reg[3]) != A6L_FB_RESERVED ||
	    !fdt_getprop(initial_boot_params, node, "no-map", NULL))
		return -EINVAL;
	BUILD_BUG_ON(A6L_FB_BYTES > A6L_FB_RESERVED);
	dev->con->write = a6l_write;
	a6l_console = dev->con;
	a6l_write(dev->con, "A6L EARLY KERNEL CONSOLE\n", 25);
	return 0;
}
EARLYCON_DECLARE(a6lfb, a6l_earlycon_setup);
