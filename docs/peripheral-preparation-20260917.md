# Wi-Fi, modem and audio preparation — 17 September 2026

Offline preparation completed while the user was away. The spare remains in stock Android, with the existing V38 recovery. No reboot, firmware write, radio start, audio playback, or V44 launch occurred.

## Completed and checked

- Built the configured modules in a separate copy of the validated output: `/home/a6l/kernel/out-a6l-peripheral-prep-20260917`. Build succeeded in 101 seconds; original V38 Image, configuration and Module.symvers hashes are unchanged.
- Selected **50 modules**, including ath10k SNOC, QRTR, Qualcomm modem/ADSP remoteproc, APR/QDSP6, SDM660 sound-card support and internal codec drivers, with their complete modinfo dependency closure. Manifest records dependency order, filenames, vermagic and hashes.
- Loaded and unloaded all 50 real modules under the exact V38 kernel in QEMU: **103/103 checks passed**, including cleanup and no panic. This proves ABI/dependency compatibility, not functioning phone hardware.
- Extracted **430 modem-partition files and 29 DSP-partition files**, read-only from the backup. Both partition SHA256 values matched the original verification report before extraction.
- Checked ELF32 program headers and required non-hash PT_LOAD segment sizes for stock `modem`, `adsp` and `cdsp`. Preserved unchanged selected firmware, service descriptors and audio files in a **102-file hashed bundle**. This does not validate Qualcomm signatures or establish that firmware will boot with our drivers.
- Preserved all **25** stock `bdwlan.*` candidates without choosing a board or installing a generic `board.bin`.
- Preserved `tfa98xx.cnt` and the stock mixer/platform/policy XMLs. ACDB files remain available in the original extracted vendor tree.

Artifacts: `firmware/extracted/peripheral-prep-20260917/{build-report.json,module-manifest.json,qemu-module-report.json,qemu-module.log,firmware-manifest.json,modules,firmware,stock-audio}`. Full stock extraction and provenance: `firmware/extracted/peripheral-firmware-20260917-r3`.

The first extraction attempts stopped on duplicate FAT names. The stock VERINFO directory contains two distinct `VER_INFO.TXT` entries; r3 preserves both using an explicit FAT-cluster suffix. The earlier directories are incomplete and must not be used. No backup bytes were modified.

## Hardware map and remaining work

| Component | Evidence and prepared support | What remains before claiming it works |
|---|---|---|
| Wi-Fi | WCN3990, stock ICNSS at `0x18800000`; ath10k SNOC/QRTR dependency set compiled and ABI tested; own `wlanmdsp.mbn`, service descriptors and board files preserved | Enable correct power/IOMMU/remoteproc plumbing, provide modem firmware services, obtain QMI chip/board IDs, select matching stock board data, verify regulatory database, then scan/connect and Android Wi-Fi HAL |
| Modem / 4G control | Stock baseband `MPSS.AT.3.1-00819-SDM660_GEN_PACK-1.223899.1.247858.1`; mainline `qcom,sdm660-mss-pil` support; own MBA and split modem firmware validated structurally | RMTFS memory/config and userspace services, firmware authentication/startup, QRTR service discovery, modem control, Android radio integration |
| 4G data | Stock IPA reports hardware enum 6, corresponding to IPA 2.6L in Qualcomm's SDM660 source | **Current mainline IPA has no SDM660/IPA2.6L support.** Modem boot/control alone will not establish a cellular data interface. Audit SDM660 downstream/mainline work and implement a suitable data path separately; do not use an unrelated IPA compatible |
| Internal audio / earpiece / headset | Stock PM660L analog codec at SPMI SID3 offset `0xf000`, digital codec `0x152c0000`; APR/QDSP6 and internal MI2S support in our kernel; codec modules built | A6L codec DT nodes, supplies/clocks, correct DAI links, routing and jack detection; DSP service/firmware startup; Android audio HAL |
| Loudspeaker | Stock `tfa98xx` driver actually bound to I²C `6-0034` at QUP6 `0xc1b6000`; stock container identifies `9894N1A1`; PM660 L13 supply, GPIO76 reset and GPIO77 IRQ from stock DT | Confirm silicon revision, port a suitable TFA9894/TFA2 driver with A6L tuning/protection, establish exact MI2S/TDM wiring and routing, then bounded playback |

WLAN stock supplies resolve to PM660 L5, pin-controlled L9/L6/L19. Stock ICNSS requests respectively 0.848 V, 1.75–1.90 V, 1.20–1.37 V and 3.20–3.40 V. Preserve these board constraints and validate regulator binding differences; do not copy another phone's supply mapping.

