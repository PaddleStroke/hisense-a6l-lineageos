# research/claude-eink — A6L e-ink SPI calibration-storage interface

Independent, offline reverse-engineering of the Hisense A6L (HLTE730T)
`/dev/epd_flash` interface: how the stock kernel reads/writes the e-ink SPI
storage (waveform + VCOM calibration), what modifies it, who calls it, and a
defensible read-only backup procedure. No hardware was accessed; no writes,
ioctls, or activations were performed. ELF addresses are link-time virtual
addresses, not file offsets.

## Files

- `SUMMARY.md` — concise findings and the backup recommendation.
- `EVIDENCE.md` — detailed, disassembly-backed interface map with addresses,
  opcodes, hashes, confidence and gaps.
- `eink-flash-interface-map.json` — machine-readable interface map.
- `backup-procedure.md` — proposed read-only backup (specification only; the
  coordinating agent executes it, not this directory).
- `tools/parse_epd_flash.py` — offline parser / dump-tool candidate. Parses a
  captured 0x70080 dump and reconstructs VCOM; the hardware path is disabled by
  default and the tool contains no device-access code.
- `tools/kdis.py`, `tools/kdump.py`, `tools/kxref.py` — read-only AArch64
  disassembly / data-dump / xref helpers used for this analysis (need
  `llvm-objdump`; point them at the stock kernel ELF + kallsyms).

## Reproduce

```
# disassemble a kernel symbol (needs the private stock ELF + kallsyms)
python3 tools/kdis.py --elf <stock-symbolized.elf> --kallsyms <stock.kallsyms> \
    --sym epd_spi_read --sym epd_read_vcom --sym epd_write_vcom

# parse a captured dump offline
python3 tools/parse_epd_flash.py --json epd-flash-capture-A.bin
```

## One-paragraph result

`/dev/epd_flash` is real SPI NOR on the ed052tc2 panel bus (spi@c1b8000, CS0,
19.2 MHz). Its read fop returns a fixed 0x70080 (458 880) bytes from address
0x000000 — the waveform image the vendor HWC loads into the software TCON — and
the digital VCOM digits at 0x070011/13/14. The node's write fop is a stub and it
has no ioctl, so the only flash-modifying path is the `fb1/epd_vcom` sysfs store,
which erases and rewrites the 4 KiB sector at 0x070000. A read-only backup is
feasible through the existing read fop with the reviewed reader, but the 0x70080
window is not a proven full-chip dump.
