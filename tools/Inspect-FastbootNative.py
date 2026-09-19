#!/usr/bin/env python3
"""Run fixed standard-client queries with a passive, spare-filtered usbmon trace."""
import argparse
import hashlib
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import select
import subprocess
import threading
import time

SERIAL = '1e529013'
GOOGLE_SHA256 = 'a686e2c7e8dc9cf4cba0cb8a2eef05f7b2bd682c925abd032fe203215d80b618'
RAM_PAYLOADS = [
    ('ram-transfer-1m.bin', 1048576, '001aa6d3c0145f608ccb74ebd69f9e4cf46e7ec355af42c665b00ea47c29510a'),
    ('recovery-diagnostic-unsigned.img', 67108864, 'ff3c54525d12a7f5d479fdc36cfd6ab0e6e103abcb9e95d398bac9ca58a27a7b'),
]


def verify_ram_payloads(directory):
    if not directory.is_absolute() or not directory.is_dir() or directory.is_symlink():
        raise ValueError('RAM payload directory must be an existing absolute non-symlink directory')
    entries = []
    for name, size, expected_hash in RAM_PAYLOADS:
        path = directory / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size != size:
            raise ValueError('RAM payload has unexpected type or size: ' + name)
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest() if hasattr(hashlib, 'file_digest') else hashlib.sha256(stream.read()).hexdigest()
        if digest != expected_hash:
            raise ValueError('RAM payload hash differs: ' + name)
        entries.append({'path': str(path), 'bytes': size, 'sha256': digest})
    return entries


