# Kernel and boot investigation, 2026-09-14

This work ran alongside the first system-image compilation. No kernel, boot
image, settings, calibration, or partition was written to the phone.

## Boot-container reproduction

`tools/Verify-BootRoundtrip.py` reads boot and recovery from the authoritative
prefix, verifies their recorded SHA-256 hashes, and invokes the synced AOSP
unpack_bootimg/mkbootimg tools through argument arrays (no shell evaluation).

Both kernel/ramdisk payload sets and the recovery DTBO reproduce correctly.
Current mkbootimg sets the unused second_addr field to zero when second_size is
zero. Both saved images retain 0x00f00000 in that field. The raw tool output
therefore differs at byte 30. A separate output preserves that one explicitly
identified field; both resulting image bodies then match the captured bodies
byte for byte. No AVB footer/signature or trailing partition data is recreated.

Successful report: firmware/extracted/boot-roundtrip-20260914-v4/report.json.
Boot body: 12,390,400 bytes. Recovery body: 22,700,032 bytes. These are local
analysis outputs, not installation images or a tested restore procedure.

## Kernel symbols and e-ink interfaces

Built and used marin-m/vmlinux-to-elf 1.3.6, commit
34b064caf647d363f2257c0e246a321fddc31a6c, in a separate WSL virtual environment.
Python development headers were installed for its minilzo dependency. The
package/version list and extraction logs are retained in private analysis files.

Recovered 140,166 symbols and generated an AArch64 ELF for analysis. The tool
inferred base address 0xffffff8008080000. This is a reconstructed analysis ELF,
not the original linker's vmlinux, debug information or recovered C source.

`tools/Audit-KernelInterfaces.py` independently decompresses the kernel directly
from the hash-verified boot partition and checks it against the saved raw kernel.
It verifies that seven selected code ranges in the reconstructed ELF match those
raw bytes. `Inspect-StockElf.py --infer-zero-sizes` can inspect such recovered
symbols; bounds are inferred from adjacent symbols and are not debug-info sizes.

Verified handlers:

| Handler | Link-time address | Inspection bytes |
| --- | --- | ---: |
| epd_spi_probe | 0xffffff80084c429c | 532 |
| epd_spi_read | 0xffffff80084c44b0 | 408 |
| epd_read_vcom | 0xffffff80084c4648 | 280 |
| mdss_fb_set_epd_display_mode | 0xffffff80084c8cd0 | 136 |
| mdss_fb_set_epd_commit_bitmap | 0xffffff80084c900c | 236 |
| mdss_fb_set_epd_connect | 0xffffff80084c90f8 | 216 |
| mirror_state_store | 0xffffff8008c0c7ac | 108 |

The connection handler parses a decimal integer. Input 1 causes a KOBJ_CHANGE
uevent containing EPD_CONNECT=1; the handler also increments a counter after a
successfully parsed input. This is a userspace notification, not a full panel
initialization sequence by itself. The commit-bitmap handler emits
EPD_CONTROL=%d for values 1 through 3. The display-mode handler stores an integer;
these observations alone do not identify the active waveform.

Decoded ten device_attribute records from the raw kernel and crosschecked the
name pointers against symbols and the show/store callback addresses: epd_vcom,
epd_commit_bitmap, epd_connect, epd_force_clear, epd_display_mode, epd_display_type,
epd_white_threshold, epd_black_threshold, epd_contrast, epd_info. Their compiled
modes are 0644; Android init and SELinux can impose different runtime access.
No handler was invoked and no panel voltage/calibration value was changed.

### Separate SPI region and attempted backup

The epd_spi_read handler powers the EPD path, issues SPI command 03 with address
000000, reads a fixed 0x70080 bytes (458,880), powers the path down, and copies
that fixed length to the caller. It ignores the requested read length and file
position. Its success return is the copy_to_user residual (zero), not the byte
count. An ordinary small-buffer reader is therefore unsuitable. The separate
epd_read_vcom handler reads 256 bytes at flash address 0x070000; this also means
the exposed 0x70080-byte region is not established as a full-chip dump.

