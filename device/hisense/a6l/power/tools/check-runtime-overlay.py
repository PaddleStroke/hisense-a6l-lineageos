#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# check-runtime-overlay.py (pwr27, 27 Sep 2026): offline check that a .dtbo can be applied to a LIVE tree by the
# kernel's of_overlay_fdt_apply() (runtime overlay modules such as a6l_chg_ovl / a6l_fl_ovl), not only by
# fdtoverlay at build time. fdtoverlay silently merges things the kernel refuses (drivers/of/overlay.c 7.2):
#   - add_changeset_node(): an overlay node that carries a phandle onto an EXISTING live node that has a phandle -> -EINVAL
#     (this is what broke a6l-charger-test on 27 Sep: "a6l_battery: battery" re-declared over the V74 /battery)
#   - add_changeset_property(): a __symbols__ entry that already exists in the live tree -> -EINVAL
#   - a target label that the live __symbols__ does not know -> of_resolve_phandles() fails
# usage: check-runtime-overlay.py <base.dtb> <overlay.dtbo> [...]   -> prints A6L_RUNTIME_OVL_OK|FAIL per dtbo
import struct
import sys


def parse(path):
    b = open(path, 'rb').read()
    magic, _tot, off_struct, off_strings = struct.unpack('>IIII', b[:16])
    assert magic == 0xd00dfeed, path
    strs = lambda o: b[off_strings + o:b.index(b'\0', off_strings + o)].decode()
    root = None
    stack = []
    p = off_struct
    while True:
        tok = struct.unpack('>I', b[p:p + 4])[0]
        p += 4
        if tok == 1:  # BEGIN_NODE
            e = b.index(b'\0', p)
            name = b[p:e].decode()
            p = (e + 4) & ~3
            n = {'name': name, 'props': {}, 'kids': {}}
            if stack:
                stack[-1]['kids'][name] = n
            else:
                root = n
            stack.append(n)
        elif tok == 2:
            stack.pop()
        elif tok == 3:
            ln, no = struct.unpack('>II', b[p:p + 8])
            p += 8
            stack[-1]['props'][strs(no)] = b[p:p + ln]
            p = (p + ln + 3) & ~3
        elif tok == 4:
            continue
        elif tok == 9:
            break
        else:
            raise ValueError('bad token %d' % tok)
    return root


def lookup(root, path):
    n = root
    for c in [x for x in path.split('/') if x]:
        n = n['kids'].get(c)
        if n is None:
            return None
    return n


def s(v):
    return v.rstrip(b'\0').decode()


def check(base, ovl_path):
    ov = parse(ovl_path)
    errs = []
    bsym = lookup(base, '/__symbols__')['props']
    fix = {}
    for label, v in lookup(ov, '/__fixups__')['props'].items() if lookup(ov, '/__fixups__') else []:
        for ref in s(v).split('\0'):
            fix[ref.rsplit(':', 1)[0]] = label
    for fname, frag in ov['kids'].items():
        if not fname.startswith('fragment@'):
            continue
        tgt = None
        if 'target-path' in frag['props']:
            tgt = s(frag['props']['target-path'])
        elif '/%s:target' % fname in fix:
            label = fix['/%s:target' % fname]
            if label not in bsym:
                errs.append('%s: target label &%s not in live __symbols__' % (fname, label))
                continue
            tgt = s(bsym[label])
        else:
            errs.append('%s: target is a local phandle (unsupported here)' % fname)
            continue
        if lookup(base, tgt) is None:
            errs.append('%s: target %s missing in base' % (fname, tgt))
            continue

        def walk(onode, bpath):
            for cname, c in onode['kids'].items():
                cpath = bpath.rstrip('/') + '/' + cname
                live = lookup(base, cpath)
                if live is not None and 'phandle' in c['props'] and 'phandle' in live['props']:
                    errs.append('%s: node %s has a phandle and the live node already has one -> kernel -EINVAL '
                                '(modify it by label instead of re-declaring it)' % (fname, cpath))
                if live is not None:
                    walk(c, cpath)
        walk(frag['kids'].get('__overlay__', {'kids': {}}), tgt)
    osym = lookup(ov, '/__symbols__')
    for label in (osym['props'] if osym else {}):
        if label in bsym:
            errs.append('__symbols__: %s already in the live tree -> kernel -EINVAL' % label)
    return errs


def main():
    base = parse(sys.argv[1])
    bad = 0
    for o in sys.argv[2:]:
        errs = check(base, o)
        for e in errs:
            print('  %s: %s' % (o.rsplit('/', 1)[-1], e))
        print('%s %s' % ('A6L_RUNTIME_OVL_OK' if not errs else 'A6L_RUNTIME_OVL_FAIL', o.rsplit('/', 1)[-1]))
        bad += bool(errs)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
