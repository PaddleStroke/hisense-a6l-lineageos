# V21: trace within SDHCI host setup

V20 captured184104bytes SHA1e7b79f3049e79f66a3669f8fcd82df212286b8aa94695de0655f8dba42c4c80. All27 checkpoints through add_mmc_host arrived. No following host_added_autosuspend marker or panic was received. This narrows failure to the interval around sdhci_add_host but does not establish a single instruction; later messages could still be lost. User observed poweroff. Coordinator verified normal Android and restored services at14:01:05.878032UTC; final session and desktop analysis are saved.

V21 adds21 paced checkpoint sites in sdhci_setup_host, its initial __sdhci_read_caps call and __sdhci_add_host. They distinguish reset-all, version/capability reads, DMA allocation, constraints, IRQ and host registration. For A6L these are the sleepable first-probe paths. Existing gate restricts tracing to the explicit boot flag, exact A6L compatible and c0c4000.mmc. Shared cap64 pauses remains16s maximum across all calls. No power policy, register values, extra register reads or regulator-load changes.

Exact V20 RAM init, module and ramdisk are reused byte-for-byte; the printed RAM-stage name remains V20 intentionally. V21 identity is established by pinned recovery/kernel hashes and capture directory. Adapted kernel5717b756307399dae8767922963ef592d6661edf2ddb8a457487f3fe06f37643. RAM6863ffacb5463affded5277bf96beae4e56e17492e52f50fcdf3427ec06ef439. Module3ff190e3886fa5bb9a2f61e5a5b86ac1d176d82ae88bd238a7240e4b016c4fb3. Matching-module QEMU test passed; actual RAM boot test running. No V21 transfer or phone write yet.

Actual RAM/QEMU and module pairing passed. Packaging retained V20 DT, overlays, RAM and command line byte-for-byte. Captured ABL routines, six protocol fault tests and four transition tests passed. Candidate d4fa4f8b5b50e3c7b9e123ad5cde913d5e5000fee3535620af4e50773b13d407. Staged hashes and offline laptop preflight passed. Installation launched14:10:11UTC PID138338 under GNOME A6L-v21-install; do not relaunch the USED install. Await full readbacks. Capture/restore UNUSED.

V21 installed14:10:42.937716UTC, full12 copied readbacks independently verified on desktop. Exact V20 predecessor and known boot-once message preserved; EDL disappeared after poweroff and services restored. User asked Power-only stock Android startup; awaiting reply. Capture/restore UNUSED.

Stock Android verified and V21 capture launched14:13:43UTC PID138948. Exact fastboot+logger confirmed; user asked Recovery selection and50s hands-off, no filming. Capture now USED.

Physical V21 result:188676bytes; last checkpoint46 core_register_led, before led_classdev_register. No following core_register_mmc_host or panic captured. Reset and setup completed. Normal Android and host service cleanup verified14:15:51.837869UTC. Final session and independent analysis saved. Next controlled variant uses existing SDHCI_QUIRK_NO_LED only on the A6L eMMC, preserving regulator behavior.
