# Round 10 offline reproductions

See the [review report](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-round10-20260928.md) for F64 and the F22 NMEA follow-up, their limits, proposed fixes and acceptance criteria.

`gnss.cc` runs the production GNSS engine, QMI client/encoders, Android mapping and NMEA functions against the existing in-process fake modem. The transport factory always selects that fake transport. No phone, QRTR socket, modem NV, RF, system clock change or firmware installation is involved.

Recorded results:

```text
F64 queue_delay_ms=351 UTC_advanced_ms=0 advertised_uncertainty_ms=1
F64 later_request_positive_control supplied_UTC_and_uncertainty_encoded=1
F22_NMEA ellipsoid_only=80 GGA_MSL=80.0 GGA_geoid=0.0
F22_NMEA no_altitude GGA_MSL=0.0 GGA_geoid=0.0
F22_NMEA both_datums_positive_control GGA_MSL=35.0 GGA_geoid=45.0
```

The controlled queue hold is 350 ms; measured delay can vary. The engine is stopped normally after the timing assertions. The NMEA checks also assert the corrected Android ellipsoid mapping. These assertions intentionally describe current defects: a passing run confirms reproduction, not a successful fix.

From Windows with WSL and Linux `g++` installed:

```powershell
wsl --exec python3 /mnt/c/Users/Pierre/Desktop/A6L/research/hardware-review-round10-20260928/reproduce.py
```

The runner snapshots the current integration GNSS library and HAL source, compiles in a temporary WSL directory using C++17, pthreads, AddressSanitizer and UndefinedBehaviorSanitizer, and writes build/run evidence here. The temporary build is removed on completion. **Rerunning replaces the source snapshot and logs with evidence from the then-current source; preserve this directory first if comparing Claude's later fixes.**

- `reviewed-source.zip`: exact source snapshot used by this run.
- `source-hashes.json`: SHA-256 hashes of those production files.
- `build.log`: empty on the recorded successful compilation.
- `run.log` and `results.json`: successful defect assertions, exit code zero, no sanitizer diagnostics.
- `changed-during-run.json`: `[]`, confirming the snapshotted files did not change during execution.

The timing fixture enters the engine directly and introduces a deterministic barrier in an earlier assistance request. The HAL-entry clock adjustment and callback paths were inspected in source; a full Android Binder stack was not run. NMEA inputs are synthetic, and neither test measures acquisition performance or physical height on the device.
