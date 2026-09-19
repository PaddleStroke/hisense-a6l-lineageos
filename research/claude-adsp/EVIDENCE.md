# A6L ADSP startup diagnostic — evidence

Prepared 18 September 2026, offline. No phone, laptop, ADB, flashing or
activation. Nothing outside `research/claude-adsp/` was modified.

Legend: **[F]** documented fact read from a primary source named here;
**[I]** inference from those facts; **[H]** untested hypothesis.

## A. Identity of the compared artefacts

| Item | Identity | Source |
|---|---|---|
| Pinned kernel source | `sdm660-mainline/linux` commit `e47d622cb6d2440a9eacdc8bb2df32c037bec7b8`, Makefile `VERSION=7 PATCHLEVEL=2 SUBLEVEL=3` [F] | `docs/kernel72-v36-20260917.md` names the commit; `https://raw.githubusercontent.com/sdm660-mainline/linux/e47d622cb6d2440a9eacdc8bb2df32c037bec7b8/Makefile` returns 7.2.3; the same hash on `torvalds/linux` is a different, unrelated commit ("fixup! Add GitHub Actions CI", minlexx), i.e. the pinned tree is the sdm660-mainline fork, not vanilla stable [F] |
| Built ADSP driver | `qcom_q6v5_pas.ko` sha256 `fe2aabc87e27029bb166fabfc8f575501f15d4239be01b54af5dcf3d4f103104`, vermagic `7.2.3-a6l-probe+ SMP preempt mod_unload aarch64`, srcversion `9D078F2378536FE7E5758CB`, alias `of:N*T*Cqcom,sdm660-adsp-pas` present, depends `qcom_q6v5,qcom_common,qcom_sysmon,mdt_loader,qcom_pil_info` [F] | `modinfo firmware/extracted/peripheral-prep-20260917/modules/qcom_q6v5_pas.ko` |
| Kernel config | `firmware/extracted/peripheral-prep-20260917/kernel.config` [F] | see C4 |
| Booting tree (latest) | `firmware/extracted/recovery-controls-v46-20260918/merged-captured-abl.dtb` sha256 `4ed016fdf38baedd85a6046467486e09f679abffafee0311b770d88f3487f2ac` (base `081b66ce…`, captured-ABL overlay output, `report.json`) [F] | audited by `tools/audit_adsp_dt.py` → `checks/adsp-dt-audit.json` |
| Stock DTB | `firmware/extracted/device-trees/stock-00.dtb` sha256 `ef0b93babaa97992003f3edb0795f8c4c44993a73d1b98fb9e494e8131100030` (stock-01 differs only in unrelated nodes; `grep -i adsp` diff empty) [F] | `device-trees/manifest.json`, `stock-00.dts` |
| Stock ADSP firmware | `adsp.mdt` sha256 `9a5105caf136434128f0893e4993fe77b33a00a3df3f42286559cbaf35822800` (7964 B) + `adsp.b02`…`adsp.b21`; per-file hashes in `checks/adsp-mdt-loader-audit.json` [F] | `firmware/extracted/peripheral-prep-20260917/firmware/qcom/hisense/a6l/`, provenance in `peripheral-firmware-20260917-r3/provenance.json` |

Note: the assignment's "pinned modern driver" is therefore the sdm660-mainline
fork at 7.2.3. All source line numbers below refer to raw files at that commit
(`https://raw.githubusercontent.com/sdm660-mainline/linux/e47d622cb6d2440a9eacdc8bb2df32c037bec7b8/<path>`).

## B. Stock ADSP configuration (HLTE730T, Android 9, kernel 4.4)

All from `firmware/extracted/device-trees/stock-00.dts` [F]:

