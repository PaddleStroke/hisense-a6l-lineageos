#!/usr/bin/env python3
"""Recover checked, chronological head/tail bytes from a fixed A6L log ring."""
import argparse
from pathlib import Path
import struct
import sys
import zlib


def decode(path):
    raw = Path(path).read_bytes()
    if len(raw) < 32 or raw[:8] != b'A6LRING1':
        raise ValueError('invalid ring header')
    chunk, head, tail = struct.unpack_from('<III', raw, 8)
    if (chunk, head, tail) != (32768, 4, 16) or len(raw) != 32 + (head + tail) * (24 + chunk):
        raise ValueError('invalid ring geometry/size')
    slots = []
    rejected = []
    for i in range(head + tail):
        start = 32 + i * (24 + chunk)
        h = raw[start:start + 24]
        if h[16:24] == b'\0' * 8:
            continue
        seq, n, crc = struct.unpack_from('<QII', h)
        expected = seq if seq < head else head + (seq - head) % tail
        data = raw[start + 24:start + 24 + n]
        if h[16:24] != b'A6LSLOT1' or not 0 < n <= chunk or expected != i or zlib.crc32(data, zlib.crc32(h[:12])) != crc:
            rejected.append(i)
            continue
        slots.append((seq, data))
    slots.sort()
    # No synthetic text mixed into kernel/logcat data. Gaps/incomplete slots are
    # reported separately, and chunks can legitimately start mid-line.
    gaps = [(a[0] + 1, b[0] - 1) for a, b in zip(slots, slots[1:]) if b[0] > a[0] + 1]
    return b''.join(data for _, data in slots), {'slots': len(slots), 'rejected_slots': rejected, 'sequence_gaps': gaps}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ring')
    parser.add_argument('output')
    args = parser.parse_args()
    data, health = decode(args.ring)
    Path(args.output).write_bytes(data)
    print(f'{args.ring}: {health}', file=sys.stderr)
