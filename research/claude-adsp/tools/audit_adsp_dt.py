#!/usr/bin/env python3
"""Audit the A6L ADSP remoteproc wiring in a compiled/merged DTB and simulate
the candidate diagnostic overlay (a6l-adsp-diag.dtso) without dtc/libfdt.

Inputs : a merged DTB produced by the existing packaging pipeline (base + dtso +
         captured-ABL overlay), e.g. recovery-controls-v46-20260918/merged-captured-abl.dtb
Outputs: JSON audit, a candidate DTB with the overlay effects applied, and the
         scoped property diff (same style as the existing tree-report.json).

Checks are derived from:
  * drivers/remoteproc/qcom_q6v5_pas.c, qcom_q6v5.c, qcom_common.c
  * drivers/soc/qcom/mdt_loader.c, drivers/pmdomain/qcom/rpmpd.c
  * arch/arm64/boot/dts/qcom/sdm630.dtsi (adsp_pil node)
  all at sdm660-mainline/linux e47d622cb6d2440a9eacdc8bb2df32c037bec7b8 (7.2.3),
  * the stock HLTE730T DTB (firmware/extracted/device-trees/stock-00.dts).

Usage: audit_adsp_dt.py <merged.dtb> <outdir> [--firmware-name NAME] [--marker VALUE]
"""
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fdt_lite import FDT, diff  # noqa: E402

# Stock HLTE730T facts (verified DTB stock-00.dtb, sha256 ef0b93ba...; dts lines cited in EVIDENCE.md)
STOCK = {
    'lpass_node': 'qcom,lpass@15700000',
    'compatible': 'qcom,pil-tz-generic',
    'reg': (0x15700000, 0x100),
    'wdog_spi': 0xa2,                 # interrupts = <0 0xa2 1>
    'pas_id': 1,
    'smem_id': 0x1a7,                 # 423 crash reason
    'sysmon_id': 1,
    'ssctl_instance_id': 0x14,
    'firmware_name': 'adsp',
    'memory_region': (0x92a00000, 0x1e00000),
    'smp2p_remote_pid': 2,
    'smp2p_irq_spi': 0x9e,            # 158
    'smp2p_irq_bitmask': 0x400,       # apcs bit 10
    'glink_irq_spi': 0x9d,            # 157
    'glink_irq_mask': 0x200,          # apcs bit 9
    'smp2p_in_bits': {'err-fatal': 0, 'err-ready': 1, 'proxy-unvote': 2, 'stop-ack': 3},
    'smp2p_out_bits': {'force-stop': 0},
    'proxy_supply': 'pm660l_l9_level -> RPM resource "rwlc" id 0, level 0x180 (384, TURBO), load 100000 uA, qcom,set=3',
    'proxy_clock': 'qcom,rpmcc-sdm660 index 0x52 (downstream numbering), name "xo"',
    'proxy_timeout_ms': 10000,
    'reserved': [  # name, base, size (stock no-map / removed-dma-pool fixed regions)
        ('wlan_msa_guard', 0x85600000, 0x100000), ('wlan_msa_mem', 0x85700000, 0x100000),
        ('removed_regions', 0x85800000, 0x3700000), ('modem_fw_region', 0x8ac00000, 0x7e00000),
        ('adsp_fw_region', 0x92a00000, 0x1e00000), ('pil_mba_region', 0x94800000, 0x200000),
        ('cdsp_fw_region', 0x94a00000, 0x600000), ('splash_region', 0x9d400000, 0x23ff000),
        ('dfps_data_mem', 0x9f7ff000, 0x1000), ('boot_log_region', 0xb0000000, 0x80000),
        ('subsys_trap_region', 0xb0080000, 0x100000), ('rs_recorder_region', 0xb0180000, 0x400000),
    ],
}

