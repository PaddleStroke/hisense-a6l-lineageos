#!/usr/bin/env python3
"""Exercise sustained floods, restarts, idle flush, torn slots and allocation failure."""
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('decoder', ROOT / 'tools/decode-a6l-log-ring.py')
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)
CHUNK = 32768
SIZE = 32 + 20 * (24 + CHUNK)


def main():
    with tempfile.TemporaryDirectory(prefix='a6l-ring-test-') as tmp:
        tmp = Path(tmp)
        exe = tmp / 'writer'
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(ROOT / 'device/hisense/a6l/rom/debug/a6l-log-ring.c'), '-o', str(exe)], check=True)
        def flood(name):
            path = tmp / name
            p = subprocess.Popen([str(exe), str(path)], stdin=subprocess.PIPE)
            for i in range(1024):  # 32 MiB each; ~50 times the fixed capacity
                p.stdin.write(bytes([i % 251]) * CHUNK)
            p.stdin.close()
            assert p.wait(timeout=30) == 0
            data, health = decoder.decode(path)
            assert path.stat().st_size == SIZE
            assert not health['rejected_slots']
            assert health['sequence_gaps'] == [(4, 1007)]
            assert data == b''.join(bytes([i % 251]) * CHUNK for i in list(range(4)) + list(range(1008, 1024)))
            return path
        with ThreadPoolExecutor(2) as pool:
            paths = list(pool.map(flood, ['kernel.ring', 'android.ring']))
        assert sum(p.stat().st_size for p in paths) == 2 * SIZE
        path = paths[0]
        # Reopen the same fixed file after reader failure: keep the immutable head,
        # resume the sequence, no additional files or failed-attempt prefixes.
        subprocess.run([str(exe), str(path)], input=b'latest complete line\n', check=True)
        data, health = decoder.decode(path)
        assert data.endswith(b'latest complete line\n') and path.stat().st_size == SIZE
        assert health['sequence_gaps'] == [(4, 1008)]
        # Emulate a reset between payload and header commit: reject that slot.
        raw = bytearray(path.read_bytes())
        raw[32 + (4 + (1024 - 4) % 16) * (24 + CHUNK) + 24] ^= 1
        path.write_bytes(raw)
        data, health = decoder.decode(path)
        assert len(health['rejected_slots']) == 1 and not data.endswith(b'latest complete line\n')
        # Quiet streams must persist a partial chunk while still running.
        idle = tmp / 'idle.ring'
        p = subprocess.Popen([str(exe), str(idle)], stdin=subprocess.PIPE)
        p.stdin.write(b'last PM callback before freeze\n'); p.stdin.flush()
        for _ in range(30):
            time.sleep(.1)
            if idle.exists() and decoder.decode(idle)[0]:
                break
        assert decoder.decode(idle)[0] == b'last PM callback before freeze\n'
        p.send_signal(signal.SIGTERM)
        assert p.wait(timeout=3) == 0
        p.stdin.close()
        # An insufficient file-size allowance must fail before accepting input.
        def limited():
            resource.setrlimit(resource.RLIMIT_FSIZE, (65536, 65536))
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        failed = subprocess.run([str(exe), str(tmp / 'allocation-failure.ring')], input=b'important',
                                capture_output=True, preexec_fn=limited)
        assert failed.returncode != 0 and b'ring reserve' in failed.stderr
        # Symlinks/unknown old formats are refused, avoiding accidental writes.
        target = tmp / 'target'; target.write_bytes(b'untouched')
        link = tmp / 'link'; link.symlink_to(target)
        assert subprocess.run([str(exe), str(link)], input=b'bad', capture_output=True).returncode != 0
        assert target.read_bytes() == b'untouched'
        print(json.dumps({'passed': True, 'concurrent_input_bytes': 64 * 1024 * 1024,
                          'two_ring_bytes': 2 * SIZE, 'restart': True, 'idle_flush': True,
                          'torn_chunk_rejected': True, 'allocation_failure_visible': True}))


if __name__ == '__main__':
    main()
