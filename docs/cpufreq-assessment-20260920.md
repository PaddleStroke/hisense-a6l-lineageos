# CPU frequency scaling on SDM660 — assessment (20 September 2026, offline)

- The pinned sdm660-mainline 7.2.3 tree has no CPU clock/cpufreq provider for SDM630/660 (checked: no OSM clock driver,
  no `qcom,sdm630/660` match in cpufreq or CPR code, no CPU OPP tables in `sdm630.dtsi`).
- The unmerged work is **more recent than assumed**: SoMainline/linux branch `topic/cpr3hh` is rebased on
  linux-next 2024-07-03 (~v6.10). It contains `drivers/pmdomain/qcom/cpr3.c` (2711 lines, CPR3/CPR4/CPR-Hardened),
  `cpr-common.c`, and "cpufreq: qcom-hw: Implement CPRh aware OSM programming" (OSM handled inside `qcom-cpufreq-hw`).
  Fetched to `~/src-cpr/linux` in WSL (FETCH_HEAD of topic/cpr3hh).
- **Gap:** that revision carries SoC data for **MSM8998 only**. SDM630/660 need their own `cpr_desc`/thread descriptors
  (ring-oscillator scaling factors, fuse corners, voltage limits) and OSM register/DT description. The 2021 mailing-list
  revisions of the same series included SDM630 data; the A6L stock DT + downstream `cpr3-hmss`/`cprh-kbss` sources are the
  reference for the numbers. Note SDM660 uses CPRh "kbss" controllers like 8998, so the driver model fits.
- Effort estimate: forward-port 6.10→7.2 (mechanical) + SDM660 data tables + DT nodes + conservative bring-up
  (open-loop voltages first). Multi-day; voltage-affecting, attended only. After GPU.
- Tomorrow's `cpu-speed.sh` measurement decides urgency.
