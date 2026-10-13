# Modem DIAG fix: diag-router over QRTR (agent `radio2`, 23 Sep 2026, offline only)

**Status:** built and staged OFFLINE. Nothing ran on the phone. The modem and Wi-Fi steps are RF-capable and
**need Pierre's explicit go** (`A6L_RF_APPROVED=1`). Builds on `docs/radio-prep-20260923.md` (agent `radio`).

## 1. Root cause: SDM660 modem DIAG runs over QRTR (IPC router), not glink

Today's facts: diag-router printed `cannot open device folder /dev/ffs-diag`, **no `DIAG*` rpmsg device appeared** on
the modem edge (only DATA*, DS, IPCRTR, LOOPBACK_CTL_MPSS, apr_*, glink_ssr, rpmsg_ctrl), and the modem died at about
100 s with `dog_hb.c:266 Task starvation: diag`.

- **The `ffs-diag` message is not fatal.** In `router/usb.c` `ffs_diag_init()` only `warn()`s and `diag_usb_open()`
  returns -1. `main()` goes on (unix socket, `peripheral_init()`, `watch_run()`). The only ways the @23c12c1 build (sysfs
  port) exits early are `err()`/`errx()` calls: unix socket bind (for example a second diag-router already holding
  `\0diag`), eventfd or `io_setup`. `CONFIG_AIO=y` and `CONFIG_EVENTFD=y` are set in `out-a6l-phone-v67`. So "exited
  immediately" is not proven: Pierre's terminal output was not saved (`v74/logs` only has tinymix). Either way it made no
  difference, because the build had **`HAS_LIBQRTR` off** and so could only serve glink channels that never came.
- **Downstream SDM660 (msm-4.4) diag uses sockets for the modem.** Sources: LineageOS
  `android_kernel_xiaomi_sdm660` lineage-17.1 `drivers/char/diag/`, and the stock kallsyms, which contain
  `diag_socket_*`, `diag_smd_*` and `diag_glink_*`.
  - `diagfwd_peripheral_init()`: `diag_smd_init(); if (supports_sockets /* =1 */) diag_socket_init(); diag_glink_init();`
  - `diagfwd_close_transport()`: glink is used **only for PERIPHERAL_WDSP**. MPSS/LPASS use SOCKET, with SMD as the
    alternative.
  - `diagfwd_socket.c`: `DIAG_SVC_ID 0x1001` (4097), modem instance base 0. **The apps side is the SERVER** for
    CNTL (inst 0), DATA (inst 2) and DCI (inst 4), and the **CLIENT** for CMD (1) and DCI_CMD (3).
  So the modem's diag task looks up QRTR service 4097 and **waits for Linux to publish it**. No one did, so the
  watchdog fired. Nobody has to open a DIAG glink channel: the modem never announces one. The 21 Sep
  `rpmsg_ctrl CREATE_EPT` of DIAG* made Linux open glink channels the modem does not serve, which set off the
  `glink_channel_migration.c:602` assertion. **Never do that again.**
- linux-msm diag already implements exactly this in `router/peripheral-qrtr.c`: it publishes 4097/{0,2,4}, looks up
  4097/1, and packs the DIAG instance into the "version" byte so that the wire instance equals the downstream `ins_id`.
  postmarketOS builds `qcom-diag` with libqrtr and added it to `soc-qcom-sdm660-rproc` for this reason (pmaports MR !4897,
  the xiaomi-lavender port). A web search for "Task starvation: diag" found no public hits, and gitlab pages were blocked
  from here.
- **Plan B (`a6l_diag_sink.ko`) and plan C (glink OPEN `req_xprt` patch) are both dropped.** They act on glink DIAG
  channels that do not exist on this modem. The DT and remoteproc need no changes: IPCRTR on glink already works
  (`qrtr-smd` bound, all QMI services came up today). A kernel-only alternative would be an in-kernel QRTR 4097 server,
  which is more code and more risk than the userspace router.

## 2. What was built

