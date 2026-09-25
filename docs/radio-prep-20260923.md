# Radio prep for the next attended test — 23 Sep 2026 (agent `radio`, offline only)

**Status:** everything below was prepared OFFLINE. Nothing ran on the phone. Modem, Wi-Fi and Bluetooth steps are
RF-capable and **need Pierre's explicit go** (`A6L_RF_APPROVED=1`).
Inputs: `docs/radio-first-boot-20260921.md`, laptop `v71/logs/dmesg-radio.txt`, `tqftpserv.log`, `rmtfs.log`,
`v71/bundle/{modem-wifi,bluetooth}`, stock firmware `firmware/extracted/peripheral-firmware-20260917-r3/modem/IMAGE`, stock DT.

## 1. Modem: "dog_hb.c:266: Task starvation: diag"

### What the 21 Sep logs show
- Without any DIAG client (control run): "remote processor is now up" at 199.41 s → `fatal error … Task starvation: diag`
  at 215.59 s, then the same every ~18 s (crash #1…#3). Only three rpmsg devices were bound on the modem edge:
  `rpmsg_ctrl.0.0`, `glink_ssr.-1.-1`, `IPCRTR.-1.-1`. A channel the modem opens and no driver claims is registered
  silently (no log line), so the log cannot show whether the modem opened `DIAG*`.
- With `diag_drain.c` (started after the restart at 335.28 s): `rpmsg rpmsgN: failed to open DIAG_CNTL` at 340.96,
  `DIAG_DCI_CNTL` 346.08, `DIAG` 351.20, `DIAG_DCI` 356.32, `DIAG_CMD` 361.44 (**exactly one every 5.1 s**), then
  `glink_channel_migration.c:602: Assertion status == GLINK_STATUS_SUCCESS failed` at 362.86.

### Why diag_drain got EINVAL and set off the assertion
1. `RPMSG_CREATE_EPT_IOCTL` on `rpmsg_ctrl` only creates an eptdev. The `open()` calls `rpmsg_create_ept()` on the
   **control** device. In `qcom_glink_create_ept()` a name that is not in the remote-opened list (`rcids`) goes through
   `qcom_glink_create_local()`: **Linux sends the OPEN first** and waits 5 s for `open_ack` and 5 s for the remote open.
   On timeout `create_ept` returns NULL, and `rpmsg_eptdev_open()` turns any NULL into **-EINVAL**
   (`drivers/rpmsg/rpmsg_char.c`: "failed to open %s" → `return -EINVAL`). The 5.1 s spacing matches that timeout.
   So EINVAL means "the modem never acked our OPEN". It does not mean a bad argument.
2. Mainline glink sends OPEN with `param2 = name_len` only. Qualcomm's own glink packs the requested transport in the
   high 16 bits (`req_xprt`, SMEM = 100 in the downstream `xprt_ids`). On an OPEN that **Linux starts**, the modem's
   channel-migration code must pick a transport from `req_xprt = 0`, fails, and asserts
   (`glink_channel_migration.c:602 status == SUCCESS`). The drain opened five channels this way (two of them, DCI, the
   modem might not even serve), which fits "no ack, then assert". *(Inference from the log and the protocol layout.
   The modem source is not available.)*
3. Channels that **the modem opens first** go through `qcom_glink_create_remote()` (open_ack + OPEN), and that path
   **already works on this modem**: IPCRTR and glink_ssr are both remote-first (`-1.-1` devices) and came up on every boot.

### Chosen fix (plan A): linux-msm `diag-router`, Android/sysfs port, remote-first only
- Upstream: `https://github.com/linux-msm/diag` @ `23c12c1` (BSD-3). postmarketOS ships it for SDM660 (`qcom-diag` is
  part of `soc-qcom-sdm660-rproc`, pmaports MR !4897). Upstream finds channels through **libudev** (enumerate +
  netlink monitor) and a udev rule that runs `rpmsgexport` (the same ctrl-ioctl pattern as diag_drain).
- A6L patch (`firmware/extracted/radio-20260923/src/diag-router-a6l-sysfs.patch`) replaces udev with a 1 s sysfs scan:
  1. for each `/sys/bus/rpmsg/devices/*` named `DIAG`, `DIAG_CNTL` or `DIAG_CMD` on the `modem` edge that has no driver:
     `driver_override=rpmsg_chrdev` + bind. Binding runs the rpmsg core probe → `create_ept` on the channel's own
     rpdev → **create_remote path, the same path IPCRTR uses**. Linux never starts a channel open;
  2. picks up the resulting `/sys/class/rpmsg/rpmsgN` eptdevs (edge name from the glink `rpmsg_name` attribute),
     mknods `/dev/rpmsgN` if devtmpfs has not created it, and opens the peripheral once both DIAG and DIAG_CNTL exist.
     Upstream opens once, 1 s after DIAG appears, and gives up if DIAG_CNTL is missing then. The port drops a failed
     peripheral so the next scan retries it;
  3. forgets stale nodes after a modem restart. DCI channels are not touched. Edge filter: env `A6L_DIAG_EDGES`
     (default `modem`, so the ADSP, which works today, is left alone).
- Kernel needs, all present in the V71 kernel (`out-a6l-phone-v67/.config`): `RPMSG_CHAR=y`, `RPMSG_CTRL=y`,
  `RPMSG_QCOM_GLINK_SMEM=y`, `QRTR=m`, `QRTR_SMD=m`. HAS_LIBQRTR is off: SDM660 modem DIAG runs over glink, not QRTR.
- What it sends to the modem: the normal DIAG control handshake (feature mask, empty log/msg/event masks). This is what
  the stock `diag` driver does. Data from the modem is read and discarded (no USB `ffs-diag`, unix socket `\0diag` only).
- Build: NDK r27c, `aarch64-linux-android34-clang -static`. Host gcc build of the same patch also compiles.

### Plan B (kernel side): `a6l_diag_sink.ko`
An rpmsg driver matching `DIAG`/`DIAG_CNTL`/`DIAG_CMD` (edge filter `edges=modem`). The core probe acks the modem's open
(create_remote) and the callback discards everything. It sends no data and never starts an open. Use it if diag-router
misbehaves. Risk: the modem might want the CNTL feature handshake. Then only plan A helps.

### Plan C (not built, only if the modem never opens DIAG itself)
If the rpmsg snapshot shows **no** `DIAG*` device on the modem edge, the modem expects Linux to open first. Then Linux
must send the downstream transport field. Candidate kernel patch (glink is built in, so a new recovery kernel is needed):
`qcom_glink_send_open_req(): req->param2 = cpu_to_le32(name_len | (100 << 16))` for SMEM edges, plus reading
`xprt_resp` in the open-ack. Only worth doing after the test below.

## 2. Wi-Fi: tqftpserv layout

**Key finding:** the pinned tqftpserv (`c2559a2`, Android build) does not serve literal `/readonly/...` directories.
`translate.c` maps `/readonly/firmware/image/<f>`, `/readonly/firmware/modem_pr/...`, `/readonly/vendor/firmware/<f>` and
`/readonly/vendor/firmware_mnt/image/<f>` to `<firmware_class.path>/<dirname(remoteproc firmware)>/<f>`, then
`/vendor/firmware/updates/…`, then `/vendor/firmware/<dirname>/<f>`. `/readwrite/` maps to `/data/vendor/tmp/tqftpserv`.
On 21 Sep `firmware_class.path` was empty and `/vendor/firmware/qcom/hisense/a6l/` did not exist. So
`wlanmdsp.mbn`, which was already in `/lib/firmware/qcom/hisense/a6l/`, was never found. Fix in run.sh: write `/lib/firmware`
to `/sys/module/firmware_class/parameters/path` (only if empty; the kernel already searches that directory by default).

| Modem request (21 Sep) | Served from (v74) | Source |
|---|---|---|
| `/readonly/firmware/image/wlanmdsp.mbn`, `/readonly/vendor/firmware/wlanmdsp.mbn` | `/lib/firmware/qcom/hisense/a6l/wlanmdsp.mbn` (already in the V71 bundle; sha `26af61da…` = stock `WLANMDSP.MBN`) | stock modem IMAGE |
| `…/modem_pr/mcfg/configs/mcfg_{hw,sw}/mbn_{hw,sw}.dig` | `/lib/firmware/qcom/hisense/a6l/modem_pr/…` **lowercased**, 155 files, 7.0 MB | stock `MODEM_PR/` (vfat names upper case) |
| `/readwrite/mcfg.tmp`, `server_check.txt` | `/data/vendor/tmp/tqftpserv` = **tmpfs** mounted by run.sh (refuses if `/data` is a block device) | — |
| `/readwrite/ota_firewall/ruleset` | **missing** (stock content unknown, read-only probe, rejected harmlessly on 21 Sep) | — |
| bdwlan (if the WLAN FW asks through TFTP) | `/lib/firmware/qcom/hisense/a6l/bdwlan.*` (lowercase copies of all 26 stock files) | stock IMAGE |

ath10k host side:
- `firmware-5.bin` from the V71 bundle is kept.
- **board-2.bin replaced.** The V71 bundle's copy was linux-firmware's Chromebook file (`qmi-board-id=67` variants plus a
  generic `ff`), not A6L calibration. The new `board-2.bin` (sha `8f74bab4…`) is built by `src/make-board2.py` from the
  stock BDWLAN files: `BDWLAN.BIN` → `bus=snoc,qmi-board-id=ff`, `BDWLAN.Bxx` → id `xx`, `BDWLAN.1xx` → id `1xx`.
  ath10k prints the real id ("qmi … board_id 0x…"). If it asks for an id that is not in the file, the fix is adding one name.
- Other fixes over the V71 bundle: `qrtr.ko` + `qrtr-smd.ko` added (same kernel, vermagic `7.2.3-a6l-probe+`) and loaded
  before the modem; `/dev/qcom_rmtfs_mem1` mknod'ed from sysfs (21 Sep rmtfs: "failed to open /dev/qcom_rmtfs_mem1");
  explicit `echo start`.
- `iw` = Lineage out `system/bin/iw` + `libnl.so` + `libc++.so` in `bin/` (dynamic bionic, run with
  `LD_LIBRARY_PATH=$D/bin` like tqftpserv). **Untested in recovery.**

## 3. Bluetooth: `msm_serial c1af000.serial` probe -22

**Cause: the serial line collides with the console. Clocks, DMA, compatible and pinctrl are fine.**
`msm_serial_probe()` takes the line from `of_alias_get_id(np, "serial")`. The recovery DT only has `serial0 = &blsp1_uart2`
(console, ttyMSM0). `serial@c1af000` has no alias, so it gets `atomic_inc_return(&msm_uart_next_id) - 1` = **0**. The
clocks resolve ("uartclk = 19200000" is printed), then `uart_add_one_port()` finds line 0 taken (`state->uart_port` set) and
returns **-EINVAL**. Mainline boards that use blsp2_uart1 for BT (Sony nile) have **no** serial aliases at all, so both
ports get dynamic lines. The A6L mixes an aliased and an unaliased port. Side effect: the failed probe had already written
its `dev/clk/mapbase/irq` into line 0's static `msm_port` (the console's). A fixed alias also avoids that.
The node itself matches `sdm630.dtsi` and stock: stock BT is on `uart@c1af000` (`blsp2_uart1_hs`, `bluetooth = /bt_wcn3990`),
the same BLSP2 BAM pipes 0/1 and the same GCC clock.