Prepared tools/a6l-epd-read.c, a standalone static AArch64 reader with the exact
buffer size and a single read. It uses only openat(O_RDONLY), read, close,
write-to-stdout/stderr and exit syscalls. It compiled with the existing
clang-r584948 toolchain and -Wall -Wextra -Werror; readelf confirms AArch64 and no
dynamic interpreter. Helper SHA-256:
60b09c8875ae32d3efaa38a030076842bc0363611ae77466fe069ff3726d9360.

tools/Capture-EpdRegion.py verified the phone's reported model/build, temporarily
pushed that helper to /data/local/tmp, and ran it over ADB shell-v2 without a
PTY. The stock device denied the read-only open; helper exit 10 and zero payload
bytes. No read or power cycle occurred. The temporary helper was removed.
Report: firmware/extracted/epd-spi-capture-20260914/report.json.

The separate e-ink SPI region is NOT backed up. No root or permission bypass was
attempted. Future work needs suitable access before any change that could affect
that region. The verified eMMC backup remains valid for its stated regions.

Read-only live visibility check: /sys/kernel/mirror is absent;
/sys/debug_control/mirror and chosen device-tree contents deny shell access.
The active tree/overlay and bootloader-added command line remain unresolved.

Private report: firmware/extracted/kernel-interface-audit.json. Related files:
stock.kallsyms, stock-symbolized.elf, kernel-eink-handlers.txt and
kernel-symbol-recovery.log.

## Concrete compatibility matrix

| Area | Captured kernel / firmware | Current source finding | Required work |
| --- | --- | --- | --- |
| CPU | AArch64 kernel | Current networking expects 64-bit kernel | Architecture matches; not a driver compatibility result |
| Networking | 4.4.153; BPF_SYSCALL and BPF_JIT disabled | This product uses API 37.0/REL and the 26Q2 init script; NetBpfLoad enforces >=5.10 | Suitable newer kernel or a separately verified compatibility/backport implementation |
| Data encryption | forceencrypt=footer; dm-crypt and old fscrypt options enabled | Current fstab parser explicitly rejects forceencrypt/forcefdeorfbe; old internal-storage FDE is unsupported | Design an FBE-capable data layout and verify keymaster/kernel support; plan any migration/wipe explicitly |
| Shared memory | ashmem enabled; ION/MSM enabled | libcutils retains conditional ashmem fallback; memfd path depends on kernel policy capability and vendor API | Audit actual callers and graphics buffer interfaces; absence of a newer option alone is not a proven failure |
| Binder/security | binder, hwbinder, vndbinder, SELinux, seccomp present | Presence of config options does not establish the required ABI/policy support | Validate userspace/kernel behavior and vendor policy compatibility |
| Loadable drivers | Six modules carry 4.4.153-perf vermagic and versioned symbols | Includes qca_cld3_wlan.ko | Rebuild matching driver source or supply a supported replacement for a new kernel; changing vermagic does not fix ABI differences |
| Crash logging | PSTORE disabled; pstore path absent live | Persistent kernel crash logs unavailable through this mechanism | Provide an early-boot logging strategy; assess reserved memory before enabling ramoops |
| E-ink | Hisense-specific MDSS, SPI flash and controls | Recovered named functions and callback/event contract | Preserve/reimplement hardware path and its userspace contract |
| Early init | Root comes from the system partition; saved boot ramdisk has no executable init | Modern init_second_stage aborts if started without a stage argument; first-stage executable is separate | Verify final root /init packaging and first-stage fstab path before any boot test |

The staged build now confirms root/init -> /system/bin/init. The source creating
that link is system/core/rootdir/create_root_structure.mk:150, and the selected
init_system phony module requires only init_second_stage. A direct kernel entry
into that program without stage arguments reaches the fatal path in main.cpp.
Proper first-stage setup is required; passing a second_stage argument alone
would omit the earlier mount/SELinux work. Whether to place first-stage init in
a boot ramdisk or support the existing root layout depends on the chosen boot
and kernel strategy. No early-init binary has been replaced yet.

