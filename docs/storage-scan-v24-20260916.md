# V24: first MMC card scan

V23 completed initial storage power-up, then disconnected after checkpoint62 mmc_schedule_scan. Last heartbeat36s,192259bytes, no panic or block device. Stock Android and laptop cleanup verified14:57:30.768317UTC.

V24 adds16 process-context checkpoints around mmc_rescan, mmc_rescan_try_freq and mmc_hw_reset_for_init. It keeps the existing power/clock/regulator settings, DT and matching module/RAM init. The shared bounded trace budget increases from64 to96 pauses of250ms to accommodate these later operations. It does not add sleeping to IRQ or atomic paths. Source patches archived in firmware/extracted/storage-scan-source-v24-20260916.

Logger success observation is extended from20 to28s after the actual module fork; total post-fastboot capture bound65s. This avoids stopping during the additional checkpoints. No filming is needed.

Kernel7335a99fd4fcffdce926654ea10a1599eb86b28f036505acc2e23637e6447ff5. Module and RAM byte-identical toV23. Module QEMU passed; actual RAM test in progress. No V24 phone write yet.

Both QEMU checks, captured ABL routines,6 protocol/4 transition tests and collector timing guards passed. Candidate6cc9c9940c068057ed58f3a210213b444ef2321eee01e1aa96b082e275a28731. Staged hashes/offline preflight passed. Guarded installation launched; awaiting verification.

Installed15:08:02.261869UTC. All12desktop-copied readbacks passed; exactV23 predecessor and knownBCB preserved. Powered off with services restored. User asked Power to normalAndroid; awaiting reply. Capture/restoreUNUSED.

Stock Android baseline verified. Capture launched15:09:52.086378UTC PID143901. Exact fastboot identity and logger confirmed. User asked Recovery selection and60s untouched, no filming. Capture now USED.

Physical V24 result: {"bytes": 196627, "sha256": "7d0fb2804521d7f7caee9869d0e83149ff0164e3568d9af9cda95316518ee557", "last_checkpoint": ["76", "scan_go_idle"], "last_heartbeat_seconds": 40, "kernel_panic_seen": false, "storage_devices_seen": false, "module_load_succeeded": true, "observation": "Module load and host registration succeeded; scan reached76 scan_go_idle. USB lost before interface-condition checkpoint. Failure may be in chip-select setup, CMD0 submission/completion, or concurrent work; no exact fault instruction captured.", "collector_completed": false, "permission_retry_count": 0}
Stock Android and host services restored15:11:48.780136UTC; final session archived. V24 remains installed.
