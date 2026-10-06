# Round 5 review reproductions

These review-only tests assert the defective behavior described in [the report](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-round5-20260928.md). A PASS confirms reproduction; it is not feature acceptance.

Run from the project's existing WSL environment:

```text
wsl --exec python3 /mnt/c/Users/Pierre/Desktop/A6L/research/hardware-review-round5-20260928/reproduce.py
wsl --exec python3 /mnt/c/Users/Pierre/Desktop/A6L/research/hardware-review-round5-20260928/extended.py
```

Requires Python 3, GCC/G++, the local Android tree at `/home/a6l/android/a6l-lineage24`, and (for the extended source archive) `/home/a6l/kernel/a6l-baseline-7.2`. Only temporary host binaries and these evidence files are written. No adb, phone, RF, SIM credential attempt, partition operation, or production edit occurs.

| Files | Purpose |
|---|---|
| `contracts.cc`, `contracts.log` | F42/F44/F45/F46/F48; exact HAL method bodies, small Binder/core stubs, real QMI client/builders and FakeModem |
| `identity.cc`, `identity.log` | F43; complete ModemCore with reversed physical-card provisioning and dummy ICCIDs |
| `apdu.cc`, `apdu.log`, `apdu-offset1.log` | F47; exact READ BINARY method, fake filesystem, fatal CHECK equivalent; expected SIGABRT child |
| `vibrator.cc`, `vibrator.log` | F49; unchanged constructor/play/isPresent with fake evdev calls, rejected A6L name and accepted control |
| `backlight.c`, `backlight.log` | F50; exact enforcement function, failed unblank then 100 healthy iterations, positive retry control |
| `results.json`, `extended-results.json` | Successful reproduction outputs |
| `*-build.log` | Compiler diagnostics; empty means no emitted diagnostics |
| `source-hashes.json`, `extended-source-hashes.json` | SHA-256 of production sources used |
| `reviewed-source.zip`, `extended-reviewed-source.zip` | Frozen copies of reviewed device/platform/kernel sources |
| `changed-during-run.json`, `extended-changed-during-run.json` | Both `[]` in the completed runs |
| `changed-at-report-finalization.json` | Final comparison with the current sources |
| `initial-radio-evidence/` | Preserved first radio run, before concurrent QMI integration changes |
| `concurrent-revalidation/` | Later, partially integrated radio snapshot and build-mismatch details; not a complete passing rerun |

The runners build in a Linux temporary directory, extract methods from their captured source, and check the original source hashes afterward. The radio snapshot is broad enough to include the actual QMI implementation and tests. The main evidence files retain the fully successful original run; the later incomplete rerun is preserved separately under `concurrent-revalidation/`. Rerunning after a fix should fail the bug assertions and overwrite the evidence with the new snapshot; preserve these archives first if comparing versions.

The host tests use AddressSanitizer and UndefinedBehaviorSanitizer. Binder stubs do not emulate Android service scheduling; the APDU test does not launch the full radio daemon; the evdev/sysfs mocks do not simulate hardware. Those boundaries are intentional and described in the report.

`verify_evidence.py` checks the report's numbered findings and local links, recorded results, and whether the current sources still match the reviewed hashes. A source mismatch means the live worktree changed and needs review/revalidation; it is not itself a new product defect.
