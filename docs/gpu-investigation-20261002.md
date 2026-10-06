# Hardware graphics investigation — 2 October 2026

Pierre requests prioritizing usable hardware rendering, then bundling the
camera, radio and confirmed graphics fixes into one next ROM. No camera/radio
only flash is planned. Those two fixes compiled successfully; no GPU fix is
confirmed and no new ROM is ready to flash.

r6f with Mesa FD512 and `sysmem` rendered responsive Settings for about eleven
minutes but still cropped glyphs. Opening Aperture triggered the first GPU
ringbuffer drain timeout at uptime 659.938 s and a5xx hang detection at 659.939 s.
The fault report's C30001C1 value is RBBM status, **not** an SMMU fault address
or proof of a particular shader error. Camera service had no active client at
that point. Full fault evidence is retained in
`logs/r6f-install-20261002/camera-stuck-20261002T070616Z/`.

The local kernel a5xx handler reports this line from hardware hang detection.
No specific root cause can be inferred from the status word alone. The existing
driver recovery repeatedly faults and cannot reliably restore the interface.
The cropped-text screenshot proves corruption before panel scanout; Pierre's
clock-resizing observation suggests testing scale and shader precision, but
does not establish the failing stage or show that the text and hangs share a
cause.

Both installed Mesa ABI builds use 26.1.0-devel from the existing NDK source
copy. In `freedreno_screen.c`, `FD_DBG(NOFP16)` disables the advertised fp16
capability for a5xx/a6xx fragment and compute stages. The `nofp16` option therefore
changes a real path on Adreno 512 while retaining hardware rendering.
`serialc` disables asynchronous shader compilation; it does not serialize GPU
command execution. `noblit` disables the a5xx custom blitter. On a5xx, texture
tiling is enabled through the experimental `ttile` flag; do not assume `notile`
will explain all corruption without checking buffer origin and imports.

Next controlled test launched from recovery on the already installed r6f:
guard the fresh boot/build and wait for persistent properties to finish loading,
disable radio before boot completes, retain `sysmem`, add only `nofp16`, restart
SurfaceFlinger, verify FD512 and modem offline, restart Settings, then compare
Display and the expanding shade clock. Original geometry remains 1080x2340.
Live laptop kernel/logcat streams start before the compositor restart.
The property change is a diagnostic candidate, not a graphics fix or a flash.

Observer:
`~/A6L-usb-20260915/rom-r6f/extra/gpu-nofp16-20261002.py`.
Logs:
`~/A6L-usb-20260915/rom-r6f/logs/gpu-nofp16-20261002/`.
Fresh boot ID `9fd2c287-6574-4aba-abcc-fc8ac611e676` confirmed. Radio property
is 0; only ADSP remoteproc is present (modem modules were not started).
Observer initially rejected the absent modem sysfs node; corrected the guard
to accept absent/offline, verified this same boot, and resumed the compositor
step without another reboot. GLES reports freedreno FD512, ES 3.1 Mesa
26.1.0-devel. `sysmem,nofp16` property and original geometry confirmed.
After compositor restart, the first Settings launch ran before framework policy
initialization; a later retry at uptime about 243 s succeeded. Attended Display
and clock comparison requested. No GPU fault is observed in the short startup
capture; long-term stability and any effect on text remain unverified.

Attended result: Pierre reports the phone is very responsive, but garbled text
persists and does not always occur in the same places. `nofp16` does not resolve
text corruption; do not package it as a proven fix. Responsiveness alone does
not establish that the camera-triggered GPU hang is fixed.