| Property | Stock value | Line(s) |
|---|---|---|
| Node | `qcom,lpass@15700000`, `compatible = "qcom,pil-tz-generic"`, `reg = <0x15700000 0x100>` | 5745–5748 |
| Watchdog IRQ | `interrupts = <0 0xa2 1>` (SPI 162) | 5749 |
| Proxy supply | `vdd_cx-supply = <0xf0>` → `pm660l_l9_level` under `rpm-regulator-ldob9` with `qcom,resource-name = "rwlc"`, `qcom,resource-id = <0>`, `regulator-l9-level` min 0x10 / max **0x180 (384)**, `qcom,use-voltage-level`, `qcom,set = <3>`; node votes `qcom,vdd_cx-uV-uA = <0x180 0x186a0>` (level 384, 100 000 µA) | 5750–5752, 5541–5563 |
| Proxy clock | `clocks = <0x26 0x52>`, `clock-names = "xo"` where 0x26 = `qcom,rpmcc-sdm660` (downstream index 0x52) | 5753–5755, 4210–4213 |
| PAS id / SMEM / sysmon | `qcom,pas-id = <1>`, `qcom,smem-id = <0x1a7>` (423), `qcom,sysmon-id = <1>`, `qcom,ssctl-instance-id = <0x14>`, `qcom,proxy-timeout-ms = <10000>` | 5756–5760 |
| Firmware | `qcom,firmware-name = "adsp"` (split `adsp.mdt` + `adsp.bNN`) | 5761 |
| Memory | `memory-region = <0xf1>` → `adsp_fw_region@92a00000`, `compatible = "removed-dma-pool"`, `no-map`, `reg = <0 0x92a00000 0 0x1e00000>` | 5762, 14511–14517 |
| SMP2P bits (in, remote-pid 2) | err-fatal 0, err-ready 1, proxy-unvote 2, stop-ack 3; force-stop out bit 0 | 5763–5767, 519–534 |
| SMP2P transport | `qcom,smp2p-adsp@17911008`, `qcom,remote-pid = <2>`, `qcom,irq-bitmask = <0x400>` (APCS bit 10), `interrupts = <0 0x9e 1>` (SPI 158) | 326–332 |
| GLINK transport | `qcom,glink-smem-native-xprt-adsp@86000000`, SMEM `0x86000000/0x200000`, `qcom,irq-mask = <0x200>` (APCS bit 9), `interrupts = <0 0x9d 1>` (SPI 157), `label = "lpass"` | 4722–4731 |
| IPC router | `qcom,ipc_router_q6_xprt`: channel `IPCRTR` on edge `lpass` | 5686–5692 |
| Userspace loader | `qcom,msm-adsp-loader { qcom,adsp-state = <0>; }` (ADSP started by userspace on demand, not by the kernel at boot) | 11174–11177 |
| LPASS Q6 SMMU | `arm,smmu-lpass_q6@5100000`, `qcom,skip-init`, `qcom,register-save`, TZ-programmed `attach-impl-defs` (HLOS does not initialise it) | 8731–8744 |
| IMEM PIL info | `pil@94c { compatible = "qcom,msm-imem-pil"; reg = <0x94c 0xc8>; }` | 5883–5886 |
| Other reserved fixed regions | `removed_regions@85800000/0x3700000`, modem `0x8ac00000/0x7e00000`, MBA `0x94800000/0x200000`, CDSP `0x94a00000/0x600000`, WLAN MSA `0x85700000/0x100000` + guard, splash `0x9d400000/0x23ff000`, dfps `0x9f7ff000/0x1000`, Hisense log/trap/recorder `0xb0000000…` | 14478–14611 |
| PD-mapper JSON | `adspr.jsn` (root_pd, instance 74, tms/servreg), `adsps.jsn` (sensor_pd, 74, tms/servreg), `adspua.jsn` (audio_pd, 74, tms/servreg + avs/audio) | `peripheral-prep-20260917/firmware/qcom/hisense/a6l/*.jsn` |

## C. Pinned driver and tree

### C1. Driver match data (`drivers/remoteproc/qcom_q6v5_pas.c`) [F]

