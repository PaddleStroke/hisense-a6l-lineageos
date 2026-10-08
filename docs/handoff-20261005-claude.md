# A6L handoff for Claude — 5 October 2026 evening

**LATEST: The bounded GPU restoration diagnostic is built, audited and staged
for the morning in `rom-r7c-gpu-trace-20261005`. It has NOT been flashed or tested.
The phone was left asleep at the user's request; the native guard was released
and USB disappeared. Overnight stability is unknown. The installed resume-order
candidate FAILED with the same opcode/ringbuffer fault. Read the final ready
section and `gpu-restoration-test-20261006.md` before using earlier instructions.**

Read this first, then `docs/pm-diagnostics-20261005.md`. User requests continuity
because Codex usage is almost exhausted. This is the same authorized ongoing
port/fix/flash task. Do not ask again for flash permission. Ask only for physical
actions needed when the phone cannot be controlled remotely.

## Current installation and authoritative status

Morning diagnostic payload: vendor
`eea474c336d85abdac7bf9001ec7db8b977ba4a458ee83acaa1ccc3f2a93ceb2`
(186793984 bytes), module
`25e060bbc81f3ed7b0004ca878d152dced2329c3aed0f953692f4ea04ac5acb6`.
Laptop kit `/home/pierrelouis/A6L-usb-20260915/rom-r7c-gpu-trace-20261005`.
Its full KIT checksums, Stage verification and offline updater dry-run passed.
Only vendor differs from the installed kit; the updater checks live identity
and skips unchanged boot/system/DTBO. No phone checks or writes ran tonight.

After preserving overnight evidence and obtaining recovery tomorrow, the
checksummed helpers are in this new kit's `extra/`:

1. `python3 extra/start-gpu-trace-flash.py` starts the attended hash-only update.
2. `python3 extra/flash-progress-gpu-trace.py` reports progress; after completion,
   `python3 extra/check-gpu-trace-install.py` must pass before booting.
3. Copy `extra/check-runtime-gpu-trace.py` to the laptop's
   `/tmp/check-runtime-gpu-trace.py`, then use
   `python3 extra/boot-gpu-trace-comparison.py`. This depends on the retained,
   previously reviewed `/tmp/prepare-quiet-wfi-test.py`,
   `/tmp/verify-release-quiet-wfi.py`, `/tmp/restart-host-collector.py` and selected
   harness. If those are missing, restore their reviewed originals; do not
   improvise or reuse stale boot receipts.
4. Ask for a fresh main rear video → save → Home, then run
   `python3 extra/run-prefix-gpu-trace.py 42` exactly once on that prepared boot.
   Let the 120-second observation complete and preserve all evidence before
   any further boot. See the morning procedure for interpretation and limits.

**Installed image: GPU resume-order vendor update, readback verified and then
physically tested; it did not fix the crash. Latest phone state: Android was
put to sleep, framework Dozing observed and native guard released. The new
diagnostic kit is staged separately and NOT installed.** Original installation
capture `20261005T190602Z`, workerexit0,
services restored, vendor235d2545… matches, protected invariants unchanged.
Boot/system/DTBO were skipped. See the local install receipt and final update
below. Tomorrow's planned vendor-only diagnostic flash is a different payload.

- Live state: `firmware/extracted/pm-logging-20261005/status.json` and
  `firmware/extracted/rom-r7c-20261004/status.json`.
- Candidate audit/staging/install receipts:
  `firmware/extracted/pm-logging-20261005/gpu-resume-vendor-candidate/`.
- Laptop kit: `/home/pierrelouis/A6L-usb-20260915/rom-r7c-gpu-resume-20261005`.
- WSL kit: `/home/a6l/rom-v2/kit-r7c-gpu-resume-20261005/rom-v2`.
- Candidate boot unchanged:
  `d1ecec46e10d1bf072f3ac91192e1b01cefe7d4354d4eaa4877a8fa84e75c33f`.
- Candidate vendor:
  `235d2545e04e1c41ae7cafcc5132cd620d186977fcbcef0c2bf148ed433ff4ec`,186789888bytes.
- Candidate mounted `msm.ko`:
  `34d552eb967164bd4dc4a91c691829e667001572c7dc2e92e8fbb1d23e4e1d9d`.
- System unchanged:
  `a696ad3d41888696841c6289bb487d2e3a2d2cd7574314dc927f65d1ee7f7dd7`.
- DTBO unchanged:
  `6925112258af276ad2d16757df054bbc6e98148b5273f0a214e7d8ddd5044e7e`.
- ROM remains r7c, kernel `7.2.3-a6l-probe+`; this is a narrow trial, no new full ROM.
- Previous vendor (rollback kit `rom-r7c-pm-prefix-20261005`):
  `66f127be0a8a2a305ced8b9f1e299010e2a911853dcdc3048cae3961ddffe4cf`.

User explicitly rejects accumulating development-ROM backups. Use updater
`--backup-policy hash-only`, skip identical partitions, reuse kits/hardlinks.
Keep existing stock Android9 backup; do not create another full disk/ROM backup.

## What finally localized the crash

After fresh main rear video, boot `fe1ee196-3529-4d31-b8c4-c76ff492fdbf`:

1. Prefix40 nonce `20261005T181335Z-prefix40`:40suspend/34resume callbacks,
   includes display/DSI/DPU, exact paired wrappers return0. Stable120.171s,
   59same-boot checks, no connection gaps.
2. Prefix42 nonce `20261005T181631Z-prefix42`: identical first40 callback
   identities, adds null PHY41 and actual Adreno42.42/36 wrappers return0,
   returned snapshots durable before inhibitor release. eMMC/USB suspend excluded.
3. GPU suspend/resume returns0; suspend exits5730.750673. About17.54s later,
   host kernel logger captures drain timeout, CP opcode error, GPU fault and
   hangcheck recovery, offending SystemUI RenderThread. Phone-disk tail stopped
   before this fault. USB initially disappeared, then phone returned on new boot
   `ac419bcc-0cb8-43f9-92b3-1616abf2513d`. No terminal reset/panic stack captured.

Evidence:
- `gpu-prefix42-physical-result.json` in pm-logging directory.
- `laptop-logs/awake-20261005T181335Z/kmsg.decoded.txt`:
  at5748.294056 ring0 rptr/wptr60/9 timeout;
  at5748.294585 possible opcode2;
  fence2440/status800001C1;5749.294600 hangcheck recover.
- `laptop-logs/pm-20261005T181335Z-prefix40/` and
  `laptop-logs/pm-20261005T181631Z-prefix42/`.
- New-boot retained retrieval `laptop-logs/android-capture-20261005T181913Z/`:
  6993657bytes, no errors, CRC-valid old rings. Pstore empty.

This implicates GPU transition independently of storage/USB suspend. It does
NOT prove a particular worker actually ran inside the power-restoration window.
Zero callback returns do not establish sustained stability. wptr9 resembles
CP_ME_INIT payload length, but no ring bytes prove that interpretation.

## GPU candidate: scope, validation, rebuild trap

Source review finds adreno_system_resume starts nonfreezable submission workers
BEFORE pm_runtime_force_resume. GPU mutex/runtime-get does not close this forced
PM window; runtime-get error is ignored elsewhere. Candidate calls force-resume
first, returns its error unchanged, then restarts scheduler only on success.
One function changes; failed restoration leaves queues parked. Trial fix only.
The old ordering also exists upstream; do not call this a known upstream fix.

Frozen directories inside pm-logging-20261005:
- `adreno-resume-order-review-20261005/` — source/race audit.
- `adreno-resume-order-candidate-20261005/` — patch, actual-function fixture,
  strict and ASan/UBSan740assertions each, root independently replayed.
- `adreno-resume-order-module-build-20261005/` — frozen module/build receipt,
  manifest SHA256 `45e987a0464b2c05452fdd6a21f75c475c96f5b54ac7881024706e35dff89852`.