Online research requested by Pierre: no exact matching A6L/Adreno 512 Android
glyph-corruption report has been established. Relevant primary sources include
the September a5xx fixes listed in
[Mesa 26.2.3 release notes](https://docs.mesa3d.org/relnotes/26.2.3.html),
including compatible-format 2D blits and separate-stencil copying.
The installed build identifies as 26.1.0-devel; its AOSP source snapshot is
`875160ed32cc5bc32777fa04870a0a57ed38c987`, dated 24 March 2026. The NDK
source copy is not itself a Git repository. Inspection confirms its a5xx
blitter lacks the separate-stencil operation; `can_do_blit` has only the older
format restrictions. Its VPC NUMNONPOSVAR calculation also predates the recent
fix, but that upstream bug concerns transform-feedback programs, so it is a
weaker match for normal UI text.

The upstream
[separate-stencil fix](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/1453845c317b2b64aeb4f5fd8e924e93cfc6a7ec)
describes stale stencil bytes when copying GL_DEPTH32F_STENCIL8 surfaces on
Adreno 530. This proves an a5xx defect, not that our UI uses that format or
shares this cause. Texture/stencil-copy and ordering remain hypotheses until
a controlled test or trace identifies the path. Mesa's
[misrendering guide](https://docs.mesa3d.org/graphics-debugging/debugging-misrenderings-crashes.html)
supports isolating draw/blit paths and checking synchronization. Use the live
`noblit` diagnostic next rather than assuming a broad Mesa update fixes this.
Downloaded primary commit descriptions are retained in
`firmware/extracted/gpu-upstream-20261002/`.

Best current source lead:
[66d34e1a9f83: compatible-format a5xx blits](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/66d34e1a9f83b6bfc2f335d4d884f5abbc18cb5e).
The upstream author reproduces wrong GL copy pixels/alpha on Adreno 530,
including linear buffers. The older a5xx guard permits unsupported format
conversion whenever both surfaces are linear; our source has that guard.
This makes `noblit` a direct test of a known defective subsystem, although an
actual UI capture is still required to establish whether it uses an affected
conversion. The primary recent commit log with descriptions was downloaded;
no upstream patches have been applied or installed yet.

The follow-up phone query returned device-not-found. The live kernel stream
contains no a5xx fault/hangcheck entries in its captured interval; absence of
USB currently prevents a new property comparison and is not proof of a GPU
hang or of continued phone stability.

Pierre clarified the first lost-USB event: the screen was black and he held
Power for a long time to force a restart. Do not report a spontaneous reboot.
New boot ID `b7db7350-2949-4072-9c84-4c8347d0f690`, bootreason `reboot`, normal
mode, radio=0. Added `noblit` to the prior `sysmem,nofp16` configuration and
restarted SurfaceFlinger with live capture. GLES again reports FD512 and
Settings initially launched successfully. A later attempt to launch Display
timed out; then even a short adb shell timed out while the device remained
enumerated. Pierre confirms the LCD is frozen. No attended text comparison
was obtained; do not call this a text pass/fail or a confirmed blitter cause.
The last kernel line is at uptime 263.42 s, with no captured a5xx GPU fault or
hangcheck. Last logcat has unrelated Bluetooth startup abort and audio-route
errors; those do not establish the whole-system freeze's cause.

Phone return to recovery requested, unplugging USB before the forced reset.
Both live capture archives are saved on the laptop (`gpu-nofp16-20261002.tar.gz`,
about 2.3 MB; `gpu-noblit-20261002.tar.gz`, about 1.4 MB). Next capture recovery
metadata/pstore before any further Android boot. The three primary-source
format-copy/shadow-stencil/stencil-plane patches pass code-only `git apply
--check` against both existing Mesa source variants, without applying changes;
see `firmware/extracted/gpu-upstream-20261002/backport-check.json`.

## Recovery off-screen comparison, 2 October

Both isolated patched Mesa builds (arm64 and arm32) completed. Three code-only
upstream patches were applied: 66d34e1a9f83 (format compatibility), eca654871ed0
(shadow stencil), and 1453845c317b (separate stencil). No ROM libraries have
been replaced and no additional ROM has been flashed.

Recovery metadata/pstore capture after the noblit freeze is retained in
`logs/r6f-install-20261002/meta-after-noblit-freeze-20261002.tar.gz`.
Pstore is empty; the persistent writer stopped near uptime 254.72 s, before
the live kernel stream ended at 263.42 s. No captured fatal message establishes
the freeze's cause.

`tools/gpu-copy-probe.c` runs surfaceless ES3 on actual FD512, with CPU/source
references, format-dependent rounding allowance, and explicit GL-error checks.
The first harness version incorrectly required exact packed-UNORM rounding;
that was corrected before interpreting results. Framebuffer copies alone
passed on both drivers and did not reproduce the upstream texture-copy defect.
The final probe adds CopyTexSubImage2D, including destination clearing to avoid
counting stale successful contents as a pass.

Android r6f modules were safely rejected by recovery: Android enables module
versions, whereas recovery does not. The test used the existing pinned V71
recovery GPU modules instead, with `separate_gpu_kms=1`, no panels/modem, and
stock GPU firmware staged in RAM. The firmware search path was restored.
No modules were force-loaded or unloaded and no partitions were written.
This kernel difference limits conclusions about Android stability.

| Driver / option | Cases | Failed cases | Skipped |
| --- | ---: | ---: | ---: |
| Original / sysmem | 338 | 144 | 0 |
| Patched / sysmem | 338 | 18 | 0 |
| Original / sysmem,notile | 338 | 144 | 0 |
| Patched / sysmem,notile | 338 | 18 | 0 |
| Original / sysmem,noblit | 338 | 0 | 0 |
| Patched / sysmem,noblit | 338 | 0 | 0 |

All framebuffer-blit cases pass; the failures are texture copies. Remaining
patched failures involve compatible/same-format copies that retain the 2D
path. Explicit glFinish before, after, or both around the copy does not change
144/18 failures. Disabling the 2D path passes all cases while retaining FD512
GPU rendering. This establishes an off-screen copy defect and a tested copy
workaround on this recovery configuration. It does **not** yet establish the
Android glyph root cause, a complete text fix, or a whole-system freeze fix.

Evidence: `logs/r6f-install-20261002/gpu-copy-probe-recovery-20261002-092759.tar.gz`.
Payload/source/module hashes: `firmware/extracted/gpu-upstream-20261002/`.
Ordering probe SHA256:
`6d728b608c8dd91812c77a57219e8148814c2e084da86d872ea0348eba90edc1`.
The planned Android comparison was interrupted by the kernel crash below.
Resume it on r6g after a stable boot, with `sysmem,noblit` and default precision.

## Display-fence kernel crash — 2 October 2026

Fresh r6f boot `a9e7d13e-4920-4906-9805-4708ddf31b4d` crashed at uptime
53.395118 seconds, before the private patched Mesa probe or a SurfaceFlinger
restart could run. The persistent recovery log confirms:

```text
kernel BUG at drivers/gpu/drm/drm_crtc.c:161!
pc : drm_crtc_fence_get_timeline_name+0x2c/0x34
```

This matches the faulty fence-ops check removed by upstream commit
[57acb869490b504996ea46cbb80af49e35ec66e1](https://android.googlesource.com/kernel/common/+/57acb869490b504996ea46cbb80af49e35ec66e1).
A fence can be signalled, detaching its operations, while an already-entered
name callback still runs. Testing that the operations remain attached can
therefore trigger BUG_ON during normal concurrent operation. The recorded
callback, source line, and local source check match this specific defect.
This does not establish the cause of every earlier freeze or slow wake.

Changing a live graphics property can alter timing and expose an existing
kernel defect. The latest crash happened with the installed r6f kernel and
libraries; the new libraries had not run in Android. Recovery copy tests used
RAM files and did not alter installed images. Do not repeat UI comparisons on
r6f; prepare one combined r6g image first.

The exact upstream patch is retained as
`device/hisense/a6l/kernel/rom-v2/drm-crtc-fence-signal-race.patch` and listed in
the kernel series. Its rebuilt kernel compiles with the original configuration,
unchanged Module.symvers, and matching built-in export CRCs. This permits use
of the existing modules; it remains physically untested.

Evidence: `logs/r6f-install-20261002/gpu-copy-android-20261002-093544.tar.gz`
and `logs/r6f-install-20261002/meta-after-fence-oops-20261002.tar.gz`.
The latter contains text diagnostics, no raw partition image (1,535,630 bytes;
SHA256 `172a0275db9ab9975507d05974e5809c17e2e6fc51e4f595197da4e2be501567`).
Build report: `firmware/extracted/kernel-r5p-fencefix-20261002/report.json`.

## r6g physical GPU fault and persistent capture

r6g is installed with the fence patch, three Mesa backports, and `sysmem,noblit`.
The first hardware boot completes at 71.919405 s and reports freedreno FD512.
Pierre sees no garbled text during the initial Display/quick-settings check,
but scrolling Display is followed by a real reboot. The last decisive GPU line:

```text
[222.293810] *** gpu fault: ttbr0=000000011007b000 iova=00000000126bd040 dir=WRITE type=TRANSLATION source=1030001 (0,0,0,0)
```

The kernel stream ends at 222.609284 s and logcat shortly afterward. No captured
panic or hangcheck identifies the final reset mechanism; the older CRTC-fence
BUG is not captured again. The four zeros are CP scratch-register values, not
four hardware block IDs. On a5xx `source` is raw SMMU FSYNR1 formatted in hex.
The fault establishes an invalid GPU virtual-address write, but the log alone
cannot distinguish an out-of-bounds resource access from an absent/stale mapping
or incorrect page-table selection. Do not apply an unproven allocation-padding
or synchronization workaround to all buffers based only on this address.

The next r6g boot runs camera/e-ink tests without another captured GPU fault
through about 832 s. This does not establish stability. E-ink mirroring was
inactive during the first Display-scroll fault, so its later CPU readback path
does not explain that first fault. The recovery metadata archive has no later
fault/panic evidence and pstore is empty.

Evidence: `firmware/extracted/rom-r6g-20261002/r6g-display-gpu-fault-20261002.tar.gz`,
`r6g-camera-eink-attended-20261002.tar.gz`, and
`r6g-recovery-metadata-20261002.tar.gz` in that directory.

The kernel already enables `CONFIG_DEV_COREDUMP` and `CONFIG_DRM_MSM_GPU_STATE`.
MSM fault dumps include the offending process, current page-table walk, BO
addresses/sizes and optional VM map/unmap history. This is the evidence needed
to locate the fault within a live resource. Mesa's primary
[GPU devcoredump documentation](https://docs.mesa3d.org/drivers/freedreno.html)
describes retaining this dump and decoding its commands with Freedreno tools.
Android adb shell cannot read the root-only dump (DAC 0600). A later attended
boot already exposes an early **display-controller** dump before radio starts;
it is a separate dump source and must not consume the GPU capture slot.

Prepared debug-only service `a6l_gpu_coredump` saves one existing dump per device
kind (GPU and display) per boot. It reads no active `gpu`/`rd` debugfs files and
does not reset, clear or retune the GPU. Fixed files under
`/data/vendor/a6l-gpu-debug/` retain at most two 8 MiB prefixes; metadata gets
two 128 KiB headers only when enough free space remains to preserve at least
1.25 MiB. These are small diagnostic prefixes, not partition backups. The
module loader supplies `vm_log_shift=8` only in debug builds, retaining 256
mapping operations per VM. It changes diagnostic memory/recording overhead,
not rendering commands, frequency, voltage or recovery behavior.

Syntax, both-device capture, size bounds, non-MSM filtering and metadata
low-space fixtures pass. Existing `A6L_ROM_STATIC_TEST` also passes. These
changes are prepared for the next image and have not run physically yet.
Report: `firmware/extracted/rom-r6g-20261002/gpu-coredump-collector-checks.json`.

## Confirmed fault-dump string lifetime defect

Source inspection finds a separate concrete a5xx defect: its fault callback
formats `source` into local `char block[12]`; `adreno_fault_handler` assigns
that pointer into `msm_gpu_fault_info`, which is copied into the retained
crashstate. Reading the dump after the callback returns dereferences expired
stack storage. The same pointer pattern remains in inspected upstream Linux
source. A host AddressSanitizer reproduction of the exact storage/copy pattern
reports stack-use-after-return; changing the retained field to an owned array
passes. This may corrupt diagnostic reads; it is **not** evidence that this
defect caused the original GPU invalid write or reboot.

`rom-v2/drm-msm-owned-fault-source.patch` changes the internal field to
`char block[32]` and copies the source with `strscpy`. The a5xx hexadecimal source
and all inspected a6xx/a8xx source labels fit. Targeted `msm.ko` compilation and
strict modpost pass against existing dependency exports. The original kernel
Image, configuration and full Module.symvers hashes are preserved. Only the
MSM module needs replacing; there is no full kernel/Image rebuild requirement.

Artifact: `firmware/extracted/gpu-coredump-lifetime-20261002/msm.ko`, SHA256
is recorded in its `report.json`; deploy the final **msm-stripped.ko** payload
described below. Source/ASan/build evidence is retained beside it. Physical
validation is pending.

The final module also includes opt-in `fault_bo_log` diagnostics before
`gpu_state_get` starts hardware MMIO: submitting PID/process, ring/sequence,
actual and cached expected TTBR0, first 16 BO extents, any containing BO, and
nearest bounds. It reads no object contents and takes no extra locks. The
existing ratelimited fault line now reports raw `fsr` and `capture` eligibility;
without the stalled-fault FSR bit, the driver does not collect a fault dump.
The loader enables the summary only on debug builds and only if the exact
module's `parm=fault_bo_log:` metadata exists. Release/default and older
retained modules do not receive an unknown parameter. Three actual loader
fixtures (release-old, debug-old, debug-new) pass.

Final frozen payload `msm-stripped.ko`: 2,297,144 bytes, SHA256
`94f10d3d6a7d2a23cbb8b8772406261640393b0e9c1be42197d179b23cf9de43`.
Its exact vermagic is
`7.2.3-a6l-probe+ SMP preempt mod_unload modversions aarch64`.
An initial single-module make regenerated the release without the `+`; staging
rejected that payload before compilation/installation. Rebuilt correctly with
Kbuild `LOCALVERSION=+`, preserving the existing kernel release through generated
headers rather than editing binary bytes. The current report explicitly checks
the full vermagic against the immutable r6g vendor module.

All 759 import symbol names/CRCs exactly match the r6g packaged module and the
unchanged full kernel export table. A temporary read-only EROFS mount verifies
the installed kit's `/lib/modules/msm.ko` is byte-identical to the retained r5
baseline, SHA256
`971ff205a5521463264f96f6bb804ae09683ca0d9b46a96d3a4c72d63c91b3cc`.
No vendor image extraction/copy was created. The strip-debug step preserves
all CRCs and parameter metadata. Earlier published payload hashes are superseded.

A later attended modem/call session records another fault at 661.048449 s:
READ of IOVA `0x13b85000`, TTBR0 `0x114ce6000`, raw source `5030001`, followed by
lost logging and a new boot. The first Display-scroll fault was a WRITE to
`0x126bd040`, raw source `1030001`. Both have low FSYNR1 byte 1; applying an a6xx
engine label to these raw a5xx syndromes is unproven. Different directions and
addresses strengthen the need for resource/mapping evidence rather than assuming
one framebuffer-tail write. This second event also does not establish a modem
failure: modem/network operation precedes it and the recorded failure is GPU.
Evidence: `r6g-modem-call-test-20261002.tar.gz` in the r6g artifact directory.

Next controlled GPU test: run the prepared debug image with the existing
`sysmem,noblit` graphics settings, verify FD512 and collector startup, then
reproduce Display scrolling/clock enlargement with live kernel/logcat capture.
If it faults, retain both devcore files before reset and compare the fault IOVA
with BO bounds, expected/current TTBR0, page-table entries and VM history. Only
then select the implicated allocator, import/unmap or command-generation fix.

## r6h first boot and read-only module audit

r6h installed the final `94f10d3d...` stripped payload with verified readback.
First boot ID `9d7bc48d-b6a4-49b1-832f-d57a13c97650` records a different GPU
failure at 28.694267 s: `adreno_idle` times out draining ring 0, followed by
`CP | opcode error | possible opcode=0x00000000`. The a5xx IRQ reports fence
`ffffff08`, status `800001C1`, ring pointers `0070/0032`, and IB addresses
`0x103d000` and `0x1045000`. Recovery starts and identifies `BootAnimation`;
the live kernel stream ends at 28.736140 s. There is no captured final panic
or reset mechanism and no preceding SMMU translation-fault line in this stream.
Parent confirmed a real restart by a new boot ID, rather than inferring one
from an empty adb reply. Archive:
`firmware/extracted/rom-r6h-20261002/r6h-firstboot-gpu-hang-20261002.tar.gz`.

The second boot, `2212a330-7737-4e6b-9c4a-343c64f0b123`, reaches Android boot
completion by 79 s with FD512 and `sysmem,noblit`. At 196 s parent reports no
new GPU fault and a saved 97,549-byte display-controller dump. This validates
collector startup and display capture, but does not establish GPU stability or
successful GPU dump capture. The first boot's early failure may have interrupted
hardware snapshot creation before a GPU devcore became readable.

To check whether rebuilding introduced unrelated kernel behavior, compared the
packaged r6g module against the frozen r6h payload without rebuilding or operating
the phone. Both have 1,955 executable function symbols. Of these, 1,935 function
byte sequences are identical; the 20 changed functions are fault handling,
snapshot collection/show/destruction, and the recovery caller. The changed
snapshot functions consume `msm_gpu_state`, whose embedded fault-info string
now occupies an owned array and shifts later field offsets. In particular,
a5xx submission, flush, hardware initialization, microcode, preemption, IRQ and
power routines, and allocator/mapping routines have identical function bytes.
No unrelated command submission or power-control function change was found.

An additional object-symbol comparison finds 2,513 baseline objects and 2,516
new objects. All existing object byte sequences match except a `.modinfo` build
identifier. The three additions are the `fault_bo_log` variable and its parameter
descriptors. Existing GPU constant tables match in this check. These audits are
raw symbol-byte comparisons, not a proof of every relocation or unsymbolized
byte; reports retain that limitation:
`firmware/extracted/gpu-coredump-lifetime-20261002/function-byte-audit.json` and
`object-byte-audit.json`.

The owned-string change and opt-in BO summary execute after a GPU fault, so they
cannot directly generate the preceding invalid BootAnimation opcode. However,
`vm_log_shift=8` does add allocation and recording overhead before a fault;
it can change addresses or timing and is not proven irrelevant. The current
evidence supports retaining the bounded diagnostics and inspecting any saved
GPU ring/BO/VM snapshot before choosing a correction. It does not support claiming
that instrumentation fixed stability, or that the first-boot hang is necessarily
the same mechanism as r6g's translation faults. No phone operations or payload
rebuilds were performed during this audit.

## Recovery-path review and isolated candidate

The second-boot USB-loss archive ends at uptime 836.666 s with routine kernel
messages. Its last heartbeat with phone data is at 829.71 s; later queries
return no data and eventually `error: closed`. There is no GPU fault, hangcheck,
panic or suspend entry captured near this loss. The only confirmed coredump
capture is **display**, 97,549 bytes, at 24.402423 s. Retrieve the saved `.info`
files with the dumps to establish their device kind and boot ID; a generic
devcoredump presence alone does not prove a GPU crash.

Exact local/current-upstream source comparison confirms the a5xx opcode handler,
fault detection, crashdumper, state capture, and generic ring capture are the
same. `possible opcode=0` is an indexed PFP status read, not a decoded command
stream. The idle timeout samples the software write pointer while the later IRQ
reads hardware pointers; their differing values do not establish corruption.
Recovery snapshots the GPU before freeing the offending submit or resetting
hardware. It calls capture with no SMMU fault-info, so the opt-in translation
fault BO summary is intentionally absent for this CP hang.

Found a supported recovery correction missing from our existing baseline:
[upstream b303e1d52811](https://github.com/torvalds/linux/commit/b303e1d52811de7d1bcf793560754d4df68d4a1c)
resets hardware before retiring the hung submit, preserving buffers until the
GPU stops accessing them. This fixes a documented opportunity for additional
page faults during recovery; it cannot explain the initial opcode error.
The separate upstream IRQ-storm change applies when `disable_err_irq` is set
for the intentional recovery test; no evidence shows that condition here.
Source comparison and original commit evidence are retained in the lifetime
artifact directory.

Also confirmed an a5xx snapshot cleanup defect: on crashdumper timeout,
`a5xx_gpu_state_get_hlsq_regs` frees `hlsqregs` without clearing the pointer.
The later dump reader can use freed memory and snapshot destruction can free
it again. Adjacent register-allocation failure leaks the previously created
1 MiB crashdumper BO. A separate minimal candidate clears the freed pointer and
releases that BO on allocation failure. The actual helper source, with host
mocks only for allocation and hardware timeout, reproduces both lifetime
errors under AddressSanitizer and the BO leak. Fixed timeout/read/destruction,
allocation failure and successful capture fixtures all pass. This establishes
the cleanup defects, not that the physical reboot took either failure path.

After root authorized an isolated build, compiled only `msm.ko` with both
corrections, preserving existing diagnostics. Candidate directory:
`firmware/extracted/gpu-recovery-candidate-20261002-170137/`.
Separate source diffs are `recover-before-retire.patch` and
`a5xx-hlsq-cleanup.patch`. Stripped payload is 2,297,144 bytes, SHA256
`42e8e5d80536e0e235d2e1a23f8d9672b87d20f1d207239444ce9f896a7b45e8`.
Strict compilation passes; full vermagic and dependencies match installed r6g,
and all 759 import CRCs match the installed module and preserved export table.
Neither module exports symbols. Kernel Image, configuration, full symvers and
release headers retain their exact hashes; temporary source edits were restored.
The r6h/r6i pinned payload and shared Android prebuilt were not replaced.

Candidate binary audit finds body changes in `recover_worker` and
`a5xx_gpu_state_get` (the cleanup helper is inlined). Ten other a5xx functions
differ only in branch displacements to the same named targets, chiefly the
unchanged `trace_msm_gpu_regaccess` helper shifted by four bytes. Instruction
diff and summary reports are retained with the candidate. Parent reviewed both
patches and accepted `42e8e5d...` for the r6i vendor-only repack; this does not
require a kernel Image or system rebuild. The candidate has received no further
payload edits.

This candidate can be reviewed and bundled through the explicit MSM override
with one vendor repack after system compilation, without rebuilding the system
or boot Image. It is untested on the phone and does not claim to fix initial GPU
faults. After installation, first preserve existing diagnostic files. For a new
failure, retain the GPU dump header, rings and command BOs plus VM mapping logs;
decode commands near the ring read pointer and IB addresses before assigning the
initial failure to Mesa, mapping lifetime, or hardware. If GPU capture again
never becomes available, recovery-stage progress markers would distinguish a
snapshot stall from retirement/reset failure in a later diagnostic change.

## Recovered r6h unlock-freeze evidence

User returned to a frozen unlock screen, explicitly not a black screen, and
manually entered recovery. The recovery archive
`rom-r6h-20261002/r6h-unlock-freeze-recovery-20261002.tar.gz` contains no saved
GPU devcore. Its display dump is complete: 97,549 bytes, identical in userdata
and metadata, SHA256
`db5bf6441283842e89abb2deca0f37af1b450602b9f661f84442e05602f7b9ae`.
`display.info` ties it to second boot `2212a330-7737-4e6b-9c4a-343c64f0b123`
and collector uptime 24.15 s. Dump `time: 26551.104109158` is wall-clock time
from `ktime_get_real_ts64`, before Android corrected the clock, rather than
26,551 s of uptime. Nearby kernel audit wall-clock values corroborate this.

The snapshot shows the startup fbcon framebuffer 71 on both CRTCs, with
LCD 1080x2340 and rear 384x725 modes. It predates Android composition and the
later freeze. Interface 1's timing engine is disabled while interface 2's is
enabled in this transitional snapshot; this alone is not proof of a later
display failure. The dump does not retain its trigger reason. Local source
requests snapshots on first underrun, reset failure, and several completion
timeouts. No matching timeout/reset message appears around this capture;
first-underrun capture is plausible but remains unproven. An earlier warning
at uptime 22.132 s says `mdp_clk_src` failed to update its RCG configuration,
while DPU initializes. This is a startup clock clue, not an established cause
of the later freeze, and no clock changes were made during analysis.

Metadata boot logging explicitly stops at uptime 301.40 s; its final kmsg is
about 301.6 s. The laptop's separate live stream continues to 836.666 s, then
USB is lost without a captured GPU/panic/suspend marker. Empty pstore and no
saved GPU dump do not exclude a fault beyond those capture windows, a snapshot
that never completed, or another system stall. The startup display dump has
no USB state and cannot explain the USB disappearance. Preserve this distinction
when assessing r6i: the user's later freeze is confirmed, its mechanism is not.
Machine-readable extraction is `r6h-display-dump-analysis.json` beside the archive.

## Proposed late-freeze diagnostics (read-only review)

Keep the existing bounded boot log, and add a separate debug-only event recorder
after `boot_completed`. It would continuously drain `/dev/kmsg` while retaining
only GPU/display/MMC/USB faults, panic/Oops/watchdog/RCU/hung-task messages,
suspend entry/exit and `A6L_IOSTALL`. Exclude ordinary audit/init chatter.
Keep boot ID, kernel sequence and monotonic timestamp with each selected event;
sequence tracking prevents EPIPE/reopen from duplicating the entire kernel ring.
Following an error, capture a bounded context window (for example 16 KiB), so
the report includes call traces and sysrq backtraces whose lines may not carry
the original error's priority. Sync only after an event/context block.

Use fixed filenames in a dedicated diagnostics directory with a strict
**cumulative write allowance per boot**, for example 512 KiB ordinary events
plus 128 KiB reserved for the first fatal report. Bound individual records and
context windows. At the allowance limit, write one exhaustion marker and drain
without further storage writes. A rotating file's size cap alone would not
bound total writes over an overnight session. Keep metadata copies small and
preserve its existing free-space reserve; userdata holds the principal log.
No continuous late logcat or periodically repeated full snapshots are needed.
This is a proposal only; no diagnostic source or init changes were made during
the corrective r6i build/install.

Two existing capture gaps matter. The all-boot IO watchdog still writes
`A6L_IOSTALL` and sysrq reports to the kernel after boot logging exits, but they
currently have no persistent late reader. The devcore collector saves only the
first dump of each device kind per boot: an early display dump consumes its
display slot, so a distinct later display failure is skipped. A future bounded
extension could retain the first and one distinct later display dump, tracking
the sysfs entry identity and never clearing kernel dumps. This would retain at
most two display captures per boot, with explicit size/write budgets. GPU and
display slots remain separate.

Neither proposal guarantees evidence from a complete CPU lockup or from an
eMMC stall: both storage destinations reside on the same device, and recording
can block. Likewise `timeout 8` cannot guarantee termination of a read stuck
in an uninterruptible kernel wait. The recorder should allow only one writer
and never repeatedly spawn replacements for a stuck writer. USB-independent
event recording improves coverage; proving the final reset/lockup mechanism
still requires a retained dump, pstore record, external serial trace or a
recorded recovery-stage boundary. Preserve these limits when interpreting an
empty late log.


## r6i EPD-switch freeze: GPU initialization and fence starvation

The complete saved `firmware/extracted/rom-r6i-20261002/epd-freeze-kmsg.txt`
records the corrected r6i boot `531f8c48-5472-4d4b-8bd5-cfd74dc8d0cf`.
At uptime 191.710 s, `adreno_idle` times out with software ring 0
rptr/wptr `a0/9`; the IRQ reports fence 1442, status `c30001c1`, hardware
ring pointers `00a0/002e`, IB1 `0x08dd1000`, IB2 `0x03b49000`.
Further initialization timeouts and recovery-worker entries occur at
192.718, 193.734 and 212.490 s, with fences 1442 through 1444. There is
no captured SMMU translation-fault report, offending-task report,
`CP_SCRATCH_REG` reset output or GPU devcoredump. The root collector saved
only the separate startup display dump around 24.6 s. The EPD rails switch
near the initial failure, but temporal proximity does not establish that
EPD buffer capture caused it.

The 376 s sysrq snapshot establishes the UI blocking mechanism: SurfaceFlinger
RenderEngine (pid 1266, tgid 1203) and four app RenderThreads are in
`msm_gem_close -> dma_resv_wait_timeout -> dma_fence_wait_timeout`.
The atomic display commit worker also waits for a fence. This kernel's
`msm_gem_close` waits with `MAX_SCHEDULE_TIMEOUT` before unmapping a BO.
The phone retains adb and native screen-switch responses; root measured zero
IO pressure/in-flight requests. This particular freeze is GPU fence starvation,
not evidence of the earlier proposed eMMC discard stall. Removing the close
wait or tearing down mappings while hardware may still use them is unsafe.

The recurring software wptr of nine identifies a useful initialization clue.
`a5xx_me_init` emits one packet header plus eight words. The staged PFP
firmware SHA256 is
`cc4f2b1cd6b3a9401592e2498ec43b87ba926a23848fa231f417ab08df9c47ba`.
`adreno_fw_create_bo` skips the firmware's first four bytes; BO word zero
is `0x005ff112`, whose low nibble is 2 rather than the required `a`.
Thus this firmware does not enable `has_whereami`, no additional WHERE_AM_I
packet is appended, and preemption is disabled. The observed wptr nine
therefore strongly points to failed ME initialization, with stale hardware
read pointers instead of a clean ring starting at zero. This inference
still needs an explicit hw-init return/stage record.

Recovery-worker entry is not proof of a hardware reset. `recover_worker`
looks for the submit whose sequence is `memptrs->fence + 1`, and exits before
snapshot and reset if none is found. The absence of its unconditional
subsequent offending-task output and of `a5xx_recover`'s scratch output
supports this early exit, although a new bounded branch marker would prove
it directly. The accepted reset-before-retire repair is downstream of this
check, so cannot repair that path. Current upstream retains the same early
exit and also calls `msm_gpu_hw_init` without checking its return in
`msm_gpu_submit`, allowing a submit to be queued after failed initialization.
See [upstream msm_gpu.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/msm_gpu.c).

A source-supported next comparison is to keep only the GPU device runtime
active (`power/control=on`) from a debug-only root init action before the
first Android rendering submission. Preserve the existing GPU firmware,
frequencies, voltages and renderer. Record passive GPU runtime status and
active/suspended-time counters before and after an attended Display/EPD test;
restore normal `auto` behavior for the comparison. This isolates repeated
runtime reinitialization without changing global power-domain policy. It is
a diagnostic workaround with a battery cost, not a validated production fix.
The saved boot confirms A6L preserves both `gpu_cx` and `gpu_gx`; generic
power-domain code returns without powering off domains with `stay_on` set.
Consequently a software runtime suspend/resume may not provide the hardware
collapse assumed by ring reinitialization. That mechanism remains a hypothesis
until correlated with runtime counters/stage records.

A subsequent minimal diagnostic module could record cached `needs_hw_init`,
initialization return, runtime-resume count, software ring pointers, memory
fence, fence-context completion/last fence, and bounded queued-submit
sequence numbers at the no-submit recovery exit. Record before any snapshot
or hardware access. Do not blindly remove the early exit: it intentionally
covers a submit that retired while the worker waited. No supported safe
shell-accessible force-GPU-reset interface exists in this kernel's debugfs
or sysfs. Reading its debugfs `gpu` file invokes snapshot/hardware initialization;
changing hangcheck timing or disabling error interrupts does not force reset.
No phone mutation, source change or payload build was performed in this review.


## r6j debug runtime-PM hold trial implemented

The existing debug init file now applies `persist.vendor.a6l.gpu.pm_hold`
at `post-fs-data`: 1 writes `on`, 0 writes `auto`, exclusively to
`/sys/devices/platform/soc@0/5000000.gpu/power/control`. The default is 1
only through debug/bootlog.mk's existing non-user build gate; user builds
install neither this debug init file nor that property default. The property
has an exact `enum 0 1` context using existing `vendor_a6l_prop`.
Persisted 0 overrides the build default across boots.

A single synchronous root shell command at post-fs-data evaluates the current
property value at execution. This matters because Android selects matching
event-plus-property actions before running the event's commands, while
persisted values load inside system post-fs-data. The normal vendor action
already performs its bounded display-module wait before this debug action.
There is no extra wait, retry or polling; if the exact GPU path is absent,
the write fails and init reports it. The GPU was initialized before
post-fs-data in saved attended boots; the action cannot guarantee preventing
GPU activity earlier than this stage or recovering an already hung GPU.

Property-only actions apply later attended 1/0 changes when
`vendor.a6l.display=done`. Their initial evaluation occurs after boot, so a
repeat of the same applied state and a second status line at boot is expected.
The transient status service reads only control/runtime_status and passive
runtime active/suspended time counters, writing one short `A6L_GPU_PM` line
to kmsg per applied action. It performs no persistent file writes, GPU register
access or devcoredump/debugfs access. Clock, voltage, frequency, firmware,
module, Image and rail configuration are unchanged. GPU runtime-PM exclusion
can increase power use; it remains an attended diagnostic trial, not a proven
fix. Before evaluating the result, confirm control=on and whether suspended
time remains stable during the trial; compare later with property=0.

Validation: the existing Android `host_init_verifier` accepts the final rc
and property contexts; isolated make evaluation confirms installation/default
in userdebug and eng and exclusion in user. Shell fixtures verify 1 -> on,
0 -> auto, invalid values -> no control write, and the exact bounded passive
status line. Init's real service argument expansion was checked: `$$` becomes
literal `$` for shell substitutions. The final three source files are identical
in the repository and WSL Android device tree. No build or phone operation was
performed by the GPU agent.

## r6j unattended lock observation: baseline and limits

The user reports the attended hardware-graphics baseline remains responsive:
Display scrolling has no observed garbled text or crash, and Camera and EPD
were used before the phone was left locked overnight. This is initial evidence
for the runtime-PM hypothesis, not proof that the underlying fault is repaired.

Read-only review of the laptop's existing `live-r6j-firstboot-20261002` logs
found no remaining collector process. The stream's selected GPU/PM evidence is
saved as `firmware/extracted/rom-r6j-20261002/gpu-overnight-baseline.txt`:
GPU control is on, runtime status becomes active at 28.644635 s, and no GPU
fault, hangcheck or initialization-timeout marker precedes the final
`PM: suspend entry (s2idle)` at 616.318309 s. The last successful heartbeat
at 22:41:54 Paris records uptime 614.63 s and zero current IO pressure/in-flight
eMMC requests; the next heartbeat at 22:42:04 loses USB. Entering suspend is
positive evidence, and the USB loss is compatible with suspend rather than
proof of a crash. There is no captured suspend exit or later overnight status.

The collector saved `previous-stay-awake.txt = 7`; its restore attempt failed
with `device '1e529013' not found`. Even successful restoration to that saved
value would retain stay-awake while charging. For ordinary future powered-idle
tests, explicitly set `stay_on_while_plugged_in` to 0 (or remove the override)
when attended and connected; do not restore 7. No setting was changed for this
overnight observation. Android's local PowerManagerService source shows StayOn
controls automatic powered-idle timeout, while explicit Power sleep invokes
PowerGroup doze/sleep without that check. The actual suspend-entry log confirms
it did not prevent this particular explicit lock from reaching system suspend.

The GPU hold blocks ordinary device runtime suspend, not system sleep.
Local `adreno_system_suspend` stops the schedulers, waits up to one second for
active submissions and calls `pm_runtime_force_suspend`; system resume calls
`pm_runtime_force_resume`. Thus an overnight lock/wake still exercises a GPU
system-suspend/resume path. It does not validate automatic GPU idle transitions
with power/control=auto. Power use can increase while the system remains awake;
the current evidence does not measure overnight battery drain. See the
[kernel runtime-PM documentation](https://docs.kernel.org/power/runtime_pm.html)
for the distinction between device runtime and system power transitions.

No new adb session, wake, periodic host poll, persistent logging service,
automation, reboot, app action or policy change was started. Existing on-device
coredump collection remains available, but the bounded metadata boot stream
has its previously documented late-capture gap. Tomorrow, obtain the user's
wake/response report first, then compare boot ID and uptime with baseline
`753d5822-e304-479f-85be-9ba37abba8a4`, collect the retained kernel/dump evidence
and passive GPU counters, and distinguish a successful wake from an unnoticed
reboot. Unchanged boot ID alone would not prove the UI stayed usable throughout.

## r6k switch readiness and sleep observation, 3 October

Read-only source/log review used the laptop's saved
`rom-r6k/logs/live-r6k-firstboot-20261003/{logcat-live,kmsg-live}.txt`.
Eight of ten screen-switch requests, including all five LCD returns, reached
the WM completed listener with an invalid present fence. These callbacks
arrived about 0.89–1.64 seconds after the native key event; native preparation
then waited its full 3.01–3.05 seconds and failed open. For example, LCD return
at 07:56:49.902 produced the no-fence warning at 07:56:51.141, timeout at
07:56:52.912 and LCD unblank at 07:56:52.916. This is a readiness delay, with no
GPU fault or recovery in the saved kernel stream.

Two rear entries passed the fence gate. At 07:53:13.893, the key event preceded
WM presented acknowledgment at 07:53:15.083 and rear-frame acknowledgment at
07:53:17.578. The second entry began at 07:57:26.937, passed WM at
07:57:27.645, and received its rear-frame acknowledgment at 07:57:29.694.
The 2.50/2.05 seconds after WM readiness belong to the mirror/rear-frame path,
which the e-ink agent is investigating separately.

The exact local SF `TransactionCallbackInvoker.cpp` attaches its present fence
only if transaction latch time is nonnegative. An empty transaction can
complete without a fence. `ViewRootImpl.java` around lines 13978–13992 permits
a requested BLAST draw to produce no buffer, clears its pending buffer sync,
gathers other pending transactions and marks its group ready. A redraw
requested after apps already handled the theme change can therefore be
redundant. This is a source-supported hypothesis, not yet established for
these eight requests because r6k did not record latch time. The two successful
present fences also rule out assuming that this device never supports them.
Neither invalid fence nor latch alone proves a new image was scanned out.

The canonical `0002-a6l-rear-white-wallpaper.patch` and matching WSL
`WallpaperController.java` now add bounded per-switch diagnostics: property
receipt, wallpaper commit, theme receipt, one WM-engine-busy message, selected
window sequence/buffer sequence/config-report state (at most eight windows),
sync start, transaction ID/readiness, commit, completed latch time/fence
validity, explicit failure kind and successful frame publication. Elapsed
realtime and time since WM observed the request distinguish suspend duration
from ordinary scheduling delay. Existing readiness success checks, 1500 ms
group deadline, 500 ms fence wait, and native timeout remain unchanged.
The exact source diff passed whitespace validation and the canonical patch
passed reverse-apply validation against the current source. No module,
graphics policy, voltage, clock, firmware or phone state changed; no Android
build was run by this agent.

If diagnostics establish a no-buffer redraw, a focused next candidate is to
prepare the sync group before the theme changes and acknowledge preparation
to the app before it invokes UiModeManager. This needs an exact-pair protocol
and timeout/failure handling during app relaunch. Silently treating an absent
present fence as presentation, or shortening native preparation without an
alternative freshness check, would weaken the requested first-frame behavior.

Explicit LCD lock occurred at 07:57:37.385; Android reported sleeping at
07:57:38.154 and native noticed LCD asleep at 07:57:38.400. Kernel suspend
entry was at 577.410654 seconds, a brief exit at 577.434875, then entry at
577.636946. The host stream ends there. It does not contain the later reported
ten-second wake, so it cannot establish whether that delay is in power-key
wakeup, kernel resume, display enable or UI redraw. In current native source,
an awake-state change alone does not start an appearance request; only a
screen-target change does. The retained GPU runtime hold still allows system
suspend/resume, as documented above. USB loss at suspend is not reboot or
GPU-crash evidence. A later attended wake capture must correlate key/wakeup,
PM resume, Android waking and native backlight events before changing the
power-key or resume path.

### Focused fresh-root rendering trial for the next combined build

Further source review found a specific missing operation behind the no-damage
hypothesis. `ViewRootImpl.performDraw` already treats a sync group as requiring
a full Java dirty rectangle. However, the hardware path clears that rectangle
before HWUI draw; `ThreadedRenderer.invalidateRoot` rebuilds the root display
list only when `mInvalidateRootRequested` or other root changes request it.
`forceDrawNextFrame` bypasses CanvasContext's already-drawn-vsync check, while
CanvasContext's later empty-HWUI-damage check can still skip producing a buffer.

The parent approved a narrow ViewRootImpl trial in canonical patch `0002`:
for A6L only, default display, requested redraw with buffer synchronization,
current appearance pair exactly matching theme_ready and not frame_ready,
set both `mFullRedrawNeeded` and `mInvalidateRootRequested` once per pair per
ViewRoot. This records the root display list again instead of merely enlarging
the Java dirty rectangle. A new ViewRoot after activity relaunch can redraw
the same pair independently. No draw-state reset or renderer-output toggle is
used. The existing valid present-fence success condition, 1500 ms sync deadline
and 500 ms fence wait remain intact; missing/disabled surfaces can still fail.
This is a supported fresh-render trial, not a physical first-frame guarantee
or a proven one-second screen-switch result.

The actual helper and callsite are extracted into the Java fixture in
`firmware/extracted/wm-theme-readiness-20261003/`. Its 41 checks cover platform,
display and sync gates, stale/malformed/current/already-ready pairs, duplicate
requests, a new pair, relaunched ViewRoot and both invalidation flags. The
canonical patch passed whitespace and reverse-apply checks and was frozen at
SHA256 `1abb05e7d19704ccafaca807e972cb61fd93e9f6870cde4984e8cffbf0cb5f36`
before root started the combined framework/services build. This agent did not
run concurrent Soong or change the phone.

## r6l Camera SurfaceFlinger abort: missing legacy gralloc metadata bridge

The camera reaches Preview STREAMING, then SurfaceFlinger aborts at
09:21:53.025 importing a 1600 x 1200 PRIVATE buffer. Kernel boot ID does not
change. Startup logs already warn that the selected fallback gralloc has no
lock_ycbcr support. Retained Mesa returns -EINVAL during PRIVATE buffer metadata
discovery, before DRM image import, and returns a NULL EGL image without setting
an EGL error. This matches Skia's misleading EGL_SUCCESS 0x3000 failure.

Actual vendor packaging has only gralloc.default.so despite the already-correct
ro.hardware.gralloc=minigbm selector. Root owns the minimal candidate: add
generic gralloc.minigbm in both ABIs, using the same existing
libminigbm_gralloc.so as stable mapper. The sibling _msm variant would use
another registry and is unsuitable. Both clients use the sphal namespace and
the same registered GraphicBuffer handle pointer. The generic shared library
already has SOONG_CONFIG minigbm platform=msm. Legacy perform reports true
resolved DRM format, plane count/fds/offsets/strides/modifier, excluding the
optional metadata fd. It enables Mesa's already-compiled CrOS API backend
without changing camera formats or rebuilding Mesa.

The isolated actual-source ASan/UBSan metadata regression passed 41 checks:
PRIVATE NV12/RGB, RGBA, padded plane offsets, optional metadata fd, modifiers,
invalid/unregistered handles and the current absent-lock_ycbcr failure. This
does not test EGL or GPU import on hardware. Full evidence, unchanged shared
binary hashes and physical verification plan are in
firmware/extracted/gralloc-bridge-20261003/report.md. No Android source, Soong,
kernel, phone or packaging changes were made by this agent during this task.

## r6m ordinary LCD sleep and missing USB return

Saved physical power press/release at boot 951.292 / 951.434 s leads to
framework Sleeping within 529 ms and native LCD asleep within 769 ms.
Kernel s2idle entry is later at 961.974778 s, with brief exit 961.998562 and
re-entry 962.201267 ending the host stream. Laptop journal disconnects port
3-2 at 10:07:21 and observes no Android re-enumeration before recovery at
10:24:14. The later reported ten-second wake is outside captured logs; do
not assign its delay to a particular PM, GPU, key or panel stage yet.

The extended debug-only bounded metadata observation window is needed:
the r6m bootlog had stopped at uptime 305.60 s. Existing power PM tracepoints
and KRETPROBES are enabled and can capture resume callback timing plus the
currently ignored dwc3_gadget_resume errno without rebuilding Image. USB
core/glue already forbid runtime PM and mark it active before system reconnect;
blindly adding another runtime hold or rewriting connect is not supported by
current evidence. Ordinary LCD wake does not invoke target/theme preparation.

Full timeline, source exclusions, bounded trace plan and isolated unbuilt
observation-only patch are in firmware/extracted/usb-pm-diagnostic-20261003/.
Shared kernel sources, current r6m, power policy and the phone were untouched.

The isolated tracefs helper candidate is now frozen at SHA256
ba3749890f6d6220642e608d77041262d35d940b753c7ae8a54460869c340b2c.
It uses only an owned a6l_pm instance, 256 KiB per CPU, three available power
events and optional symbol-checked dwc3_gadget_resume return probing. Latest
and final disk snapshots are capped at 128 KiB, with one transient replacement
of the same size; root integration must include these in the unchanged 3 MiB
metadata accounting. Cleanup checks boot ID and inode, refuses changed probe
definitions, disables only owned events and deletes only the owned probe and
instance. Kernel source confirms normalized r<maxactive>: output; matching
allows that count without relaxing the exact symbol/argument requirement.

Actual helper syntax and lifecycle passed 248 assertions across 21 isolated
host scenarios, repeated with Android-built toybox applets. The fixture does
not execute kernel probes or verify phone permissions, and the host shell was
dash rather than Android mksh. No Image rebuild, PM-policy change or physical
activation occurred. Root owns packaging/lifecycle integration and subsequent
attended lock/wake capture. See the isolated report for failure-path coverage,
same-boot logger restart handling, storage overhead and diagnostic limits.

## r6o staged ART dependency audit, 3 October 2026

The 80 changed system files include 74 explicitly explained framework/derived
paths and six direct build-property/DisplaySwitcher paths owned by integration.
The frozen `firmware/extracted/rom-r6o-20261003/approved-derived-system-paths.json`
contains the exact 74 paths, `passed: true` and no unexplained differences. SHA256
is `17eafe77106e2ad7cc91f24de569718bbd22af4e8c66cea200b26a52111aaca3`.

All 27 unrelated changed ODEX files preserve every byte except the services.jar
class-loader checksum (2238823946 -> 1826232271), the derived OAT checksum and GNU
build ID. Their executable code, VDEX, dex signatures and source containers are
unchanged. Four dependent ARTs change only their own/paired OAT checksum fields.
Services OAT/VDEX checksums and signatures match its three staged input dex
files; profiles resolve and preserve the same 4,650 profiled members. All 35
changed integrity sidecars reproduce exactly for both builds, and BuildManifest
changes precisely their 35 input digests; both APK/v4 signatures verify. ProtoLog
changes only 42 source-line positions in our two edited WindowManager files.

Full method, exact path coverage, retained header/profile/signature evidence,
source-scope responsibilities and runtime limits are documented in
`firmware/extracted/rom-r6o-20261003/oat-dependency-audit/report.md`. This audit
used only read-only EROFS mounts and existing tools; it did not modify sources,
staged images, the phone, power policy or kernel. Attended r6o validation remains
necessary; static dependency consistency does not prove physical frame timing.

## r6o false DRM damage: definite blend ABI mismatch

The mirror passed raw KMS `pixel blend mode` integers to an enum with the wrong
numeric mapping: NONE=0/PREMULTI=1/COVERAGE=2. The retained kernel's ABI is
PREMULTI=0/COVERAGE=1/NONE=2. Consequently true opaque planes were multiplied by
ignored pixel alpha, and premultiplied planes could be misclassified as opaque
and cull lower layers. This is a capture bug; no unsupported GPU-driver cause
is needed to explain the reproduced behavior.

The isolated one-line header correction is frozen at patch SHA256
4193e8bafda1ea296f29594dda0b5b5e4fcc26ee6243c9eb5d521d49aa917c13,
with candidate header SHA256
f3696ef13538141254c7c5ac76e220c0c47e9d800ef18cc55c01f5dc6fb595a9.
Independent raw-integer regression fails 39/150 checks before correction and
passes 150/150 afterward, covering RGBA/XRGB, plane/pixel alpha, identity/general
paths and culling. Existing 179-check logic and 1,536-image plane suites pass.
Root integrated the production/test changes and verified the test with -Werror
after explicit enum-comparison casts; receipt is
`firmware/extracted/rom-r6p-20261003/blend-integration.json`.

Saved clock-only screenshots produce 20/16,200 changed tiles through the actual
stretch/contrast-zero conversion, much smaller than the repeated 982-tile
signal. The captures were not simultaneous, so this is not full causal proof.
A deliberately selected 982-tile irrelevant-alpha pattern reproduces the exact
0.060617 metric before correction and zero afterward. Real buffer alpha/spatial
damage still needs a bounded device capture if artifacts persist. Other
snapshot/coherence limits, optical evidence, pinned patches/test sources and
the focused follow-up plan are documented in
`firmware/extracted/rom-r6o-20261003/drm-capture-investigation/report.md`.
