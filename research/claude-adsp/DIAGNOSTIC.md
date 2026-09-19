# Proposed physical diagnostic: one bounded ADSP start/stop (for the coordinating agent)

Status: **proposal only**. Nothing here has run on hardware. Candidate A uses
only already QEMU-checked, unmodified modules and a three-property DT overlay.

## Purpose and bounds

Start the stock, signed ADSP image once through the pinned `qcom_q6v5_pas`
driver, record (1) PAS authentication/relocation, (2) the ready/handover
handshake, (3) in-kernel service discovery (rpmsg channels, QRTR services),
then stop it cleanly. **No** audio playback, microphone, APR/q6*/codec/sound
modules, modem, CDSP, Wi-Fi, eMMC writes, module unloads, or reboots inside
the script. Everything lands in tmpfs and is pulled over the authenticated ADB
session, then the phone returns to stock exactly as after V46/V47.

## Preparation (offline, WSL, coordinator)

1. Image: package the V46/V47 recovery kernel with `patches/a6l-adsp-diag.dtso`
   applied to `base.dtb` (existing fdtoverlay + captured-ABL pipeline).
   Acceptance: the scoped DT diff equals `checks/adsp-dt-audit.json →
   overlay_effect_diff` (exactly `/soc@0/remoteproc@15700000/status`,
   `/soc@0/remoteproc@15700000/firmware-name`, `/chosen/hisense,a6l-adsp`);
   kernel, ramdisk and command line unchanged; captured-ABL checks pass.
   `tools/audit_adsp_dt.py <new merged.dtb> <outdir>` must still pass 30/30
   (it also confirms `adsp_pil` was `disabled` in the input and `okay` after
   simulation; run it on the pipeline's real merged output too and compare
   hashes with `checks/candidate-adsp-merged.dtb` — a byte-identical result is
   not expected because string-table layout differs, property equality is).
2. Payload: the 10 modules of `tools/adsp-diag-modules.txt` (hash-check
   against `peripheral-prep-20260917/module-manifest.json`), `order.txt` made
   from that list, the 21 firmware files `adsp.mdt adsp.b02..b21`
   (hashes in `checks/adsp-mdt-loader-audit.json`), `tools/a6l_adsp_diag.sh`,
   optionally `a6l_qrtr_lookup` built statically for AArch64 from
   `tools/a6l_qrtr_lookup.c`.
   Optional QEMU pre-check: load the 10 modules in the existing diskless
   fixture in this order (they were part of the 103/103 pass; the subset
   order is new).
3. Do **not** put the firmware under `/lib/firmware` in the ramdisk: the
   pinned driver auto-boots at insmod if it finds the file (EVIDENCE D3).

## On-phone sequence (attended, ~2 min)

```
adb push modules/  /tmp/adsp-diag/modules/      # incl. order.txt
adb push firmware/ /tmp/adsp-diag/firmware/
adb push a6l_adsp_diag.sh a6l_qrtr_lookup /tmp/adsp-diag/
adb shell sh /tmp/adsp-diag/a6l_adsp_diag.sh    # HOLD_S=20 default
adb pull /tmp/adsp-diag/evidence captures/<session>/adsp-diag/
```

Phases of the script (each snapshot = dmesg, /proc/interrupts, genpd summary,
rpmsg devices, remoteproc state, meminfo):

0. Preflight (read-only): DT marker `diag1`, `adsp_pil` status `okay`, smem /
   smp2p-adsp / scm / mailbox / hwlock / 5100000.iommu bound, firmware hashes,
   no firmware yet installed. Aborts with exit 2 on any mismatch.
1. insmod in order; expect `remoteproc remoteproc0: adsp is available`, then an
   auto-boot attempt that fails harmlessly (`request_firmware failed: -2` or
   `Direct firmware load for qcom/hisense/a6l/adsp.mdt failed with error -2`),
   state `offline`. Abort (exit 3) if a module fails or the state is not offline.
2. Copy firmware to `/lib/firmware/qcom/hisense/a6l/`, `recovery = disabled`,
   `echo start > state`, poll up to 30 s.
3. If `running` and no stop condition: hold `HOLD_S` s, list rpmsg devices and
   QRTR services (read-only lookup) twice.
4. `echo stop > state`, poll up to 15 s for `offline`, final snapshots, write
   `RESULT` (`PASS`, `PASS-START` if start passed but stop was unclean, `FAIL`).

## Expected log lines (kernel, candidate A)