* Line 1613: `{ .compatible = "qcom,sdm660-adsp-pas", .data = &adsp_resource_init }`.
* Lines 900–908: `adsp_resource_init = { .crash_reason_smem = 423, .firmware_name = "adsp.mdt", .pas_id = 1, .auto_boot = true, .ssr_name = "lpass", .sysmon_name = "adsp", .ssctl_id = 0x14 }` — **no `proxy_pd_names`**, no `load_state`, no `smem_host_id`, no region assignment.
* Probe (734–880): requires `qcom_scm_is_available()` (else `-EPROBE_DEFER`); optional `firmware-name` string (751); `has_iommu = of_property_present("iommus")` (774); `alloc_memory_region()` maps `memory-region` index 0 write-combined (622–639); `init_clock()` mandatory `xo`, optional `aggre2` (532–545); `init_regulator()` optional `cx`/`px` supplies (547–569); `pds_attach()` returns 0 when `proxy_pd_names == NULL` (577–578) so the DT's single `cx` power domain is only what the platform core attaches at probe; `qcom_q6v5_init()` requests the five named interrupts and the `stop` smem-state (qcom_q6v5.c 248–355; `qmp_get()` -ENODEV tolerated 335–340; `devm_of_icc_get(NULL)` 349); subdevs glink/smd/pdm/sns-reg/sysmon/ssr (828–838); PAS context for `pas_id`, region (840–845); `use_tzmem = has_iommu` (855) → false; `rproc_add()` with `auto_boot = true` (775, 857).
* Start (273–382): `qcom_q6v5_prepare` → enable proxy PDs (none) → `clk_prepare_enable(xo)` → `qcom_mdt_pas_load()` → `qcom_pil_info_store("adsp", …)` → `qcom_scm_pas_prepare_and_auth_reset()` (= `qcom_scm_pas_auth_and_reset(pas_id)` when `use_tzmem` false, qcom_scm.c 1004–1015) → `qcom_q6v5_wait_for_start(5000 ms)`; on `-ETIMEDOUT` logs `start timed out`, calls `qcom_scm_pas_shutdown()` and unwinds.
* Handover (384–395, on smp2p bit 2): drops px/cx supplies, aggre2, xo and proxy PDs.
* Stop (397–432): `qcom_q6v5_request_stop()` (sysmon SSCTL shutdown if discovered, else smp2p `stop` bit + wait 5 s for stop-ack, qcom_q6v5.c 198–216) → `qcom_scm_pas_shutdown(1)` → unmap → `qcom_q6v5_unprepare()` → handover cleanup if never signalled.
* Crash: wdog/fatal IRQ handlers read SMEM item 423 (`crash_reason`) and call `rproc_report_crash()` (qcom_q6v5.c 91–133).

### C2. Tree node (`arch/arm64/boot/dts/qcom/sdm630.dtsi` 2438–2560; compiled form in the V46 merged DTB) [F]

`adsp_pil: remoteproc@15700000 { compatible = "qcom,sdm660-adsp-pas"; reg = <0x15700000 0x4040>; interrupts-extended = <&intc GIC_SPI 162 …>, <&adsp_smp2p_in 0..3 …>; interrupt-names = "wdog","fatal","ready","handover","stop-ack"; clocks = <&rpmcc RPM_SMD_XO_CLK_SRC>; clock-names = "xo"; memory-region = <&adsp_region>; power-domains = <&rpmpd RPMPD_VDDCX>; power-domain-names = "cx"; qcom,smem-states = <&adsp_smp2p_out 0>; qcom,smem-state-names = "stop"; glink-edge { interrupts = <GIC_SPI 157 …>; label = "lpass"; mboxes = <&apcs_glb 9>; qcom,remote-pid = <2>; apr {…}; fastrpc {…}; }; }` — enabled by default in the dtsi; `device/hisense/a6l/kernel/sdm660-hisense-a6l-probe.dts` sets `&adsp_pil { status = "disabled"; }` (and disables `remoteproc_cdsp`, `remoteproc_mss`).
`smp2p-adsp` (537–555): `qcom,smem = <443>, <429>`, SPI 158, `mboxes = <&apcs_glb 10>`, remote-pid 2. `adsp_region: adsp@92a00000 { reg = <0 0x92a00000 0 0x1e00000>; no-map; }` (485–488). `adsp_mem` 8 MiB CMA pool for fastrpc (505–511).

### C3. Matching result (stock ↔ pinned) — `checks/adsp-dt-audit.json`, 30/30 checks passed [F]

Identical or equivalent: QDSP6SS base 0x15700000; wdog SPI 162; smp2p remote-pid 2 / SPI 158 / APCS bit 10 (stock irq-bitmask 0x400); glink SPI 157 / APCS bit 9 (stock 0x200) / label `lpass` / remote-pid 2; smp2p bit mapping fatal=0 ready=1 handover(proxy-unvote)=2 stop-ack=3, force-stop out bit 0; PAS id 1; crash SMEM 423; SSCTL instance 0x14; `sysmon`/`ssr` names; XO clock from the RPM clock controller; reserved region `0x92a00000/0x1e00000` no-map with no other fixed claimant and no overlaps; every stock fixed reserved region is covered by the merged tree (tz_mem was already widened to `0x86200000/0x2d00000` to match stock `removed_regions`).
Differences: see D below.

