# A6L e-ink calibration-storage interface — EVIDENCE

All facts below come from static analysis of the exact stock binaries. No phone
access, no writes, no ioctls, no hardware activation. Addresses are **ELF virtual
(link-time) addresses**, not file offsets. Disassembly was produced with
`llvm-objdump` (LLVM 18.1.3) against the reconstructed kernel ELF and the vendor
HWC, cross-checked with `stock.kallsyms`. Analysis scripts are in `tools/`
(`kdis.py`, `kdump.py`, `kxref.py`) and are read-only.

## 0. Binaries analysed (hashes)

| Binary | SHA-256 |
| --- | --- |
| `firmware/extracted/stock-symbolized.elf` (analysis ELF) | `bac80712bb318d2beba5b7d0e60e38862eaedd031071100f95c500fc35a84958` |
| `firmware/extracted/vendor/lib64/hw/hwcomposer.sdm660.so` | `d1dfac49c24e4329db38a53d7b4c862f479a893090c0d93cf7e67d54be5ec4db` |
| `firmware/extracted/vendor/lib/hw/hwcomposer.sdm660.so` (32-bit) | `aa6e4bb809c3e2c62d98eb43493e5acbf3d9dd9c3a755549ca07b16d10f6425b` |
| `firmware/extracted/vendor/lib64/libtcon_eink.so` | `4eaf6536bc5f2418b0945abde3404432083a0016491524da4635174518325b3c` |
| `firmware/extracted/device-trees/stock-00.dtb` | `ef0b93babaa97992003f3edb0795f8c4c44993a73d1b98fb9e494e8131100030` |
| `firmware/extracted/device-trees/stock-01.dtb` | `80dd10e309fd138d58ff740aa75643ea58e08dce5bbed174e16623c8e447280a` |

The prior kernel audit recorded `raw_kernel_sha256`
`44b8228173d70f0798a3ee2f29aec4c352d153e08a823abc529500dac33395b6` and text base
`0xffffff8008080000`; the reconstructed ELF preserves those code bytes (verified
earlier by `tools/Audit-KernelInterfaces.py`). Text base and per-symbol bounds in
this ELF are inferred (next-symbol), not debug-info sizes.

## 1. SPI controller / chip-select / panel binding

From `stock-00.dts` (identical EPD nodes in `stock-01.dts`):

```
/soc/spi@c1b8000 {                       // Qualcomm QUP v2, BLSP spi8
    compatible = "qcom,spi-qup-v2";
    reg = <0xc1b8000 0x600  0xc184000 0x1f000>;   // core + BAM
    spi-max-frequency = <0x2faf080>;      // controller ceiling 50 MHz
    status = "okay";
    eink,ed052tc2@0 {
        compatible = "eink,ed052tc2";
        spi-max-frequency = <0x124f800>;  // 19 200 000 = 19.2 MHz for this device
        reg = <0x00>;                     // chip-select 0
    };
};
```

BLSP spi8 pinmux (`spi_8_active` phandle 0x1ab): `gpio28..gpio31`, function
`blsp_spi8_a`, drive-strength 6.

Kernel driver bind (disassembly of `epd_spi_probe @ 0xffffff80084c429c`):

- `__spi_register_driver(epd_spi_driver @ 0xffffff800a07f6a8)` in `epd_spi_init`
  (device_initcall level 6, `__initcall_epd_spi_init6`).
- `epd_spi_driver` fields: probe `epd_spi_probe`, remove `epd_spi_remove`
  (a stub returning 0), `of_match_table = edp_flash_of_match @ 0xffffff800a07f818`
  whose compatible string is `"eink,ed052tc2"`; the driver name string is
  `"ed052tc2"`.
- Probe: `devm_kmalloc(0x80)` → `epd_flash_data @ 0xffffff800a32e5e8`; stores the
  `spi_device*` at `epd_flash_data[0]`; sets bits-per-word = 8
  (`strb #8, [spi+0x2dd]`) and calls `spi_setup`.
- Char device creation: `alloc_chrdev_region`/`register_chrdev_region` with name
  `"epd_flash"`, major cached in `epd_major @ 0xffffff800a32e5f0`; `cdev_init` +
  `cdev_add` with `epd_spi_fops`; `__class_create` + `device_create` → node
  `/dev/epd_flash`.

## 2. `/dev/epd_flash` file operations