- `gpu-resume-module-root-review.json` — independent ELF/ABI review.

759import CRCs/vermagic unchanged;127parent modules checked; of1985functions
only adreno_system_resume differs. Compilation is isolated module-only; accepted
kernel output and canonical source were preserved/restored. No kernel Image or
system rebuild requested. A single4.3GiB isolated working cache exists.

CRITICAL: Installed old GPU module42e8e5d8… contained two October2 patches whose
canonical sources were later restored! Rebuilding canonical sources alone would
drop shipped fixes. Builder explicitly replays exact
`firmware/extracted/gpu-recovery-candidate-20261002-170137/recover-before-retire.patch`
and `a5xx-hlsq-cleanup.patch` plus this candidate. Fresh baseline matches every
installed ELF section except build ID. Preserve these patches for future builds.

First vendor build accidentally reused cached old module; content audit caught
it before any flash. Corrected build explicitly staged only msm.ko at
`/home/a6l/android/a6l-lineage24/device/hisense/a6l/rom/prebuilt/vendor/lib/modules/msm.ko`.
It now contains candidate34d552…; other modules unchanged. Future generic pipeline
staging may overwrite it with old payload. Promote explicit module/source input
only AFTER physical acceptance, and preserve older GPU patches. New vendor audit
passes: msm.ko plus three timestamp-only property files changed,1059unchanged,
no files added/deleted. See `image-audit.json`.

## Installation and exact next comparison

Workspace PowerShell, SSH configuration `tools/a6l-laptop-ssh.conf`:

```powershell
ssh.exe -F tools/a6l-laptop-ssh.conf a6l-laptop 'python3 /tmp/flash-progress-gpu-resume.py'
```

Flash start helper `/tmp/start-gpu-resume-flash.py` runs pinned checks/dry-run,
then launches updater detached with hash-only policy. **Do not start twice**:
first inspect kit `logs/flash-current.json`, log and newest capture/session/report.
Completion requires session worker_exit0, services_restoredtrue, noerror,
report readback_verified/invariants_unchanged, onlyvendor written, boot/system/
DTBO skipped, vendor readback equals235d2545…. Flash progress helper is read-only.

After complete readback, save local install receipt before reboot. Recovery→
Android uses explicitly targeted `adb -s HLTE730T-PROBE shell 'echo b > /proc/sysrq-trigger'`.
The updated GPU is loaded only on the new Android boot. Restore adb root, wait
for completion and run `/tmp/check-runtime-gpu.py` with the NEW laptop kit path:
it checks installed boot, full vendor prefix, mounted module, logger/config pins,
prefix parameter-1, PM testnone, quietconsole1 and healthy logs.

Next physical test must use a **fresh prepared boot**; old ready files are stale:

1. `prepare-quiet-wfi-test.py` with OLD `rom-r7c-pm-prefix-20261005` kit path
   is intentionally used for existing harness/log whitelist. No --adopt option
   on a clean boot. It saves48original idle controls, disables40non-WFI states,
   quietconsole1, holds owned native guard and prepares tracing/recorder.
2. `verify-release-quiet-wfi.py` requires exact preparation path; it proves four
   fresh CRC rings and correct trace ownership before releasing native guard.
3. Write oldkit `logs/pm-prefix-comparison-current.json` with readytrue, NEW boot
   ID and **gpu_module_sha25634d552…**. Preserve preparation/trace proof.
4. Start host collector using `/tmp/restart-host-collector.py` and oldkit path.
   Keep it through120s after return; it finally captured the missing fault.
5. Ask user fresh10s main rear video→Stop/save→Home, LCDawake. Then explicitly
   `/tmp/run-prefix-selected.py 42` ONLY. Controller module pin now reads ready
   file; stale old boot or module refuses before GO. Actual callback order can
   change between boots: inspect identities before interpreting ordinal42.
6. Run read-only `/tmp/observe-prefix-return.py` with that exact returned receipt
   (inspect its argument schema); sustained120s same-boot observation is separate
   from callback return proof. If USB drops, restart collector if it ended and
   evidence permits. USB loss alone is unavailable evidence, NOT proven reboot.
7. If stable, test ordinary fresh video→Home→Power lock20s→wake and another minute
   responsive, with same logging. Only this can clear the user's real failure.

If candidate fails, STOP widening callback groups: captured GPU fault already
localized transition. Add bounded worker/force-runtime/job/ring-init tracing
to distinguish race from bad postcollapse CP initialization/stale state. Keep
source/kernel/module provenance and surviving host tail. Do not mix eink/encoder
changes or claim a cause from proximity to the last disk log.

Before user recovery, ac419boot had native diagnostic guard retained, PMnone,
prefix-1, NOT fresh WFI-prepared. Recovery reboot clears temporary guards/idle
controls; never reuse its old ready/guard receipts on new Android boot.

## Other fixes, findings and outstanding work

- Quiet debug console fixed measured wake delay:7.2seconds→submillisecond console
  resume and attended instantaneous wake. Do not restore verbose console.
- USB legacy parent lost QSCRATCH configuration; restoration physically passes
  two attended real sleep cycles. Does not solve post-video GPU crash. Deeper
  CPU idle remains unvalidated; current isolation uses WFI-only.
- Camera fence-FD ownership repair shipped in oldvendor66f127…; actual FD fixture
 1667checks. It did not clear crash. New GPU vendor retains it unchanged.
- User accepts all three camera previews and recording/save/playback with audible
  video sound. Optical washed-out colors remain; calibration/stock comparison
  pending. Native previews ~30fps. Video software bottleneck persists: r7c9.295fps;
  Camera-only direct-YUV signed APK trial16.463fps, saved406distinct frames, AAC
  continuous. Current /data/app trial survives vendor-only flash. No r7d exists.
- Actual DT Venus disabled. Hardware encoder offline modeled candidate exists,
  not physically activated. See `video-encoder-audit-20261004/README.md` and live
  status. Need firmware/ABI/actual C2 lifecycle/hardware acceptance, not just an
  offline ioctl replay. Encoder/thermal warnings do not establish electrical short.
- Audio video recording now audible per user; separate voice WAV/playback and
  two-way modem call audio still need confirmation. Modem data already works;
  SIM moved between phones, ask availability before modem retest.
- E-ink labels black and top row visible per user; notification moving artifacts
  persist. White bars fixed. Faster behavior accepted as improved, not stock parity.
  Avoid intrusive repeated sharpening; fastest has less flashing with tolerable
  ghosting. Dualux watchdog candidate remains offline/uninstalled. See latest
  eink agent evidence rather than promise15/30/60fps panel performance.

## Practical safety/continuity

- Android serial1e529013; recoveryHLTE730T-PROBE. Always explicit `adb -s`.
  Daily phone3c99e8/HLTE730T may also be attached: never touch it.
- Recovery entry: unplug USB→Power untiloff→Power+VolDown→reconnect. Charging-mode
  trap can produce backlit black/noadb and wipe ramoops; unplug before forced reset.
- Preserve logs before another Android boot. Userdata/metadata prev rings and host
  rings are useful; pstore/retention failed across tested warm and recovery resets.
  Do not claim a reliable surviving RAM logger. No safe raw persistent scratch
  backend has been established. Do not repurpose RTC/IMEM registers.
- Repo `C:/Users/Pierre/Desktop/A6L`; WSL Ubuntu-24.04/root; SSH laptop192.168.1.22
  configured key/pinned host. Build `/home/a6l/android/a6l-lineage24`.
- Avoid pkill patterns matching your own command; use unique relay/task names.
  Recovery output has linker warning lines; tools already handle markers.
- Do not rerun old record-prefix-zero/delayed-restart generators: they overwrite
  current status with obsolete boot/preparation. Preserve chronology and receipts.

## Final installation/runtime update

