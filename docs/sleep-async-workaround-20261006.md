# Sleep after video: `pm_async=0` workaround → fixed in the device tree (6 Oct 2026)

**Status: FIXED, workaround removed.** The real fix is in `device/hisense/a6l/kernel/a6l-camera-v75.dtso`: `&cci` now also
takes `mnoc_ahb`, `smmu_ahb` and `smmu_axi`, as the stock device tree does. The `pm_async=0` line was removed from
`init.qcom.rc` the same day. Analysis: `firmware/extracted/pm-async-race-20261006/README.md`.

**Root cause.** The rear-camera focus motor (GT9769, `4-000c`, held runtime-active by libcamera) parks the lens in
its suspend callback with CCI I²C writes, about 1 ms per 16 positions. That is 82 ms measured at lens position 1023.
The MMSS NoC/SMMU clocks the CCI needs were consumed only by the MMSS SMMU `iommu@cd00000`, which gates them in its
own system-suspend callback. With async suspend, the motor's writes ran in parallel with that gating, the CCI access
stalled the bus and the watchdog fired. A video recording only mattered because autofocus moves the lens.

**Proof.**
- Before the fix: fresh boot, no camera use, lens set to 1023, `pm_async=1`. The first `pm_test=devices` hung
  (durable record: BEGIN, no END).
- Fix boot `f517fef5…` (only the DTB changed): `cci@ca0c000` is listed as a consumer of the three clocks.
- The same provocation then passed 6/6 (3× devices, 3× platform).
- The user did three real post-video sleeps with `pm_async=1`: 3/3 pass.

The original investigation notes follow.

## Symptom

After recording a camera video (with or without audio), the next system sleep sometimes hard-hangs the phone. The
watchdog then resets it, and the reset ends on a backlit black screen (see `boot-hang-fixes-20261006`). Preview,
voice recording, screen recording and heavy CPU load followed by sleep are all fine.

This is a second bug. The first was the GPU stale-CP replay fixed the same morning (`docs/gpu-cp-reset-fix-20261006.md`).
The GPU fault had masked it.

## Evidence (fix build, GPU module `2bddfcdb…`)

| After video | Suspend mode | Result |
|---|---|---|
| `pm_test=devices` | async (default) | pass |
| `pm_test=platform` | async | crash (twice, including with `pm_test_delay=0`) |
| real sleep | async | crash 2 of 4 |
| late/noirq prefix trial N=2048 (all 19 late/noirq callbacks run, then unwind) | forced **serial** by the instrumentation | pass (twice) |
| `pm_test=platform` | `pm_async=0` | pass |
| real sleep, 3 rounds, alternating last camera | `pm_async=0` | pass 3/3 |

Controls without video, all with async: platform passes, and overnight real sleep passes. Each late/noirq callback
returns 0 both with and without video (`firmware/extracted/pm-logging-20261005/pm-late-noirq-prefix-20261006`, trial logs
under the laptop kit `rom-r7c-pm-ln-20261006/logs/`).

**Interpretation:** a race. With async suspend/resume, two devices that depend on each other run in parallel, and the
kernel does not know about the dependency (no `device_link`, or a wrong parent or power domain). The candidates are the
late/noirq participants: `genpd:0/1:ca00020.camss` (VFE domains), `ca0c000.cci` (camss_top), the IOMMUs `5100000`, `5180000` and `5040000`
(the GPU SMMU, which owns gpu_gx), `c901000.display-controller`, the DSIs, `5000000.gpu`, the BAM DMAs and `soc@0`. Note also
`drivers/pmdomain/core.c` `a6l_keep_boot_domains()`: it skips genpd sync_state, so boot-on domains stay on at runtime
and are powered off for the first time in the system-suspend noirq phase.

## Cost

Device suspend/resume callbacks run one at a time, so suspend and resume are slightly slower. It was not noticeable in
the attended tests.

## Removing it

Add the missing link (for example `device_link_add` between the consumer and its IOMMU or power-domain supplier, or
fix the DT relationship). Then boot with `pm_async=1` and repeat: record main and front videos, run `pm_test=platform`
(the stage harness), then real sleep, several times. Remove the init line only after repeated passes.
