#!/usr/bin/env python3
"""
parse_epd_flash.py  --  Offline parser / dump-tool candidate for the Hisense
A6L (HLTE730T) e-ink SPI storage exposed at /dev/epd_flash.

SCOPE AND SAFETY
================
This tool is OFFLINE-FIRST. By default it only *parses* a dump file that was
captured elsewhere. The hardware dump path is a *candidate* and is DISABLED by
default: it refuses to run unless invoked with  --i-have-hardware-authorization
AND run on an actual A6L. It NEVER writes to the device. It issues no ioctls
and never opens the node writable. It is provided so the coordinating agent has
a reviewed reader; it must not be treated as a tested capture.

WHAT THE STOCK DRIVER DOES (established by static analysis; see EVIDENCE.md)
==========================================================================
The character device /dev/epd_flash has file-ops:
    read  = epd_spi_read        (fixed-length, see below)
    write = epd_spi_write        -> returns 0, no side effect (a stub)
    open  = epd_spi_open
    release = epd_spi_release
    unlocked_ioctl = NULL        (no ioctl interface exists)
    llseek = no_llseek           (file position is meaningless)

epd_spi_read (the .read handler) ignores the caller's count and file offset and
ALWAYS performs, on the ed052tc2 SPI device (spi@c1b8000, CS0, 19.2 MHz):
    mdss_epd_power_up(1)                       # drives 3 EPD GPIOs high
    tx = [0x03, 0x00, 0x00, 0x00]             # SPI NOR READ opcode @ 0x000000
    spi_write_then_read(spi, tx, 4, rx, 0x70080)   # 458880 bytes
    mdss_epd_power_up(0)                       # drives the 3 GPIOs low
    copy_to_user(user, rx, 0x70080)           # fixed length
    return copy_to_user_residual              # 0 on success, NOT a byte count

Therefore a correct reader must:
  * provide a buffer of exactly 0x70080 bytes,
  * issue ONE read() of that size,
  * treat return value 0 (not 0x70080) as success.

COVERAGE WARNING
================
The fixed 0x70080 (458880) byte window is NOT proven to be the whole chip.
The separate kernel VCOM path (epd_read_vcom) reads 256 bytes at flash address
0x070000, i.e. absolute 0x070000..0x0700FF (458752..459007). The fixed read
ends at 0x070080 (458880), so 128 bytes of the VCOM block (0x070080..0x0700FF)
lie OUTSIDE the /dev/epd_flash read window and are not captured by it. The
digital VCOM digits actually consumed by the kernel are at absolute offsets
0x070011 / 0x070013 / 0x070014, which ARE inside the window. See EVIDENCE.md.
"""

import argparse
import hashlib
import json
import sys

READ_WINDOW = 0x70080          # 458880, fixed by epd_spi_read
VCOM_BLOCK_ADDR = 0x070000     # where epd_read_vcom / epd_write_vcom operate
VCOM_BLOCK_LEN = 0x100         # 256 bytes read by epd_read_vcom
# VCOM digit offsets WITHIN the 0x070000 block (from epd_read_vcom disassembly):
#   vcom_mV = buf[0x11]*1000 + buf[0x13]*100 + buf[0x14]*10
VCOM_OFF_THOUSANDS = 0x11
VCOM_OFF_HUNDREDS = 0x13
VCOM_OFF_TENS = 0x14


def parse_vcom_from_dump(data: bytes):
    """Reproduce the kernel epd_read_vcom arithmetic against a captured dump.
    The dump is the 0x70080 window starting at flash address 0x000000, so the
    VCOM block begins at index VCOM_BLOCK_ADDR within the dump."""
    base = VCOM_BLOCK_ADDR
    need = base + VCOM_OFF_TENS + 1
    if len(data) < need:
        return {"error": f"dump too short for VCOM digits (need >= {need} bytes)"}
    b = data
    t = b[base + VCOM_OFF_THOUSANDS]
    h = b[base + VCOM_OFF_HUNDREDS]
    te = b[base + VCOM_OFF_TENS]
    vcom_mV = t * 1000 + h * 100 + te * 10
    covered = (base + VCOM_BLOCK_LEN) <= len(data)
    return {
        "vcom_block_flash_addr": hex(base),
        "digit_bytes": {
            hex(base + VCOM_OFF_THOUSANDS): t,
            hex(base + VCOM_OFF_HUNDREDS): h,
            hex(base + VCOM_OFF_TENS): te,
        },
        "reconstructed_vcom_millivolts": vcom_mV,
        "kernel_valid_range_mV": [2000, 3000],
        "in_kernel_valid_range": 2000 <= vcom_mV <= 3000,
        "full_vcom_block_captured": covered,
        "note": ("digits used by kernel are inside the read window; "
                 "bytes 0x070080..0x0700FF of the VCOM block are NOT in a "
                 "0x70080 dump" if not covered else
                 "entire 256-byte VCOM block present"),
    }


def analyze(path: str):
    with open(path, "rb") as f:
        data = f.read()
    rep = {
        "input": path,
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "expected_read_window": READ_WINDOW,
        "size_matches_read_window": len(data) == READ_WINDOW,
    }
    # Cheap SPI-NOR sanity signals (heuristic only, NOT authoritative):
    if data:
        rep["all_ff"] = data.count(0xFF) == len(data)
        rep["all_00"] = data.count(0x00) == len(data)
        rep["all_a5_fill"] = data.count(0xA5) == len(data)  # reader's pre-fill
        rep["first_16_bytes_hex"] = data[:16].hex()
        # entropy-ish: distinct byte count in first 4 KiB
        rep["distinct_bytes_first_4k"] = len(set(data[:4096]))
    rep["vcom"] = parse_vcom_from_dump(data)
    return rep


HARDWARE_NOTE = """\
HARDWARE DUMP PATH IS DISABLED.

To capture on a real A6L, the reviewed static reader tools/a6l-epd-read.c
(SHA-256 60b09c88...d9360) is the correct mechanism: it opens /dev/epd_flash
O_RDONLY, provides a 0x70080 buffer, does ONE read(), and treats a 0 return as
success. That capture must be performed by the coordinating agent under the
prerequisites and stop-conditions in ../backup-procedure.md, NOT by this tool.

This parser deliberately contains no device-access code so it can never touch
hardware by accident.
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", nargs="?", help="captured 0x70080-byte /dev/epd_flash dump")
    ap.add_argument("--json", action="store_true", help="emit JSON report")
    ap.add_argument("--dump-hardware", action="store_true",
                    help="(disabled) placeholder for a hardware capture path")
    ap.add_argument("--i-have-hardware-authorization", action="store_true",
                    help="required acknowledgement; hardware path is still disabled")
    a = ap.parse_args()

    if a.dump_hardware:
        sys.stderr.write(HARDWARE_NOTE)
        return 2

    if not a.dump:
        ap.print_help()
        return 1

    rep = analyze(a.dump)
    if a.json:
        print(json.dumps(rep, indent=2))
    else:
        print(f"file      : {rep['input']}")
        print(f"bytes     : {rep['bytes']} (expected {READ_WINDOW}, "
              f"match={rep['size_matches_read_window']})")
        print(f"sha256    : {rep['sha256']}")
        v = rep["vcom"]
        if "error" in v:
            print(f"vcom      : {v['error']}")
        else:
            print(f"vcom (mV) : {v['reconstructed_vcom_millivolts']} "
                  f"(kernel-valid={v['in_kernel_valid_range']})")
            print(f"vcom block: {v['note']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