Pending at initial write. Codex will append verified result here, and update
status.json and candidate install receipt before any next test.


2026-10-05T19:06:57.173379+00:00 — **Flash running**: PID2688441, laptop log `/home/pierrelouis/A6L-usb-20260915/rom-r7c-gpu-resume-20261005/logs/flash-20261005T190601Z.log`. Local `gpu-resume-vendor-candidate/flash-current.json` saved. Do not launch updater again or disconnect until worker/report completion.



2026-10-05T19:08:51.709404+00:00 — **Installation complete and verified. Phone deliberately remains in recovery.** Only vendor written/readback235d2545…; boot/system/DTBO skipped, protected invariants unchanged, workerexit0/servicesrestored. Hash-only, no development-image snapshots. Local candidate `install-receipt.json` is authoritative. New GPU has not yet been loaded into Android; runtime and physical acceptance pending. Next: boot Android and follow the fresh preparation/runtime steps above.


2026-10-05T19:11:29.040966+00:00 — User requested testing now; GPU update already installed, boot requested via reviewed `/tmp/boot-gpu-resume-comparison.py`. Runtime/fresh preparation in progress. Do not flash again.

2026-10-05T19:13:19.783983+00:00 — **Supersedes recovery state above: Android runtime verified and fresh comparison ready.** Boot `4b82534d-3b5a-435f-8493-b4bd79928eaf`; full vendor, boot and mounted GPU pins pass.40non-WFI states disabled for isolation, quietconsole1, four fresh rings proved before inhibitor release. Preparation `/home/pierrelouis/A6L-usb-20260915/rom-r7c-pm-prefix-20261005/logs/quiet-wfi-20261005T191227Z`. Host collector active; PMnone/prefix-1. Awaiting fresh10s main rear video→save→Home, then explicit prefix42 and120s observation; no physical acceptance yet. Local candidate gpu-runtime-current.json/gpu-comparison-current.json and status.json saved.


## GPU candidate physical comparison FAILED — supersedes earlier ready/recovery states

User confirmed spontaneous reboot after fresh main rear video and candidate
prefix42 (`20261005T191411Z-prefix42`). Mounted module34d552… was verified before
GO.42/36 callbacks return0; returned proof/durable snapshots pass, but120second
stability fails.17same-boot samples then loss; observation49.248s correctly
classified unavailable until user report and new boot confirmed restart.

Host CRC-valid kernel ring again captures the same failure signature:
suspendexit204.040381, drain timeout242.378048 rptr/wptr60/9, CP opcode error
possibleopcode1, GPU fault ring0/fenced4b/status800001C1, hangcheck recovery
243.378589, SystemUI RenderThread offending. Thus restore-before-scheduler
ordering alone does NOT fix the observed fault. Fault about38.34s after return;
longer delay is not evidence of improvement. No terminal reset stack captured.

New boot `a89a93c7-4526-4a08-81b6-235938a2c610`; adbroot regained. Retained
capture `android-capture-20261005T191641Z` saves5762999bytes, no errors, sameboot.
Phone is in Android with an owned native partial inhibitor retained to protect
diagnostics, PMnone/prefix-1. Current boot has NOT been freshly WFI-prepared.
Do NOT reuse old ready files or release the guard for another test unchanged.

Local `gpu-resume-vendor-candidate/physical-trial-result.json` and
`gpu-trial-restart-current.json` are authoritative. Host trace under
`laptop-logs/awake-20261005T191249Z/`; trial/private callback receipts under
`laptop-logs/pm-20261005T191411Z-prefix42/`; retained capture copied locally.

Next: direct source audit/bounded instrumentation of A5xx postcollapse CP/ring
initialization and actual worker/power ordering. The recurring wptr9 and opcode
fault motivates inspecting initialization, but packet bytes/register state are
still needed; do not declare bad CP_ME_INIT proven. No further blind driver
prefix sweep, unchanged trial, or automatic real sleep test. Candidate module
remains installed, physical acceptance failed; retain oldvendor66f127… kit for
deliberate rollback if needed. Other e-ink/encoder/audio candidates remain out.


### Latest phone state: user requested overnight sleep

Supersedes retained-guard state above. On boota89a93c7, explicit user request
fulfilled: stay_on_while_plugged_in=0, inputKEYCODE_SLEEP223; framework reported
Dozing, then owned native partial guard released (`held=false, refCount=0`).
USB subsequently absent. No restart requested. Full kernel sleep and overnight
stability are unverified; USB loss alone is not evidence of another crash.
Leave phone alone tonight. Next session ask its physical state/wake behavior,
reconnect if needed, preserve logs before any forced restart. Restore fresh
diagnostic preparation deliberately before another test; old ready files stale.
Local `gpu-resume-vendor-candidate/night-sleep-receipt.json` and status saved.


### Overnight diagnostic preparation authorized

User requested instrumentation now and physical test tomorrow morning. Phone
must remain asleep tonight; no wake/flash. Existing GPU agent is preparing
`firmware/extracted/pm-logging-20261005/gpu-restore-trace-20261005/`: default-off,
bounded one-shot restore/first-submit/initialization/ring evidence, starting from
exact installed34d552… including October2 shipped patches. Reuse isolated build
cache; preserve accepted output and canonical source/mtime. Root reviews source,
safe powered-MMIO placement, log budgets and ABI before vendor-only packaging.
Not yet built/packaged at this note. Look for WIP README and later build receipts.
Recurring hardware rptr60 versuswptr9 motivates ring/CPinit measurement, not a
proven pointer-reset bug. Capture forcePM/runtime status, actual init return,
first job and existing safe powered registers/ring words. No speculative reset,
sleep fix or encoder/e-ink patch mixed into this diagnostic. Status updated.


### GPU restoration diagnostic module review complete; packaging running

Frozen manifest `2f2cf8de52fa21f3beb9f152d53d0326d0305715fb04ebcc6e3c57e3766251e5`; module `25e060bbc81f3ed7b0004ca878d152dced2329c3aed0f953692f4ea04ac5acb6`. Baseline matches installed34d552 byte-for-byte;759import records/CRCs unchanged (section ordering changes harmlessly),127parent ABI checks pass, no exports/new imports. Root verifies all61pins and strict/sanitizer replay, exact production-statement undo comparison, bounded source/control/MMIO review. One consumed capture perboot,96generic+12fault lines,120s active. Existing source, accepted kernel output and shippedpatches preserved. No physical diagnosis/fix yet.


### GPU restoration diagnostic ready for morning

Frozen manifest `2f2cf8de52fa21f3beb9f152d53d0326d0305715fb04ebcc6e3c57e3766251e5`; module `25e060bbc81f3ed7b0004ca878d152dced2329c3aed0f953692f4ea04ac5acb6`. Baseline matches installed34d552 byte-for-byte;759import records/CRCs unchanged (section ordering changes harmlessly),127parent ABI checks pass, no exports/new imports. Root verifies all61pins and strict/sanitizer replay, exact production-statement undo comparison, bounded source/control/MMIO review. One consumed capture perboot,96generic+12fault lines,120s active. Existing source, accepted kernel output and shippedpatches preserved. No physical diagnosis/fix yet.
Vendor audit SHA `eea474c336d85abdac7bf9001ec7db8b977ba4a458ee83acaa1ccc3f2a93ceb2`, 186793984bytes; onlymsm.ko plus allowedtimestamp props changed. Kit `/home/pierrelouis/A6L-usb-20260915/rom-r7c-gpu-trace-20261005` staged/verified; **NOT flashed**. Phone left untouched after userrequested sleep. Boot/system/DTBO reused; hash-only updater. See `gpu-restoration-test-20261006.md`; pinned helpers/controller in kitextra and workspace.
Android prebuilt inputmsm restored to34d552 after temporary packaging staging; its timestamp intentionally refreshed so nextvendorbuild restages baseline. Android cachedvendorimage is diagnostic; use explicit kit/payloadpins forfuturebuild. Isolatedmodulecache currentlycontains diagnosticrawc351…; frozenbuildhelper requires reviewedbaseline-cache provenance forreplay, do not rerunblindly.


