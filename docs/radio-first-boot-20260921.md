# Modem / Wi-Fi / Bluetooth first attended run — 21 Sep 2026 (V71, approved by Pierre at run time)

Phone back on stock afterwards; laptop services resumed. Logs: laptop `v71/logs/dmesg-radio.txt`, `tqftpserv.log`, `rmtfs.log`.

## Modem: BOOTS
- `rmtfs` ran on RAM copies of modemst1/modemst2/fsg/fsc (read-only dd; live partitions never opened for writing).
- Bundle gaps fixed by hand: `qrtr.ko` + `qrtr-smd.ko` were missing from the modem area (`AF_QIPCRTR` unsupported →
  q6v5-mss "failed to initialize qmi handle" -517); `/dev/qcom_rmtfs_mem1` must be mknod'ed from sysfs; the remoteproc
  does not auto-boot after the deferred probe (`echo start`).
- MBA + mpss load, "remote processor is now up", **all modem QMI services register** (NAS, UIM, voice, WMS, WDS, DMS,
  location, IPA control, thermal, …) and RMTFS/TFTP traffic flows.
- ~16 s later: `fatal error: dog_hb.c:266: Task starvation: diag` → crash/restart loop. Cause: nobody services the modem's
  DIAG channels. My minimal drain tool (`diagnostic/diag_drain.c`, rpmsg_ctrl endpoints) is NOT the answer: opening the
  eptdevs returns EINVAL and creating them triggered `glink_channel_migration.c:602` assertion in the modem.
  → port the real `linux-msm/diag` diag-router (needs rpmsg/udev bits replaced for Android) or find the kernel-side option.
- tqftpserv needs these paths: `/data/vendor/tmp/tqftpserv`, `/readwrite/`, `/readonly/firmware/image/` (wlanmdsp.mbn,
  modem_pr/mcfg/...), `/readonly/vendor/firmware/`. It asked for `wlanmdsp.mbn` in both readonly locations and for
  `modem_pr/mcfg/configs/mcfg_{hw,sw}/mbn_*.dig` (from the stock modem partition image) and `/readwrite/ota_firewall/ruleset`.

## Wi-Fi: not reached
`ath10k_snoc` probes and waits for the WLAN firmware service, which only appears once the modem has loaded `wlanmdsp.mbn`
through tqftpserv and stays up. Blocked on the diag crash + tqftp paths.

## Bluetooth: no hci0
`hci_uart` + QCA protocol load, but there is no serdev device: **`msm_serial c1af000.serial` fails to probe with -22 at boot**
(uartclk = 19200000). Fix the UART node first (check a6l-bluetooth.dtso against sdm630.dtsi blsp2_uart1: clocks, dmas,
pinctrl) — offline.

## Side note
The laptop dropped off the LAN (no ping/SSH) for ~10 min during the modem crash loop and came back after Pierre re-joined
the Wi-Fi. Possibly coincidence; possibly RF/USB related. Watch for it next time.