- **DT fix:** `device/hisense/a6l/kernel/a6l-bluetooth-v74.dtso` ("B2") = B1 + `/aliases/serial1 = "/soc@0/serial@c1af000"`
  + chosen marker `b2`. Checked with dtc + fdtoverlay (no image built):
  - A: v74 overlay on the V71 `base.dtb` merges; the only differences are `serial1` and the marker
    (`check-v71base+bt-v74.dtb` sha `b1b4dfbb…`, dtbo sha `3f998613…`).
  - B: V68 base + the eight V71 overlays with bluetooth-v74 in place of bluetooth merges; the same two differences, plus
    `xon-gpios` in one node that comes from a later change to another overlay (not radio).
- **Runtime workaround for the current V71 (no reflash):** the counter has already moved on, so
  `echo c1af000.serial > /sys/bus/platform/drivers/msm_serial/bind` should probe as line 1 and create the serdev → hci_uart/QCA
  → `hci0`. `run-bt.sh` does this. Firmware `qca/crbtfw*.tlv`, `crnv*.bin` from `controls-radio-prep-20260917`.

## Artifacts
Repo `firmware/extracted/radio-20260923/`:
| file | sha256 |
|---|---|
| bin/diag-router (static aarch64, A6L sysfs port) | dcd7adc8b236b3b298069eaae5e2d8728781ba8d1bd5ac3bab0d4ce6ab6a6470 |
| bin/a6l_diag_sink.ko | e2f6ee9dc5ca84db86a177187b7018de89919d6142c5d467ce1a0c1167b4b48f |
| bin/qrtr.ko / bin/qrtr-smd.ko | 69083d1f… / e5593d9d… |
| bin/board-2.bin | 8f74bab43b2c3a5e69a5202b1dcae5f4ea0788a849b3773962b684d7491c5285 |
| bin/run.sh / bin/run-bt.sh | a76c6317… / 900c0fb6… |
| bin/iw/{iw,libnl.so,libc++.so} | f161bf8b… / 10bc9a68… / (see SHA256SUMS on laptop) |
| v74-radio-overlay.tgz | f1c528ba7de825b6a66388b7d5b8141e32281d8b50c4e227976994e3a9fd6a8c |
| a6l-bluetooth-v74.dtbo, check-v71base+bt-v74.dtb | 3f998613… / b1b4dfbb… |
| src/ (diag-src.tgz, patch, diag-sink/{a6l_diag_sink.c,Kbuild}, make-board2.py, run*.sh) | — |
Build: `tools/build-radio-v74.sh` (build + laptop staging). New overlay: `device/hisense/a6l/kernel/a6l-bluetooth-v74.dtso`.
Laptop: `~/A6L-usb-20260915/v74/radio/` = V71 `modem-wifi` modules/bin/firmware + `modules-bt/` (V71 bluetooth modules) + the
above, 75 MB, `SHA256SUMS` regenerated (259+ entries). The V71 bundle was only read. Its old board-2.bin was kept as
`v74/radio-v71-board-2.bin`.

