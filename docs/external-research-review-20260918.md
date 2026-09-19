# External research review — 18 September 2026

Three reports received under `research/claude-{haptics,eink,adsp}`. These are offline findings, not hardware passes. Original deliverables are retained unchanged. No supplied phone script has been run.

## Haptics

The cited stock DTS, R4 candidate driver and pulse helper hashes match our files. Stock DTS requests 505 kHz internal PWM, 800 mA current limit and 3200 mV maximum. R4 does not program the carrier registers and uses 400 mA; our deliberately small test requests approximately 1276 mV. These are useful discrepancies. They do **not** prove why no vibration was felt: reset values and actual drive still need measurement. The summary's statement that this is the deeper reason is stronger than its evidence.

Keep the optional driver patch as an unbuilt candidate. Its encoding-only test is not a hardware validation. Before adoption, compare PM660 register semantics with downstream source and account for auto-resonance differences too.

Do not execute `diagnostic/haptic-drive-readback.sh` as delivered. Despite its read-only wording it loads a module and fires a pulse; it hardcodes event3, clears dmesg, optionally writes whole registers without saving/restoring them, and unconditionally unloads the module. Replace it with our guarded diagnostic runner, dynamic input identity checks, non-destructive log capture and explicit cleanup. No direct register writes planned.

## E-ink

Static analysis supports a fixed 0x70080-byte waveform/calibration read through `/dev/epd_flash`, with unusual zero-on-success semantics. It corroborates our existing fixed-size reader and identifies VCOM digit offsets. This is **not a full flash-chip backup**: total chip size is unknown and the read omits the final 128 bytes of the 256-byte VCOM block.

Reading the device toggles EPD power GPIOs. Describe it as non-writing to flash, not free of hardware side effects. Stock access permissions remain a blocker; do not substitute a random privileged context. The sysfs VCOM store erases a sector and must not be used for discovery. The report's binary addresses and reconstructed structure offsets remain reverse-engineering evidence, not a hardware specification.

## ADSP

The report identifies the existing disabled SDM660 ADSP node and a plausible minimal enablement. The cited merged DTB and MDT hashes match our files. Independent reads of pinned `sdm630.dtsi` and `adsp_resource_init` confirm the node, reserved region, PAS ID, SMEM crash ID, SSCTL ID and automatic-start default. Its DT and MDT-loader audits are useful static checks; signature authentication, DSP startup and QRTR discovery remain untested. CX proxy vote and SMMU ownership are explicit unresolved differences. Candidate A and B are alternatives, not patches to apply together.

Do not execute `tools/a6l_adsp_diag.sh` as delivered. Its synchronous start write happens before the timeout loop, so it is not bounded as claimed. Firmware hashes are recorded rather than compared with trusted values, early failures lack reliable cleanup, recovery settings are not restored and final failure may return zero. A host-supervised test must verify manifests, RAM-only paths, remoteproc identity and firmware/autoboot ordering, with bounded start/stop and preserved evidence. No ADSP candidate is deployed.

## Integration decision

Retain all three reports as research inputs. Continue real Android runtime/APEX integration in the disposable VM first. Later hardware tests can combine reviewed haptics observations with ADSP readiness checks, but should not combine unvalidated power changes. The validated V46/V47 phone baseline is unchanged.
