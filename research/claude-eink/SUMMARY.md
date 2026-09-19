# A6L e-ink calibration-storage interface — SUMMARY

Date: 2026-09-18
Author role: independent offline e-ink research (research/claude-eink)
Method: static disassembly of the exact stock kernel and vendor HWC. No phone,
no ADB/fastboot/EDL, no writes, no hardware activation. ELF addresses below are
link-time virtual addresses, not file offsets.

## Bottom line

`/dev/epd_flash` is a **real SPI NOR flash** on the ed052tc2 panel's SPI bus,
holding the e-ink **waveform image** (from address 0x000000) and the **digital
VCOM calibration** (at address 0x070000). It is driven by standard SPI NOR
opcodes (0x03 read, 0x05 RDSR, 0x06 WREN, 0x01 WRSR, 0x20 sector-erase, 0x02
page-program). The relationship the brief flagged as "unproven" is now proven:
the same 0x70080-byte region the kernel exposes for reading is exactly what the
vendor HWC loads and hands to the software TCON as the waveform.

A defensible **read-only backup is feasible** through the existing `/dev/epd_flash`
read path, but with two hard caveats: (1) the node's read is **fixed-length and
side-effecting on panel power**, so it needs a purpose-built reader (already
present as `tools/a6l-epd-read.c`), and (2) the fixed 0x70080 window is **not
proven to be the whole chip** and in fact stops 128 bytes short of the end of the
256-byte VCOM block. Full-chip coverage cannot be guaranteed from this interface
alone.

## What `/dev/epd_flash` actually is

- SPI child `eink,ed052tc2@0` on `/soc/spi@c1b8000` (Qualcomm QUP v2, BLSP
  spi8, gpio28–31), **chip-select 0**, **19.2 MHz** max.
- Character device `epd_flash`, dynamic major, fops `epd_spi_fops`:
  - `read` = `epd_spi_read` (fixed 0x70080-byte read at flash 0x000000)
  - `write` = `epd_spi_write` — **a stub that returns 0 and does nothing**
  - `open`/`release` — trivial
  - **`unlocked_ioctl` is NULL** — there is no ioctl interface at all
  - `llseek` = `no_llseek` — the file position is meaningless
- SELinux grants `/dev/epd_flash` **read+open only** to `hal_graphics_composer_default`
  and `surfaceflinger`; **no domain has write**. Persistent VCOM writes never go
  through this node — they go through the `fb1/epd_vcom` sysfs → kernel path.

## The read path (backup-relevant)

`epd_spi_read` **ignores the caller's count and file offset** and always:
powers the EPD path up, issues SPI `03 00 00 00` (READ @ 0x000000), reads a
**fixed 0x70080 (458 880) bytes**, powers down, copies that fixed length to the
user, and **returns the copy_to_user residual (0 on success), not a byte count**.
A normal `read()`-until-EOF loop is therefore wrong; the correct reader supplies a
0x70080 buffer, does one `read()`, and treats a `0` return as success. That is
exactly what `tools/a6l-epd-read.c` does.

Independent corroboration: the vendor HWC `EinkSwTconInit()` mallocs exactly
0x70080, calls `ReadEpdFlash(buf, 0x70080)`, and passes the buffer straight to
`Init_Eink_SWTcon(...)` as the waveform. Two independently compiled components
agree on the same length and purpose.

## The VCOM story (two separate stores)

1. **Persistent digital VCOM in the flash** at 0x070000. `epd_read_vcom`
   (sysfs `fb1/epd_vcom` show) reads 256 bytes there and computes
   `vcom_mV = flash[0x070011]*1000 + flash[0x070013]*100 + flash[0x070014]*10`.
2. **Volatile PMIC VCOM register** in the TI **TPS65185** on i2c-2 @ 0x68. HWC
   `SetEpdVcom` writes the runtime value to
   `/sys/devices/soc/c176000.i2c/i2c-2/2-0068/vcom`.

The flash copy is the calibration of record; the PMIC register is volatile state.
A trustworthy calibration backup must capture the **flash** copy.

## The only write-capable path, and why it is dangerous

`epd_write_vcom` (sysfs `fb1/epd_vcom` **store**, range 2000–3000 mV) performs a
real WREN → WRSR-unprotect → **sector-erase 0x070000** → page-program 256 bytes →
WRSR-reprotect. It **erases the entire 4 KiB sector at 0x070000** and re-writes
only the first 256 bytes; bytes 0x070100–0x070FFF in that sector are erased to
0xFF and not restored. Nothing should write `fb1/epd_vcom` until the flash is
backed up and this sector's full contents are understood.

## Backup recommendation (proposal only — do not execute here)

A bounded, read-only capture through `tools/a6l-epd-read.c` over the existing
`/dev/epd_flash` read fop, run twice for a matching-hash check, with the panel
power prerequisites and stop-conditions in `backup-procedure.md`. This yields a
verifiable 0x70080-byte image containing the waveform and the VCOM digits the
kernel actually uses. It is explicitly **not** a proven full-chip dump.

## Missing evidence that blocks a *complete* backup

- No JEDEC/RDID (0x9F) is ever issued by stock, so **chip identity and true
  capacity are unknown**; the 0x70080 window may not be the whole device.
- 128 bytes of the VCOM block (0x070080–0x0700FF) lie **outside** the read window.
- The live DT/overlay must actually bind the ed052tc2 SPI node on the port, and
  `mdss_dsi_res`/panel power must be up, or the read returns garbage or fails.
- The only prior on-device attempt (2026-09-14) was **denied at open** (0 bytes).

Full detail, disassembly and addresses: `EVIDENCE.md`,
`eink-flash-interface-map.json`. Offline parser: `tools/parse_epd_flash.py`.
