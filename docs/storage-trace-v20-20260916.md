# V20 paced storage initialization trace

V19 reached USB configured/high-speed and transmitted 176747 bytes, then disconnected around the scheduled module-loading boundary. Neither fork-begin nor load-begin reached the host. This does not establish the exact failure location. Independent stock Android return was verified at 13:42:14 UTC, after the coordinator deadline, with services restored. V19 remains installed until V20 is verified and installed.

V20 preserves V19 device tree, regulator constraints, kernel configuration, existing USB fix and boot addresses. It adds 29 checkpoint call sites: eight in the driver-core probe path and 21 in sdhci_msm_probe. Each logs the upcoming operation and sleeps250ms in process context. Gating requires a6l_storage_trace=1, hisense,hlte730t and c0c4000.mmc. A shared atomic counter permits at most64 pauses (16s total), including retries. No extra register reads or power-setting changes are introduced. These timing changes may affect symptoms; this is a diagnostic, not a proposed production fix.

RAM init announces the fork at18s. It starts the child no earlier than20s and only after the serial write has accepted the announcement. The child logs LOAD_BEGIN and waits1s before finit_module. Host delivery still requires the USB capture; device-side write acceptance alone is not a receipt acknowledgment. The parent continues its100ms kmsg/serial loop. Storage is loaded once, without module unloading, shell, persistent mounts or userspace block-device opens.

Host capture will target ALIVE40s, bounded at55s after fastboot departure. This should not require filming.

Source transformation occurrence guards stopped three preparation attempts before any kernel/source edit. Their backup-only directories are preserved. An inadvertently started no-change build is archived in storage-trace-kernel-v20-20260916 and is NOT the V20 payload. Actual modified sources are in storage-trace-source-v20-r4-20260916; actual kernel/module are in storage-trace-kernel-v20-r2-20260916. The current offline tools pin these paths and require checkpoint strings.

Matching kernel/module diskless QEMU load test passed. Adapted kernel SHA2567449f5391f55e6e21df495ab4ea0cf4b8c1ec75e96b468827e66aa9458078258. Module SHA2563ff190e3886fa5bb9a2f61e5a5b86ac1d176d82ae88bd238a7240e4b016c4fb3. QEMU does not emulate Qualcomm hardware. Actual RAM test, boot-image packaging, ABL checks and physical installation are still pending at this checkpoint.

Small upstream search: see research/storage-upstream-20260916/notes.md. The relevant Qualcomm regulator-load fix is already present in our driver; no exact matching reset report established.

Actual RAM-init QEMU test passed, preserving the no-USB/no-storage-load gate. Packaging, exact captured ABL routines, six protocol fault tests and four predecessor/boot-message guard tests passed. Candidate c1ba3da846fa31cc9a40a8ba6c2aba434d8711d0f67b795469bf65c6180d4392; RAM6863ffacb5463affded5277bf96beae4e56e17492e52f50fcdf3427ec06ef439. Device tree, overlays and voltage settings remain byte-identical to V19; command line only adds a6l_storage_trace=1.

Transfer and laptop hash/offline preflight passed. V20 installation launched at13:49:12 UTC under GNOME inhibitor A6L-v20-install, PID136899. Run-LaptopDiagnosticInstall-user-v20.py is now USED; do not relaunch. Await capture-diagnostic-install-user-v20/session.json and complete readbacks. Capture and stock restore are UNUSED.

V20 installation completed13:49:43UTC. All12 copied before/after readbacks passed independent desktop verification in captures/capture-diagnostic-install-user-v20/desktop-verification.json. Exact V19 predecessor and known boot-once message preserved; laptop services restored and EDL disappeared after poweroff. User asked to hold only Power for stock Android, no recording yet. Await reply; V20 diagnostic has NOT yet booted.

Stock Android return independently verified before V20 capture. Capture launched13:58:55UTC PID137446, exact fastboot and logger confirmed. User asked to select Recovery and leave connected50s without filming. Capture is now USED; no additional image write in this stage. User proposes the newer regulator-load behavior itself may be incompatible; retain as a hypothesis and locate failure with V20 before changing behavior.

Physical V20 result:184104 USB bytes,27 checkpoints, last add_mmc_host atkernel33.023703s. No host_added_autosuspend or panic captured. Normal Android and host services verified by coordinator14:01:05.878032UTC. Final session copied; V20 remains installed. Next narrow sdhci_setup_host and __sdhci_add_host with paced process-context checkpoints, preserving functional settings.