### C4. Kernel config facts (`peripheral-prep-20260917/kernel.config`) [F]

Built-in: `QCOM_SCM`, `QCOM_TZMEM` + `QCOM_TZMEM_MODE_GENERIC`, `QCOM_SMEM`, `QCOM_SMEM_STATE`, `QCOM_SMP2P`, `QCOM_SMSM`, `QCOM_APCS_IPC`, `HWSPINLOCK_QCOM`, `RPMSG`, `RPMSG_QCOM_GLINK(_SMEM,_RPM)`, `RPMSG_QCOM_SMD`, `QCOM_SMD_RPM`, `QCOM_RPMPD`, `QCOM_CLK_SMD_RPM`, `REMOTEPROC`, `ARM_SMMU` + `ARM_SMMU_QCOM` + `ARM_SMMU_DISABLE_BYPASS_BY_DEFAULT`, `FW_LOADER`, `FW_LOADER_USER_HELPER` (fallback **not** set), `DEBUG_FS`, `SDM_GCC_660`, `INTERCONNECT_QCOM_SDM660`.
Modules: `QCOM_Q6V5_PAS`, `QCOM_Q6V5_COMMON`, `QCOM_RPROC_COMMON`, `QCOM_SYSMON`, `QCOM_MDT_LOADER`, `QCOM_PIL_INFO`, `QCOM_AOSS_QMP`, `QRTR`, `QRTR_SMD`, `QCOM_QMI_HELPERS`, `QCOM_PD_MAPPER`, `QCOM_SNS_REG`, `QCOM_PDR_HELPERS`, `QCOM_APR`, `QCOM_FASTRPC`, all `SND_SOC_QDSP6_*`.
Not set: `QCOM_Q6V5_ADSP` (the non-PAS lpass driver, not needed), `QCOM_RMTFS_MEM` (modem only), `REMOTEPROC_CDEV`.

### C5. Phone evidence already on file (V46, same kernel 7.2.3-a6l-probe+) — `captures/capture-controls-user-v46/stages/baseline/dmesg-after.txt`, `interrupts.txt` [F]

* `qcom_scm: convention: smc arm 64`, `probe of firmware:scm returned 0` (lines 468–469).
* `probe of smem returned -517` then `returned 0 after 5081 usecs` (241, 1570); `probe of 1f40000.hwlock returned 0` (250); `17911000.mailbox returned 0` (252).
* `smp2p-adsp` deferred then `returned 0` (1019, 1572); `/proc/interrupts` IRQ 17 `GICv3 190 Edge smp2p-adsp` (SPI 158 + 32 = 190 ✔); `smp2p-mpss`, `smp2p-cdsp` also bound; `qcom-socinfo returned 0` (1569).
* `remoteproc:glink-edge` (RPM glink) probed; `rpm-requests:clock-controller` (rpmcc) and `power-controller` (rpmpd) `returned 0` (512, 546); regulators-0/1 probed.
* `arm-smmu 5100000.iommu` (LPASS Q6 SMMU) probed: SMMUv2, stage-1, 13 SMR groups, 14 context banks, **"preserved 0 boot mappings"** (1085–1096); GDSCs `hlos1_vote_lpass_adsp_gdsc` probed (248) and kept by the local boot-domain preservation patch (1665).
* Boot command line includes `clk_ignore_unused pd_ignore_unused regulator_ignore_unused` (`recovery-controls-v46-20260918/report.json`).

[I] Every kernel-side dependency of the PAS driver except the driver stack itself (modules) was therefore already exercised on the phone: SCM, SMEM, hwlock, APCS mailbox, smp2p-adsp, rpmcc, rpmpd, LPASS SMMU. Not exercised: SCM PAS calls, mdt loading, glink-smem to the ADSP, QRTR over IPCRTR, sysmon.

## D. Differences and their assessment

### D1. Firmware format/name [F]
Stock `qcom,firmware-name = "adsp"` (downstream appends `.mdt`/`.bNN`); pinned default `adsp.mdt`; the preserved files live under `qcom/hisense/a6l/`. The overlay sets `firmware-name = "qcom/hisense/a6l/adsp.mdt"`; `mdt_load_split_segment()` derives `…/adsp.b02` etc. by replacing the last three characters (mdt_loader.c 76–83).

