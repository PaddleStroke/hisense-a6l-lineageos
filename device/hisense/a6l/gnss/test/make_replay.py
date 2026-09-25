#!/usr/bin/env python3
"""Synthesize an a6l_gnss_test packet log (agent gnss, 24 Sep 2026) = what `a6l_gnss_test --record` writes on the phone.
Encodes the QMI LOC indications independently of the C++ code (struct module), so `--replay` of this file checks the
parsers + engine + printing end to end. usage: make_replay.py <out.log>"""
import struct, sys, time


def tlv(t, payload):
    return struct.pack('<BH', t, len(payload)) + payload


def ind(msg_id, *tlvs):
    body = b''.join(tlvs)
    return struct.pack('<BHHH', 4, 0, msg_id, len(body)) + body


def nmea(s):
    c = 0
    for ch in s:
        c ^= ord(ch)
    return '$%s*%02X' % (s, c)


def sv_list(svs):
    b = struct.pack('<B', len(svs))
    for system, svid, status, el, az, snr in svs:
        b += struct.pack('<IIHBIBfff', 0xff, system, svid, 1, status, 1, el, az, snr)
    return b


def position(status, lat=None, lon=None, acc=None, utc_ms=None, used=()):
    t = [tlv(0x01, struct.pack('<I', status)), tlv(0x02, struct.pack('<B', 1))]
    if lat is not None:
        t += [tlv(0x10, struct.pack('<d', lat)), tlv(0x11, struct.pack('<d', lon)), tlv(0x12, struct.pack('<f', acc)),
              tlv(0x18, struct.pack('<f', 0.3)), tlv(0x1A, struct.pack('<f', 312.0)), tlv(0x1B, struct.pack('<f', 262.0)),
              tlv(0x1C, struct.pack('<f', 12.0)), tlv(0x20, struct.pack('<f', 181.5)),
              tlv(0x24, struct.pack('<fff', 1.6, 0.9, 1.3)), tlv(0x25, struct.pack('<Q', utc_ms)),
              tlv(0x2C, struct.pack('<B', len(used)) + b''.join(struct.pack('<H', u) for u in used))]
    return ind(0x24, *t)


def main():
    out = []
    t = 0
    out.append((t, ind(0x2B, tlv(0x01, struct.pack('<I', 1)))))                     # engine on
    out.append((t + 10, ind(0x2C, tlv(0x01, struct.pack('<I', 1)), tlv(0x10, struct.pack('<B', 1)))))
    svs = [(1, 5, 3, 55.0, 120.0, 41.0), (1, 13, 3, 30.0, 250.0, 36.0), (1, 15, 3, 70.0, 20.0, 44.0),
           (5, 70, 3, 40.0, 300.0, 33.0), (2, 305, 3, 25.0, 80.0, 30.0), (1, 24, 2, 5.0, 10.0, 0.0)]
    base_utc = 1790236800000  # 2026-09-24T08:00:00Z
    for i in range(8):
        t = 200 + i * 1000
        out.append((t, ind(0x25, tlv(0x01, b'\x00'), tlv(0x10, sv_list(svs)))))
        if i < 3:
            out.append((t + 50, position(1)))                                            # IN_PROGRESS, no position
            continue
        utc = base_utc + i * 1000
        hh = time.strftime('%H%M%S', time.gmtime(utc // 1000))
        g = nmea('GPGGA,%s.00,4540.9000,N,00452.1000,E,1,05,0.9,262.0,M,50.0,M,,' % hh)
        r = nmea('GPRMC,%s.00,A,4540.9000,N,00452.1000,E,0.58,181.5,240926,,,A' % hh)
        out.append((t + 60, ind(0x26, tlv(0x01, (g + '\r\n' + r + '\r\n').encode() + b'\0'))))
        out.append((t + 70, position(0, 45.681667, 4.868333, 6.5, utc, used=(5, 13, 15, 70, 305))))
    out.append((t + 200, ind(0x2C, tlv(0x01, struct.pack('<I', 2)))))                   # session finished
    with open(sys.argv[1], 'w') as f:
        f.write('# synthesized by make_replay.py (not a phone recording)\n')
        for ms, pkt in out:
            f.write('%d < 0:16385 %s\n' % (ms, pkt.hex()))


if __name__ == '__main__':
    main()
