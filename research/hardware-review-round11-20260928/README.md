# Round 11 offline reproductions

See the [review report](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-round11-20260928.md) for F65 and the F17 recovery follow-up, proposed fixes and acceptance criteria.

`radio.cc` compiles the production ModemCore and QMI implementation against the repository's fake modem and Android property/logging fixtures. It rejects VOICE/WMS registration at startup and service return, clears the fault, and checks for missing retries. Healthy service cycles are positive controls. It also injects network-time indications with absent or present timezone/DST fields and saves the actual emitted NITZ strings.

`CheckNitz.java` feeds those strings to the unchanged `NitzData.java` from the local Android tree. Only its logging, annotation and log-tag dependencies are stubbed. The Java checks distinguish unknown DST from an explicit zero and verify that valid metadata still works without changing UTC time.

Run from Windows with WSL:

```powershell
wsl --exec python3 /mnt/c/Users/Pierre/Desktop/A6L/research/hardware-review-round11-20260928/reproduce.py
```

Requires Linux `g++` and the existing Android tree at `/home/a6l/android/a6l-lineage24`, including `prebuilts/jdk/jdk21/linux-x86`. No network download, phone connection, QRTR socket, real SIM request, RF operation, firmware installation or system-clock change is performed. The injected transport is always the in-process fake modem.

The runner snapshots production radio sources and test fixtures plus Android's parser, compiles in a temporary WSL directory, records results here and removes the temporary build. **Rerunning replaces the snapshot and logs with evidence for the then-current source; preserve this directory before testing Claude's subsequent fixes.**

Recorded results:

```text
F17 startup ready=1 voice_attempts=1 sms_attempts=1 successes_after_fault_cleared=0/0
F17 healthy_service_return_positive_control registrations_accepted=1/1
F17 rejected_service_return retries_after_fault_cleared=0/0 ready=1
F17 second_service_cycle_positive_control registrations_accepted=2/2
F65 nitz=26/09/28,12:00:00+0,0
F65 nitz=26/09/28,12:00:00+8,0
F65 nitz=26/09/28,12:00:00+8,1
ROUND11_RADIO defect assertions passed=31 failed=0
F65 Android parser: missing timezone accepted as offset_ms=0; missing DST accepted as dst_ms=0
F65 omitted_DST_positive_control dst=null; known_fields_positive_control offset_ms=7200000 dst_ms=3600000; UTC_unchanged=1
```

- `reviewed-source.zip` and `source-hashes.json`: exact reviewed sources and SHA-256 fingerprints.
- `radio-build.log` / `java-build.log`: empty on successful compilation.
- `radio.log` / `java.log`: test output; the radio log also includes normal fake-service diagnostics.
- `nitz.txt`: strings actually emitted by the production core and consumed by the Java test.
- `results.json`: all build/run exit codes and stdout.
- `changed-during-run.json`: `[]` for the recorded run.

C++ checks run with AddressSanitizer and UndefinedBehaviorSanitizer. As in the repository's existing ModemCore harness, `_Exit` terminates the process-lifetime singleton threads; cleanup/leak behavior is not tested. The fake modem does not enforce notification subscriptions, so the test proves failed registration and missing retries, not the delivery of real incoming calls or SMS. The Java test exercises parsing, not the full automatic timezone-selection service. All assertions describe reproduced defects or positive controls; a passing run is not acceptance of a fix.
