# V17: restore the stock USB PHY regulator load

The SDM660 community fixed USB cable insertion resets by adding a 14,000 uA system load to pm660_l10:
https://github.com/sdm660-mainline/linux/pull/86

The A6L stock merged device tree independently confirms the same requirement. In `firmware/extracted/stock-dtbo-20260914/stock-00-merged.dts`, pm660_l10 has `qcom,proxy-consumer-current = <0x36b0>` (14,000 uA), proxy-consumer-enable, and 1.78–1.95 V limits. Our mainline device tree retained the voltage limits but omitted the load. Public source evidence is saved under `research/usb-upstream-20260916/`.

The new separately named DT includes the existing USB-only DT and adds exactly `regulator-system-load = <14000>` and `regulator-allow-set-load` to vreg_l10a. Packaging verifies those are the only parsed DT changes. No voltage or pin assignment changes. Kernel, RAM filesystem, command line, overlay and load addresses are byte-identical to V16. The existing V16 QEMU test therefore remains applicable to the unchanged RAM/kernel code; it cannot validate Qualcomm electrical behavior.

V16 was installed and readback-verified, then returned to stock Android, but was never booted as a diagnostic. Skip that recording and test the stock-supported correction directly. V17 deliberately retains the V16 RAM marker and four-second staging: configure immediately, bind at 4 s, connect at 8 s, serial at 12 s. About 30 seconds of video is sufficient; host logging lasts up to 45 seconds after leaving fastboot.

Candidate SHA-256: `50bd78a2a94a7d151defbe637b28492207bdd6b5f857cc413175b13535658c15`.

This is a strong candidate explanation, not a confirmed root cause. A physical boot must establish whether the reset disappears and USB enumerates. Keep full predecessor/identity/GPT/BCB/readback guards; replace only recovery. Stock recovery remains available, and normal Android is the tested return path.

V17 installation completed at 12:25:16 UTC. All twelve copied readbacks independently passed desktop verification, exact V16 predecessor and known bootloader BCB preserved. Host services restored. Awaiting manual normal Android startup; diagnostic capture has not started.

## Physical result

V17 successfully enumerated as 1d6b:0104, serial HLTE730T-PROBE, on laptop USB port 3-2. The collector opened /dev/ttyACM0 and received 174,878 bytes (SHA-256 12a77d002a8648fab593fb5f1d5f22002dd5710711f6324f7ab25d5514f4edb7). Device logs show configured high-speed USB, successful serial open/write and a subsequent heartbeat. Capture finished successfully at 12:28:35 UTC; the user reported continued operation at 90 seconds. This validates USB communication on this diagnostic boot and strongly supports the missing L10 load as the prior reset cause. Repeated boots, sustained transfers, normal Android USB, and the complete Lineage/e-ink port remain unvalidated.

Capture saved at captures/capture-probe-serial-user-v17 with desktop verification. Requested manual return to Android; host services cleanup pending that return.

Normal stock Android return and laptop service restoration were verified at 12:32:17 UTC; final session report is archived. V17 remains installed in recovery. The next storage-only V18 candidate is built and passes package/captured-ABL checks, but has not been staged or flashed.
