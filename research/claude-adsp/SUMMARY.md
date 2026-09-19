# A6L ADSP startup diagnostic — summary (claude-adsp, 18 September 2026)

Offline research only. No phone/laptop access, no builds in shared trees,
nothing outside `research/claude-adsp/` touched. Details and line-level
citations: `EVIDENCE.md`. Test plan: `DIAGNOSTIC.md`.

## Headline

The pinned kernel is the **sdm660-mainline fork at commit
`e47d622cb6d2440a9eacdc8bb2df32c037bec7b8` (7.2.3)**, whose `sdm630.dtsi`
already carries an enabled `adsp_pil` node that our A6L probe DTS explicitly
disables. Compared property by property with the stock HLTE730T DTB, the
node's addresses, interrupts, SMP2P/GLINK plumbing, PAS id, crash SMEM item,
SSCTL instance and reserved memory are identical or equivalent, and every
kernel dependency except the remoteproc driver stack itself has already
probed successfully on the phone (V46 dmesg). The preserved split firmware
passes an offline re-implementation of the pinned `mdt_loader` checks and
needs exactly the reserved `0x92a00000/0x1e00000` region.

Smallest evidence-backed diagnostic: a **three-property DT overlay**
(`patches/a6l-adsp-diag.dtso`: `status = "okay"`, `firmware-name`, a
`/chosen` marker), **10 unmodified, already QEMU-checked modules**, the
**stock firmware files in tmpfs**, and one bounded script that starts the
ADSP through `/sys/class/remoteproc`, records authentication/startup/service
discovery and stops it. No driver change is required for this attempt.

Two real differences from stock remain unresolved and are flagged, not
assumed away: the **CX proxy vote** (stock votes RPM `rwlc`/0 = LPASS CX at
TURBO during boot; the pinned tree/driver votes no level on any CX resource)
and **LPASS Q6 SMMU ownership** (stock `skip-init`, mainline resets it). A
prepared but unbuilt candidate B (driver patch + alternate overlay)
reproduces the stock vote if candidate A ends in `start timed out`.

## Readiness matrix

| Dependency | State | Evidence | Blocker / note |
|---|---|---|---|
| Driver match `qcom,sdm660-adsp-pas` | Present, built, QEMU-loaded | `qcom_q6v5_pas.ko` alias, pas.c:1613, `adsp_resource_init` 900–908 | none |
| DT node `adsp_pil` | Present, disabled by our DTS | V46 merged DTB, `checks/adsp-dt-audit.json` 30/30 | enable via overlay (3 properties) |
| Reserved memory `adsp@92a00000/0x1e00000` | Present, identical to stock, no overlaps, no other claimant | audit + stock lines 14511–14517 | firmware fills the region exactly; final `/memory` is ABL-filled on the phone — re-read in preflight |
| Firmware files | Present, structurally accepted by pinned loader | `checks/adsp-mdt-loader-audit.json` (23 phdrs, 20 split files, exact sizes, relocatable, PAS_MEM_SETUP 0x1e00000) | TZ signature verdict only on phone |
| SCM / PAS | SCM probed (`smc arm 64`); PAS commands same service as stock `pil-tz-generic` | V46 dmesg 468–469; qcom_scm.c 607–660, 1004–1015 | first phone run answers INIT_IMAGE/AUTH result |
| TZMEM (metadata buffer) | Built-in generic mode | kernel.config | none |
| SMEM + hwlock | Probed on phone | V46 dmesg 1570, 250 | none |
| SMP2P-adsp (in/out) | Probed, IRQ 190 registered, items 443/429 | V46 dmesg 1572, interrupts.txt | none |
| APCS mailbox bits 9/10 | Probed | V46 dmesg 252; audit | none |
| GLINK-smem to `lpass` | Built-in; child of node; untested | config `RPMSG_QCOM_GLINK_SMEM=y` | first evidence = rpmsg devices |
| XO clock (`rpmcc`) | Probed | V46 dmesg 512 | stock index 0x52 vs mainline 0 not cross-checked against downstream header (D4) |
| CX power domain | rpmpd probed; **resource differs from stock and no level vote** | rpmpd.c 847–858, stock 5541–5563 | **unresolved (D5)**; candidate B prepared |
| LPASS Q6 SMMU | Probed by mainline, reset | V46 dmesg 1085–1096; stock 8731–8744 `skip-init` | **unresolved (D6)**; capture fault counters |
| GDSC `hlos1_vote_lpass_adsp` | Probed, boot state preserved | V46 dmesg 248, 1665 | none for startup |
| Interconnect / AOSS QMP | Not required (no `interconnects`, `qmp_get` -ENODEV tolerated) | q6v5.c 335–352 | none |
| sysmon + QMI | Module present; needs `qrtr.ko` before probe | sysmon.c 672–678 | load order in `tools/adsp-diag-modules.txt` |
| QRTR over IPCRTR | `qrtr.ko`, `qrtr-smd.ko` present | manifest | service-discovery evidence via `tools/a6l_qrtr_lookup.c` |
| PD mapper / sensor registry | In-kernel drivers configured (`=m`) with matching sdm660 tables, **not built into bundle** | pd_mapper.c 475–483; stock `adsp*.jsn` | not needed for startup; needed before audio PDR |
| IMEM pil-reloc | Present in tree | audit | informational |
| Auto-boot at insmod | Driver default `auto_boot = true` | pas.c 775 | guarded by installing firmware after insmod (D3); candidate B sets false |
| Stop / crash / unload | Reviewed | pas.c 397–432, q6v5.c 198–216 | recovery disabled during test; no rmmod |
| Companion userspace | None required for startup | D9 | pd-mapper/qrtr tools are optional later |

## Deliverables in this directory

* `SUMMARY.md` (this file), `EVIDENCE.md` (facts/inferences/hypotheses with
  line citations and URLs), `DIAGNOSTIC.md` (bounded test, expected logs,
  success criteria, stop conditions, cleanup).
* `patches/a6l-adsp-diag.dtso` — candidate A overlay (3 properties).
* `patches/a6l-adsp-diag-ssccx.dtso` + `patches/0001-remoteproc-qcom_q6v5_pas-a6l-stock-style-proxy-cx-vote.patch` — candidate B (stock-style proxy level vote on `rwlc`/0, manual boot). Not built, not tested; hunk arithmetic validated only.
* `tools/audit_mdt_loader.py` — offline re-implementation of the pinned loader's acceptance checks (run here: pass).
* `tools/fdt_lite.py`, `tools/audit_adsp_dt.py` — dependency-free DTB audit and overlay simulation (run here on the V46 merged tree: 30/30, 3-property diff).
* `tools/adsp-diag-modules.txt` — the 10-module subset with hashes and insmod order.
* `tools/a6l_adsp_diag.sh` — on-phone bounded script (POSIX sh; not executed).
* `tools/a6l_qrtr_lookup.c` — read-only QRTR service lister (compiled natively here only).
* `checks/adsp-mdt-loader-audit.json`, `checks/adsp-dt-audit.json`, `checks/candidate-adsp-merged.dtb`.

## What this does not claim

No hardware result of any kind. Structural acceptance is not authentication.
Upstream boards using the same node (e.g. `sdm630-sony-xperia-nile.dtsi`
line 160) are usage evidence, not a test on this phone. Audio routing,
amplifier protection, sensors, modem and Wi-Fi are out of scope.