## 6 Oct morning (Claude) — trace flashed; first armed run partially lost to a staging bug

- Overnight: NO crash. Recovery capture `laptop-logs/recovery-20261006T054602Z` (CRC-valid): boot a89a93c7
  did repeated real s2idle cycles, then clean user shutdown 07:38 local. Repeated buddy-watchdog
  "hard LOCKUP cpu5/cpu6" warnings during s2idle with deep idle enabled — noted, separate issue.
- Recovery note: fresh V74 recovery has NO mmcblk until `/sdhci-msm.ko` is insmodded (updater does it);
  `capture-pm-recovery-20261005.py` needs that first. PowerShell 5.1 mangles nested quotes; `adb shell`
  in a piped `bash -s` eats stdin — use `</dev/null`.
- Trace vendor flashed/readback verified (`gpu-trace-vendor-candidate/install-receipt.json`); boot/system/DTBO skipped.
- `adb reboot` from Android -> backlit black, no USB at all (hung restart / early boot, never reached
  userspace). User forced recovery; recovery->sysrq-b path works. Avoid `adb reboot` from Android.
- Preparation must run right after boot: after ~35 min the a6l_pm trace buffer is full (2 MB) and the
  watcher grep takes >5 s, failing prepare's 3 s phases.txt check (and truncating phases.txt).
- Armed run `20261006T064418Z-prefix42` on boot 9590b652: arm consumed, 42/36 return 0, trace active,
  BUT `observe-prefix-return.py` was never staged into kit `extra/` -> controller FileNotFoundError ->
  finally disarmed ~5 s after return, before the first post-resume submit. Only 11 software-state lines.
  They show force/runtime resume ret0, needs_init=1, scheduler restarted after. Fault reproduced
  30.5 s after suspend exit, identical signature (rptr/wptr 60/9 then 5C/9, opcode 0x1, status 800001C1,
  SystemUI RenderThread). Init/ring/MMIO samples NOT captured. See `gpu-trace-vendor-candidate/physical-trial-1-result.json`.
- Fix before the next (new-boot) attempt: copy reviewed `/tmp/observe-prefix-return.py`
  (sha256 6f63e649…) next to the kit's `extra/run-prefix-gpu-trace.py`. No module/vendor change needed.

### 6 Oct — second armed run (boot 14340858): MECHANISM CAPTURED

Observer staged; run `20261006T065200Z-prefix42`, 71 trace lines in host ring
`laptop-logs/awake-20261006T064745Z/kmsg.claude-decoded.txt`; restart confirmed (new boot 072ad7ce).
Before hw_init the CP still holds pre-suspend state: CP_RB_RPTR=CP_RB_WPTR=0x1177, CP_PFP_ME_CNTL=0
(not halted). RB programming resets RPTR to 0 but WPTR stays 0x1177; clearing ME halt makes the CP
replay STALE ring contents from 0 and it stops at 0x60 before CP_ME_INIT (correct packet at ring[0..8])
is even written. WPTR flush=9 < RPTR 0x60 -> drain timeout, hw_init -22, driver dispatches anyway,
recover repeats (stale WPTR 0x2e), hangcheck, restart. So rptr/wptr 60/9 is explained.
Fix candidate: restore CP power-on state at start of a5xx_hw_init (RBBM soft reset as a5xx_recover, or
halt + zero WPTR) and refuse dispatch on hw_init failure. Open: why GX is not collapsed / why only after
video. See `gpu-trace-vendor-candidate/physical-trial-2-result.json`.

### 6 Oct — CP-reset fix flashed; controlled post-video GPU test PASSES

Fix: `docs/gpu-cp-reset-fix-20261006.md`, module 2bddfcdb…, vendor 53bfa611…, kit `rom-r7c-gpu-fix-20261006`
(vendor-only hash-only flash, readback verified). On boot da4fd400 after fresh video, prefix42 with trace:
`A6L stale CP rptr/wptr 1aa/1aa before hw_init, soft reset -> 0/0`; CP_ME_INIT consumed (rptr=wptr=9), hw_init 0,
submits normal, 0 faults, 120 s same-boot observation passed. Result:
`gpu-fix-vendor-candidate/controlled-test-result.json`. Real video->lock->wake test is next. Prebuilt msm.ko in
the Android tree is still 34d552 — promote 2bddfcdb (with all patches) only after physical acceptance.

### 6 Oct ~09:32 — real-use crash with the fix installed (not yet explained)

User: real video->lock->wake PASSED once with the fix (deep idle restored). Later, a power-menu "Power off" at
09:18:33 (clean shutdown,userrequested), a normal boot ~09:21, several videos (s5k3t1 + imx576), Power at 09:32:01,
screen off 09:32:02.7, and both disk rings stop at 09:32:04.85 (kmsg ~619 s) with no fault, no suspend entry
logged and no panic. The disk logger flushes ~1 s, so the tail was not captured. The phone then showed backlit
black (warm-reset/early-boot hang, same as `adb reboot` this morning). The host collector had died at 07:30
when USB dropped in the first real sleep (it does not reconnect). Capture: `laptop-logs/recovery-20261006T073540Z`.
Why GX keeps CP state: `drivers/pmdomain/core.c` `a6l_keep_boot_domains()` skips genpd sync_state
("A6L genpd preserving boot provider gpu_gx/gpu_cx"), so boot-on domains are never powered off.
The original user symptom in status.json was also "reboot about 5 s after Power"; the GPU-fault path was seen
tens of seconds after a prefix return. The two may be distinct.

### 6 Oct ~08:00-08:12Z — layer localization after the CP fix

- Wakelock held (no system suspend), several videos, screen off 2 min: OK.
- Releasing it after videos -> real s2idle at 893 s -> crash (host tail ends "Freezing user space processes").
- After videos: pm_test **devices** stage `20261006T080220Z-devices` PASSED + 120 s same-boot.
- After videos: pm_test **platform** stage `20261006T081114Z-platform` -> no RESULT, phone dead within 16 s, backlit black.
  (Yesterday platform passed without video.) => remaining crash is in late/noirq suspend (or their resume) after video.
- The ordinary prefix instrumentation covers only the devices phase (by design). Next: diff genpd/clk/regulator
  summaries fresh vs after-video to find what video leaves on, then targeted platform-stage trials.
- Laptop helpers: /tmp/claude-collector-supervisor-20261006.py (reconnecting host collector),
  /tmp/claude-stage-observe-20261006.py <devices|platform>. pgrep -f matches its own command: use /proc scan.
- Agents: encoder report firmware/extracted/venus-encoder-20261006/README.md (stock max 1080p30; Venus only
  DT-disabled; CX voltage not raised for Venus clocks; camera ISP 1440x1078 is the real limit). Camera colour agent
  writing firmware/extracted/camera-quality-20261006/.

### 6 Oct ~08:30-08:55Z — trigger isolated to camera RECORDING (late/noirq), instrumentation kernel commissioned

