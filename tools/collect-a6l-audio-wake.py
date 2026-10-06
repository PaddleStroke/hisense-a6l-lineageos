#!/usr/bin/env python3
"""Read-only Android diagnostics; run on laptop while the A6L is awake.

Run once idle and once while playing the failing recording. This script does
not play audio, change volume, wake/suspend, restart services or reboot.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import datetime
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--pcm-watch-seconds', type=int, default=12,
                    help='Read PCM status every half second concurrently with other dumps (0–30 seconds)')
args = parser.parse_args()
if not 0 <= args.pcm_watch_seconds <= 30:
    parser.error('--pcm-watch-seconds must be between 0 and 30')
serial = '1e529013'
adb = ['adb', '-s', serial]

def run(command, timeout=20):
    try:
        result = subprocess.run(adb + command, capture_output=True, timeout=timeout)
        return result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired as error:
        return 124, error.stdout or b'', error.stderr or b''

rc, identity, error = run(['shell', 'getprop ro.product.device; getprop ro.vendor.a6l.rom.build; cat /proc/sys/kernel/random/boot_id'])
assert rc == 0 and b'a6l' in identity.lower(), (rc, identity, error)
assert not args.output.exists(), 'Use a fresh output directory'
args.output.mkdir(parents=True)
report = {'serial': serial, 'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'identity': identity.decode(errors='replace'), 'read_only': True, 'commands': {}}
# A short recording can finish before sequential dumpsys calls reach PCM status.
# Keep a bounded independent status reader running while those dumps are taken.
watch_pool = ThreadPoolExecutor(max_workers=1)
watch_future = None
if args.pcm_watch_seconds:
    watch_command = ('n=0; while [ "$n" -lt ' + str(args.pcm_watch_seconds * 2) + ' ]; do '
                     'cat /proc/uptime; '
                     'for p in /proc/asound/card*/pcm*/sub*/status; do '
                     'echo "$p"; cat "$p"; done; '
                     'n=$((n + 1)); sleep 0.5; done')
    watch_future = watch_pool.submit(run, ['shell', watch_command], args.pcm_watch_seconds + 8)
commands = {
    'pcm': 'cat /proc/asound/cards /proc/asound/pcm; for p in /proc/asound/card*/pcm*/sub*/status /proc/asound/card*/pcm*/sub*/hw_params; do echo "$p"; cat "$p"; done',
    'mixer': 'if command -v tinymix >/dev/null 2>&1; then tinymix -D 0; else echo "tinymix unavailable"; fi',
    'audio': 'dumpsys audio',
    'audioflinger': 'dumpsys media.audio_flinger',
    'audiopolicy': 'dumpsys media.audio_policy',
    'power': 'dumpsys power',
    'display': 'dumpsys display',
    'audio-properties': 'getprop | grep -E "a6l.audio|a6l.voice|a6l.btcall|dualux|eink"',
    'kmsg': 'dmesg',
    'logcat': 'logcat -b all -d -t 3000',
    'recording-files': 'find /sdcard/Recordings /sdcard/Recorder /sdcard/Music/Recordings -type f 2>/dev/null',
}
for name, command in commands.items():
    rc, stdout, stderr = run(['shell', command])
    (args.output / (name + '.txt')).write_bytes(stdout[:8 * 1024 * 1024])
    (args.output / (name + '.stderr')).write_bytes(stderr[:65536])
    report['commands'][name] = {'exit': rc, 'bytes': len(stdout), 'truncated': len(stdout) > 8 * 1024 * 1024}
if watch_future is not None:
    rc, stdout, stderr = watch_future.result()
    (args.output / 'pcm-watch.txt').write_bytes(stdout[:512 * 1024])
    (args.output / 'pcm-watch.stderr').write_bytes(stderr[:65536])
    report['commands']['pcm-watch'] = {'exit': rc, 'bytes': len(stdout),
                                     'truncated': len(stdout) > 512 * 1024,
                                     'seconds': args.pcm_watch_seconds}
watch_pool.shutdown()
rc, identity_after, error = run(['shell', 'cat /proc/sys/kernel/random/boot_id'])
report['boot_id_after'] = identity_after.decode(errors='replace').strip()
report['same_boot'] = rc == 0 and report['boot_id_after'] in report['identity'].splitlines()
report['finished_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
(args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'output': str(args.output), 'same_boot': report['same_boot'],
                  'failed_commands': [name for name, result in report['commands'].items() if result['exit']]}))
