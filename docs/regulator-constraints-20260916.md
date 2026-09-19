# V11 regulator constraint correction

Physical V11 result (2026-09-16): both USB PHY c012000.phy and USB core a800000.usb now report probe return0 (~4.495/4.543s). SMEM/SMP2P/interconnect probes also succeed. Later temperature alarm and MMC deferrals remain. User reports shutdown/restart after text around4.71s, Android splash then battery-only screen; no panic is visible. Original Android subsequently independently verified complete via ADB. Rearm-2 observer recorded no diagnostic USB bytes. Photos and precise limits are archived in captures/regulator-constraints-screen-20260916/analysis.json. A guarded host cleanup/stock restore is now launched in exec session84731; check reports before any other phone action. No further diagnostic candidate has been installed.

Current V11 state (2026-09-16): corrected recovery installed with full and twelve desktop readbacks verified; original Android returned. Capture wrapper launched PID 25860, but laptop SSH timed out twice during status checks. Actual observer/fastboot state is not yet verified. User asked to keep laptop awake/check Wi-Fi and report phone screen. Do not relaunch any coordinator or request Recovery selection until remote state is inspected. V11 Restore remains unused.


V10 physically reaches READY and repeated ALIVE through at least 130 seconds.
Photo: captures/visible-userspace-screen-20260916/frame-01.jpg. USB remains
absent; QUSB2 reports regulator supplies unavailable; eMMC reports vmmc
unavailable. USB core waits for the PHY. The PMIC temperature alarm has a
separate absent modular ADC dependency identified in the prior supplier map.

The A6L DT constrained PM660L L4 to exactly 2950000 uV, copied from the stock
consumer request. The pinned mainline driver uses pm660_pldo660, with
1504000 + selector * 8000 uV. No selector yields 2950000. The actual pinned
machine_constraints_voltage function returns -EINVAL for that interval before
any voltage-setting stub is called. rpm_reg_probe returns an error when any
child registration fails; managed registrations for the same provider are
released. This explains a concrete configuration defect consistent with the
missing PM660L USB supplies; a physical V11 test must establish the effect.

Both stock merged DT variants bound PM660L L4 to 1700000..2950000 uV.
V11 changes only regulator-min-microvolt from 2950000 to 2944000, preserving
the 2950000 maximum. The regulator core narrows that interval to 2944000.
It is within both stock regulator ranges and does not exceed the stock cap.
No parent supply, calibration value or other hardware property is changed.

Test-A6LRegulatorConstraints.py extracts the actual unmodified pinned core
function, uses the actual PM660 descriptor tables and runs six inert-I/O
cases. All other four configured supplies pass; original L4 fails; corrected
L4 passes and sets only the representable 2944000 value in the test stub.
This is not hardware emulation or a complete devres-unwind test.
Artifacts: firmware/extracted/regulator-constraints-20260916.

The RAM diagnostic additionally writes on plus newline to printk_devkmsg.
The photo revealed EINVAL, and the old emulator log also contained it: the
previous test checked output/feedback but did not assert this setting succeeded.
The new QEMU test explicitly rejects that error and passed with READY/ALIVE,
dependency snapshots, no logging drops/feedback and bounded framebuffer writes.

The kernel binary remains 4cc5b6b159bdcef531a4e2916d2e4b7a40598e666f11b61625901d8f41a62ca8.
Only the DT minimum-voltage property and RAM newline changed. Packaging,
roundtrip, captured ABL checks, six transport fault cases and the laptop's
USB-disabled preflight passed. Full image SHA256:
54ec8ad057bbb4bd52c63ad2c82ee68efd0a4828ff674cfdc51acbb41a12259a.
Tool manifest SHA256:
8e6f09ca7cfdcc7cef0e57e26ef8499e70913d19c35ffc52752278abc45647c9.

V10 stock restore full readback completed 05:47:23.216695 UTC; Android/services
verified 05:47:46.998674. All twelve desktop readbacks passed. Stock recovery
Reboot clear-v7 completed; original Android verified 05:55:03.133425 UTC.
All V10 coordinators are used. V11 Install is now being launched; inspect its
capture before any further phone action. V11 Capture/Restore are unused.

V11 installation passed full readback at 05:59:11.336173 UTC; poweroff/USB
disappearance and laptop services verified 05:59:16.559979. User asked to
start original Android with Power. Stock recovery is not installed now.
V11 Install used; Capture/Restore unused. Desktop copies transferring.