def ram_command_allowed(arguments, payloads):
    if arguments == ['reboot']:
        return True
    if arguments in [['oem', 'device-info'], ['flashing', 'get_unlock_ability']]:
        return True
    if len(arguments) == 2 and arguments[0] == 'stage':
        return arguments[1] in {p['path'] for p in payloads}
    variables = {'product', 'unlocked', 'secure', 'max-download-size',
                 'partition-size:recovery', 'partition-size:boot'}
    return (bool(arguments) and len(arguments) % 2 == 0
            and all(arguments[n] == 'getvar' and arguments[n + 1] in variables
                    for n in range(0, len(arguments), 2)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--paced', action='store_true', help='Wait one second between separate query processes')
    parser.add_argument('--padded', action='store_true', help='Use the tested source-built 64-byte query experiment')
    parser.add_argument('--all-once', action='store_true', help='Issue a single standard getvar all query')
    parser.add_argument('--ram-staging-dir', type=Path, help='Stage only the two hash-pinned diagnostic payloads in RAM')
    parser.add_argument('--fastboot', type=Path, default=Path('/usr/bin/fastboot'),
                        help='Absolute path to the standard Linux fastboot client')
    args = parser.parse_args()
    if not args.fastboot.is_absolute() or not args.fastboot.is_file():
        parser.error('--fastboot must identify an existing absolute file')
    if args.all_once and (args.paced or args.padded):
        parser.error('--all-once is a separate standard-client experiment')
    payloads = []
    if args.ram_staging_dir:
        if args.all_once or args.paced or args.padded:
            parser.error('RAM staging cannot be combined with earlier query experiments')
        if hashlib.sha256(args.fastboot.read_bytes()).hexdigest() != GOOGLE_SHA256:
            parser.error('RAM staging requires the verified Google 37.0.1 client')
        payloads = verify_ram_payloads(args.ram_staging_dir)
    args.output.mkdir(parents=True, exist_ok=False)
    report = {'scope': 'Standard fastboot fixed queries and reboot; passive USB trace',
              'serial': SERIAL, 'started_utc': datetime.now(timezone.utc).isoformat(),
              'commands': []}
    report['query_spacing_seconds'] = 1 if args.paced else 0
    if payloads:
        report['scope'] = 'Hash-pinned RAM staging, fixed read-only queries and reboot; no flash, erase, unlock or execution'
        report['ram_payloads'] = payloads
        report['ram_transfers_succeeded'] = False
    standard_binary = str(args.fastboot)
    query_binary = standard_binary
    query_env = dict(os.environ)
    query_env.pop('A6L_QUERY_PAD64', None)
    if args.padded:
        build_log = Path('/mnt/c/Users/Pierre/Desktop/A6L/logs/build-fastboot-query.log')
        if 'A6L_FASTBOOT_QUERY_BUILD_AND_TEST_SUCCESS' not in build_log.read_text():
            raise SystemExit('Source-built query client has not passed its tests; no phone command sent')
        query_binary = '/home/a6l/android/a6l-lineage24/out/host/linux-x86/bin/fastboot'
        query_env['A6L_QUERY_PAD64'] = '1'
    report['query_client'] = {'path': query_binary,
                              'sha256': hashlib.sha256(Path(query_binary).read_bytes()).hexdigest(),
                              'padding_to_64_bytes': args.padded}

    def save():
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    save()
    found = []
    for device in Path('/sys/bus/usb/devices').iterdir():
        try:
            if ((device / 'idVendor').read_text().strip() == '18d1'
                    and (device / 'idProduct').read_text().strip() == 'd00d'
                    and (device / 'serial').read_text().strip().lower() == SERIAL):
                found.append(device)
        except (OSError, UnicodeError):
            continue
    if len(found) != 1:
        raise SystemExit('Exact spare bootloader is not present; no command sent')
    device = found[0]
    bus = int((device / 'busnum').read_text())
    address = int((device / 'devnum').read_text())
    report['usb'] = {'sysfs': str(device), 'bus': bus, 'address': address}
    if payloads and not int((device / 'quirks').read_text().strip(), 0) & 0x400:
        raise SystemExit('RAM staging requires the tested NO_LPM host workaround')
    # Open a monitor stream, not the device. Descriptor lookup above is cached sysfs data.
    monitor = os.open(f'/sys/kernel/debug/usb/usbmon/{bus}u', os.O_RDONLY | os.O_NONBLOCK)
    stop = threading.Event()
    trace = []
    errors = []

    def reader():
        partial = b''
        deadline = time.monotonic() + (180 if payloads else 55)
        try:
            while not stop.is_set() and time.monotonic() < deadline:
                if not select.select([monitor], [], [], 0.1)[0]:
                    continue
                try:
                    chunk = os.read(monitor, 65536)
                except BlockingIOError:
                    # usbmon readiness can race with an empty nonblocking read.
                    continue
                if not chunk:
                    break
                partial += chunk
                while b'\n' in partial:
                    raw, partial = partial.split(b'\n', 1)
                    line = raw.decode('ascii', errors='replace')
                    parts = line.split()
                    pipe = parts[3].split(':') if len(parts) > 3 else []
                    if len(pipe) == 4 and int(pipe[1]) == bus and int(pipe[2]) == address:
                        trace.append(line)
                        if len(trace) >= (32768 if payloads else 2000):
                            raise RuntimeError('Trace limit reached')
        except Exception as exc:
            errors.append(str(exc))

    worker = threading.Thread(target=reader, daemon=True)
    worker.start()

    def run(arguments, timeout=12):
        if payloads and not ram_command_allowed(arguments, payloads):
            raise RuntimeError('RAM test rejects commands outside its fixed allowlist')
        entry = {'arguments': arguments, 'started_utc': datetime.now(timezone.utc).isoformat()}
        report['commands'].append(entry)
        save()
        try:
            # Return always uses the standard client, regardless of query experiment.
            binary = standard_binary if arguments == ['reboot'] else query_binary
            environment = dict(query_env)
            if arguments == ['reboot']:
                environment.pop('A6L_QUERY_PAD64', None)
            entry['client'] = binary
            result = subprocess.run([binary, '-s', SERIAL, *arguments], env=environment,
                                    capture_output=True, text=True, timeout=timeout)
            entry.update(exit=result.returncode, stdout=result.stdout, stderr=result.stderr)
            entry['command_succeeded'] = result.returncode == 0 and 'FAILED' not in result.stderr
        except subprocess.TimeoutExpired as exc:
            entry.update(exit=None, timeout=timeout, command_succeeded=False,
                         stdout=(exc.stdout or b'').decode(errors='replace'),
                         stderr=(exc.stderr or b'').decode(errors='replace'))
        except OSError as exc:
            entry.update(exit=None, command_succeeded=False, error=str(exc), stderr='')
        save()
        return entry

    try:
        # Keep one standard-client process open for the repeated requests.
        if args.all_once:
            report['queries_succeeded'] = run(['getvar', 'all'], timeout=20)['command_succeeded']
            repeated_ok = False
        elif args.paced:
            repeated_results = []
            for _ in range(3):
                repeated_results.append(run(['getvar', 'product']))
                if not repeated_results[-1]['command_succeeded']:
                    break
                time.sleep(1)
            repeated_ok = len(repeated_results) == 3 and all(r['command_succeeded'] for r in repeated_results)
        else:
            repeated_ok = run(['getvar', 'product', 'getvar', 'product', 'getvar', 'product'])['command_succeeded']
        if repeated_ok:
            values = ['unlocked', 'secure', 'max-download-size',
                      'partition-size:recovery', 'partition-size:boot']
            if args.paced:
                query_results = []
                for value in values:
                    query_results.append(run(['getvar', value]))
                    if not query_results[-1]['command_succeeded']:
                        break
                    time.sleep(1)
                report['queries_succeeded'] = (len(query_results) == len(values)
                                               and all(r['command_succeeded'] for r in query_results))
            else:
                report['queries_succeeded'] = run(
                    [argument for value in values for argument in ['getvar', value]])['command_succeeded']
        elif not args.all_once:
            report['queries_succeeded'] = False
            report['queries_stopped'] = 'Repeated product queries failed; inspect trace first'
        if payloads and report.get('queries_succeeded'):
            transfers = []
            for payload in payloads:
                # Recheck immediately before use; never issue a subsequent flash
                # or boot command, regardless of success or failure.
                verify_ram_payloads(args.ram_staging_dir)
                entry = run(['stage', payload['path']], timeout=45)
                transfers.append(entry)
                if not entry['command_succeeded']:
                    break
            report['ram_transfers_succeeded'] = len(transfers) == len(payloads) and all(e['command_succeeded'] for e in transfers)
            save()
            if report['ram_transfers_succeeded']:
                # Exercise idle then resume as well as the continuous transfer.
                time.sleep(5)
                health = run(['getvar', 'product', 'getvar', 'unlocked', 'getvar', 'secure'])
                report['post_transfer_queries_succeeded'] = health['command_succeeded']
                if health['command_succeeded']:
                    report['device_info'] = run(['oem', 'device-info'])
                    report['unlock_ability'] = run(['flashing', 'get_unlock_ability'])
                save()
    finally:
        report['reboot'] = run(['reboot'], timeout=10)
        # This exact failure also affects reboot. Retry that reversible command once,
        # only following an explicit remote rejection (never after an ambiguous timeout).
        if (not report['reboot']['command_succeeded']
                and "remote: 'unknown command'" in report['reboot'].get('stderr', '')):
            report['reboot_first_attempt'] = report['reboot']
            time.sleep(0.5)
            report['reboot'] = run(['reboot'], timeout=10)
        time.sleep(0.2)
        stop.set()
        worker.join(timeout=2)
        os.close(monitor)
        (args.output / 'usbmon.txt').write_text('\n'.join(trace) + '\n')
        decoded = []
        for line in trace:
            parts = line.split()
            if len(parts) > 6 and parts[3].startswith(('Bo:', 'Bi:')) and '=' in parts:
                try:
                    payload = bytes.fromhex(''.join(parts[parts.index('=') + 1:]))
                    decoded.append({'event': parts[2], 'pipe': parts[3], 'status': parts[4],
                                    'reported_length': parts[5], 'captured_hex': payload.hex(),
                                    'ascii': payload.decode('ascii', errors='replace')})
                except ValueError:
                    continue
        (args.output / 'usbmon-decoded.json').write_text(json.dumps(decoded, indent=2) + '\n')
        report.update(trace_lines=len(trace), trace_errors=errors,
                      finished_utc=datetime.now(timezone.utc).isoformat())
        save()
    print(json.dumps(report, indent=2))
    success = report.get('queries_succeeded') and report['reboot']['command_succeeded']
    if payloads:
        success = success and report['ram_transfers_succeeded'] and report.get('post_transfer_queries_succeeded')
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())