# What the pinned driver needs from the node (qcom_q6v5_pas.c / qcom_q6v5.c)
DRIVER_NEEDS = {
    'compatible': 'qcom,sdm660-adsp-pas',
    'clock-names': ['xo'],
    'interrupt-names': ['wdog', 'fatal', 'ready', 'handover', 'stop-ack'],
    'qcom,smem-state-names': ['stop'],
    'power-domain-names': ['cx'],
    'match_data': {'crash_reason_smem': 423, 'firmware_name': 'adsp.mdt', 'pas_id': 1, 'auto_boot': True,
                   'ssr_name': 'lpass', 'sysmon_name': 'adsp', 'ssctl_id': 0x14, 'proxy_pd_names': None},
}


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def main():
    dtb_path, outdir = sys.argv[1], sys.argv[2]
    fw_name = 'qcom/hisense/a6l/adsp.mdt'
    marker = 'diag1'
    if '--firmware-name' in sys.argv:
        fw_name = sys.argv[sys.argv.index('--firmware-name') + 1]
    if '--marker' in sys.argv:
        marker = sys.argv[sys.argv.index('--marker') + 1]
    os.makedirs(outdir, exist_ok=True)
    data = open(dtb_path, 'rb').read()
    f = FDT(data)
    rep = {'input': os.path.abspath(dtb_path), 'input_sha256': sha256(data), 'checks': [], 'passed': True,
           'stock_reference': STOCK, 'driver_needs': DRIVER_NEEDS}

    def check(name, ok, detail=''):
        rep['checks'].append({'check': name, 'ok': bool(ok), 'detail': detail})
        if not ok:
            rep['passed'] = False

    def res(ph):
        n = f.by_phandle(ph)
        return n.path() if n else None

    a = f.symbol('adsp_pil')
    check('__symbols__/adsp_pil resolves', a is not None, a.path() if a else 'missing')
    if a is None:
        json.dump(rep, open(os.path.join(outdir, 'adsp-dt-audit.json'), 'w'), indent=1)
        sys.exit(1)
    node = {'path': a.path(), 'props': {n: v.hex() for n, v in a.props}}
    rep['adsp_pil'] = node
    check('compatible', a.strings('compatible') == [DRIVER_NEEDS['compatible']], str(a.strings('compatible')))
    reg = a.u32s('reg')
    check('reg base matches stock QDSP6SS base', reg and reg[0] == STOCK['reg'][0], hex(reg[0]) if reg else None)
    check('status currently disabled (baseline)', a.strings('status') == ['disabled'], str(a.strings('status')))
    check('clock-names', a.strings('clock-names') == DRIVER_NEEDS['clock-names'])
    ck = a.u32s('clocks')
    ckn = f.by_phandle(ck[0]) if ck else None
    check('clocks -> rpmcc (RPM XO)', ckn is not None and 'qcom,rpmcc' in (ckn.strings('compatible') or []),
          f'{res(ck[0]) if ck else None} index {ck[1] if ck else None} (RPM_SMD_XO_CLK_SRC=0)')
    check('interrupt-names', a.strings('interrupt-names') == DRIVER_NEEDS['interrupt-names'])
    ie = a.u32s('interrupts-extended')
    check('wdog SPI matches stock', ie and ie[2] == STOCK['wdog_spi'], f'SPI {ie[2] if ie else None}')
    sin = f.symbol('adsp_smp2p_in')
    check('fatal/ready/handover/stop-ack use adsp_smp2p_in bits 0..3 (stock err-fatal/err-ready/proxy-unvote/stop-ack)',
          ie and sin and all(ie[4 + 3 * i] == sin.phandle() and ie[5 + 3 * i] == i for i in range(4)),
          str(ie))
    ss = a.u32s('qcom,smem-states')
    sout = f.symbol('adsp_smp2p_out')
    check('stop state -> adsp_smp2p_out bit 0 (stock force-stop bit 0)', ss and sout and ss[0] == sout.phandle() and ss[1] == 0, str(ss))
    smp = f.node('/smp2p-adsp')
    check('smp2p-adsp remote-pid 2 / SPI 158 / apcs bit 10', smp is not None and smp.u32s('qcom,remote-pid') == [2]
          and smp.u32s('interrupts')[1] == STOCK['smp2p_irq_spi'] and smp.u32s('mboxes')[1] == 10,
          f"pid {smp.u32s('qcom,remote-pid') if smp else None} irq {smp.u32s('interrupts') if smp else None} mbox {smp.u32s('mboxes') if smp else None}")
    check('smp2p-adsp smem items 443/429', smp is not None and smp.u32s('qcom,smem') == [443, 429], str(smp.u32s('qcom,smem') if smp else None))
    ge = a.child('glink-edge')
    check('glink-edge present with label lpass, SPI 157, apcs bit 9, remote-pid 2',
          ge is not None and ge.strings('label') == ['lpass'] and ge.u32s('interrupts')[1] == STOCK['glink_irq_spi']
          and ge.u32s('mboxes')[1] == 9 and ge.u32s('qcom,remote-pid') == [2],
          f"{ge.strings('label') if ge else None} {ge.u32s('interrupts') if ge else None} {ge.u32s('mboxes') if ge else None}")
    mb = f.by_phandle(ge.u32s('mboxes')[0]) if ge else None
    check('glink mailbox -> apcs mailbox@17911000', mb is not None and mb.path().endswith('mailbox@17911000'), mb.path() if mb else None)
    mr = a.u32s('memory-region')
    mrn = f.by_phandle(mr[0]) if mr else None
    r = mrn.u32s('reg') if mrn else None
    base = (r[0] << 32 | r[1]) if r else None
    size = (r[2] << 32 | r[3]) if r else None
    check('memory-region == stock adsp_fw_region 0x92a00000/0x1e00000 with no-map',
          mrn is not None and (base, size) == STOCK['memory_region'] and mrn.get('no-map') is not None,
          f'{mrn.path() if mrn else None} {hex(base) if base else None}/{hex(size) if size else None}')
    pdn = f.by_phandle(a.u32s('power-domains')[0])
    check('power-domains -> sdm660 rpmpd index 0 (RPMPD_VDDCX = RPM "rwcx"/0)',
          pdn is not None and 'qcom,sdm660-rpmpd' in (pdn.strings('compatible') or []) and a.u32s('power-domains')[1] == 0,
          f'{pdn.path() if pdn else None} idx {a.u32s("power-domains")[1]}')
    rep['power_domain_note'] = ('DIFFERENCE: stock proxies vdd_cx via pm660l_l9_level = RPM resource "rwlc"/0 (LPASS/SSC CX) at level 384 '
                                'until proxy-unvote; pinned DT names rpmpd VDDCX ("rwcx"/0) and the pinned driver match data has no '
                                'proxy_pd_names, so no level is voted on either resource during boot. See EVIDENCE.md D5.')
    check('power-domain-names', a.strings('power-domain-names') == ['cx'])
    check('no iommus property (rproc->has_iommu false; use_tzmem false)', a.get('iommus') is None)
    check('no firmware-name yet in baseline (driver default adsp.mdt)', a.get('firmware-name') is None, str(a.strings('firmware-name')))
    # smem / hwlock / scm
    smem = f.node('/smem')
    check('smem node with memory-region smem-mem@86000000 and hwlock', smem is not None and res(smem.u32s('memory-region')[0]) == '/reserved-memory/smem-mem@86000000' and smem.get('hwlocks') is not None)
    scm = f.node('/firmware/scm')
    check('firmware/scm present', scm is not None, str(scm.strings('compatible') if scm else None))
    # imem pil-reloc (optional, qcom_pil_info)
    pil = None
    for n in f.root.walk():
        if 'qcom,pil-reloc-info' in (n.strings('compatible') or []):
            pil = n
    rep['pil_reloc_info_node'] = pil.path() if pil else None
    check('imem pil-reloc-info node (optional; qcom_pil_info_store only)', True, pil.path() if pil else 'absent -> qcom_pil_info_store returns error, ignored by qcom_pas_start')
    # reserved-memory overlaps + stock comparison
    rm = f.node('/reserved-memory')
    fixed = []
    for c in rm.children:
        rr = c.u32s('reg')
        if rr:
            fixed.append((c.name, rr[0] << 32 | rr[1], rr[2] << 32 | rr[3]))
    fixed.sort(key=lambda x: x[1])
    overlaps = []
    for i in range(len(fixed)):
        for j in range(i + 1, len(fixed)):
            a1, s1 = fixed[i][1], fixed[i][2]
            a2, s2 = fixed[j][1], fixed[j][2]
            if a1 < a2 + s2 and a2 < a1 + s1:
                overlaps.append((fixed[i][0], fixed[j][0]))
    check('no overlapping fixed reserved regions', not overlaps, str(overlaps))
    rep['reserved_memory'] = [{'name': n, 'base': hex(b), 'size': hex(s), 'end': hex(b + s)} for n, b, s in fixed]
    # stock coverage: every stock fixed region must be covered by our fixed regions
    def covered(b, s):
        pos = b
        for n, fb, fs in fixed:
            if fb <= pos < fb + fs:
                pos = fb + fs
            if pos >= b + s:
                return True
        return pos >= b + s
    missing = [(n, hex(b), hex(s)) for n, b, s in STOCK['reserved'] if not covered(b, s)]
    check('every stock fixed reserved region is covered by the merged tree', not missing, str(missing))
    extra = [(n, hex(b), hex(s)) for n, b, s in fixed if not any(sb <= b and b + s <= sb + ss for _, sb, ss in STOCK['reserved'])]
    rep['fixed_regions_not_in_stock'] = extra
    adsp_conflicts = [n for n, b, s in fixed if n != mrn.name and b < 0x92a00000 + 0x1e00000 and 0x92a00000 < b + s]
    check('ADSP region has no other fixed claimant', not adsp_conflicts, str(adsp_conflicts))
    # sibling remoteprocs stay disabled
    for lab in ('remoteproc_mss', 'remoteproc_cdsp'):
        n = f.symbol(lab)
        check(f'{lab} remains disabled', n is not None and n.strings('status') == ['disabled'])
    # ---- simulate overlay ------------------------------------------------
    g = FDT(data)
    ga = g.symbol('adsp_pil')
    ga.set('firmware-name', fw_name.encode() + b'\0')
    ga.set('status', b'okay\0')
    ch = g.node('/chosen')
    ch.set('hisense,a6l-adsp', marker.encode() + b'\0')
    out = g.to_bytes()
    cand = os.path.join(outdir, 'candidate-adsp-merged.dtb')
    open(cand, 'wb').write(out)
    d = diff(f, g)
    rep['candidate_dtb'] = {'path': cand, 'sha256': sha256(out), 'bytes': len(out)}
    rep['overlay_effect_diff'] = d
    expected = {'/soc@0/remoteproc@15700000/status', '/soc@0/remoteproc@15700000/firmware-name', '/chosen/hisense,a6l-adsp'}
    check('overlay scope is exactly 3 properties', set(d) == expected, str(sorted(d)))
    g2 = FDT(out)
    check('candidate re-parses; adsp_pil okay + firmware-name', g2.symbol('adsp_pil').strings('status') == ['okay'] and g2.symbol('adsp_pil').strings('firmware-name') == [fw_name])
    json.dump(rep, open(os.path.join(outdir, 'adsp-dt-audit.json'), 'w'), indent=1)
    print(json.dumps({'passed': rep['passed'], 'failed': [c for c in rep['checks'] if not c['ok']], 'diff_keys': sorted(d)}, indent=1))


if __name__ == '__main__':
    main()
