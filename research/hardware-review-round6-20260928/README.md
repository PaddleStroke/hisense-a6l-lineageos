# Sixth hardware review: offline evidence

These tests **assert defective behavior in the reviewed source**. Exit zero means the reproduction succeeded, not that the hardware passed acceptance. No phone, real modem, RF, SIM, firmware or partition access is involved.

Run from Windows:

```powershell
wsl --exec python3 /mnt/c/Users/Pierre/Desktop/A6L/research/hardware-review-round6-20260928/reproduce.py
```

The runner creates a temporary source copy, saves the reviewed files in `reviewed-source.zip`, records SHA-256 hashes, builds five isolated executables with UBSan, and checks source freshness afterward. Re-running replaces the evidence with a new snapshot of current production; preserve this directory first if historical comparison is needed. Production source is never modified. The two WSL dependencies are the local Android IGnss AIDL contract and the local kernel IIO event implementation; both are archived too.

| Harness | What actually runs | Boundary |
|---|---|---|
| `gnss.cc` | Complete production GNSS engine, QMI client and encoder, existing fake modem transport | Fake receiver state changes only on accepted START/STOP; no physical power measurement |
| `sensors.c` | Complete sensor HAL plus production motion/calibration code; fake sysfs and FIFO event FD | Failed event-enable open is injected using a directory; normal HAL polling runs for 12 calls / about 2.5 seconds. A separate poll interception records its indefinite idle timeout |
| `controls.cc` | Exact extracted `setPositionMode` and `deleteAidingData` method bodies plus real QMI START encoder | Binder and engine endpoints are recording stubs, not a complete Android service build |
| `route_fixture.c` | Complete unchanged routing daemon in an extended copy of its existing fake mixer fixture | Adds the actual XML's ADC2 MUX control to model a lost partial write. Not a real codec or complete libaudioroute emulation |
| `kernel_poll.c` | Exact extracted local kernel `iio_event_poll` body with minimal structure/queue stubs | Establishes poll-mask behavior after `info` becomes NULL; does not execute an actual kernel unbind |

The GNSS session-end indication subcase demonstrates inconsistency between the existing listener's interpretation and the engine state. It is supplemental evidence; F52's confirmed failure does not depend on assumptions about how often firmware emits that indication.

`results.json` and individual logs contain observed results. Build logs retain compiler warnings. `changed-during-run.json` should be empty for this captured run. The report separates new F51–F54 from incomplete F4/F7 fixes.
