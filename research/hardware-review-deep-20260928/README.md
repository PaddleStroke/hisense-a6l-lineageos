# Deeper A6L offline review reproductions

See [the report](../../docs/hardware-review-deep-20260928.md) for F23–F34, fixes, exact test boundaries and acceptance criteria.

Run in WSL with Python 3 and GCC/G++:

```sh
python3 /mnt/c/Users/Pierre/Desktop/A6L/research/hardware-review-deep-20260928/reproduce.py
```

Optional case names: `sensors sms data gnss cache voice`. Selected runs write into `run-<names>/`; an unfiltered run writes here. The original four-case run is in this directory; `run-cache/` and `run-voice/` retain the two subsequent additions. All six cases completed under ASan/UBSan with zero changes to their copied source inputs during each run.

The runner snapshots implementation sources into a temporary directory and builds only host review binaries. It opens no phone connection and performs no real QMI, network configuration, IIO, audio or telephony operation. Sensor paths point into the temporary directory. Modem endpoints are in-memory fakes. The voice harness compiles the two actual method bodies extracted unchanged from the copied source with stub dependencies; it does not compile a full Android HAL.

`results.txt` records observed behavior; `*-stderr.txt` and `*-build.txt` retain diagnostics. `source-hashes.json` identifies implementation inputs and `changed-during-run.json` checks concurrent changes. These are source identities, not installed-build identities.

Assertions deliberately reproduce current defects and must change after fixes. Core tests use the existing process-lifetime fixture and `_Exit`; they are not leak or shutdown tests. No ThreadSanitizer or physical device test was performed.
