# Stock vendor compatibility preparation

Offline audit on 2026-09-14 uses the verified stock filesystem extractions and
the stock Android 9 vendor linker namespace configuration (`ld.config.28.txt`).
`tools/Audit-StockDependencies.py` follows ELF DT_NEEDED dependencies, checks
each inspected file against its extraction hash, and reports unresolved library
names and candidate missing strong symbols. It never executes stock binaries.

| Entry point | Vendor objects | VNDK 28 objects | Other stock system objects |
| --- | ---: | ---: | ---: |
| Graphics composer 2.1 service | 1 | 10 | 8 |
| Graphics composer 2.1 implementation | 3 | 12 | 8 |
| SDM660 hardware composer | 13 | 24 | 36 |
| Hisense e-ink TCON library | 1 | 2 | 6 |
| Wi-Fi HAL service | 4 | 12 | 7 |
| wpa_supplicant | 14 | 14 | 7 |
| Qualcomm keymaster 4.0 service | 5 | 12 | 7 |

All seven modeled stock dependency closures resolve, with no candidate missing
strong symbols under the audit's simplified symbol check. This establishes an
offline baseline, not compatibility with Android 17. The model does not reproduce
dlopen, symbol versions, global groups or interposition; its union of exports can
also overestimate symbol visibility across namespaces. Implementation libraries
loaded indirectly were supplied as explicit roots.

A controlled repeat hiding VNDK 28 produces 45 unresolved dependency edges for
hardware composer and 28 for the Wi-Fi service. These are edge counts, not counts
of unique libraries. This experiment demonstrates concrete legacy dependencies;
it is not a simulation of Android 17's linker.

Private detailed reports are in `firmware/extracted/vendor-dependency-audit.json`
and `vendor-dependency-without-vndk28.json`. The table is not a copy list: stock
LLNDK libraries pull in additional stock system internals. Copying that entire
closure into a modern system would introduce further incompatible components.

## Work after the first compilation

1. Inspect the generated system image and its linker configuration. Compare the
   vendor-facing library and symbol boundary against this stock baseline.
2. Determine how to package an isolated VNDK 28 compatibility set, or rebuild
   affected components where suitable source exists. Verify both required ABIs
   and namespace visibility before enabling vendor services.
3. Resolve the kernel BPF requirements documented in `kernel-compatibility.md`
   and the stock system-as-root boot layout. Compilation does not resolve either.
4. Integrate the Hisense display control path documented in `eink-port.md`;
   a functioning ordinary graphics composer alone does not implement its APIs.
5. Review the exact candidate images, partition sizes, verified-boot handling,
   recovery path and data implications before any installation. The current
   generic system compile probe is not ready for a phone boot test.

Preserve the build output for incremental work. A userspace change normally
rebuilds its dependencies and the affected image; a built-in kernel driver
change requires rebuilding and repackaging the kernel/boot image. Changes to
shared interfaces may require several components to be rebuilt together.

## Live stock baseline

The read-only collector completed on the authorized spare. Capture:
`captures/20260914-142540-e0594221/`, with per-command exit codes and SHA-256
checksums. It includes Android logs, display/compositor, input routing, Wi-Fi,
connectivity, power and the Hisense `epd` service dumps.

Observed: `epd` reports system ready and reading mode 3. Android exposes the rear
720 x 1440 panel as an external display named "HDMI Screen"; the captured logical
override is 1080 x 2160 and external touch maps to the physical 720 x 1440 panel.
The reported infinite refresh rate is unusable metadata, not a physical refresh
measurement. Wi-Fi is disabled in this capture, so it does not establish a
working network connection baseline.

Stock permissions block several kernel/proc reads; `/sys/fs/pstore` is absent.
`lshal` exits with code 136; other commands can return partial results when one
of several requested paths is absent or inaccessible. Retain those failures
alongside useful output; the collector's completion is not a claim that every
read succeeded. No root, settings changes, reboots or partition writes occurred.
