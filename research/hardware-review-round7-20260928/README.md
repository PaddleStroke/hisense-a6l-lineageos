# Seventh-pass offline reproductions

These tests preserve and exercise source defects F55–F58 in the [report](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-round7-20260928.md). They make no phone, RF, ADB, firmware, partition or production-source changes.

From the Windows workspace, with the existing WSL toolchain and Android tree:

```powershell
wsl --exec python3 /mnt/c/Users/Pierre/Desktop/A6L/research/hardware-review-round7-20260928/reproduce.py
```

Requires Python 3, g++ with C++17, AddressSanitizer and UndefinedBehaviorSanitizer, and the Android interfaces at `/home/a6l/android/a6l-lineage24/hardware/interfaces`. Compilation occurs in a temporary WSL directory. The runner snapshots the current Windows radio source and the relevant local Android contracts before compiling, writes hashes and a ZIP, then checks source freshness after execution.

**Rerunning replaces the source archive, hashes and logs with evidence from the then-current source. Preserve this directory first if the original review snapshot is needed.** It does not automatically rerun against the archived version. Fixed code should cause the corresponding defect assertion to fail; these are reproductions, not acceptance tests for a corrected implementation.

## Harness boundaries

- `voice.cc`: exact extracted production methods for TTY, DTMF and rejection, with recording Binder responses, a manually dispatched executor, and a thin core accessor using the real QMI call-list helper. The production QMI client, encoders and existing fake-modem transport are compiled into the harness. This is not the full Android Binder service or production executor. Fake tone and held-call state explicitly model the accepted command semantics.
- `failcause.cc`: complete production `ModemCore` and QMI implementation, reusing the existing host test setup. An active-call indication is followed by VOICE service removal. No network busy/rejection cause is fabricated. The core is process-lifetime with no stop/join API; this harness exits via `_Exit` after assertions and output, so it does not validate shutdown or leak cleanup.
- Both builds use `-fsanitize=address,undefined`. Their successful execution covers only the exercised paths. The absence of sanitizer findings is not a general concurrency or memory-safety assessment.

## Recorded result

```text
F55 TTY_FULL_HCO_VCO success=1 getter_echoes_request=1 modem_requests=0
F56 DTMF_stop_rejected HAL_success=1 fake_continuous_tone=1 cleanup_retries=0
F56 explicit_stop_positive_control tone=0
F57 reject_with_only_held_call success=1 broad_release_request=1 no_call_id=1 fake_held_released=1
F57 incoming_call_positive_control targeted_END_CALL=1
F58 active_call_lost_with_VOICE_service last_call_fail_cause=16(NORMAL_CLEARING)
ROUND7_REPRODUCTIONS_PASS; source files changed: 0
```

`results.json` records exit codes and stdout. Build logs and execution logs preserve compiler/runtime output. `failcause-member-uses.txt` contains the entire member-use inventory from the snapshot. `changed-during-run.json` is empty. Physical behavior and the report's proposed acceptance cases remain to be tested after fixes.