`epd_spi_fops @ 0xffffff800a07f740` (dumped with `kdump.py`):

| slot | value | meaning |
| --- | --- | --- |
| +0x08 | `no_llseek` | file position meaningless |
| +0x10 | `epd_spi_read @ 0xffffff80084c44b0` | read |
| +0x18 | `epd_spi_write @ 0xffffff80084c3f60` | write |
| +0x58 | `epd_spi_open @ 0xffffff80084c3f68` | open |
| +0x68 | `epd_spi_release @ 0xffffff80084c3f7c` | release |
| (unlocked_ioctl / compat_ioctl / mmap) | 0 | **absent** |

- `epd_spi_write @ 0xffffff80084c3f60` is literally `mov x0, #0 ; ret` — a stub.
  **Writing to `/dev/epd_flash` does nothing and cannot modify the flash.**
- `epd_spi_open` stashes `epd_flash_data` into the file's private data; `release`
  returns 0. No side effects.
- There is **no ioctl** — the fops table has NULL in both ioctl slots.

## 3. Read handler `epd_spi_read` (the backup-relevant path)

Disassembly (`tools/kdis.py --sym epd_spi_read`, matches the pre-existing
`firmware/extracted/kernel-epd-flash-read.txt`):

- `mdss_epd_power_up(1)` (0xffffff8008489f90) — asserts EPD GPIOs high.
- If `epd_flash_data[0x78]` is null, `vmalloc(0x70080)` and cache it; on OOM
  `printk` and return `-12` (-ENOMEM).
- `memset(buf, 0, 0x70080)`.
- Build a 4-byte command on the stack: bytes `[0]=0x03, [1]=0x00, [2]=0x00,
  [3]=0x00` (`strb #3` + three `strb wzr`).
- `spi_write_then_read(spi, tx=cmd, n_tx=4, rx=buf, n_rx=0x70080)`
  — i.e. SPI NOR **READ (0x03)** at address **0x000000**, **0x70080 = 458 880
  bytes**.
- `mdss_epd_power_up(0)` — deasserts GPIOs.
- `access_ok`-style bound check on `(user_ptr + 0x70080)` then
  `__arch_copy_to_user(user, buf, 0x70080)`.
- `vfree(buf)`, null the cache pointer.
- **Return value is the `copy_to_user` residual** (`sxtw x19, w0` from the
  earlier spi return is overwritten; the final return is the residual, 0 on
  success). It ignores the caller's requested `count` and the file offset.

Consequence for a reader: supply a 0x70080 buffer, one `read()`, treat `0` as
success. `tools/a6l-epd-read.c` (SHA-256 `60b09c88...d9360`) already implements
exactly this and nothing else (openat O_RDONLY, read, close, write to stdout).

## 4. VCOM read handler `epd_read_vcom`

`epd_read_vcom @ 0xffffff80084c4648` (exported `T`), invoked by
`mdss_fb_get_epd_vcom` (sysfs `show` for `/sys/class/graphics/fb1/epd_vcom`):

- `vmalloc(0x100)`; `mdss_epd_flash_power_on`.
- Command bytes `[0]=0x03, [1]=0x07, [2]=0x00, [3]=0x00` → **READ (0x03) at
  address 0x070000**.
- `spi_write_then_read(spi, cmd, n_tx=4, rx=buf, n_rx=0x100)` → 256 bytes.
- VCOM reconstruction (from the `ldrb`/`mul`/`madd` sequence):
  `vcom_mV = buf[0x11]*1000 + buf[0x13]*100 + buf[0x14]*10`
  i.e. absolute flash offsets **0x070011, 0x070013, 0x070014**.
- `mdss_epd_flash_power_release`; `vfree`; return `vcom_mV`.

These three digit bytes fall at absolute 0x070011/13/14, which are **inside** the
0x70080 read window (window ends at 0x070080). So a 0x70080 capture does contain
the VCOM value the kernel uses — but not the whole 256-byte block (0x070080..
0x0700FF is beyond the window).

## 5. VCOM write handler `epd_write_vcom` (the only write-capable path)

`epd_write_vcom @ 0xffffff80084c4760` (exported `T`), invoked by
`mdss_fb_set_epd_vcom` (sysfs `store`). Confirmed call chain: `epd_spi_read_rdsr`
(0x05), `epd_spi_set_wren` (0x06), `epd_spi_write_wrsr` (0x01 + data),
`epd_wait_wip_clean` (polls RDSR WIP up to 1000× with udelay). Sequence:

1. Range-check: accept `2000 <= vcom <= 3000` mV (`sub #0x7d0; cmp #0x3e8;
   b.ls`), else log `"vcom is invalid"` and return `-1` **without touching flash**.
2. `mdss_epd_flash_power_on`.
3. Opcode **0x2B** (1 tx / 1 rx) — reads a config/security register.
4. `epd_spi_read_rdsr` (0x05).
5. `epd_spi_set_wren` (0x06) + `epd_spi_write_wrsr` (0x01+byte) + `epd_wait_wip_clean`
   — unprotect status register.
6. `vmalloc(0x104)`; build write buffer `[0]=0x02,[1]=0x07,[2]=0,[3]=0` (PAGE
   PROGRAM at 0x070000) followed by 256 data bytes read back from 0x070000;
   overwrite 3 digit bytes at data offsets 0x11/0x13/0x14 with the new VCOM.
7. `epd_spi_set_wren`; **SECTOR ERASE opcode 0x20 at 0x070000** (`cmd
   20 07 00 00`, n_tx=4); `epd_wait_wip_clean`.
8. `epd_spi_set_wren`; **PAGE PROGRAM 0x02**, 256 bytes at 0x070000
   (n_tx=0x104=260); `epd_wait_wip_clean`.
9. `epd_spi_set_wren`; `epd_spi_write_wrsr(original)` — re-protect;
   `mdss_epd_flash_power_release`.

**Destructive scope:** step 7 erases the whole 4 KiB sector at 0x070000; step 8
re-writes only 0x070000..0x0700FF. Bytes 0x070100..0x070FFF are erased to 0xFF and
not restored. This is why `fb1/epd_vcom` store must not be exercised before a
backup.

These write opcodes (0x06/0x01/0x20/0x02) exist **only** inside `epd_write_vcom`'s
call graph. They are **not reachable via `/dev/epd_flash`** (write fop is a stub,
ioctl is NULL). They are reachable **only** through the `fb1/epd_vcom` sysfs store.
Confirms `/dev/epd_flash` is a read-only surface for calibration.

Standard SPI NOR opcode set observed end-to-end: 0x03 READ, 0x05 RDSR, 0x06 WREN,
0x01 WRSR, 0x20 SECTOR ERASE (4 KiB), 0x02 PAGE PROGRAM, 0x2B (config/security
read). This establishes the device as ordinary SPI NOR with 4 KiB sectors — but
note **RDID/JEDEC (0x9F) is never issued**, so chip identity and total capacity
are unknown from software.

## 6. Power / GPIO prerequisites

- `mdss_epd_power_up @ 0xffffff8008489f90` drives three GPIOs from
  `ctrl_pdata`: epd_pwr_on (+0x914), dsi2dpi_vdcc_en (+0x918), epd_i2c_en
  (+0x91c) to the argument value, but **only if** `mdss_dsi_res` and
  `ctrl_pdata (mdss_dsi_res->[0x18])` are non-null and `ctrl+0x198` is clear;
  otherwise it just `printk`s and returns (a no-op). Used by `epd_spi_read`.
- `mdss_epd_flash_power_on @ 0xffffff800848a02c` drives epd_pwr_on (+0x914) high,
  sets a **flash-busy flag `ctrl+0x935 = 1`**, and delays ~10 ms. Used by
  `epd_read_vcom`/`epd_write_vcom`.
- `mdss_epd_flash_power_release @ 0xffffff800848a0b8` clears `ctrl+0x935 = 0`
  (does **not** drop the GPIO).
- The flag matters: `mdss_dsi_panel_power_ctrl @ 0xffffff8008486518` checks
  `ctrl+0x935` at power-off (`ldrb w1,[x21+0x935]; cbnz ...skip`) and **skips
  dropping epd_pwr_on while a flash op is in progress**, preventing power being
  cut mid-transaction.

GPIO sources (parsed by `dsi_panel_device_register`, xrefs found with
`tools/kxref.py`): `qcom,platform-epd-xon-gpio` (+0x910),
`qcom,platform-epd-power-on-gpio` (+0x914),
`qcom,platform-dsi2dpi-vdcc-en-gpio` (+0x918),
`qcom,platform-epd-i2c-en` (+0x91c).