`firmware/extracted/radio2-20260923/`
| file | sha256 |
|---|---|
| `bin/diag-router` (static aarch64 bionic, NDK r27c; diag @23c12c1 + radio sysfs patch + **qrtr patch**, libqrtr @29e36ae linked in) | `bf32c0966e6af0a71d0c1b2b81b40f84457b23c6efd95c7be7084f40bf20bec4` |
| `bin/run.sh` (= `src/run.sh`) | `d4be735940341e3ef22cfcc3de3e2b38543b963a352a6f906854e5e907265993` |
| `src/diag-router-a6l-qrtr.patch` (applies on top of `radio-20260923/src/diag-src.tgz`) | - |
Build + staging: `.relay/inbox/radio2-04-build.sh` (a host gcc link of the same sources also passed).

Patch contents:
- `HAS_LIBQRTR=1`. The QRTR transport serves only the subsystems listed in env `A6L_DIAG_QRTR` (default `modem`;
  `all` = upstream/stock behaviour). The glink sysfs scan from `radio` is kept: it only binds DIAG channels that the
  remote announced, which on this modem is none.
- `/dev/ffs-diag` is opened only if it exists (otherwise the log says "running without a USB host client"). A failure
  of the unix `\0diag` socket is no longer fatal. stdout is line-buffered.
- ENETRESET (modem restart/SSR) and decode errors no longer drop the socket watch, so the router survives a modem
  restart. A failing CMD `connect()` no longer calls `err(1)`.
- Log lines: `A6L_DIAG qrtr modem: published service 4097 instances 0/2/4, looking up 1`,
  `A6L_DIAG qrtr modem CNTL client N:P connected`, `A6L_DIAG qrtr modem CMD server N:P, connecting`, `[modem] mask: ...`
  (feature handshake), `... reset`.

Laptop bundle `~/A6L-usb-20260915/v74/radio2/` = `cp -a v74/radio` plus the new `bin/diag-router` and `run.sh`.
SHA256SUMS was regenerated (260 entries, `sha256sum -c` OK, sha of the list `04ad110f…`, 77 MB). The old binary is kept as
`v74/radio2-incoming/diag-router.radio-v1`.

run.sh changes against `radio`:
- if `modemst1` is missing it runs `insmod /sdhci-msm.ko` and waits up to 20 s;
- kills stale `bin/rmtfs|tqftpserv|diag-router` and records the PIDs;
- starts diag-router **before** the modem. It checks the router is still alive after 2 s, prints its log and our own
  4097 servers (qrtr-lookup), and **refuses to start the modem** if the router died (`A6L_HW_FAIL ... exit 13`);
- default `A6L_WATCH=300`. Every 5 s it prints `A6L_MODEM_WATCH t= state= crashes= diag=alive`. At 20 s it prints the
  diag handshake lines and the modem's 4097 servers;
- Wi-Fi: prints `A6L_WLAN0_MAC` and `A6L_PERSIST_WLAN_MAC` (see §3). `A6L_WLAN_MAC=persist|xx:..` sets the MAC before
  `up`;
- at the end: logs go to `/tmp/radio2-logs/` and then `stop_all`: `echo stop` to the mss remoteproc, kill
  rmtfs/tqftpserv/diag-router, umount the tqftp tmpfs (`A6L_STOPPED ...`). `A6L_KEEP=1` skips this. A trap on
  INT/TERM/HUP runs the same stop.

## 3. Wi-Fi MAC

The stock WLAN MAC is in **`/persist/wlan_mac.bin`**, a text file (`Intf0MacAddress=XXXXXXXXXXXX`, then Intf1..).
Stock `vendor/bin/diag_ext` writes it ("write wifi addr to /persist/wlan_mac.bin for FEATURE_SDM660",
`/mnt/vendor/persist/wlan_mac.bin`, from NV / `persist.sys.wifi.addr`). `ftmdaemon` writes it in the same format.
qcacld (`qca_cld3_wlan.ko`) loads it as firmware `wlan/qca_cld/wlan_mac.bin` (`vendor/firmware/wlan/qca_cld/` is the
symlink location; the extracted copy is empty). `WCNSS_qcom_cfg.ini` has only placeholder `000AF58989FF`. The stock DT
has no `local-mac-address`, and ABL does not add one. Mainline ath10k takes the MAC from the firmware or from
`device_get_mac_address()` (the DT property), otherwise it picks a random one.
- **Test now (run.sh):** read-only `dd` of the `persist` partition to RAM, `grep -a` for the `Intf0MacAddress`
  string, no mount, then delete the image. With `A6L_WLAN_MAC=persist`: `ip link set wlan0 address` (or
  `ifconfig hw ether`) while wlan0 is down.