Platform-stage matrix on fix build (boot c678eb03 unless noted), each followed by 120 s same-boot observation:
fresh (no camera) PASS (control, also via audio test boot) | Recorder 20 s audio PASS | `screenrecord` 20 s
(c2.android.avc.encoder, no camera) PASS (harness post-copy error only) | camera preview main+front PASS |
90 s 8-core load, SoC 56-59 C PASS | camera VIDEO recording (boot e9474547) CRASH within 16 s.
Static diff after-preview vs after-video (genpd/clk/regulator/icc/rpm): camera state identical; only GPU OPP varies.
Unbinding camss to test crashed by itself: upstream bug, camss_genpd_cleanup() calls dev_pm_domain_detach(NULL)
when SDM660 has no whole-block genpd (genpd_num == vfepd_num). Remove-path only, not the sleep bug; trivial guard fix.
Note: no cpufreq driver active (CPUs at fixed frequency). debugfs not mounted by default (mount -t debugfs).
Commissioned agent: late/noirq prefix kernel -> firmware/extracted/pm-logging-20261005/pm-late-noirq-prefix-20261006/.
Camera: Stage A CCM trial via bind mounts (/home/pierrelouis/A6L-usb-20260915/camera-tuning-trial-20261006/trial.py)
— user: less washed out, stock still better (contrast/brightness/sharpness). Stage B IPA (contrast patch 0030) built and
signed, reproduction byte-identical: firmware/extracted/camera-ipa-stageB-20261006/. Not yet packaged.
Other agent outputs: venus-encoder-20261006/, camera-quality-20261006/, boot-hang-fixes-20261006/ (in progress).

### 6 Oct ~13:50 local — COMBINED vendor installed (current state)

- Boot: late/noirq diagnostic kernel `7bb0aa81…` (inert unless armed; normal kernel was `d1ecec46…`).
- Vendor `e7e95d11…` (kit `/home/pierrelouis/A6L-usb-20260915/rom-r7c-combined-20261006`, receipts in
  `firmware/extracted/pm-logging-20261005/combined-vendor-20261006/`):
  GPU CP-reset fix msm.ko `2bddfcdb…` now PROMOTED into the Android-tree prebuilt (was 34d552; previous copy kept in the
  receipt dir) — includes default-off restore_trace; camera Stage B (IPA `024ccd91…` with patch 0030 + stock TL84 CCMs +
  contrast 1.2 for imx576/s5k3t1/hi846); e-ink patches 0001-0003 (stock REGAL mode, ordered dither default, 100 ms capture);
  `init.qcom.rc` early-init `write /sys/power/pm_async 0` (docs/sleep-async-workaround-20261006.md).
- Runtime boot 1c28f7cb verified (check-runtime-combined.py asserts pm_async=0). Set persist.sys.a6l.eink.refresh=stock.
- Dualux app (system_ext) not rebuilt: its UI lacks the "stock" entry until the next system build.
- Source changes (uncommitted) in workspace AND /home/a6l/android/a6l-lineage24: eink patches, camera prebuilts,
  init.qcom.rc. Agents running: pm-async race root cause -> firmware/extracted/pm-async-race-20261006/.
- Reboot hang: runtime `echo warm > /sys/kernel/reboot/mode` + adb reboot still hung (backlit black) -> boot-hang
  agent's theory incomplete; still open.

### 6 Oct ~14:20 local — async sleep race: root cause confirmed (pending fix test)

Agent analysis firmware/extracted/pm-async-race-20261006/README.md: pm_async only parallelises async-flagged devices
(I2C/MMC/wiphy) in the ORDINARY phases; the GT9769 VCM (4-000c, held runtime-active by libcamera) parks the lens in
its suspend with CCI writes (~1 ms per 16 steps). MMSS NoC/SMMU clocks (mnoc_ahb, bimc_smmu_ahb/axi) are consumed
only by iommu@cd00000 (verified in clk_summary), which gates them in its system suspend; CCI has no link to it.
- Step 1 (pm_async=0, lens 1023, platform, pm_print_times): VCM suspend 81986 us, finishes before cd00000 suspend;
  devices phase 118 ms total. PASS.
- sysrq b FROM ANDROID reboots cleanly (HRST, 40 s) -> usable remote reboot path (adb reboot is the broken one).
- Step 2a: fresh boot c6ca54cf, NO camera use, lens 1023, pm_async=1, devices stage -> phone never returned (hang).
  Durable log /data/local/tmp/claude-vcm-2a/events on the phone (pull from recovery).
Fix candidate staged: DTB adds the 3 clocks to &cci (stock does): boot-cci-mmssclk.img f517fef5… (= installed
7bb0aa81 with only the DTB changed), boot-only kit /home/pierrelouis/A6L-usb-20260915/rom-r7c-cci-20261006
(extra/start-cci-flash.py, check-cci-install.py, check-runtime-cci.py). Verify: lens 1023 + pm_async=1 devices/platform
x3 must pass, then video + real sleep with pm_async=1, then drop the init.qcom.rc workaround.

### 6 Oct ~16:30 local — round 2 installed; HARDWARE ISP M1 WORKS ON THE PHONE; system build running

- Installed: boot f517fef5 (CCI DTB fix on the late/noirq diag kernel), vendor 9b0a200f (round 2). pm_async=1 (default),
  workaround removed from init.qcom.rc. persist.sys.a6l.eink.clear_every=30, refresh=stock. Commit 7c75af6 pushed.
- Remote reboot: `sync; echo b > /proc/sysrq-trigger` from Android works (HRST). adb reboot / Restart still hang.
- HW ISP milestone 1 (firmware/extracted/hw-isp-20261006/, evidence phone-test-20261006/): camera stack disabled via
  persist.vendor.a6l.camera=0 + sysrq reboot, manual insmod of qcom-camss-m1-rom1.ko a6l_pix=1. Colour bars perfect at
  28 fps (1440x1078 NV12 via VFE scaler); full 2880x2156 NV12 at 29.3 fps, CPU idle; real scene (live.png) correct
  but rotated/cyan/overexposed (no 3A). Sensor test pattern persists: use -t 0. Every frame logs `VFE: violation = 0xd`.
  Restored stock stack (persist 1 + sysrq). Agent continuing to milestone 2 (violation fix, stats, AE/AWB/AF, libcamera).
- User feedback round 2: main cam most off, selfie close to stock; AE adaptation ~2 s; noise on all; AF unreliable
  (photos in firmware/extracted/round2-feedback-20261006/media). E-ink: drawer artifacts still moving; SEVERE LCD
  scan-out corruption after e-ink -> LCD switch (screenshots normal => scan-out level). Agents: camera-round3, eink-round3.
- System build running: e-ink 0010-0012 applied (Dualux reconcile + high-contrast + frameworks/base 0004 instant fling,
  applied to frameworks/base working tree like the pipeline prep does). Output round2-system-20261006/.

### 6 Oct ~17:00 local — HW ISP M2 closed loop works on the phone; camera round 3 + system kit ready

- HW ISP test A (M1 module + ispcap 12344faf `-a ae,awb,ccm,af`): 29.6 fps, AE/AWB/CCM/AF converge; image neutral,
  well exposed on a backlit window, sharp (phone-test-20261006/auto.png). Test B: m2a module (c998cc7c) loads after a
  guarded rmmod of M1 (0000 guard verified: no oops); contrast 1.2 applied (auto-c12.png). Violation 0xd A/B inconclusive
  (+10 bars xform=1, +0 xform=0, +0 auto xform=1). Agent continues: violation, libcamera integration + packaging.
- System kit staged: /home/pierrelouis/A6L-usb-20260915/rom-r7c-r2sys-20261006 (system 4f29076d: framework.jar fling,
  A6LDisplaySwitcher.apk, system_ext sepolicy; Aperture.apk inside = 0238cb6b, identical to the /data/app trial the
  phone runs). extra/start-r2sys-flash.py, check-r2sys-install.py. Commit 89212e8.
- Camera round 3 (firmware/extracted/camera-round3-20261006/): AF 0034 (rescans climb from current position, keep
  focus after lock), AE 0035 (faster convergence), chroma denoise 0036 (core) + 0037 (IPA). IPA-only build be807526;
  core-coupled build libcamera.so 0de15051 + IPA 1e3e20be (same signing key; core reproduced byte-identically).

### 6 Oct ~17:55 local — HW ISP staged trial: kernel + libcamera streaming STABLE; HAL sees 0 cameras (fixing)