## Attended procedure (needs Pierre's explicit go for every RF step)
Precondition: phone in V71 recovery, ADB up, sdhci-msm loaded (the modemst partitions are visible). SIM/antenna state as Pierre decides.
1. Push: `adb push ~/A6L-usb-20260915/v74/radio /tmp/radio`.
2. Modem + Wi-Fi: `adb shell 'A6L_RF_APPROVED=1 D=/tmp/radio sh /tmp/radio/run.sh'` (defaults: `A6L_DIAG_MODE=router`, `A6L_WATCH=90`).
   Expected, in order: `A6L_STEP storage-guard-ok`, 4× `A6L_RMTFS_COPY`, `A6L_FW_CLASS_PATH=/lib/firmware`, `ls` of the three
   tqftp files, `A6L_STEP qrtr-rmtfs-mem-ok`, `A6L_STEP modem state=running`, then **`A6L_RPMSG … name=DIAG driver=rpmsg_chrdev`**
   (the key line). diag-router.log shows `A6L_DIAG bound…`, `A6L_DIAG found /dev/rpmsgN = DIAG…`, `A6L_DIAG peripheral modem open`.
   Then `A6L_MODEM_WATCH … crashes=0` every 5 s, **`A6L_MODEM_STABLE_PASS 90s`**, qrtr-lookup (look for service 69 = WLAN FW),
   **`A6L_WLAN0_PASS`**, and BSS/SSID lines from the scan (no association).