**Implication:** if the panel/`mdss_dsi_res` is not up, `epd_spi_read`'s
power-up is a silent no-op and the SPI read may return uninitialised data. A
capture must verify the data is non-trivial.

## 7. VCOM's second (volatile) store — TPS65185 PMIC

DT: `/soc/i2c@c176000/tps65185@68`, `compatible = "ti,tps65185"`, with
`pm-pwrup/pm-pwrcom/pm-power-en/pm-wake-up/pm-state` GPIOs. Kernel
`tps65185_set_vcom @ 0xffffff80084c30f0` parses an int and stores it at
`pdata+0x14`; `tps65185_show_vcom` reads it back. `tps65185_power_on`,
`tps65185_active_mode`, `tps65185_sleep_mode` are called from
`mdss_dsi_panel_power_ctrl`/`mdss_dsi_on` and from `tps65185_set_power_en`.

The PMIC VCOM sysfs is `/sys/devices/soc/c176000.i2c/i2c-2/2-0068/vcom`
(SELinux type `flash_vcom`), writable by `hal_graphics_composer_default` and
`system_app`. This is a **runtime register**, distinct from the flash copy.

## 8. Userspace callers (HWC)

From `hwcomposer.sdm660.so` disassembly and strings:

- `HWCDisplayExternalEpd::ReadEpdFlash(void*, size_t) @ 0x419e8` — opens
  `/dev/epd_flash` via `__open_2(path, 0)` (**O_RDONLY**), one `read(fd, buf,
  count)`. This is the **only** stock userspace reader of the node. SELinux
  confirms only `hal_graphics_composer_default` and `surfaceflinger` may read it.
- `HWCDisplayExternalEpd::EinkSwTconInit() @ 0x42078` —
  `malloc(0x70080); memset; ReadEpdFlash(buf, 0x70080);
  Init_Eink_SWTcon(this+0x4f8, mode=5, cfg@sp, buf, 0x70080, out=this+0x5a4);
  SetEpdVcom(this+0x660); SaveEpdInfo(); free(buf)`. **The 0x70080 flash window
  is the waveform** handed to the software TCON — the request length exactly
  matches the kernel's fixed read length (independent corroboration of §3).
- `HWCDisplayExternalEpd::SetEpdVcom(uint) @ 0x41d34` — opens the **i2c**
  `2-0068/vcom` node O_RDWR and writes decimal mV (runtime PMIC VCOM, not the
  flash copy).
- Other observed buffer sizes in `EinkSwTconInit`: five TCON target buffers of
  0x10FE00 each, plus 0x438000 / 0x3F4800 / 0x1FA400 working buffers. (These are
  frame/conversion buffers, not flash.)
- `libtcon_eink.so` ABI already partly validated offline (prior work): version
  function returns 2.2; `Init_Eink_SWTcon`, `Release_Eink_SWTcon`,
  `SetEinkContrast`, `ReportEinkSWTconLibVersion` exported. Init/frame-conversion
  side effects remain unverified.

## 9. Prior on-device attempt (context)

`firmware/extracted/epd-spi-capture-20260914/report.json`: the reader helper was
pushed and run over ADB shell on 2026-09-14; the **read-only open was denied**
(exit 10, 0 bytes, stderr "Cannot open /dev/epd_flash read-only; no read
attempted"). No read or power cycle occurred; the helper was removed. The SPI
region remains un-backed-up. This is the access prerequisite the backup procedure
must resolve legitimately (correct SELinux domain/context), not by weakening
policy.

## 10. Confidence and gaps

**High confidence (source/binary-backed):** SPI binding and CS0/19.2 MHz; the
fops table and the stub write / absent ioctl; the fixed 0x70080 read at 0x000000
and its return semantics; the VCOM read at 0x070000 and its digit arithmetic; the
VCOM write's erase+program of the 0x070000 sector; the GPIO/power prerequisites
and the flash-busy flag; HWC using the same 0x70080 window as the waveform.

**Unresolved / untested:**
- Chip JEDEC identity and true capacity (no RDID issued by stock).
- Whether any data exists/matters above 0x070080; the interface cannot reach it.
- The exact meaning of opcode 0x2B (shape only: 1 in / 1 out).
- Live DT/overlay binding of the ed052tc2 node on the modern port.
- No successful capture exists yet; the one attempt was denied at open.
