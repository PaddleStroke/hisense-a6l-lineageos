# V33: omit the optional eMMC block reset

Candidate SHA256: `365b880a19edbf7945bc82ca672cc853ea90fd059aaf9a480bd7e43039666305`.

Exactly one resolved DT property changes: remove `/soc@0/mmc@c0c4000/resets` (GCC selector10). The complete tree comparison enforces this. Kernel, RAM, module, init, command line, voltage, clock settings and staged command branches remain V32. Runtime state-marker labels therefore still say V32; this is intentional. Standard SDHCI software resets remain enabled.

This tests whether the optional GCC reset discards bootloader setup required by the A6L. Stock DT does not request it; this alone is not proof of the failure cause. Research: `research/storage-noreset-v33-20260917/notes.md`.

Package and roundtrip checks passed. Captured ABL checks passed. Six protocol and four transition checks passed; collector byte-identical to checked V32. Staged nine tool hashes, candidate hash and offline USB-disabled Inspect passed. Installation launched once at 2026-09-17T07:47:23.491720 UTC, PID203414; awaiting completion and desktop readbacks. Existing V32 C/model/QEMU checks apply to byte-identical binaries; no new C build or new hardware-emulation claim.

Installation completed 2026-09-17T07:47:54.696992 UTC. Verified exact V32 predecessor and preserved known BCB; poweroff acknowledged, EDL disappeared and host services restored. All twelve copied readbacks passed independent desktop verification. Awaiting user Power-button start into normal Android, then V33 capture. Install coordinator used; capture/restore unused.

V33 capture launched 2026-09-17T07:56:54.004041 UTC, PID203999 under GNOME inhibition. Baseline/port/services and pinned tools passed. Exact fastboot identity and USB logger confirmed; awaiting manual Recovery selection.

Physical result: no fix. 204728 bytes SHA256 ac9ac961594e077ed8740efb1e90027b90599ee98151cda8e979fcf4adf38f1d. All ten register reads delivered. Vendor capabilities change 0x322dc8b2 -> 0x762dc8b2 (xor0x44000000); other nine vendor registers, supply reports and preissue state unchanged. Last ALIVE34; no step2/issue return/outcome delivered. USB disappeared 07:58:33.073316 UTC, stock USB reappeared 07:59:02.947328; no additional user restart requested. Capture completed 07:59:04.972678. Stock Android and service cleanup verified 07:59:16.030248 UTC. Final session archived. V33 remains installed in recovery.