3. Read the result:
   - `A6L_RPMSG` lists **no DIAG** devices and the modem still crashes → the modem waits for Linux to open first → plan C (kernel).
     Stop.
   - DIAG bound but crashes continue → retry once with `A6L_DIAG_MODE=sink` after a reboot to V71. Collect `/tmp/diag-router.log`.
   - `A6L_MODEM_STABLE_PASS` but no wlan0 → check tqftpserv lines for `wlanmdsp.mbn` (it should be served, not "reject"), and ath10k
     lines "qmi … board_id" / "failed to fetch board data".
   - `A6L_DIAG_MODE=none` is the control run (expect the crash at ~16 s).
4. Bluetooth (separate go): `adb shell 'A6L_RF_APPROVED=1 D=/tmp/radio sh /tmp/radio/run-bt.sh'` → `A6L_BT_UART unbound -> re-bind`,
   `A6L_BT_UART_BOUND_PASS`, `A6L_BT_HCI0_PASS`. It only reads the controller version: no scan, no pairing.
5. Stop: `echo stop > /sys/class/remoteproc/remoteprocN/state`, kill rmtfs/tqftpserv/diag-router, `umount /data/vendor/tmp/tqftpserv`.
   Reboot to stock as usual. The RAM EFS copies are thrown away.

