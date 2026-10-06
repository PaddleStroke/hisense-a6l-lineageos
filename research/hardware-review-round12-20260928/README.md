# Round 12 GNSS accuracy reproduction

See the [report](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-round12-20260928.md) for F66, its impact, limits, proposed fix and acceptance criteria.

`accuracy.cc` exercises the production QMI encoder/decoder, position parser and Android location mapper with synthetic position reports. It verifies confidence TLVs survive the wire codec, then demonstrates that horizontal/vertical confidence values of 39, 50, 68 and 95 percent all produce identical Android accuracy values with validity flags set. An omitted-confidence case also produces the same values. Controls cover correctly matched 68% input and omitted uncertainty fields.

Run from Windows with WSL and Linux `g++`:

```powershell
wsl --exec python3 /mnt/c/Users/Pierre/Desktop/A6L/research/hardware-review-round12-20260928/reproduce.py
```

The runner also reads `GnssLocation.aidl` from the existing Android tree at `/home/a6l/android/a6l-lineage24`. It snapshots all production files used or inspected for this reproduction, compiles in a temporary WSL directory, runs with ASan/UBSan, records evidence here and removes the temporary build. No phone, modem transport, RF, SIM, clock change or production write is involved.

**Rerunning replaces this directory's evidence with a snapshot of the then-current source. Preserve it before comparing subsequent fixes.** The assertions describe the current defect; a passing test is not evidence that the bug is fixed.

Recorded output:

```text
F66 input_confidence_pct=39 Android_horizontal_m=10.0 Android_vertical_m=20.0 flags=51
F66 input_confidence_pct=50 Android_horizontal_m=10.0 Android_vertical_m=20.0 flags=51
F66 input_confidence_pct=68 Android_horizontal_m=10.0 Android_vertical_m=20.0 flags=51
F66 input_confidence_pct=95 Android_horizontal_m=10.0 Android_vertical_m=20.0 flags=51
F66 absent_confidence Android_horizontal_m=10.0 Android_vertical_m=20.0
F66 no_uncertainty_positive_control accuracy_flags_absent=1
ROUND12_ACCURACY_REPRODUCTION_PASS (defect present, not fixed)
```

- `reviewed-source.zip` / `source-hashes.json`: exact reviewed sources and SHA-256 fingerprints.
- `build.log`: empty on the recorded successful compilation.
- `run.log` / `results.json`: successful defect/control assertions and exit code zero, with no sanitizer diagnostics.
- `changed-during-run.json`: `[]`, showing that the snapshotted sources did not change during the run.

The process exits normally. The test does not run Binder or measure physical GNSS accuracy, and it does not assume that missing confidence means 50% or 68% on this firmware.
