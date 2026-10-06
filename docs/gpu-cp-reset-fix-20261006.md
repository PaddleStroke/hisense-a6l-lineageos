# A6L GPU stale-CP reset fix — 6 October 2026

Fixes (candidate) the post-video sleep crash localized by the restore trace. Not yet physically accepted.

## Cause captured by the trace (run `20261006T065200Z-prefix42`)

Before `a5xx_hw_init`, the A512 CP still held its pre-suspend state: `CP_RB_RPTR = CP_RB_WPTR = 0x1177`.
Programming `CP_RB_CNTL` zeroes RPTR but not WPTR, so clearing ME_HALT made the CP replay stale ring words.
It stopped at RPTR 0x60 before `CP_ME_INIT` was written. The flush set WPTR 9 < RPTR 0x60, so `a5xx_idle` timed out
(`rptr/wptr = 60/9`), hw_init returned -22 and the job was dispatched anyway, followed by a CP opcode error,
hangcheck and a restart. Evidence: `firmware/extracted/pm-logging-20261005/gpu-trace-vendor-candidate/physical-trial-2-result.json`.
Still unexplained: why GX is not power-collapsed, and why this happens only after video.

## Change

`firmware/extracted/pm-logging-20261005/gpu-cp-reset-fix-20261006/` (`gpu-cp-reset.patch`), on top of the
installed restore-trace module `25e060…`. That base keeps the Oct 2 recover-before-retire/HLSQ patches, the Oct 5
resume order and the default-off trace.

At the start of `a5xx_hw_init`, A512 only: if `CP_RB_RPTR` or `CP_RB_WPTR` is non-zero, run
`RBBM_SW_RESET_CMD` 1/read/0, the same sequence as `a5xx_recover`. Then redo the A512 clock setup from
`a5xx_pm_resume` (`RBBM_CLOCK_CNTL` 0x55, `a5xx_set_hwcg`, rmw 0xff→0) and log one line:
`A6L stale CP rptr/wptr X/Y before hw_init, soft reset -> X/Y me Z`. A cold boot (both pointers 0) is a no-op.

- Module `2bddfcdbcce5352a34945025a19b98d242122d7dca0fec5fb6837ad2773cbc94`.
- Vendor `53bfa611db59b79d994568d5fe9465aa6bced780362543aa91bfc056394b0d4f` (186793984 bytes). Only `msm.ko`
  and timestamp props differ from the trace vendor `eea474…`.
- Laptop kit `/home/pierrelouis/A6L-usb-20260915/rom-r7c-gpu-fix-20261006`.

Checks:
- The baseline rebuild reproduced `25e060…` with zero changed sections.
- 759 imports are byte-exact, with no new imports and 0 exports; 127 parent modules pass.
- Compile scope was msm only, and the disassembly matches the source.
- The host fixture passes in both strict and ASan/UBSan builds.

An independent agent review judged it safe to test. Its open points:
- The reset does not halt VBIF first. The CP is idle on the target path, and `a5xx_recover` does not halt VBIF either.
  Hardening candidate if needed.
- Perf-counter selects and one devfreq busy sample may be disturbed after a reset.
- `msm_gpu_submit` still ignores hw_init failure (upstream behavior).

## Test plan

1. In recovery: `python3 extra/start-gpu-fix-flash.py`, then `flash-progress-gpu-fix.py`, then
   `check-gpu-fix-install.py`. This is a vendor-only, hash-only update.
2. Copy `extra/check-runtime-gpu-fix.py` to `/tmp`, then run `extra/boot-gpu-fix-comparison.py`.
   It boots via `sysrq b`, checks the runtime, prepares WFI-only, verifies four rings and starts the host collector.
   Run prep right after boot: a full trace buffer breaks it. Never use `adb reboot` from Android: it hangs on
   backlit black.
3. **Controlled A/B against today's failures:** fresh 10 s main rear video → save → Home, then
   `extra/run-prefix-gpu-fix.py 42` once. This is the same prefix42 boundary that failed twice today.
   Pass requires all of:
   - the `A6L stale CP …` line shows `-> 0/0`;
   - the trace shows `hw-init-after ret=0`;
   - no drain timeout, opcode error or GPU fault;
   - the same boot is observed for 120 s.
4. **Only if 3 passes:** an ordinary fresh video → Home → Power lock 20 s → wake → a minute of use. This is
   the user's real failure. A fix is claimed only after it passes.
