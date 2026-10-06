# A6L Android USB device mode — r5 android-side prep (29 Sep 2026, offline)

No phone, no adb, no `m`. Everything below is source + offline checks; nothing has run on the A6L yet.

## Finding: the current ROM has no working USB gadget
`rom/init/init.a6l.usb.rc` (rom-v1) only set `sys.usb.configfs=1` + the controller and relied on the system
`init.usb.configfs.rc`. That file never creates `/config/usb_gadget/g1`, the functions or the FunctionFS mounts, and
links `mtp.gs0`/`ptp.gs1`/`accessory.gs2`, which do not exist on mainline. Result on r1–r5: no adb in Android, no MTP/PTP,
no tethering (it was never exercised: QEMU has no UDC, the phone tests used the recovery's adbd). Also, without the
android_usb uevents UsbDeviceManager never sees "connected", so MTP could not start even with a gadget.

## What is ready (device/hisense/a6l/usb, new area)
| Piece | File | Notes |
|---|---|---|
| Gadget skeleton | `rom/init/init.a6l.usb.rc` (rewritten) | early-boot: g1, ids 18d1:4ee7, serial `${ro.serialno:-A6L0000000}`, manufacturer/product from ro.product.*, os_desc (MSFT100), config b.1 (500 mA), functions ffs.adb/ffs.mtp/ffs.ptp/rndis.gs4 (+RNDIS os_desc compat id)/ncm.gs9/midi.gs5, FunctionFS mounts adb (shell) and mtp/ptp (mtp, no_disconnect), `sys.usb.controller=a800000.usb`, `sys.usb.configfs=2`. Early adb (before system_server) gated by `vendor.a6l.usb.hal=0`, with soft_connect (a6l_manual_usb). RAM-container guard kept (`androidboot.a6l.container=1` = hands off). |
| Gadget HAL | `usb/gadget/*` → `android.hardware.usb.gadget-service.a6l` (AIDL IUsbGadget **V2**, class hal) | Real configfs implementation (core `a6l_gadget_core.cpp`, no Android deps): teardown (UDC none, unlink), ids/class/config string, links, waits for FunctionFS endpoints (adb ep1-2, mtp/ptp ep1-3), binds UDC + `soft_connect connect`; monitor thread (inotify + poll fallback) re-binds after adbd/MTP restarts; superseded requests released; reset(); getUsbSpeed from `current_speed`; dump. Sets `vendor.a6l.usb.hal=1` on the first request (init early-adb rules stop). |
| Compositions | core `compose()` | charging-only (NONE = nothing bound), adb 4ee7, mtp 4ee1 / mtp+adb 4ee2, ptp 4ee5/4ee6, rndis 4ee3/4ee4 (IAD class EF/02/01), ncm 4eeb/4eec (IAD), midi 4ee8/4ee9. ACCESSORY/AUDIO_SOURCE/UVC/CTRL -> CONFIGURATION_NOT_SUPPORTED (no f_accessory/f_audio_source on mainline). Framework falls back to charging on errors. |
| MTP | framework (UsbDeviceManager opens `/dev/usb-ffs/mtp/ep0` + writes descriptors at boot; MediaProvider MtpServer uses MtpFfsHandle) | needs only the mounts + HAL above |
| USB state | `usb/overlay/.../config.xml` `config_enableUdcSysfsUsbStateUpdate=true` | flag `enable_udc_sysfs_usb_state_update` is ENABLED in this tree; genfs labels version 202604 > 202404 |
| Tethering | RNDIS default (`usb0` matches `usb\d`); NCM iface also `usb%d` | |
| sepolicy | `usb/sepolicy/vendor` (in the default `A6L_SEPOLICY_DIRS`) | binary label, `vendor.a6l.usb.` prop, functionfs watch, sysfs_udc (bind/soft_connect/state), `usb_control_prop` read. genfs path `/devices/platform/soc@0/a8f8800.usb/a800000.usb/udc` **UNVERIFIED** |
| Wiring | `rom/rom.mk` inherits `usb/usb.mk`; `tools/rom-v2-pipeline.sh` syncs + CRLF-normalises `usb/` (hals/usb-gadget stub stays excluded) | lands in the NEXT build (r5 prep already ran) |

Not provided (deliberate): IUsb port HAL (no Type-C class device, usb_nop_phy, USB2 only: UsbPortManager runs without
it, no role-swap UI); `android.hardware.usb.accessory` feature; MIDI until `usb_f_midi.ko` (CONFIG_USB_F_MIDI=m) is built
and staged (kernel area; the rc mkdir then succeeds and the HAL offers it); UVC.
Policy note: `rom.mk` still ships `persist.sys.usb.config=adb` (adb on by default, also on user builds; ro.adb.secure
still requires the RSA prompt) — decide before release.
Interaction: power29 (not merged) disconnects the gadget on DCP for HVDCP; the HAL re-pulls D+ on every (re)bind.

## Checks run (all PASS)
* `usb/tests/run-host-tests.sh` — A6L_USB_GADGET_TEST 123/123 x3 (ASan/UBSan) + TSan clean: compositions, missing
  descriptors -> ERROR then monitor bind, soft_connect, switch mtp/rndis/ptp/ncm/midi, adbd restart re-bind, reset,
  NONE, unsupported, superseded request, UDC fallback / missing UDC.
* `usb/tests/check-usb-syntax.sh` (WSL, tree clang r596125, gadget-V2-ndk headers, -Wall -Wextra -Werror) — A6L_USB_SYNTAX PASS.
* `usb/tests/check-usb-wiring.sh` — A6L_USB_WIRING PASS 40/40.
* `tools/release/check-a6l-sepolicy.sh {user,userdebug} usb/sepolicy/vendor` — PASS (r5 conf already contains rom/sepolicy/vendor).
* `rom/tests/test-rom-static.sh` — PASS.

## Attended tests after the first install (permissive)
1. `getprop | grep -E "sys.usb|vendor.a6l.usb"`: configfs=2, controller=a800000.usb, vendor.a6l.usb.hal=1;
   `ls /config/usb_gadget/g1/functions` (midi.gs5 absent is expected); `mount | grep functionfs` (3 mounts).
2. Laptop `adb devices` in the first minute of boot (early adb) and after boot_completed (HAL); `dumpsys usb` and
   `lshal`/`service list | grep gadget`; `logcat -s android.hardware.usb.gadget-service.a6l UsbDeviceManager`.
3. Settings > USB preferences: File transfer -> Windows/Linux shows the A6L (MTP, 18d1:4ee2), copy a file both ways;
   PTP (4ee6); No data transfer (device disappears, adb stays only if enabled); USB tethering (host gets `usb0`/RNDIS
   address, ping through Wi-Fi/mobile); toggle adb off/on (monitor re-bind).
4. `readlink -f /sys/class/udc/a800000.usb` + `ls -lZ` -> fix `usb/sepolicy/vendor/genfs_contexts` if the path differs;
   `dmesg | grep -i "A6L_USB\|dwc3\|configfs"`; `cat /sys/class/udc/a800000.usb/state` changes with the cable.
5. Serial: `adb devices` shows androidboot.serialno (not A6L0000000); if it falls back, the captured ABL does not pass
   it -> decide a serial source.