Success path, in order:
```
remoteproc remoteproc0: adsp is available
remoteproc remoteproc0: powering up adsp
remoteproc remoteproc0: Booting fw image qcom/hisense/a6l/adsp.mdt, size 7964
(qcom_pil_info: writes IMEM entry "adsp" 0x92a00000/0x1e00000 - silent)
remoteproc remoteproc0: remote processor adsp is now up
```
Between "Booting" and "is now up": PAS_INIT_IMAGE with the 7964-byte metadata,
PAS_MEM_SETUP(1, 0x92a00000, 0x1e00000), 20 `adsp.bNN` loads (each a
`request_firmware` of an exact size), AUTH_AND_RESET, then the smp2p `ready`
bit within 5 s; `handover` (bit 2) follows once the ADSP has taken its own
votes. Afterwards: `qcom_glink` channel open messages / new entries in
`/sys/bus/rpmsg/devices` (expect at least `IPCRTR`, `sys_mon`,
`apr_audio_svc`, `fastrpcglink-apps-dsp`, `adsp_apps`-style names; exact set is
firmware-defined and is itself the evidence), `qrtr` node announcement, and
`a6l_qrtr_lookup` listing at least SSCTL (service 43, instance 0x14 =
version/instance decode 20) from a non-local node.
Stop path:
```
(sysmon: SSCTL shutdown request or sys_mon channel; or smp2p stop bit)
remoteproc remoteproc0: stopped remote processor adsp
```

Failure signatures and what they mean:
* `error -22 reading firmware … metadata` / `no hash segment found` — loader
  rejected the .mdt (should not happen: EVIDENCE D2).
* `error <n> initializing firmware qcom/hisense/a6l/adsp.mdt` — TZ rejected
  PAS_INIT_IMAGE (metadata/signature/rollback). Authentication blocker;
  record the SCM return code; do not retry with modified files.
* `error <n> setting up firmware` — PAS_MEM_SETUP refused the region
  (memory ownership/hypervisor). Do not change addresses blindly; compare with
  `/proc/device-tree/reserved-memory` captured in phase 0.
* `failed to authenticate image and release reset` — AUTH_AND_RESET failed
  (signature, or TZ-side resource state). Blocker; capture SCM code.
* `start timed out` — TZ accepted the image but no `ready` within 5 s. Most
  likely the CX proxy difference (EVIDENCE D5) or SMMU ownership (D6). Next
  step: candidate B, not repeated retries.
* `watchdog received: …` / `fatal error received: …` with the SMEM 423 text —
  ADSP booted then crashed; the text is the evidence (often names the missing
  service or memory).
* `arm-smmu 5100000.iommu: Unhandled context fault` or global fault counters
  advancing — SMMU ownership issue (D6).
* `timed out on wait` at stop — stop-ack missing; the script still expects
  `offline` after PAS_SHUTDOWN.

## Success criteria

PASS = all of: state reached `running` within 30 s without any stop-condition
line; at least one new rpmsg device appeared on the `lpass` edge; state stayed
`running` for `HOLD_S`; `stop` reached `offline` within 15 s; no `BUG`/`Oops`;
arm-smmu fault counters unchanged. Service-discovery bonus: QRTR SSCTL
instance 0x14 listed. Anything less is reported as the specific failure
signature above, never as "ADSP works".

## Stop conditions and cleanup

* The script stops the ADSP itself on any failure signature, on state change
  during the hold, or after the hold; it never restarts it (recovery disabled).
* If the shell hangs > 90 s (e.g. an SCM call that never returns), the
  coordinator ends the session with the normal power-off/return-to-stock
  procedure; no persistent state exists (tmpfs only).
* Thermal/current: the ADSP idle after boot is a normal stock state; no
  playback means no amplifier activity. Abort if the phone becomes hot or
  the USB link drops.
* Modules stay loaded until the session ends (no rmmod, EVIDENCE D10).

## Uncertainties that could prevent activation (flagged)

1. **CX proxy vote resource** (EVIDENCE D5): stock votes `rwlc`/0 at TURBO;
   the pinned tree/driver votes no level at all. Candidate B is prepared but
   requires a module rebuild and is untested.
2. **LPASS Q6 SMMU ownership** (D6): stock skip-init vs mainline reset.
3. **TZ authentication** of the split image under a foreign kernel with the
   captured (Magisk-patched, AVB flags=2) boot chain cannot be predicted
   offline; the diagnostic's first two SCM calls answer it.
4. **Auto-boot race** (D3): guarded by installing firmware after insmod; if the
   coordinator's ramdisk already contains `/lib/firmware/qcom/hisense/a6l/`
   the script aborts in preflight.
