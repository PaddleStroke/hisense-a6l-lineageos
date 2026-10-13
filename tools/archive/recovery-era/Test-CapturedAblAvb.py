#!/usr/bin/env python3
"""Run the captured ABL libavb verifier with read-only in-memory partition fixtures.

This emulates verification, not UEFI board setup or the physical boot chain.
No phone or USB access. All storage callbacks use verified local backup bytes.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('exit_helper', ROOT / 'tools/Inspect-RecoveryExit.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
abl = helper.pinned(ROOT / 'firmware/extracted/stock-abl-20260914/LinuxLoader.efi',
                    '6bbe53f345729c77c9a0ba74aa7f7aadb6cd68b0ac1511f6f160bc69de6ab523')
_, mapped, _ = helper.map_pe(abl)


def verify(partitions, trusted_key=False):
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.mem_map(0, (len(mapped) + 4095) & ~4095)
    cpu.mem_write(0, bytes(mapped))
    cpu.mem_map(0x2000000, 0x20000)
    cpu.mem_map(0x3000000, 0x20000)
    cpu.mem_map(0x4000000, 0x10000000)
    cpu.mem_map(0x18000000, 0x10000)
    cpu.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
    cpu.reg_write(UC_ARM64_REG_SP, 0x301fff0)
    cpu.reg_write(UC_ARM64_REG_LR, 0x1800fff0)
    cpu.mem_write(0x2000000, struct.pack('<Q', 0x201fff0))  # SafeStack TLS pointer.
    ops, requested, suffix, out = 0x3001000, 0x3002000, 0x3003000, 0x3004000
    cpu.mem_write(0x3005000, b'recovery\0')
    cpu.mem_write(requested, struct.pack('<QQ', 0x3005000, 0))
    functions = {0x18: 'read', 0x20: 'write', 0x28: 'public_key', 0x30: 'read_rollback',
                 0x38: 'write_rollback', 0x40: 'unlocked', 0x48: 'guid', 0x50: 'size'}
    callbacks = {}
    for index, (offset, name) in enumerate(functions.items()):
        address = 0x18000000 + index * 16
        callbacks[address] = name
        cpu.mem_write(ops + offset, struct.pack('<Q', address))
    for register, value in [(UC_ARM64_REG_X0, ops), (UC_ARM64_REG_X1, requested),
                            (UC_ARM64_REG_X2, suffix), (UC_ARM64_REG_X3, 1),
                            (UC_ARM64_REG_X4, 0), (UC_ARM64_REG_X5, out)]:
        cpu.reg_write(register, value)
    heap = 0x4000000
    events, logs, trace = [], [], []
    sha256 = {}

    def reg(index):
        return cpu.reg_read([UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                             UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5][index])

    def cstring(address, limit=2048):
        if not address:
            return ''
        data = bytearray()
        for offset in range(limit):
            byte = cpu.mem_read(address + offset, 1)[0]
            if not byte:
                return data.decode(errors='replace')
            data.append(byte)
        raise ValueError('Unterminated emulated string')

    def ret(value=0):
        cpu.reg_write(UC_ARM64_REG_X0, value & ((1 << 64) - 1))
        cpu.reg_write(UC_ARM64_REG_PC, cpu.reg_read(UC_ARM64_REG_LR))

    def hook(uc, address, size, context):
        nonlocal heap
        trace.append([hex(address), hex(reg(0)), hex(cpu.reg_read(UC_ARM64_REG_X23))])
        if len(trace) > 60:
            trace.pop(0)
        if address in callbacks:
            name = callbacks[address]
            event = {'callback': name}
            events.append(event)
            if name == 'read':
                partition = cstring(reg(1))
                offset, count = reg(2), reg(3)
                if offset >= 1 << 63:
                    offset -= 1 << 64
                event.update(partition=partition, offset=offset, bytes=count)
                if partition not in partitions:
                    ret(3)
                    return
                data = partitions[partition]
                if offset < 0:
                    offset += len(data)
                if not 0 <= offset <= len(data):
                    ret(4)
                    return
                chunk = data[offset:offset + count]
                if chunk:
                    uc.mem_write(reg(4), chunk)
                uc.mem_write(reg(5), struct.pack('<Q', len(chunk)))
                ret()
            elif name == 'size':
                partition = cstring(reg(1))
                event['partition'] = partition
                if partition not in partitions:
                    ret(3)
                    return
                uc.mem_write(reg(2), struct.pack('<Q', len(partitions[partition])))
                ret()
            elif name == 'read_rollback':
                uc.mem_write(reg(2), struct.pack('<Q', 0))
                ret()
            elif name == 'public_key':
                uc.mem_write(reg(5), bytes([trusted_key]))
                ret()
            elif name == 'unlocked':
                uc.mem_write(reg(1), b'\1')
                ret()
            elif name == 'guid':
                event['partition'] = cstring(reg(1))
                value = b'11111111-2222-3333-4444-555555555555\0'
                assert reg(3) >= len(value)
                uc.mem_write(reg(2), value)
                ret()
            else:
                raise RuntimeError('Unexpected storage mutation requested by emulated verifier: ' + name)
        elif address == 0x1474:
            ret(0x2000000)
        elif address == 0x2008:  # CopyMem: faithful host memory copy fixture.
            destination, source, count = reg(0), reg(1), reg(2)
            if count:
                cpu.mem_write(destination, bytes(cpu.mem_read(source, count)))
            ret(destination)
        elif address == 0x35b74:  # SetMem(destination, length, value).
            destination, count, value = reg(0), reg(1), reg(2) & 255
            assert count < 0x8000000
            if count:
                cpu.mem_write(destination, bytes([value]) * count)
            ret(destination)
        elif address == 0x7ff4:  # SHA-256 firmware service wrapper.
            sha256[reg(0)] = hashlib.sha256()
            ret()
        elif address == 0x810c:
            sha256[reg(0)].update(bytes(cpu.mem_read(reg(1), reg(2))))
            ret()
        elif address == 0x81b4:
            digest_address = reg(0) + 0xa8
            cpu.mem_write(digest_address, sha256.pop(reg(0)).digest())
            ret(digest_address)
        elif address in (0xb884, 0xb8d0):  # avb_malloc / avb_calloc.
            count = reg(0)
            assert 0 < count < 0x8000000
            pointer = heap
            heap += (count + 15) & ~15
            assert heap < 0x14000000
            ret(pointer)
        elif address == 0xb504:  # avb_free: arena fixture lives for this call.
            ret()
        elif address == 0xb5fc:
            ret(reg(0))  # avb_basename; retaining the full name only affects logs.
        elif address == 0xb410:
            fragments = []
            for i in range(6):
                if not reg(i):
                    break
                try:
                    fragments.append(cstring(reg(i)))
                except Exception:
                    fragments.append(hex(reg(i)))
            logs.append(''.join(fragments))
            ret()
        elif address == 0x29f8:
            ret()
        elif address in (0xb39c, 0x10e40, 0x28cc):
            raise RuntimeError('Emulated verifier assert/abort: ' + hex(address))

    cpu.hook_add(UC_HOOK_CODE, hook)
    result = {}
    try:
        cpu.emu_start(0x8948, 0x1800fff0, timeout=30000000, count=30000000)
        result.update(returned=cpu.reg_read(UC_ARM64_REG_PC) == 0x1800fff0,
                      result=cpu.reg_read(UC_ARM64_REG_X0))
        slot = struct.unpack('<Q', cpu.mem_read(out, 8))[0]
        result['slot_data_present'] = bool(slot)
        if slot:
            loaded, count = struct.unpack('<QQ', cpu.mem_read(slot + 0x18, 16))
            result['loaded_partitions'] = []
            assert count <= 32
            for i in range(count):
                name, data, size = struct.unpack('<QQQ', cpu.mem_read(loaded + i * 24, 24))
                result['loaded_partitions'].append({'name': cstring(name), 'bytes': size,
                    'sha256': hashlib.sha256(bytes(cpu.mem_read(data, size))).hexdigest()})
    except Exception as error:
        result['error'] = str(error)
        result['pc'] = hex(cpu.reg_read(UC_ARM64_REG_PC))
        result['ops_hex'] = bytes(cpu.mem_read(ops, 0x58)).hex()
        result['x23'] = hex(cpu.reg_read(UC_ARM64_REG_X23))
        result['trace'] = trace
    result.update(callbacks=events, log_fragments=logs,
                  fixtures={'unlocked': True, 'stored_rollback_indices': 0,
                            'public_key_accepted': trusted_key, 'guid': 'Fixed placeholder for command-line construction',
                            'sha256': 'Firmware service wrappers use Python hashlib; byte inputs/digests are unchanged',
                            'host_primitives': 'Arena allocation/free, memory copy/set, logging/basename and SafeStack pointer'})
    return result


def main():
    backup = ROOT / 'firmware/raw-backup-20260914'
    manifest = json.loads((backup / 'firmware-verification.json').read_text())
    part = next(p for p in manifest['partitions'] if p['name'] == 'vbmeta')
    with (backup / 'emmc-firmware-prefix.bin').open('rb') as f:
        f.seek(part['offset'])
        vbmeta = f.read(part['bytes'])
    assert hashlib.sha256(vbmeta).hexdigest() == part['sha256']
    results = {'scope': 'Offline captured libavb instruction emulation with verified backup fixtures',
               'vbmeta_sha256': part['sha256'], 'images': {}}
    for name in ['restore-stock-recovery.img', 'recovery-diagnostic-unsigned.img']:
        image = (ROOT / 'firmware/extracted/recovery-probe-20260914' / name).read_bytes()
        result = verify({'vbmeta': vbmeta, 'recovery': image})
        assert result.get('returned') and result.get('result') == 5 and not result.get('error'), result
        assert result['loaded_partitions'] == [{'name': 'recovery', 'bytes': len(image),
                                                'sha256': hashlib.sha256(image).hexdigest()}]
        results['images'][name] = result
    broken = b'FAIL' + vbmeta[4:]
    control = verify({'vbmeta': broken, 'recovery': image})
    assert control.get('returned') and control.get('result') == 6 and not control.get('slot_data_present'), control
    results['invalid_vbmeta_control'] = control
    results['passed'] = True
    results['interpretation'] = 'The exact verifier loads stock and diagnostic recovery with the captured vbmeta. Result 5 (untrusted key) is in the exact unlocked continue mask 0x39 at RVA 0x62d8. Invalid metadata returns 6 without slot data.'
    results['limits'] = 'Uses captured vbmeta, not a fresh post-trial vbmeta read; does not emulate the rest of UEFI or hardware. The missing recovery footer is not a blocker under this verified fixture.'
    path = ROOT / 'firmware/extracted/diagnostic-boot-v1-20260915/avb-emulation.json'
    path.write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