### D2. Firmware structure as the pinned loader sees it — `checks/adsp-mdt-loader-audit.json` [F]
`tools/audit_mdt_loader.py` re-implements mdt_loader.c: ELF32 header valid; 23 program headers; phdr[0] is the ELF-header segment (not PT_LOAD); phdr[1] is the hash segment (flags type 2, p_paddr 0x94800000, 0x2000, not loaded); `ehdr_size 788 + hash_size 7176 == 7964 == mdt size` ⇒ split image with hash packed after the header ⇒ metadata passed to `QCOM_SCM_PIL_PAS_INIT_IMAGE` is the whole `.mdt`; all loadable segments carry `QCOM_MDT_RELOCATABLE`; `min_addr = 0x92a00000`, `max_addr = 0x94800000`, so `QCOM_SCM_PIL_PAS_MEM_SETUP(1, 0x92a00000, 0x1e00000)` — the image needs the **entire** reserved region (last segment: 0x943dc000 + 0x424000 bss); every `adsp.bNN` exists with exactly `p_filesz` bytes (loader requires equality, mdt_loader.c 91–96); no `p_filesz > p_memsz`; nothing outside the region. Signature/authentication is only decidable by TZ on the phone.

### D3. Auto-boot [F]/[I]
Pinned `auto_boot = true`: `rproc_add()` requests the firmware asynchronously and boots immediately if found. Stock started the ADSP from userspace (`qcom,adsp-loader`, state 0). For a bounded test the diagnostic inserts the module **before** the firmware exists under `/lib/firmware`; the auto-boot attempt then fails with `request_firmware failed: -2` (or the `-ENOENT` variant printed by remoteproc_core) and the rproc stays `offline`; `FW_LOADER_USER_HELPER_FALLBACK` is not set, so no 60 s sysfs wait is expected [I]. The firmware is then copied in and `echo start > state` performs a synchronous, observable start. Candidate B additionally sets `auto_boot = false`.

### D4. Power/clock proxy — clock [F]/[I]
Stock "xo" = `qcom,rpmcc-sdm660` index 0x52 (downstream numbering); pinned `rpmcc RPM_SMD_XO_CLK_SRC` (index 0). Both are the RPM-managed XO; the exact RPM resource equality was not verified against the downstream header (offline) [I].

### D5. Power/clock proxy — CX rail (the main unresolved difference) [F]/[H]
* Stock proxies **`rwlc`/0** (LPASS/SSC CX, PM660L L9 in level mode) at level 384 (TURBO) with 100 mA load, on both RPM sets, until the ADSP raises proxy-unvote (B, `rpm-regulator-ldob9`).
* Pinned tree: `power-domains = <&rpmpd RPMPD_VDDCX>` where `RPMPD_VDDCX = 0` (`include/dt-bindings/power/qcom-rpmpd.h`) and `sdm660_rpmpds[RPMPD_VDDCX] = &cx_rwcx0_lvl` = RPM resource **`rwcx`/0** (`drivers/pmdomain/qcom/rpmpd.c` 847–858; `RPMPD_SSCCX = 6` → `ssc_cx_rwlc0_lvl` = `rwlc`/0, lines 569–574, 854). Because `adsp_resource_init` has no `proxy_pd_names`, the driver never calls `dev_pm_genpd_set_performance_state()`; the platform core only powers the domain on (`rpmpd_power_on`: sends `swen`, aggregates a level only if `pd->corner != 0`, lines 1030–1047). **Net effect: no level vote on either `rwcx`/0 or `rwlc`/0 during ADSP boot.**
* Upstream usage evidence: the pinned tree's `sdm630-sony-xperia-nile.dtsi` (line 160) sets `&adsp_pil { firmware-name = "qcom/sdm630/Sony/nile/adsp.mbn"; }` with this exact node, i.e. upstream contributors run the SDM630 ADSP without a proxy level vote [F]. Whether RPM keeps `rwlc` at a level sufficient for TZ to release the Q6 without any HLOS vote on this Hisense board is **not known** [H]. This is the most likely cause if candidate A ends in `start timed out`; candidate B (`patches/0001-…` + `a6l-adsp-diag-ssccx.dtso`) reproduces the stock vote (single `cx` domain pointed at `RPMPD_SSCCX`, `proxy_pd_names = {"cx"}` ⇒ `INT_MAX` clamped to `max_state = RPM_SMD_LEVEL_TURBO` (rpmpd.c 863, 1070–1071), released at handover).
* Caveat for both candidates: `rpmpd_aggregate_corner()` clamps to `max_state` until rpmpd's `sync_state()` has run (rpmpd.c 1009–1013). With many consumers disabled, sync_state may never fire in the diagnostic image, so a released proxy vote may remain at TURBO on that resource until reboot [I]. Bounded, but note for power-consumption reading.

