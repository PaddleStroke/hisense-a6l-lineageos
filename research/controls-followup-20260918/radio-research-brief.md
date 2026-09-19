# Radio research brief — 18 September 2026

**Primary-agent review:** treat this as leads, not newly verified hardware facts.
`docs/peripheral-preparation-20260917.md` already records stock modem/ADSP/MBA/CDSP
and WLAN reserved regions, structural firmware checks, and the RMTFS configuration
gap. The brief's statement that the memory map has not been audited is too broad:
the remaining review concerns the final bootloader-adjusted tree and complete
startup/service contracts. Do not repeat the initial inventory as new work.

The genuinely shared prerequisite is the Qualcomm control plane around the modem: an SDM660 MSS remoteproc definition that can load the device’s signed modem images through PAS/SCM, the correct reserved-memory/carve-outs and SMEM/GLINK (or SMD) channels, then QRTR/QMI service discovery. RMTFS is part of that chain where the modem expects host-backed modem filesystem storage; linux-msm’s implementation is a userspace service, so enabling a kernel memory device alone is insufficient. The same QRTR/QMI plane is the useful common dependency for cellular control and the GNSS LOC service identified locally. This does not make IPA data, SIM registration, or GNSS fixing automatic.

ADSP/APR/QDSP6 is a second shared prerequisite for internal audio and ADSP-backed sensors. It shares remoteproc-style firmware, memory, power-domain and GLINK/SSR mechanics, but it is a separate processor path from MSS. WCN3990 Wi-Fi/Bluetooth should therefore be treated as a consumer of its own transport, firmware and board data; modem SSR coupling can affect recovery, but MSS bring-up is not evidence that Wi-Fi works. Audio still needs the A6L codec/DAI routes and protection policy after ADSP is alive.

The most relevant supported SDM660 example is mainline Linux’s `qcom_q6v5_mss` driver: it has an explicit `MSS_SDM660` variant and SDM660-specific power, memory-clamp and BHS handling. It is a credible host-side starting point, but it does not supply A6L’s DT, firmware naming, carve-outs, regulators, or modem NV. The linux-msm RMTFS and QRTR projects document the companion service boundaries. A useful device-family cross-check is Lineage’s Xiaomi SDM660 tree, which packages `qrtr-cfg`, `rmt_storage`, ADSP RPC and modem-facing blobs; it is an Android integration example, not proof of Hisense compatibility.

For A6L, the exact gaps are: `CONFIG_QCOM_RMTFS_MEM` is disabled; no radio DT is enabled; MSS/ADSP reserved-memory and firmware-to-node mapping have not been audited against the verified stock DT; no RMTFS service/socket topology or QRTR endpoint inventory is established; and no safe modem/ADSP start/stop evidence exists. The highest-value next offline investigation is a read-only, side-by-side inventory of stock DT and extracted firmware: identify every MSS and ADSP node, reserved-memory region, regulator/clock/reset/SMEM/GLINK/SSR property, firmware filename, and RMTFS memory channel, then compare each property to the mainline SDM660 driver’s required bindings and the bundled userspace service expectations. Record unknowns explicitly; do not infer addresses or activate a subsystem from resemblance.

Primary references: [mainline qcom_q6v5_mss SDM660 implementation](https://github.com/torvalds/linux/blob/master/drivers/remoteproc/qcom_q6v5_mss.c), [linux-msm RMTFS](https://github.com/linux-msm/rmtfs), [linux-msm QRTR service lookup](https://github.com/linux-msm/qrtr/blob/master/src/lookup.c).
