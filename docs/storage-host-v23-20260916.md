# V23: MMC host registration and initial power-up trace

V22 passed the optional SDHCI activity LED step with the board-specific NO_LED quirk, then lost USB after checkpoint47 core_register_mmc_host. No panic or storage block device was captured. This identifies the next interval; it does not establish an exact faulting instruction or rule out asynchronous activity.

V23 adds16 checkpoints inside mmc_add_host, mmc_start_host and mmc_power_up. All use the existing board/device/boot-flag gated helper,250ms pauses and shared maximum64 pauses. These are sleepable process-context paths. No new delays enter interrupt handlers or spinlocks. DT, clocks, voltage/load settings and bundled module/RAM init are unchanged fromV22. Source snapshots and patches: firmware/extracted/storage-host-source-v23-20260916.

Host logger now retries only EACCES for at most5s, rechecking USB identity and physical port on every loop. Other errors still fail immediately. Its completion window requires a heartbeat at least20s after the actual storage fork, preventing old buffered heartbeats from falsely completing late capture. Four focused tests passed, including bounded retry, nonpermission error, and late-fork parsing.

Kernel adaptedSHA230bab281c9ece9bdd724b801355fec53ba5653fef59e57a04bdafdf0d6f74cc. ModuleSHA6a6e13de188f8137cccd2428256e13b8df02c2ee907a60c35a910e7dd9806100 and RAMSHAc619bbf588ab47220c2d7cef32ad9351b6bd7744dc60d6d4964d30e1653491a6 are byte-identical toV22. Module-load and actual RAM startup QEMU checks passed; generic QEMU does not emulate SDM660 storage.

Candidate fcc665755df0dfb3324f606701f8072dcdda3cef98227e416c593ba9ff113afa. Packaging passed. Captured ABL check pending. No V23 staging or phone write yet. Spare verified normal Android, V22 remains in recovery; host services restored.

Captured ABL,6 protocol,4 transition,5 collector tests passed. Initial tool generation stopped on CRLF; normalized collector input and resumed only byte-identical partial tools before pinning. All staged hashes and offline laptop preflight passed. Authorized guarded V23 installation launched; inspect logs before any next device action.

Installed14:44:22.840166UTC; all12copiedreadbacks verified independently. V22 predecessor and known boot message preserved. Poweroff acknowledged; EDL gone and services restored. User asked Power to normal Android; awaiting reply. Capture/restore remain UNUSED.

Stock Android verified. Capture launched14:55:34.983829UTC PID142482; exact fastboot and logger confirmed. User asked select Recovery, wait50s, no filming. Capture now USED.

V23 physical result: {"bytes": 192259, "sha256": "d3a80bc5cee6b91b4c07df74b2119069f029fab438fabc5cead7d7ba3b04d901", "last_checkpoint": ["62", "mmc_schedule_scan"], "last_heartbeat_seconds": 36, "kernel_panic_seen": false, "storage_devices_seen": false, "observation": "Passed initial MMC power-up; last checkpoint62 mmc_schedule_scan. USB lost before mmc_add_return. Async scan or another concurrent operation may be responsible; no exact fault established.", "collector_completed": false, "permission_retry_count": 0}
Capture finished14:57:02.974840UTC; user asked Power to normal Android, cleanup pending.

Stock Android and services restored14:57:30.768317UTC; final session copied. No cleanup pending. V23 remains installed.