### D6. LPASS Q6 SMMU ownership [F]/[H]
Stock: `qcom,skip-init` + `qcom,register-save` (HLOS never resets or programs the LPASS SMMU; TZ/ADSP own the stream mapping). Pinned: `qcom,sdm630-smmu-v2` handled by arm-smmu-qcom, reset at probe ("preserved 0 boot mappings"), unmatched streams faulting by config. Whether TZ (re)programs the Q6 stream-to-context mapping during `PAS_AUTH_AND_RESET`, or whether the mainline SMMU state blocks the Q6, is inferred only from upstream boards using the same node. Diagnostic captures `arm-smmu global fault` counters (`/proc/interrupts` IRQs 21/22) and `Unhandled context fault` lines before/after.

### D7. Memory ownership [F]/[I]
Region identical to stock; `no-map` in both; the pas driver `ioremap_wc`s and writes the segments **before** `PAS_MEM_SETUP`/`AUTH_AND_RESET`, exactly as stock `pil-tz-generic` does with the same SCM service/commands, so HLOS write access to `0x92a00000…0x947fffff` before authentication is the established stock flow [I]. The ABL-captured merged tree retains all stock fixed regions; three mainline-only fixed regions exist (`venus@9f800000`, `qseecom-region@f6800000`, `gpu@fed00000`) that were already present in every booted V38–V47 tree and do not touch the ADSP region. Real `/memory` and `/chosen/bootargs` are filled by the phone's ABL at boot (the offline capture shows a zero memory reg from synthetic fixtures); the diagnostic re-reads `/proc/device-tree/reserved-memory` on the phone before touching anything.

### D8. IMEM PIL-info [F]
Pinned tree has `/soc@0/sram@146bf000/pil-reloc@94c` (`qcom,pil-reloc-info`) matching stock `pil@94c`; `qcom_pil_info_store()` will write the "adsp" entry (informational; failure is ignored by `qcom_pas_start`).

### D9. Service discovery / companion services [F]/[I]
* GLINK channels the ADSP opens appear as `/sys/bus/rpmsg/devices/*` (names such as `apr_audio_svc`, `IPCRTR`, `sys_mon`, `fastrpcglink-apps-dsp`) without any userspace daemon — primary in-kernel discovery evidence.
* QRTR services: `qrtr-smd.ko` binds `IPCRTR`; services (SSCTL id 43 instance 0x14 via sysmon; sensor/audio PD services) can be listed with `tools/a6l_qrtr_lookup.c` (a 90-line re-implementation of linux-msm `qrtr-lookup`, read-only). Built natively here (x86) only to prove it compiles; AArch64 static build must use the coordinator's toolchain.
* Protection-domain mapper: in-kernel `qcom_pd_mapper.ko` (`CONFIG_QCOM_PD_MAPPER=m`) carries a `qcom,sdm660` table (`drivers/soc/qcom/qcom_pd_mapper.c` 475–483, 612) whose `adsp_audio_pd` (instance 74, `avs/audio`), `adsp_root_pd`, `adsp_sensor_pd` match the stock JSONs (instance 74); root/sensor omit the stock `tms/servreg` entry (mainline design). The module was **not** built into the prep bundle. Not needed for startup; needed before `q6afe`/`q6asm` PDR (`qcom,protection-domain = "avs/audio", "msm/adsp/audio_pd"`).
* Sensor registry: `qcom_sns_reg.ko` (`CONFIG_QCOM_SNS_REG=m`, `qcom_add_sns_reg_subdev`, qcom_common.c 662–725) — also not in the bundle; sensors follow-up only.
* Userspace alternatives (linux-msm `pd-mapper`, `qrtr` tools) are not present in the diagnostic ramdisk and are not required for this test. No RMTFS/tqftpserv involvement for ADSP.