Stock reserved firmware regions: WLAN MSA `0x85700000/0x100000`, modem `0x8ac00000/0x7e00000`, ADSP `0x92a00000/0x1e00000`, MBA `0x94800000/0x200000`, CDSP `0x94a00000/0x600000`. Audit the final bootloader-adjusted tree and metadata/RMTFS buffers before activating remote processors; addresses alone are insufficient.

Stock `MODEMUW.JSN` explicitly describes `msm/modem/wlan_pd`, QMI instance 180, and the WLAN firmware service. This is direct evidence of the WLAN/modem firmware relationship on this phone.

The mainline `sm8250` filename is not a mismatch: its match table includes `qcom,sdm660-sndcard`, and its code handles LPI MI2S RX0/TX3. Codec binding schemas explicitly support `qcom,pm660l-wcd-analog-codec` with fallback `qcom,pm8953-wcd-analog-codec`, and `qcom,sdm660-wcd-digital-codec` with fallback `qcom,msm8916-wcd-digital-codec`. The fallback names explain why those built modules do not contain a literal PM660L match string. This is supported upstream plumbing, still requiring A6L DT/routing.

Do not mistake generic Tavil/WCD9340 stock DT/config entries for populated A6L hardware. Live stock slimbus enumeration showed Bluetooth/FM devices, while the active sound node references internal analog/digital codecs.

## Next tests, in order

1. Finish the already prepared V44 SurfaceFlinger test and front-touch diagnostic when the user returns. These provide a useful interactive base.
2. Prepare an ADSP/remoteproc candidate with reviewed memory, clock, power and firmware paths. Obtain USB logs for firmware requests, authentication/startup, QRTR/APR discovery and graceful stop. Initially omit physical playback.
3. Prepare modem/RMTFS and WLAN bring-up using stock firmware. The current kernel configuration has `CONFIG_QCOM_RMTFS_MEM` disabled, so this needs an isolated new kernel build and reviewed DT, not just insmod. Do not point a new RMTFS daemon at writable live NV/EFS partitions by default; first review its backend and backup-preserving test strategy.
4. On WLAN QMI discovery, record chip/board IDs and determine the matching stock board-data variant. Then test scan/association, disconnect/reconnect, throughput and suspend. A board-file guess is not a valid success criterion.
5. For audio, establish ALSA card/DAI/mixer availability first, preserve amplifier tuning, then exercise one output at a time. Separately implement and test the modem's missing cellular data path and Android radio integration; voice, SMS, data and IMS each need their own validation.

No firmware enablement overlay, phone module-load script, or radio/audio activation sequence has been installed. The prepared module bundle is not a flashable ROM. Current blockers are hardware integration, not waiting for another full Android build.

## Research references

- [Linux wireless ath10k firmware documentation](https://wireless.docs.kernel.org/en/latest/en/users/drivers/ath10k/firmware.html).
- [SDM660 mainline project and audio configuration work](https://github.com/sdm660-mainline).
- [Qualcomm userspace RMTFS implementation](https://github.com/linux-msm/rmtfs).
- [Qualcomm SDM660 downstream DTS, including IPA 2.6L](https://android.googlesource.com/kernel/msm.git/+/fbf962bb2f737416a0dbe5185ef700786d847320/arch/arm/boot/dts/qcom/sdm660.dtsi).
- [Current upstream IPA implementation](https://github.com/torvalds/linux/blob/master/drivers/net/ipa/ipa_main.c); our pinned local match table was checked independently and has no SDM660 entry.
- [Author's SDM660 internal MI2S series v6](https://lwn.net/Articles/1086538/); relevant matches, LPI ports and codec fallback schemas already exist in our pinned 7.2 source.
- [TFA9894/TFA2 driver RFC, August 2026](https://www.mail-archive.com/linux-kernel@vger.kernel.org/msg2647087.html). It bypasses the DSP and lacks speaker protection/volume control. Reference only; not applied or selected for A6L playback.
- [NXP's explanation of the TFA9894 container and driver](https://community.nxp.com/t5/Other-NXP-Products/tfa9894-firmware/m-p/1202011/highlight/true). The existing A6L tuning is preferable to another phone's container.

## Last device check

Read-only SSH/ADB check: stock serial `1e529013`, ADB device, `sys.boot_completed=1`; baseband and live amplifier binding as above. Scoped host helper reports fwupd and ModemManager active, `owned_pause=null`. `/proc/asound` and firmware-mount listing were denied by the stock shell; no escalation on the phone was attempted. No SIM identities, contacts, credentials or microphone audio were collected.