- **Lineage:** a small vendor early-boot script, bootmac-style (pmOS does the same for SDM660), reads
  `/mnt/vendor/persist/wlan_mac.bin` and sets `wlan0` before the Wi-Fi HAL starts. The BT address is in
  `persist.sys.bt.addr` / `/persist` and can be handled the same way (btmgmt public-addr). A per-unit DT property is not
  suitable for a shared image.

## 4. Attended test (needs Pierre's explicit RF go)

Phone: fresh V71 recovery, ADB up. From the laptop shell in `~/A6L-usb-20260915`, **one line**:
```
adb -s HLTE730T-PROBE push v74/radio2 /tmp/radio2 && adb -s HLTE730T-PROBE shell 'export PATH=/tmp/bin:$PATH; export A6L_RF_APPROVED=1; export D=/tmp/radio2; export A6L_WLAN_MAC=persist; sh /tmp/radio2/run.sh' 2>&1 | tee v74/logs/radio2-$(date +%H%M).txt
```
(About 7 min: 300 s watch + scan + stop. `/tmp/radio2` must not exist yet, or adb nests the directory.)

Expected output:
1. `A6L_STEP emmc partitions after Ns` (if sdhci was not loaded yet), 4× `A6L_RMTFS_COPY`, `A6L_STEP qrtr-rmtfs-mem-ok`.
2. `A6L_DIAG_ROUTER_PID=… alive=yes`, then the log: `A6L_DIAG no /dev/ffs-diag…`,
   `A6L_DIAG qrtr modem: published service 4097 instances 0/2/4, looking up 1`, `A6L_DIAG router running (qrtr on, …)`.
3. `A6L_STEP modem state=running`. At t=20 s **the key lines**: `A6L_DIAG qrtr modem CNTL client 1:N connected`,
   `[modem] mask: …`, `A6L_DIAG qrtr modem CMD server 1:N, connecting`, and qrtr-lookup rows for 4097 on node 1.
4. `A6L_MODEM_WATCH … crashes=0 diag=yes` through t=300 s, then **`A6L_MODEM_STABLE_PASS 300s`**.
5. `A6L_WLAN0_PASS`, `A6L_PERSIST_WLAN_MAC xx:…`, `A6L_WLAN0_MAC_SET …`, scan BSS lines. Then
   `A6L_STOPPED modem state=offline …` and `A6L_RADIO_DONE mode=router crashes=0`.

Reading the result:
- No `CNTL client … connected` and a crash at about 100 s: the modem does not reach our QRTR server. Check the
  qrtr-lookup rows (are our 4097 servers listed? does node 1 appear at all?). Next candidate: `A6L_DIAG_QRTR=all`.
- CNTL connects but it still crashes with `Task starvation: diag`: send `/tmp/radio2-logs/diag-router.log` (look at the
  mask handshake). A second candidate is data backpressure.
- A crash with a different cause (for example today's second crash in `wlan_process` after recovery) means diag is solved
  and this is the next problem. Collect `/tmp/radio2-logs/dmesg.txt`.
- Pull the logs before rebooting: `adb -s HLTE730T-PROBE pull /tmp/radio2-logs v74/logs/`.
What Pierre should watch: the laptop Wi-Fi/LAN link. Stop the run (Ctrl-C triggers the stop trap) on any unexpected drop.

## Risks and unknowns
- diag-router sends the normal DIAG control handshake (feature mask, empty masks), as stock diag does. It sends no
  commands to the modem.
- Not tested on hardware. The QRTR path is upstream code (used on SDM845/SM8x50) but has not been run on SDM660 + 7.2 here.
- The `persist` read is a read-only dd of the whole partition into RAM (typically 32 MB). It writes nothing.
- It is still unknown why diag-router "exited" today: the router output was not captured. The new run.sh prints it and
  refuses to start the modem if the router is dead.