### D10. Stop / crash / unload behaviour [F]/[I]
* Stop: sysmon graceful path (SSCTL QMI if discovered within 0.5 s, else `sys_mon` channel), then smp2p `stop` bit with 5 s stop-ack wait, then `PAS_SHUTDOWN`; the driver logs `timed out on wait` / `failed to shutdown: N` on failure but always proceeds to unmap and unprepare.
* Crash: `watchdog received: <SMEM 423 text>` / `fatal error received: …`, then `rproc_report_crash` → recovery restart unless `/sys/class/remoteproc/remoteprocN/recovery = disabled` (the diagnostic disables it so a crash is observed once, not looped). Coredump default is disabled.
* Unload: `qcom_pas_remove()` → `rproc_del()` stops a running ADSP; `pds_detach` → genpd detach may power the `cx` genpd off (`swen = 0`), which is the pre-probe state. The diagnostic does **not** unload modules; the session ends by the usual return-to-stock path.

## E. Reproducible checks run here

| Tool | Input | Output | Result |
|---|---|---|---|
| `tools/audit_mdt_loader.py` (loader re-implementation) | `adsp.mdt` + `adsp.bNN`, region 0x92a00000/0x1e00000 | `checks/adsp-mdt-loader-audit.json` | passed; 23 phdrs, 20 split files, exact sizes, fits region exactly |
| `tools/audit_adsp_dt.py` + `tools/fdt_lite.py` (dependency-free DTB parser; round-trip verified identical) | V46 `merged-captured-abl.dtb` | `checks/adsp-dt-audit.json`, `checks/candidate-adsp-merged.dtb` (sha256 `c63e901602ae2c62bc111706f11b6e090ad884f3955ed1c4a45673495eef32e2`) | 30/30 checks; simulated overlay changes exactly 3 properties (`…/remoteproc@15700000/status`, `…/firmware-name`, `/chosen/hisense,a6l-adsp`) |
| `gcc` native build of `tools/a6l_qrtr_lookup.c` | — | — | compiles clean with `-Wall -Wextra`; runs and reports `AF_QIPCRTR` unsupported on this host (expected) |

Not done here (needs the WSL tree/toolchain): compiling the `.dtso` with dtc, `fdtoverlay` onto `base.dtb`, rebuilding `qcom_q6v5_pas.ko` for candidate B, AArch64 build of the helper, QEMU module load of the 10-module subset (all 10 were already in the 103/103 QEMU pass as part of the 50-module set).

## F. Primary sources (URLs)

* Pinned tree files (commit e47d622cb6d2440a9eacdc8bb2df32c037bec7b8): `drivers/remoteproc/qcom_q6v5_pas.c`, `drivers/remoteproc/qcom_q6v5.c`, `drivers/remoteproc/qcom_common.c`, `drivers/remoteproc/qcom_sysmon.c`, `drivers/soc/qcom/mdt_loader.c`, `drivers/soc/qcom/qcom_pd_mapper.c`, `drivers/firmware/qcom/qcom_scm.c`, `drivers/pmdomain/qcom/rpmpd.c`, `include/dt-bindings/power/qcom-rpmpd.h`, `arch/arm64/boot/dts/qcom/sdm630.dtsi`, `arch/arm64/boot/dts/qcom/sdm630-sony-xperia-nile.dtsi`, `arch/arm64/boot/dts/qcom/sdm660-xiaomi-lavender-common.dtsi`, `arch/arm64/boot/dts/qcom/Makefile` — all read from `https://raw.githubusercontent.com/sdm660-mainline/linux/e47d622cb6d2440a9eacdc8bb2df32c037bec7b8/<path>`.
* Commit identity cross-check: `https://github.com/torvalds/linux/commit/e47d622cb6d2440a9eacdc8bb2df32c037bec7b8` (different commit, "fixup! Add GitHub Actions CI"), `https://raw.githubusercontent.com/gregkh/linux/v7.2.3/Makefile` (7.2.3 base).
* linux-msm QRTR lookup reference: `https://github.com/linux-msm/qrtr` (`src/lookup.c`).
* Local: `docs/peripheral-preparation-20260917.md`, `docs/port-status.md`, `research/controls-followup-20260918/radio-research-brief.md`, `docs/kernel72-v36-20260917.md`, `docs/controls-v46-20260918.md`, `firmware/extracted/peripheral-prep-20260917/{kernel.config,module-manifest.json,firmware-manifest.json}`, `firmware/extracted/device-trees/stock-00.dts`, `firmware/extracted/recovery-controls-v46-20260918/{merged-captured-abl.dtb,report.json}`, `captures/capture-controls-user-v46/stages/baseline/{dmesg-after.txt,interrupts.txt}`.
