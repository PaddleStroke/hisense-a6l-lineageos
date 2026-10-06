#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Hisense A6L (telephony-flows, 29 Sep 2026): read-only MMS readiness check of the APN list that ships in the ROM.
# The APN list itself is owned by the Android-side carrier/APN work (rom/android: Lineage vendor/apn + the Orange
# IPV4V6 patch); this script only verifies what MMS needs and changes nothing.
#
# Usage: check-mms-apn.py <apns-conf.xml | vendor/apn/FR.xml> [more xml ...]
# Checks, for Orange France 208-00/01/02:
#   1. "Orange World" (APN orange, no MVNO filter) carries type mms together with default -> MMS rides the default
#      PDN, no second data call needed; MMSC http://mms.orange.fr, no MMS proxy (direct HTTP, current Orange setup).
#   2. every mms-typed Orange-network entry has an MMSC; one with an MMS proxy also has an MMS port
#      (MVNO "orange.acte" 192.168.10.200:8080 = second PDN, host-tested in radio/tests/telephony_flows_tests.cc M).
#   3. the Orange World protocol is IPv4-capable (IPV4V6 after rom/android/patches/vendor/apn/0001-*; plain IPV6 is
#      reported as a WARNING only: that is the unpatched Lineage source).
# Exit 0 = MMS_APN_PASS, 1 = FAIL (message says why).
import re
import sys

APN_RE = re.compile(r"<apn\b[^>]*?/>", re.S)
ATTR_RE = re.compile(r'(\w+)="([^"]*)"')
ORANGE = {("208", "00"), ("208", "01"), ("208", "02")}


def entries(paths):
    for p in paths:
        with open(p, encoding="utf-8") as f:
            for m in APN_RE.finditer(f.read()):
                yield p, dict(ATTR_RE.findall(m.group(0)))


def main(argv):
    if len(argv) < 2:
        print(__doc__ or "usage: check-mms-apn.py <xml>...")
        return 2
    fails, warns, world, mms_only = [], [], {}, 0
    for path, a in entries(argv[1:]):
        key = (a.get("mcc"), a.get("mnc"))
        if key not in ORANGE:
            continue
        types = [t.strip() for t in a.get("type", "").split(",") if t.strip()]
        if a.get("carrier") == "Orange World" and a.get("apn") == "orange" and "mvno_type" not in a:
            world[key] = a
            if "mms" not in types or "default" not in types:
                fails.append("%s-%s Orange World type=%s: mms must share the default APN" % (*key, a.get("type")))
            if a.get("mmsc", "").rstrip("/") != "http://mms.orange.fr":
                fails.append("%s-%s Orange World mmsc=%r" % (*key, a.get("mmsc")))
            if a.get("mmsproxy"):
                fails.append("%s-%s Orange World has an MMS proxy %r (Orange MMS is direct)" % (*key, a.get("mmsproxy")))
            proto = a.get("protocol", "IP")
            if proto == "IPV6":
                warns.append("%s-%s Orange World protocol IPV6 (unpatched Lineage source; the ROM patch makes it IPV4V6)" % key)
            elif proto not in ("IP", "IPV4V6"):
                fails.append("%s-%s Orange World protocol %s" % (*key, proto))
        if "mms" in types:
            if not a.get("mmsc"):
                fails.append("%s-%s %s (apn %s) is type mms without an MMSC" % (*key, a.get("carrier"), a.get("apn")))
            if a.get("mmsproxy") and not a.get("mmsport"):
                fails.append("%s-%s %s mms proxy without port" % (*key, a.get("carrier")))
            if types == ["mms"]:
                mms_only += 1
    for key in sorted(ORANGE):
        if key not in world:
            fails.append("%s-%s: no Orange World entry" % key)
    for w in warns:
        print("WARNING", w)
    for f in fails:
        print("FAIL", f)
    print("orange-world=%d mms-only-entries(secondary PDN)=%d" % (len(world), mms_only))
    if fails:
        return 1
    print("MMS_APN_PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
