# V56 — missing AIDL Power HAL compatibility

V55 r3 passes all 15 foundation checks, including genuine Health callbacks, BPF readiness, Health uevent filter attachment and mount cleanup. SystemServer completes core services but HintManagerService dereferences null `mSupportInfo` when there is no declared AIDL Power HAL. The previous missing vendor API property is fixed; this is a separate failure.

The exact null-handling defect has an existing **merged** LineageOS fix: [change 434585](https://review.lineageos.org/c/LineageOS/android_frameworks_base/+/434585), commit `5518dd64224c3dfe2674a496c771770904458a78`, lineage-23.0. Public review metadata and patch are saved under `research/framework-hint-v56`. Our pinned frameworks/base HEAD is `bc6573742889e49c42fc17c0dcdc6a60421b3320` and lacked the fix.

`Prepare-HintCompatV56.py` adapts its two small hunks without changing their behavior: initialize fallback support information when AIDL IPower is absent, sharing the existing old-HAL fallback. This reports CPU/GPU headroom unsupported, keeps native hint-session support detection and zeroes unsupported capability masks. No Power HAL test double is installed, no supported capability is invented, and physical A6L power/performance management remains open. The script checks that the target file is clean, asserts exact edit anchors, and preserves before/after source, diff and hashes.

`build-framework-hint-v56.sh` builds the actual `services` module. The diskless QEMU test uses all V55 prerequisites plus `--hint-compat`; version 56 archives the modified framework source and patch. It adds a behavioral assertion that SystemServer reaches RoleManager, which follows the formerly crashing HintManager constructor. The earlier V55 failure provides the before case; successful framework continuation is the after case. Full UI remains a separate milestone. No phone, SSH or USB operation is part of this test.

## Result — 19 September 2026 (V56 r1)

The `services` rebuild completed (24 min). V56 r1 passes **16/16** foundation checks, including the new `hint_no_aidl_compat` assertion: HintManagerService constructs without an AIDL Power HAL and SystemServer reaches RoleManager. SystemServer then continues through the "other services" phase (input method, accessibility, `MakeDisplayReady`, StorageManager, StorageStats) — far beyond V55.

Next stop: `FATAL EXCEPTION IN SYSTEM PROCESS: android.display` — `ProcessList.updateOomLevels` cannot set `sys.sysctl.extra_free_kbytes` because the VM's private property service only allowlists specific names (result 0x18). This is a harness limitation, not a framework or A6L defect. The same log shows 43 rejected `ctl.start` requests whose targets were not recorded.

V57 therefore widens the harness allowlist to the `sys.` prefix and logs `ctl.*` values (`A6L_PRIVATE_CONTROL`) so the requested services become visible. Full UI is still false. Archive: `firmware/extracted/android-framework-v56-20260918-r1` (console SHA-256 `df1c3685…976b9f`). The archive step's `copytree` failed with EPERM on the Windows-backed path after the report was written; the harness now uses `copyfile`, and the four adaptation files were verified present.

Session tooling changed: work continues through `tools/claude-wsl-relay.sh`. No phone, SSH or USB operation occurred.
