# Proposed A6L e-ink SPI backup procedure (PROPOSAL — do not execute here)

This is a procedure specification for the coordinating agent. Nothing in this
research directory executes it. It is read-only by design and must be run on the
spare HLTE730T only.

## Objective

Capture a verifiable image of the e-ink SPI storage exposed at `/dev/epd_flash`,
containing the waveform region and the digital VCOM calibration, **without
writing to the flash and without altering VCOM/PMIC/controller state.**

## Coverage and its limits (state these in any report)

- The stock read fop returns a **fixed 0x70080 (458 880) bytes** from flash
  address 0x000000. This is the waveform region plus the VCOM digits at
  0x070011/0x070013/0x070014.
- It is **NOT proven to be the whole chip.** No JEDEC/RDID is available, so total
  capacity is unknown, and bytes 0x070080–0x0700FF of the VCOM block are outside
  the window. Do not describe the result as a full-chip backup.
- The separate eMMC backup does **not** contain this SPI storage.

## Prerequisites (all must hold before any read)

1. Spare device identity confirmed:
   `Hisense/HLTE730T/HLTE730T:9/PKQ1.190723.001/L1632.6.01.04:user/release-keys`.
2. The stock kernel is running (4.4.153 with the epd_spi driver), OR a kernel in
   which the `eink,ed052tc2@0` SPI node is bound and `/dev/epd_flash` exists.
   Confirm the node exists and is a char device before reading.
3. Panel power path available: `mdss_dsi_res` populated and the secondary EPD
   panel's power/GPIOs (epd_pwr_on, dsi2dpi_vdcc_en, epd_i2c_en) functional. The
   read fop itself calls `mdss_epd_power_up(1)`/`(0)`; if the panel is not
   registered the power-up is a silent no-op and the SPI read will return
   uninitialised/garbage data. Prefer capturing while the e-ink display stack is
   up (e.g. after the stock HWC has already initialised it) or immediately verify
   the data is non-trivial (not all-0x00 / all-0xFF / all-0xA5 fill).
4. Read access to `/dev/epd_flash` for the capturing context. Stock permissions
   are `system:system 0644`; SELinux only grants read to
   `hal_graphics_composer_default`/`surfaceflinger`. The 2026-09-14 attempt as an
   ordinary shell helper was **denied at open**. Resolve access legitimately
   (appropriate domain/context) — do NOT weaken SELinux world-wide or bypass
   permissions as a workaround.
5. Use the reviewed reader `tools/a6l-epd-read.c`
   (SHA-256 `60b09c8875ae32d3efaa38a030076842bc0363611ae77466fe069ff3726d9360`):
   O_RDONLY, one `read()` into a 0x70080 buffer, treats return `0` as success.

## Procedure

1. Verify device identity and that `/dev/epd_flash` is a character device.
2. Do **not** touch `fb1/epd_vcom` store, the i2c `2-0068/vcom` node, or any EPD
   sysfs control at any point. Reads only.
3. Run the reader once; save stdout to `epd-flash-capture-A.bin` and stderr.
   - Expect exactly 0x70080 bytes on stdout and the success diagnostic.
   - Reader exit codes: 10 = open denied (no read happened), 11 = unexpected
     driver return, 12 = buffer untouched (power path likely down), 13 = short
     write of output. Any non-zero exit = stop, capture nothing further.
4. Wait a few seconds and run the reader a **second** time to
   `epd-flash-capture-B.bin`.
5. Compute SHA-256 of A and B.
   - **Stop condition (pass):** sizes are both 0x70080 and `sha256(A) == sha256(B)`.
   - **Stop condition (fail):** any size mismatch, hash mismatch, or a buffer
     that is entirely 0x00 / 0xFF / 0xA5 → treat as unreliable, do not trust the
     image, and report the power/binding prerequisite as unmet.
6. Run `tools/parse_epd_flash.py --json epd-flash-capture-A.bin`. Confirm the
   reconstructed VCOM is within the kernel-valid range [2000, 3000] mV and is
   plausible for this unit. An out-of-range value is a red flag that the read did
   not hit real calibration data.
7. Record: both hashes, sizes, reader stderr, the parser report, the kernel build
   string, and whether the EPD stack was up at capture time. Store alongside the
   existing eMMC backup metadata; label explicitly as
   "SPI waveform+VCOM window (0x70080), not a proven full-chip dump."

## Verification

- Two independent reads must hash-match (guards against transient SPI errors).
- Parser VCOM must be in range and match, if known, the value shown by
  `cat /sys/class/graphics/fb1/epd_vcom` (a read-only show that also goes through
  `epd_read_vcom`) — but note reading that sysfs also powers the flash path.
- Keep the capture read-only: never exercise `epd_write_vcom`.

## Explicit stop / do-not-proceed conditions

- `/dev/epd_flash` missing or not a char device → the SPI node is not bound; stop.
- Open denied → resolve access properly; do not weaken policy; stop until then.
- Buffer all-0xA5 (reader pre-fill), all-0x00 or all-0xFF → power/binding not
  ready; stop.
- Hash mismatch between two reads → stop; do not trust either image.
- Do not attempt to read beyond 0x70080 or at other addresses through this node —
  the fop cannot, and there is no ioctl. A higher-coverage or full-chip dump would
  require a different, separately reviewed mechanism (e.g. direct SPI access with
  RDID first) which is out of scope here and must not be improvised.

## What this backup does and does not protect

- **Protects:** the waveform image and the VCOM digits the kernel consumes — the
  data needed to restore e-ink calibration behaviour if the flash is later
  disturbed.
- **Does not protect:** any flash content above 0x070080, the 128-byte VCOM tail
  0x070080–0x0700FF, or the volatile PMIC VCOM register. And it is not a proof of
  full-chip capacity. Treat restoration as an unproven, separate task.
