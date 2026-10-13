# Hisense A6L device tree (LineageOS 24)

Device tree for the unofficial LineageOS 24 / Android 17 port to the Hisense A6L (HLTE730T, SDM660, LCD + rear e-ink).
Status, installation and build notes: the top-level [README.md](../../../README.md) and
[docs/port-status.md](../../../docs/port-status.md). History: [docs/handoff-20261005-claude.md](../../../docs/handoff-20261005-claude.md).

**The ROM boots and is usable as of 13 Oct 2026** (project paused):
- Round 29b userdebug, SELinux enforcing.
- A release-keys user build also exists.
- Open items: stability under load, VoLTE (blocked by the carrier), /data encryption.

## Layout

| Path | What |
|---|---|
| `lineage_gsi_a6l.mk`, `BoardConfig*.mk` | Product: LineageOS arm64 GSI system + the A6L vendor image (mainline-based Linux 7.2, prebuilt kernel and modules) |
| `rom/` | Vendor init scripts, fstab, sepolicy (enforcing), prebuilt modules (`rom/prebuilt/vendor/lib/modules`), the `user` variant, debug-only services (`rom/debug`, userdebug/eng only) |
| `kernel/` | Kernel patch records and device-tree overlays |
| `camera/` | libcamera (simple pipeline + VFE hardware ISP), IPA, Android HAL patches, tuning |
| `eink/` | E-ink mirror, panel daemon (`a6l_epdd`), LCD/e-ink switcher, lock clock |
| `audio/`, `kvoice/` | Audio HAL patches, mixer paths, routing daemon, voice-call daemon |
| `radio/`, `gnss/`, `wifi/`, `usb/`, `power/`, `hals/`, `sens/`, `video/` | Modem (QMI RIL, rmtfs/tqftpserv), GNSS, Wi-Fi, USB gadget, charger guard and power, other HALs, sensors, Venus/Codec2 |
| `diagnostic/` | Bring-up probes kept for reference; not part of the ROM |

Stock firmware and proprietary blobs are not in this repository. They are pulled from the developer's own stock backup at
build time (see the top-level README, "Building from source").