Round 3 installed (vendor b6dca76e + system 4f29076d, boot f517fef5); commit f745424. Stopgap on phone:
persist.vendor.eink.copy_guard_kib=0 (0014 discard storm). Settings app e-ink tab patch edited (workspace) to add "stock".
Agents: eink-round4 (latency/0014/epdd stall/switch time/remove Dualux settings UI), eink-lockscreen (clock+battery
bottom-left, settings in Settings tab), reboot-hang (device_shutdown/notifier hypothesis), hw-isp (HAL init fix).
HW ISP trial (firmware/extracted/hw-isp-20261006/m2/package/trial, m3 camss a76e6b1d, libcamera 8d1815e2):
load/ispcap/bind/lclist/lccap all OK (lccap 60 frames 29 fps 0 errors through libcamera — the earlier wedge is fixed
by the PIX scratch-buffer mode). Provider: HAL camera_capabilities init fails for all cameras ("invalid
configuration") -> 0 cameras -> app has no camera. Logs phone-test-20261006/trial-20261006/. Phone left on the
camera-disabled trial boot a0429c4a with m3 loaded and bind mounts; restore with persist.vendor.a6l.camera=1 + sysrq.
Driver: /home/pierrelouis/A6L-usb-20260915/hwisp-trial-20261006/drive.sh (run via ssh with MSYS_NO_PATHCONV=1).

### 6 Oct ~18:10 local — CAMERA APP RUNS ON THE HARDWARE ISP

libcamera 18a73579 (NV12-only hw path, validate adjusts, RAW reported unavailable) fixed the HAL init: halinit 3/3,
provider ready with 3 cameras (LIMITED), Aperture preview ~26.5 fps via VFE (app-preview.png: oriented OK, AF blurry,
grainy at max gain, slight cast). Aperture video 1280x720 H.264 = 19.7 fps (205 frames/10.4 s; was 9.3 fps on the CPU
ISP), AAC OK; bottleneck now the software encoder. Evidence: firmware/extracted/hw-isp-20261006/phone-test-20261006/
trial-20261006/ (lit.png = ispcap with AF best 408, app-preview.png, hwisp-video.mp4, host logs). Phone still on the
trial boot a0429c4a (camera-disabled boot + manual insmod + bind mounts). Agents: hw-isp (AF in app, packaging 0201 for
vendor, default hwisp ON with persist.vendor.a6l.hwisp=0 fallback), venus-impl (hardware encoder), eink-round4,
eink-lockscreen, reboot-hang.

### 6 Oct ~20:30 local — round 4 integration (user away): HW ISP packaged, lock screen, reboot guard

Applied to BOTH trees (workspace + /home/a6l/android/a6l-lineage24):
- HW ISP packaging `firmware/extracted/hw-isp-20261006/m2/package/0201` + device-payload (PAYLOAD-SHA256SUMS OK):
  libcamera.so 06d73d6f (hw backend, linear-domain AF metric, per-sensor `persist.vendor.a6l.hwisp.sensors`, default
  imx576), `*_hwisp.yaml` (r3-A based), a6l-modules.sh passes `a6l_pix=1` to qcom-camss unless
  `persist.vendor.a6l.hwisp=0`, hal_camera get_prop vendor_a6l_prop. qcom-camss.ko m3 a76e6b1d in the Android-tree
  prebuilt only (previous 43c5d6ca saved as /home/a6l/android/qcom-camss.ko.rom1-43c5d6ca.bak;
  tools/stage-rom-v2-prebuilts.sh would overwrite it).
- E-ink lock screen LS1-LS4 (`firmware/extracted/eink-lockscreen-20261006/patches`, README §4, §9 test plan, §11 RTC):
  epdd lockframe, a6l_einklock daemon (vendor), Dualux app lock background/sync (system_ext), Settings
  `0002-a6l-eink-lockscreen.patch`. RTC wake from s2idle unproven (T0 not decisive); LS2 falls back to "Mis à jour à
  HH:MM" when minute ticks run late. Decisive test: `eink-lockscreen-20261006/tools/rtc-wake-test.sh arm 90` / `collect`.
- Settings (Android tree packages/apps/Settings): old 0001 reversed, new 0001 (refresh list with "stock") + 0002 applied.
- Reboot-hang fix A (vendor reboot guard: on shutdown warm mode + panic=5, shutdown-critical a6l_reboot_guard does a
  logged sysrq-b if init sits in reboot(2) >= 15 s) and fix B (qcom-wdt with watchdog_stop_on_reboot, stripped
  dfda442c; previous 63580d3b saved as /home/a6l/android/qcom-wdt.ko.prev-63580d3b.bak), from
  `firmware/extracted/reboot-hang-20261006`. Fix C (panel) NOT applied: only if E1 (sysrq-b with LCD off) hangs.
Builds (pm-logging-20261005/): round4a-vendor (HW ISP + LS1/LS2) ad05b869 audited (adds a6l_einklock + rc + hwisp
yamls; nothing removed); round4a-system and round4b-vendor (+ fixes A/B) building. E-ink round 4 and Venus agents
still running.
- ~22:00: e-ink round 4 (`firmware/extracted/eink-round4-20261006`, patches 0017-0022) applied to both trees:
  epdd CAP_BLOCK_SUSPEND (the 64 s stall + returning MDP faults), per-plane copy guard (no 15-45 s freezes; replaces the
  copy_guard_kib=0 stopgap: clear it after flashing), pipelined capture/early reply/chained updates (latency), no
  contrast change during a switch (3 s -> ~1 s), Dualux settings screen removed (tile long-press opens Settings >
  Display > E-ink), Settings 0002 regenerated. Settings sources in the Android tree re-patched (0001+0002 new).
- KITS on the laptop (both: boot f517fef5 + dtbo retained, vendor+system written; helpers in extra/:
  start-round4-flash.py, flash-progress-round4.py, check-round4-install.py, check-runtime-round4.py):
  - rom-r7c-round4e-20261006 = PRIMARY: vendor 6088e10b + system 759b089f (HW ISP + lock screen + reboot fixes A/B +
    e-ink round 4).
  - rom-r7c-round4-20261006 = fallback without e-ink round 4: vendor b545064a + system 8f1158d8.
- Venus encoder (`firmware/extracted/venus-impl-20261006`): built offline, NOT in the kits. Stage A needs
  boot-venus-cx.img (cdcce6ac, DTB 209dfd65: mmcc on CX, venus okay); staged separately for an attended test.

### 6 Oct ~21:15-23:00 local — round 4e installed; boot loop fixed (4f); user feedback; night plan

- Round 4e flashed (vendor 6088e10b + system 759b089f). First boot LOOPED: system_server
  `Signature|privileged permissions not in privileged permission allowlist: org.lineageos.a6l.dualux:
  android.permission.READ_WALLPAPER_INTERNAL` (LS3 added the permission, the privapp xml was not updated). Live
  workaround: bind mount of a patched /system_ext/etc/permissions (lost on reboot). Source fix:
  device/hisense/a6l/eink/switcher/app/privapp-permissions-a6l-dualux.xml (both trees). System 4f f2d388ae; kit
  rom-r7c-round4f-20261006 (vendor 6088e10b + system f2d388ae) — flashing at ~22:50.
- Runtime on 4e: hw-ISP a6l_pix=1 for the main camera, a6l_einklock running, reboot guard + qcom-wdt dfda442c present,
  copy_guard_kib cleared.
- User: main camera works (photos/videos; low light noisy, stock a bit less noisy). Drawer artefacts fully back
  (0018 regression vs 0014). E-ink -> lock -> open LCD => HARD RESET (kmsg ends 468.9 s, no panic/shutdown), no lock
  picture; the following boot never reached the logger (backlit black), user went to recovery. Logs:
  firmware/extracted/round4e-feedback-20261006/.
- Night agents: eink-round5-20261006 (reset, lock screen, drawer), hw-isp m3 (front/wide on HW path, low-light noise,
  AF check; gets the phone exclusively after the 4f boot). Then Venus stage A/B (kit rom-r7c-venusA-20261006).

### 7 Oct ~00:00-01:00 local — night work

- 4f installed via a kit staged against the INSTALLED build (Prepare/updater refuse a kit whose "previous" build
  differs: stage-round4-kit.py now takes the previous kit tag). Clean boot, no crash.
- E-ink round 5 (`firmware/extracted/eink-round5-20261006`, 0023-0026, vendor only) applied to both trees:
  drawer artefacts on 4e were partly MY slip (stopgap property cleared without restarting a6l_eink_mirror, which
  kept `--copy-guard-kib 0`), plus a real 0018 hole (forced torn copy); 0023 fixes both. No lock picture: a6l_dualux
  also lacked CAP_BLOCK_SUSPEND (0024). Hard reset (e-ink lock -> LCD) not pinned; 0024 removes the likely e-ink
  trigger (no e-ink modeset near an LCD switch), 0025 kmsg markers, repro/ scripts. Vendor 514a9561 = kit round5e.
- HW ISP round 6 (`hw-isp-20261006/m3`): phone trial passed 00:00-00:50 (front S5K3T1 fixed: PIX pixel clock bounded
  by the CSI-2 link rate; wide OK; ABF/BPC; low-light 15 fps extension; AF locks). NOTE: kernel has NO cpufreq driver
  (CPU slow) — the chroma filter was NEON/threaded to compensate; cpufreq is a separate future item.
  0202 + payload applied to both trees (libcamera 5d048ba2, IPA d4db6a61, camss m4 3a6a3c0f build-tree only;
  previous camss saved /home/a6l/android/qcom-camss.ko.m3-a76e6b1d.bak). Vendor round6 bc53a6f0 (e-ink r5 + hwisp r6).
- Remote recovery entry works on this build: `adb -s 1e529013 reboot recovery` -> recovery in 25 s.
- Venus: stage A boot cdcce6ac flashed (kit rom-r7c-venusA-20261006 on 4f) and PASSED (CX subdomains, perf 256 as
  baseline, no deferred, display/camera OK). Stage B: firmware authenticated, encoder /dev/video7 in 0.1 s; bench
  failed on its own format negotiation (G_FMT stride 0) -> Venus agent fixing and rerunning on the phone.
- ~01:25: Venus agent found the real stage-B cause: on HFI 3xx the venus driver never gets codec size limits from
  the firmware -> formats clamped to 0x0. Patched venus-core.ko + stage-b2.sh in venus-impl-20261006 (kmod/,
  test/kit/venus-b). NOT run: the agent was blocked by the permission system from the laptop ssh; left for the user to
  decide (run stage-b2 from the venusA boot).
- INSTALLED NOW (phone booted to Android, boot ffbd8b1b): kit rom-r7c-round6-20261006 = boot f517fef5 (Venus DT
  reverted for clean morning tests) + vendor bc53a6f0 (e-ink r5 + hw-ISP r6) + system f2d388ae. Runtime check passed:
  mirror runs with --copy-guard-kib 1024, libcamera 5d048ba2, a6l_pix=1, a6l_pix_linkcap=1, einklock running, no crash.

### 7 Oct daytime — round 7 series; restart hang = charger mode (likely); state for the next session

- Installed: boot 2a1b8a8f (f517 Image + DTB: Venus CX vote, inert cpufreq OSM nodes, CCI stock clock order) + vendor
  e1202a72 (7h: camera r7, e-ink 0023-0034, hwc 0004/0005, panel fix C, msm restart-skip dabd16e0 — NOT a fix, revert
  to 2bddfcdb later) + system f2d388ae.
- STAGED NOT FLASHED: rom-r7c-round7i-20261006 = boot 10affa76 (2a1b8a8f + cmdline `androidboot.mode=normal`) + vendor
  61f9f556 (7h + a6l-krec recorder on reserve2). Test: /proc/cmdline + ro.boot.mode=normal, then restart with USB.
- Restart hang: the user's console photos show the stuck boot loading msm at 6.7 s with NO "display: boot log" wait
  line => ro.bootmode=charger (ABL picks charger on USB_CHG). Charger mode = no mount_all/adb/UI = backlit black.
  sysrq-b (HRST) boots fine. Fix: force-normal cmdline (above). Quick confirm: restart with USB unplugged.
  Analysis + krec reader: firmware/extracted/restart-hang-20261007/README.md.
- E-ink: LCD wake verified healthy on 4d9c50bf (no EBUSY, touch back each wake); 0034 removes the residual ~250 ms
  stall. eink-round6-20261007/README.md parts A-C.
- Open: freeze at s2idle after FRONT camera + e-ink lock (14:0x boot); front-cam sleep test at 15:4x left the phone off
  adb (asleep or frozen, unverified). mdss_ahb_clk "stuck at on" warning at suspend (display analogue of the CCI fix).
- CPU freq (cpufreq-20261007): rev 3 modules with step markers + L2 SAW init, run A = gold only. Load 1 froze at the OSM
  insmod. Note logical cpu0 silver, cpu1-4 gold, cpu5-7 silver.
- Venus (venus-impl-20261006): hfi3e bisect kit ready (stage-b6.sh); hfi3d froze at the first encode; needs the venus
  DT boot (cdcce6ac or 2a1b8a8f — both carry the Venus CX vote).
- Camera mirroring request (user): when on the e-ink, mirror the rear camera preview and not the LCD-side camera.

### 7 Oct evening — round 11 installed; cpufreq bring-up (rev 1-12); state for the night

- INSTALLED: boot 10affa76 (force-normal cmdline) + vendor 1bcd0779 (round 11: e-ink 0023-0041, hwc 0004/0005,
  0032 fixed planes, panel fix C, 0037 front-touch re-bind, msm 2bddfcdb, camera r7, krec) + system acff1ea3 (round 10:
  Aperture user-facing mirror r2, Dualux 0038). Verified: restart with USB (charger-mode fix), e-ink→LCD ~1 s,
  speckles gone, camera mirror on the e-ink, LCD touch at boot. Pending user check: LCD→e-ink stale page / rear-touch
  page jump (0039/0041).
- CPU FREQ (firmware/extracted/cpufreq-20261007, rev 12 modules; NOT in the ROM, nothing auto-loads):
  - freezes solved: per-read ioremap/iounmap with delay (rev 8); console/GCC/SAW reads were red herrings.
  - bin 1 tables (gold top 2208 MHz) fixed (rev 9); genpd attach ordering (rev 11: CPRh setup after attach).
  - both OSM domains enable, policies 0 (0,5,6,7) and 1 (1-4), fused voltages applied.
  - BUT the real clock stays at index 0 (299 MHz measured by add-chain, both clusters): rev 12 self-test STUCK;
    pstate_status=0, saw_pmic_sts=0 at top index -> the CPRh->L2 SAW->PMIC voltage path is not configured (stock
    msm_spm programs saw2-avs-ctl/limit + PMIC data). Next: decode SAW2 v4.1 layout from stock, write stock SAW config
    (attended only - voltage path).
  - Note: logical cpu0 silver, cpu1-4 gold, cpu5-7 silver. Stage logs: phone-rev*.txt in that folder.
- Night agents: selinux-20261007 (rules from 2840 avcs) + mdss-ahb-20261007 (mdss_ahb stuck-at-on fix).
- Venus: hfi3e bisect kit ready (stage-b6.sh), needs Venus DT (boot 10affa76 has it).

### 7-8 Oct night — rounds 12/13: SELinux prep, Venus H.264 encoder in the ROM, e-ink 0042-0044

- Venus encoder (venus-impl-20261006): hfi3f/prod bench = 1080p 43 fps max, 1080p30 paced 29.99 fps, eos ok.
  Warm power collapse is broken on this firmware (resume ok, SYS_INIT never answered). prod2 = cold cycle
  (pas_shutdown on runtime/system suspend, full mdt boot on resume) but still -110: CPU_CS_SCIACMDARG0 kept the old
  image's status, so the boot wait passed instantly. prod3 (core 0fd410f0, enc 5f411501, dec 562dae0a) clears it:
  stage-b9b-pc 5 cold resumes in ~100 ms, all encodes pass; stage-b9b-sleep A (keep_on=0) and B (keep_on=1) encode
  after real s2idle. keep_on default 0 (core powered only while a session is open). system sleep = force_suspend =
  cold cycle; an open session refuses suspend (-EBUSY).
- Stage C (stagec/integration.diff v3 + update-prod2/prod3): v4l2_codec2 built in-tree (patches 0001-0003 applied
  to external/v4l2_codec2 by hand - the pipeline would do it), media_codecs_c2 rank 256, H.264 only (no HEVC
  component), persist.vendor.a6l.venus=1 (0 = software only), module lists video.txt/video-dec.txt.
  NOTE: I staged the Venus prebuilts into the Lineage tree by hand (modules, firmware, module lists + rom-files.mk
  lines) instead of re-running tools/stage-rom-v2-prebuilts.sh, which would rebuild the whole prebuilt set and could
  drop the direct overrides made since round 4. Re-running it later must be checked against the current tree.
- SELinux prep (selinux-20261007 pass 1, committed 2036362): built with A6L_SELINUX_PREP=1 + a6l_selinux_prep.py
  applied to the tree (rc seclabels dropped, /dev/dri card 0660). sys.use_memfd moved to system_ext props -> vendor
  and system MUST be flashed together. Round 12 avc: 320 lines (r11: 2840). Pass 2 ready, NOT applied:
  selinux-20261007/round12/0002-a6l-sepolicy-r12-pass2.patch (renames a6l_modules_{adsp,misc,offcharge} to vendor.*
  because init drops on-property triggers on init.svc.a6l_* under enforcing; renderD128 check; composer backlight).
- E-ink round 7 (eink-round7-20261007): 0042 e-ink warm-up during the LCD->e-ink handshake (default on,
  persist.sys.a6l.eink.prewarm=0 off), 0043 opt-in inverted rendering (persist.sys.a6l.eink.invert=1), 0044 reader
  sleep phase 1 (Settings > E-ink, default off, needs Screen lock: None). Settings patch 0003 applied in the tree.
- Builds: build-round12-image.py (round 8 pins + A6L_SELINUX_PREP=1). Round 12 = vendor cc41d1d0 + system 464bfd42
  (flashed, booted). Round 13 = vendor 859e9d7e (venus default 0) + system 38805b07 (flashed 23:27, booted clean).
  Round 13c = round 13 + prod3 + venus default 1 (building).
- Lesson: never leave a sleep test on stay_on_while_plugged_in 0 + plain `sleep N` - N counts only awake time and the
  phone sat in a 1 s/min wake loop for 20 min (screen_off_timeout is 120 s, not infinite). Use a wall-clock wait +
  RTC wakealarm, stayon back on afterwards.
- s2idle shows "Watchdog detected hard LOCKUP on cpu 5/6" reports (pre-existing, also in round 10). Non-fatal unless
  kernel.hardlockup_panic=1 (some bench scripts set it).
- 8 Oct 00:30 INSTALLED: round 13e = boot 10affa76 + vendor 3f4f025b (13c + v4l2_codec2 0004) + system ac1ca70a (13d:
  system_ext init.a6l.codec2-selection.rc). Verified remotely: venus ready at boot, media.c2.hal.selection=aidl,
  MediaCodecList lists c2.v4l2.avc.encoder first, screenrecord encodes on Venus (High@4.1), cold resume ~110 ms,
  powered off when idle, stage-b9b-sleep A/B pass on the integrated build. Codec2 HIDL vs AIDL: the platform default
  is hidl; AIDL is needed to see the v4l2 store (ro.vendor.api_level 202604 allows it).
- Morning (user): Aperture 1080p/720p recording (ffprobe: High/41, ~900 frames per 30 s, IDR every 30 frames),
  video playback in Gallery + audio (AIDL bufferpool2 path), LCD->e-ink switch time (0042), retest the LCD->e-ink page
  behaviour (0039/0041), optional reader sleep (Screen lock: None). Then attended: cpufreq SAW test, mdss_ahb fix,
  SELinux pass 2 (round 14) + an enforcing test boot.

### 8 Oct afternoon/evening — rounds 14-20: speaker, Wi-Fi, e-ink, CPU DVFS, hardware decoder

Full feature matrix: `docs/port-status.md` (top section, kept current). Key findings, in order of discovery:
- Speaker silent after sleep (audio-silent-20261008): one suspend reset the LPASS LPI speaker pads (gpio4-7 0xd0 -> 0x2ca)
  when a stream ran across the suspend. Fix: pinctrl-lpass-lpi restores pads on resume + snd-soc-sm8250 holds a wakeup
  source while any DSP back end runs. Verified by Pierre (round 16).
- Wi-Fi (wifi-20261008): (1) system_server's Nl80211Native initialises once before cfg80211 loaded -> cfg80211 + rfkill
  moved to the early base list (round 15). (2) Suspend while associated tore the link down -> WLAN PD crash (takes the
  modem down) -> WoWLAN triggers in wpa_supplicant_overlay.conf (round 17). (3) Wi-Fi off crashed the WLAN PD: the
  TXBF vdev param sent after VDEV_DOWN (bisected from a debug_mask=0x700132 trace); ath10k_core 0005 disassoc_quirks=1
  skips it (live A/B: 4 passes, control crashes) -> round 21.
- E-ink (eink-round8-20261008): mirror capture guard was the switch cost (not the theme); 0045 benign flips (tore) ->
  0048 exact verification -> 0051 no stitched planes; hwc 0006 GPU compose on the e-ink; 0047/0050 contrast; 0049 dark
  keyguard text; 0053 no carrier marquee; 0052 `a6l_eink_mirror --send dump` writes /data/vendor/epd/last-frame.pgm
  (a6l-eink-dump.sh) for remote checks; 0054 logs every e-ink setting change (lock_clock had flipped to 0 between 08:36
  and 09:16 on 8 Oct, writer unknown - probably the Settings toggle).
- CPU DVFS (cpufreq-20261007 rev 13b): the L2 SAW AVS init (stock AVS_LIMIT 0x4580458 / AVS_CTL 0x1010031) was what kept
  the OSM at ~300 MHz. With a6l_saw_init=1: both clusters to the top on fused open-loop voltages; sweep + 3 min all-core
  + 1080p-encode stress OK. In the ROM since round 19 (rom/modules/cpufreq.txt, pinned taskset 1, tested parameters).
  One unexplained hard freeze: cpufreq (fused) loaded by hand + b11d live venus module swaps.
- Venus decoder (venus-impl-20261006 prod5c-e, Codec2 0007-0009): shared subcore power toggling (prod5c), unknown -1x-1
  size (prod5d/e), filled_len minus data_offset, no CAPTURE G_FMT before headers (0007), MMAP bitstream input + csd merge
  (0008), ByteBuffer recycling pool (0009). b11d ACCEPT=1. Round 20: venus before cpufreq in misc + `restart media` on
  vendor.a6l.venus=ready (system_ext rc), so MediaCodecList has the c2.v4l2 codecs at boot.
- Camera IQ (camera-iq-20261008): see port-status camera row; V1 tuning A/B and core patch 0105 pending.
- Disk: C: hit 0 bytes free (128 GB of per-round images); superseded *.img/*.erofs deleted with Pierre's OK (94 GB).
- WSL default user changed to a6l: run tree edits and builds as root (`wsl -u root`); out/ is root-owned.