What Pierre should watch: the laptop Wi-Fi/LAN link (it dropped for ~10 min during the 21 Sep crash loop). Any unexpected
network or USB drop means stop the modem.

## Risks
- The modem runs real RF firmware and may register on a network if a SIM is inserted. Wi-Fi scan transmits probe requests.
- The `firmware_class.path` write is global until reboot, but harmless (same directory).
- diag-router sends control messages to the modem, like stock diag. The sink module never sends.
- `iw` and `libc++.so` from the Lineage out dir are untested in recovery (the linker warnings seen on 21 Sep are harmless).
- run-bt.sh powers the WCN3990 BT rails through hci_qca. The rails are the ones already described in M1/S1.
- The EINVAL/assertion explanation (req_xprt) is an inference from the logs and the protocol layout, not proven.

## Still unknown
- Whether the modem opens DIAG itself (the `A6L_RPMSG` snapshot answers this at once).
- The WCN3990 board id on this phone (ath10k prints it). Whether the WLAN FW wants bdwlan through TFTP or from ath10k QMI (both provided).
- Whether `/readwrite/ota_firewall/ruleset` matters (it did not stop the modem on 21 Sep).
- The next recovery build should include `a6l-bluetooth-v74.dtso` instead of `a6l-bluetooth.dtso` (the Prepare-Recovery owner decides).

Sources: linux-msm/diag (github.com/linux-msm/diag), linux-msm/tqftpserv @ c2559a2, torvalds/linux master
`drivers/rpmsg/{qcom_glink_native,rpmsg_char}.c`, `drivers/tty/serial/msm_serial.c`, `drivers/net/wireless/ath/ath10k/core.c`,
`arch/arm64/boot/dts/qcom/{sdm630.dtsi,sdm630-sony-xperia-nile.dtsi}`, postmarketOS pmaports MR !4897 (xiaomi-lavender:
"soc-qcom-sdm660-rproc was extended with rmtfs, qcom-diag and bootmac").