The separate init_first_stage component subsequently built successfully in
2m42s. Its 3,321,920-byte ELF is static AArch64 with no interpreter/dynamic
segment and a non-executable stack; entry point and load bounds passed the
offline checks in tools/Inspect-FirstStage.py. Preserved binary and report:
firmware/extracted/first-stage-init-20260914/. SHA-256:
0a2cd37177179c8544d8e1b631c9faf90ca086fa745221640e30ea302f2578e2.
This supplies a component for boot integration; it has not been executed on the
phone or placed in a replacement boot/system image.

The >=5.15 gate is for API 37.1/26Q4, not this product's 37.0/26Q2 selection.
This matrix is an evidence-based starting point, not a complete compatibility
certification. Disabling a version gate does not provide missing kernel APIs.

Inspected source revisions:

- Connectivity: 7c04d866538e6f689823b769fb5c1e614ca0e295.
- system/core: 1375cc767964bbac2070bc41ebc3c864589846b4.
- system/fs/fs_mgr: 3b3419b35a4378779bbdb3819c4e90fc165c8368.
- system/vold: 11d8982f824d52e8122c7aa732207ba66b8f23c8.

Key source locations: NetBpfLoad.cpp:1551; BpfUtils.h:96 and :156;
Connectivity/bpf/loader/netbpfload.rc:1; libfstab/fstab.cpp:243;
system/core/init/main.cpp; system/core/init/Android.bp; libcutils/ashmem-dev.cpp.

## Source candidates checked

No matching A6L tree was found in these searches. Searches covered Hisense/HMCT
repository names, HLTE730T, sdm660_overlay, FB_HS_MDSS_EPD_PANEL,
mdss_epd_power_up and epd_spi_read_rdsr, plus Chinese web queries. Search-index
absence is not proof that unpublished or unindexed source does not exist.

| Candidate | Inspected revision / version | Assessment |
| --- | --- | --- |
| LineageOS/android_kernel_qcom_sdm660 | bbe3ff951f41f542d32e42a4b977fd6675a53d2a; 4.19.325 | Qualcomm reference; below unchanged runtime gate; no A6L support established |
| LineageOS/android_kernel_xiaomi_sdm660 | 49688d8750f2fca28271f936cfcfec5d97c0d7a4; 4.19.325 | Device-port reference, not an A6L donor |
| LineageOS/android_kernel_nokia_sdm660 | 84df9525b0c27f3ebc2ebb1864fa62a97fdedb7d; Makefile 4.19.0 | Newer Lineage branch name does not imply a newer kernel ABI |
| CrosscallEngineering/CoreX4-T4_kernel_msm4.9 | b78d7a05c012eae47b2e6df8311058eadd2b50fd; 4.9.217 | Contains substantial Hisense code and HS_MDSS_SPI support; no matching EPD handlers in inspected display/Hisense directories |
| sdm660-mainline/linux | e47d622cb6d2440a9eacdc8bb2df32c037bec7b8; 7.2.3 | A modern SoC source base exists; its SDM660 tree uses the upstream Adreno interface. A6L board support and compatibility with old vendor interfaces remain unproven |

The Crosscall sparse checkout is /home/a6l/references/crosscall-msm49. Only
selected display, Hisense, input and board/config directories were populated;
the whole-tree file-name listing was also inspected. No candidate was selected
as the port's kernel and none was compiled or installed in this investigation.

Next: use the recovered interfaces to scope the missing device changes; pursue
the exact Hisense source; compare feasible backport and newer-kernel strategies
before a kernel build. A technical source request is drafted separately, unsent.

## Primary references

- https://github.com/marin-m/vmlinux-to-elf/tree/34b064caf647d363f2257c0e246a321fddc31a6c
- https://github.com/CrosscallEngineering/CoreX4-T4_kernel_msm4.9/tree/b78d7a05c012eae47b2e6df8311058eadd2b50fd
- https://github.com/sdm660-mainline/linux/blob/e47d622cb6d2440a9eacdc8bb2df32c037bec7b8/arch/arm64/boot/dts/qcom/sdm660.dtsi
