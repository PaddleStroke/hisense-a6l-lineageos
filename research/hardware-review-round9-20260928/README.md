# Ninth-pass offline reproductions

Evidence for F62–F63 in the [report](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-round9-20260928.md). These tests perform no phone, ADB, audio-device, PWM, driver-unbind or production-source operations.

```powershell
wsl --exec python3 /mnt/c/Users/Pierre/Desktop/A6L/research/hardware-review-round9-20260928/reproduce.py
```

Requires WSL Python 3, gcc/g++ with ASan/UBSan, and the local Android tree at `/home/a6l/android/a6l-lineage24`. The runner copies selected current source into a temporary build directory, extracts exact methods, saves a ZIP and SHA-256 hashes, runs both harnesses, and checks source freshness.

**Rerunning replaces evidence with the then-current source. Preserve this directory first if the original snapshot is needed.** The runner does not automatically replay archived sources. Defect assertions may fail once corrected; they are not acceptance tests for fixed implementations.

- `audio.cc`: publisher and device-name helpers extracted from the added lines of `0002-a6l-call-route-mute.patch`, plus the daemon's exact route structures and `decide()` method. Android port/patch structures and properties are recording fixtures. The test does not apply the patch to the Android checkout, compile the full HAL, run audio policy or listen to sound. The selected platform `StreamPrimary.cpp` is archived as supporting source, not compiled into this harness.
- `frontlight.c`: exact `enforce_frontlight()` method and full production `dualux_logic.c`, using controlled sysfs reads/writes. The backend disappearance/recreation and reset brightness are fixture conditions; neither real driver unbind nor suspend is performed.
- Both builds use `-fsanitize=address,undefined`. The archive preserves the tested implementation and selected integration references; other report links, including the frontlight overlay, refer to the live workspace.

## Recorded results

```text
F62 mode=NORMAL requested=speaker/main published_route=empty daemon=headphones/headset-mic
F62 mode=IN_COMMUNICATION requested=speaker/main published_route=empty daemon=headphones/headset-mic
F62 modem_call_positive_control published=speaker daemon=voice-speaker/main-mic
F63 backend_recreated desired=50 actual=0 cached=50 recovery_writes=0 iterations=100
F63 changed_slider_positive_control desired=60 actual=60
F63 rejected_write_positive_control recovery=1
ROUND9_REPRODUCTIONS_PASS; source files changed: 0
```

Build logs, execution logs, `results.json`, `source-hashes.json`, and empty `changed-during-run.json` accompany the snapshot. No sanitizer errors were reported. Device validation and the report's proposed regression cases remain necessary after integration.
