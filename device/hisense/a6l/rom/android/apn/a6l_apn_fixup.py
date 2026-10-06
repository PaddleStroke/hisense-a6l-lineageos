#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Hisense A6L (overlays-carrier-updater, 29 Sep 2026): device fix-up of LineageOS' generated apns-conf.xml (vendor/apn).
# Lineage's Orange France "Orange World" entries (208-00, 208-01 and 208-02: the internet APN "orange" and the initial-attach "ia"
# entry) are protocol="IPV6" with no roaming_protocol. IPv6-only would make every IPv4-only destination depend on 464XLAT
# (clatd) over rmnet/IPA, which is untested on this port, and the only data path proven on the phone (data3/ipa4, 27 Sep)
# is an IPv4 call; stock (Hisense apns-conf) used IPv4 for "orange". Set IPV4V6 home + roaming: the network then grants
# v4, v6 or both, and the radio HAL (RadioNetworkData.cpp) keeps whichever leg is usable.
# Shipped as a tree patch (rom/android/patches/vendor/apn/0001-*.patch, applied by tools/rom-v2-pipeline.sh prep), NOT as a
# second apns-conf.xml module: Soong writes an install rule for every module, so two modules installing
# /product/etc/apns-conf.xml collide in Kati even with `overrides`. This script generates/regenerates that patch:
#   a6l_apn_fixup.py vendor/apn/FR.xml /tmp/FR.xml && diff -u (a/ b/ paths) > patches/vendor/apn/0001-...patch
# It works on the one-line generated list and on the multi-line FR.xml source alike.
# Usage: a6l_apn_fixup.py <in xml> <out xml>. Exit 1 when no Orange World entry is found (an upstream change must be
# looked at, not silently shipped).
import re
import sys

TARGET_MCCMNC = {("208", "00"), ("208", "01"), ("208", "02")}
APN_RE = re.compile(r"<apn\b[^>]*?/>", re.S)
ATTR_RE = re.compile(r'(\w+)="([^"]*)"')


def set_attr(elem, name, value):
    if re.search(r'\b%s="[^"]*"' % name, elem):
        return re.sub(r'\b%s="[^"]*"' % name, '%s="%s"' % (name, value), elem, count=1)
    # roaming_protocol right after protocol (vendor/apn keeps attributes sorted), protocol after carrier; same separator
    anchor = "protocol" if name == "roaming_protocol" else "carrier"
    return re.sub(r'(\b%s="[^"]*")(\s+)' % anchor, lambda m: '%s%s%s="%s"%s' % (m.group(1), m.group(2), name, value, m.group(2)),
                  elem, count=1)


def fix(text):
    changed = []

    def repl(m):
        elem = m.group(0)
        a = dict(ATTR_RE.findall(elem))
        if (a.get("mcc"), a.get("mnc")) not in TARGET_MCCMNC or a.get("carrier") != "Orange World" or "mvno_type" in a:
            return elem
        types = a.get("type", "")
        if a.get("apn") != "orange" and "ia" not in types.split(","):
            return elem
        new = set_attr(elem, "protocol", "IPV4V6")
        new = set_attr(new, "roaming_protocol", "IPV4V6")
        changed.append((a["mcc"] + a["mnc"], a.get("apn", ""), types))
        return new

    out = APN_RE.sub(repl, text)
    return out, changed


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: a6l_apn_fixup.py <in> <out>\n")
        return 2
    with open(argv[1], encoding="utf-8") as f:
        text = f.read()
    out, changed = fix(text)
    if not changed:
        sys.stderr.write("a6l_apn_fixup: no Orange World (208-00/01/02) entry found in %s\n" % argv[1])
        return 1
    with open(argv[2], "w", encoding="utf-8") as f:
        f.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
