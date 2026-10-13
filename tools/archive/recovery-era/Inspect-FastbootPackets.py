#!/usr/bin/env python3
"""Read fixed fastboot variables over one USB session, record packets, then reboot."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import time

SERIAL = '1e529013'
VARIABLES = ('product', 'product', 'product', 'version-bootloader', 'unlocked',
             'secure', 'max-download-size', 'partition-size:recovery',
             'partition-size:boot', 'partition-size:system', 'has-slot:boot')
ALLOWED = frozenset('getvar:' + name for name in VARIABLES) | {'reboot'}


def encode_command(command):
    if command not in ALLOWED:
        raise ValueError('Command is outside the fixed read-only/reboot allowlist')
    encoded = command.encode('ascii')
    if not 0 < len(encoded) <= 64:
        raise ValueError('Invalid fastboot command length')
    return encoded


def parse_packet(packet):
    if not 4 <= len(packet) <= 64:
        raise ValueError('Invalid fastboot status packet length')
    status = packet[:4]
    if status not in (b'INFO', b'OKAY', b'FAIL'):
        raise ValueError('Unexpected status; this tool never enters a download phase')
    return status.decode('ascii'), packet[4:].rstrip(b'\0').decode('ascii', errors='replace')


def main():
    import usb.core
    import usb.util

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    report = {'scope': 'Fixed getvar commands and reboot only; no image download or partition writes',
              'started_utc': datetime.now(timezone.utc).isoformat(), 'serial': SERIAL,
              'commands': [], 'single_usb_session': True}

    def save():
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    save()
    matches = []
    for dev in usb.core.find(find_all=True, idVendor=0x18d1, idProduct=0xd00d):
        if usb.util.get_string(dev, dev.iSerialNumber).lower() == SERIAL:
            matches.append(dev)
    if len(matches) != 1:
        raise SystemExit('Expected exactly the spare fastboot USB identity; no command sent')
    dev = matches[0]
    config = dev.get_active_configuration()
    interfaces = [i for i in config if (i.bInterfaceClass, i.bInterfaceSubClass,
                                       i.bInterfaceProtocol) == (0xff, 0x42, 3)
                  and i.bAlternateSetting == 0]
    if len(interfaces) != 1:
        raise SystemExit('Expected exactly one fastboot interface; no command sent')
    interface = interfaces[0]
    incoming = [e for e in interface if usb.util.endpoint_type(e.bmAttributes) == 2
                and usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN]
    outgoing = [e for e in interface if usb.util.endpoint_type(e.bmAttributes) == 2
                and usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT]
    if len(incoming) != 1 or len(outgoing) != 1:
        raise SystemExit('Unexpected bulk endpoints; no command sent')
    inp, out = incoming[0], outgoing[0]
    report['usb'] = {'bus': dev.bus, 'address': dev.address,
                     'interface': interface.bInterfaceNumber,
                     'in_endpoint': inp.bEndpointAddress, 'out_endpoint': out.bEndpointAddress,
                     'in_max_packet': inp.wMaxPacketSize, 'out_max_packet': out.wMaxPacketSize}
    save()

    def exchange(command, check_extra=True):
        payload = encode_command(command)
        entry = {'command': command, 'request_hex': payload.hex(), 'packets': [],
                 'command_succeeded': False}
        report['commands'].append(entry)
        save()
        try:
            count = out.write(payload, timeout=3000)
            entry['bytes_written'] = count
            if count != len(payload):
                raise RuntimeError('Short command write')
            for _ in range(32):
                packet = bytes(inp.read(64, timeout=3000))
                entry['packets'].append({'hex': packet.hex()})
                status, message = parse_packet(packet)
                entry['packets'][-1].update(status=status, message=message)
                save()
                if status != 'INFO':
                    entry['command_succeeded'] = status == 'OKAY'
                    entry['terminal_status'] = status
                    break
            else:
                raise RuntimeError('Too many INFO packets')
            if check_extra:
                try:
                    extra = bytes(inp.read(64, timeout=150))
                except usb.core.USBTimeoutError:
                    pass
                else:
                    entry['unexpected_extra_packet_hex'] = extra.hex()
                    raise RuntimeError('Extra response after terminal status; stop to inspect framing')
            return entry
        except Exception as exc:
            entry['error'] = str(exc)
            raise
        finally:
            save()

    claimed = False
    try:
        if dev.is_kernel_driver_active(interface.bInterfaceNumber):
            raise RuntimeError('Interface already has a kernel driver; refusing to detach it')
        usb.util.claim_interface(dev, interface.bInterfaceNumber)
        claimed = True
        for variable in VARIABLES:
            exchange('getvar:' + variable)
            time.sleep(0.1)
    except Exception as exc:
        report['query_error'] = str(exc)
    finally:
        if claimed:
            try:
                report['reboot'] = exchange('reboot', check_extra=False)
            except Exception as exc:
                report['reboot_error'] = str(exc)
            finally:
                try:
                    usb.util.release_interface(dev, interface.bInterfaceNumber)
                except usb.core.USBError as exc:
                    report['release_note'] = str(exc)
        try:
            usb.util.dispose_resources(dev)
        except usb.core.USBError as exc:
            report['dispose_note'] = str(exc)
        # The standard client's reboot worked in the prior physical inspection.
        if claimed and not report.get('reboot', {}).get('command_succeeded'):
            try:
                fallback = subprocess.run(['/usr/bin/fastboot', '-s', SERIAL, 'reboot'],
                                          capture_output=True, text=True, timeout=8)
                report['fallback_reboot'] = {'exit': fallback.returncode,
                                             'stdout': fallback.stdout, 'stderr': fallback.stderr}
            except subprocess.TimeoutExpired:
                report['fallback_reboot'] = {'timeout_seconds': 8}
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    print(json.dumps(report, indent=2))
    return 0 if 'query_error' not in report and report.get('reboot', {}).get('command_succeeded') else 1


if __name__ == '__main__':
    raise SystemExit(main())
