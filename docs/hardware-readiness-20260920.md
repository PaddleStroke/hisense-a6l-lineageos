# Hardware readiness after the 20 September offline session

Sources: read-only `adb shell` snapshot of stock Android (`captures/stock-readonly-20260920`, not in git), the
decompiled stock DT (`firmware/extracted/device-trees/stock-00.dts`), the pinned 7.2.3 kernel tree and the
V67 configuration. "Candidate" = written and compile/merge-checked offline, **never booted**. Nothing here
changes the phone; every hardware step needs an attended session.

## New facts established today

| Finding | Evidence | Consequence |
|---|---|---|
| Front ALS/proximity is an **AP-side** Sensortek STK3338 (`stk,stk3x3x`) on `i2c@c1b6000` addr 0x47; rear ALS/proximity is an AP-side ams TMD3702 on `i2c@c176000` addr 0x49. Stock exposes them as evdev devices (`light_extend_front_1`, …) | stock DT 9323/9142, `getevent`, sensorservice list entries 0x29–0x2c | They do **not** need the ADSP. Upstream `stk3310` covers the STK33xx family (built as external module; STK3338 ID acceptance untested). TMD3702 has no upstream driver (`tsl2772` is a different family) → small new IIO driver or skip rear ALS. |
| Motion sensors (BMI160 accel/gyro, AK09918 mag) and all fusion/step/tilt sensors come from the ADSP (SSC) | sensorservice list 0x01–0x25, `sensors.qti` process | Depend on ADSP bring-up → QRTR → SMGR IIO driver → an IIO sensors HAL. |
| E-ink PMIC is a TI **TPS65185** at `i2c@c176000` addr 0x68 with five TLMM GPIOs (0, 2, 3, 35, 80); the pinned kernel already contains the upstream `tps65185` regulator/hwmon driver | stock DT 9129–9140; `drivers/regulator/tps65185.c` | No PMIC driver to write for e-ink power. Candidate E1 describes it; module built for V67. Gives VCOM control + panel temperature. |
| Rear touch = FocalTech "5x06" on `i2c@c1b7000` addr 0x38, IRQ gpio73, reset gpio65, 720×1440, 5 points; front touch = FocalTech FT8xxx on `i2c@c178000` | stock DT 9421/9233 | Upstream `edt-ft5x06` (already `=m`) is the natural rear-touch driver. In candidate E1. |
| Wi-Fi supplies and all remote-processor memory regions equal the upstream SDM660 reference | stock DT 5725–5741 vs `sdm630.dtsi` | Candidate M1 needs only four regulator nodes + two `status = "okay"`. |
| GPU firmware for Adreno 512 is present in stock vendor (`a512_zap.*`, `a530_pm4.fw`, `a530_pfp.fw`, `a530v3_gpmu.fw2`); kernel has `DRM_MSM=m`, GPUCC/MMCC built in; `external/mesa3d` exists in the Lineage tree | vendor/firmware listing, V67 config | Candidate G1 (render node only). Userspace = Mesa freedreno + minigbm; display stays simpledrm until native MDSS/DSI work. |
| Stock services that must be replaced or reused: `rmt_storage`, `tftp_server`, `pd-mapper`, `pm-service`, `cnss-daemon`, `qcrild`, `netmgrd`, `sensors.qti`, `thermal-engine`, `qseecomd`, fingerprint (`vendor.sw.swfingerprint`), `loc_launcher` | process list | Open replacements built today: `rmtfs`, `tqftpserv`, `qrtr-lookup` (pd-mapper is in-kernel upstream). |
| 6 GB RAM, ~4 GB available under stock; `/dev/epd_flash` is mode 0644 but SELinux denies `shell` | `/proc/meminfo`, `ls -lZ` | RAM boot of the full framework fits (EROFS payload 1.7 GB, validated in a 6 GiB VM). E-ink flash must be read from our own kernel (V68 path). |

## Per-area status and next attended step

| Area | Prepared offline | Next attended step | Blockers / unknowns |
|---|---|---|---|
| Framework/UI | V70: full boot to the LineageOS welcome screen on the **phone kernel binary**, phone-style EROFS delivery, 6 GiB; on-phone launcher `framework-phone-v71.sh`; guards require V68 + approval file | After V68 baseline: push bundle, run launcher, watch the LCD | touch needs the front-touch module loaded first; GPU is software (slow); SELinux permissive |
| E-ink panel data | V68 SPI read path + read-only tool (opcode allowlist) | `run-eink-read.sh` → full NOR image + JEDEC ID | flash power assumed = gpio42 only (stock VCOM-read path) |
| E-ink TCON | stock library runs in the VM; ABI recovered | feed real waveform offline first | drive-frame encoding + DSI/bridge transport still to reverse |
| E-ink power / rear touch | candidate E1 + `tps65185.ko` | later image: probe PMIC (I²C only), read temperature/VCOM; rear touch events | GPIO roles verified against the stock driver disassembly (WAKEUP=80, PWRUP=35, VCOM_CTRL=3, PWR_GOOD=0, VIN switch=2) |
| ADSP | candidate A in V68, `adsp_diag_r2.sh`, bundle with trusted hashes | run the bounded start/stop | CX proxy vote / LPASS SMMU differences (candidate B ready in research/) |
| Audio, motion sensors | depend on ADSP | after ADSP PASS: pd-mapper/APR modules, then codec routes | amplifier/codec identities recorded by Astra; no playback yet |
| Front ALS/prox | `stk3310.ko` for V67 | needs a small overlay on `i2c@c1b6000` (not written: supply/IRQ lines still to resolve) | chip-ID acceptance |
| Modem / Wi-Fi / GNSS | candidate M1, `rmtfs`/`tqftpserv` Android builds | modem start with rmtfs on RAM copies; then ath10k scan | RF-capable: user decides SIM/antenna state; IPA data path unresolved |
| Bluetooth | Astra's module/firmware bundle | unchanged | IO supply question open |
| GPU | candidate G1 | load `msm.ko` with firmware in RAM, check render node | GDSC/SMMU hand-over from bootloader; Mesa build not started |
| Charging / thermal / suspend | unchanged (modules prepared by Astra) | keep behind validated power management | deliberately not touched today |
| Cameras, fingerprint | none | — | long-term; fingerprint needs TEE path |
