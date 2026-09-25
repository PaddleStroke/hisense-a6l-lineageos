#!/usr/bin/env python3
"""Laptop-side: print peak/RMS (dBFS) per channel of captured WAVs (cap-*.wav from MODE=mic)."""
import math, struct, sys, wave
for p in sys.argv[1:]:
    w = wave.open(p); ch, sw, n = w.getnchannels(), w.getsampwidth(), w.getnframes(); raw = w.readframes(n)
    if sw != 2: print(p, 'unsupported sample width', sw); continue
    s = struct.unpack('<%dh' % (len(raw) // 2), raw)
    for c in range(ch):
        x = s[c::ch]; pk = max((abs(v) for v in x), default=0); rms = math.sqrt(sum(v * v for v in x) / max(1, len(x)))
        db = lambda v: 20 * math.log10(v / 32768) if v else -999
        print(f'{p} ch{c}: {len(x)} frames, peak {db(pk):.1f} dBFS, rms {db(rms):.1f} dBFS' + ('  (all zero: no signal)' if pk == 0 else ''))
