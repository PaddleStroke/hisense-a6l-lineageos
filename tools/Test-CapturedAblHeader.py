#!/usr/bin/env python3
"""Execute the captured A6L image-header checker offline, with fault controls."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('recovery_exit', ROOT / 'tools/Inspect-RecoveryExit.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
data = helper.pinned(ROOT / 'firmware/extracted/stock-abl-20260914/LinuxLoader.efi',
                     '6bbe53f345729c77c9a0ba74aa7f7aadb6cd68b0ac1511f6f160bc69de6ab523')
base, mapped, sections = helper.map_pe(data)
assert base == 0


def check(header):
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.mem_map(0, (len(mapped) + 4095) & ~4095)
    cpu.mem_write(0, bytes(mapped))
    cpu.mem_map(0x1000000, 0x10000)
    cpu.mem_map(0x2000000, 0x10000)
    cpu.mem_write(0x1000000, bytes(header[:4096]))
    cpu.reg_write(UC_ARM64_REG_SP, 0x200fff0)
    cpu.reg_write(UC_ARM64_REG_LR, 0x3000000)
    for reg, value in [(UC_ARM64_REG_X0, 0x1000000), (UC_ARM64_REG_X1, 4096),
                       (UC_ARM64_REG_X2, 0x1008000), (UC_ARM64_REG_X3, 0x1008004),
                       (UC_ARM64_REG_X4, 1)]:
        cpu.reg_write(reg, value)
    calls = []

    def hook(uc, address, size, context):
        if address == 0x29f8:  # DebugPrintEnabled: no effect on validation.
            uc.reg_write(UC_ARM64_REG_X0, 0)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
        elif not (0x13f78 <= address < 0x146a8 or 0x35a30 <= address < 0x35b70
                  or 0x2888 <= address < 0x28c8 or 0x2138 <= address < 0x219c):
            calls.append(hex(address))
            raise RuntimeError('Unexpected instruction outside header checker/memory comparison: ' + hex(address))

    cpu.hook_add(UC_HOOK_CODE, hook)
    cpu.emu_start(0x13f78, 0x3000000, timeout=2000000, count=100000)
    assert cpu.reg_read(UC_ARM64_REG_PC) == 0x3000000, 'Checker did not return before its bound'
    image_size, page_size = struct.unpack('<II', cpu.mem_read(0x1008000, 8))
    return {'status': hex(cpu.reg_read(UC_ARM64_REG_X0)), 'image_size': image_size,
            'page_size': page_size, 'unexpected_calls': calls}


def main():
    directory = ROOT / 'firmware/extracted/recovery-probe-20260914'
    results = {}
    for name in ['restore-stock-recovery.img', 'recovery-diagnostic-unsigned.img']:
        payload = (directory / name).read_bytes()
        result = check(payload[:4096])
        assert result['status'] == '0x0', (name, result)
        result['image_sha256'] = hashlib.sha256(payload).hexdigest()
        results[name] = result
    controls = {}
    for name, offset, replacement in [('bad_magic', 0, b'FAILFAIL'),
                                     ('zero_kernel', 8, struct.pack('<I', 0)),
                                     ('oversized_page', 36, struct.pack('<I', 8192)),
                                     ('wrong_v1_header_size', 1644, struct.pack('<I', 1647))]:
        header = bytearray(payload[:4096])
        header[offset:offset + len(replacement)] = replacement
        result = check(header)
        assert result['status'] != '0x0', (name, result)
        controls[name] = result
    report = {'scope': 'Exact captured ABL header-check instructions in Unicorn; no phone access',
              'checker_rva': '0x13f78', 'hook': 'DebugPrintEnabled returns false; validation code and memory comparison execute unchanged',
              'images': results, 'negative_controls': controls, 'passed': True,
              'limits': 'Does not emulate AVB, DTB selection/fixups, decompression or kernel execution'}
    (ROOT / 'firmware/extracted/diagnostic-boot-v1-20260915/header-emulation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
