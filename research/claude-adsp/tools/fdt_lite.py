#!/usr/bin/env python3
"""Minimal, dependency-free flattened-device-tree (DTB) reader/writer.

Written for offline A6L ADSP audits where neither dtc nor libfdt can be
installed. It preserves node/property order, phandles and the __symbols__ /
__fixups__ tables, so a re-serialised tree is byte-comparable in content
(not necessarily in string-table layout) with the original.

Supported: DTB v17 (magic 0xd00dfeed), FDT_BEGIN_NODE/END_NODE/PROP/NOP/END.
Not supported: overlay fixups resolution (fdtoverlay).  Overlay *effects* are
applied by explicit Python edits instead, and reported as a scoped diff.
"""
import struct

FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


class Node:
    def __init__(self, name, parent=None):
        self.name = name
        self.parent = parent
        self.props = []      # list of (name, bytes) in original order
        self.children = []

    # ---- helpers -------------------------------------------------------
    def path(self):
        if self.parent is None:
            return '/'
        p = self.parent.path()
        return (p if p != '/' else '') + '/' + self.name

    def get(self, pname):
        for n, v in self.props:
            if n == pname:
                return v
        return None

    def set(self, pname, value):
        for i, (n, _) in enumerate(self.props):
            if n == pname:
                self.props[i] = (n, value)
                return
        self.props.append((pname, value))

    def delete(self, pname):
        self.props = [(n, v) for n, v in self.props if n != pname]

    def child(self, cname):
        for c in self.children:
            if c.name == cname:
                return c
        return None

    def add_child(self, cname):
        c = Node(cname, self)
        self.children.append(c)
        return c

    def walk(self):
        yield self
        for c in self.children:
            yield from c.walk()

    def u32s(self, pname):
        v = self.get(pname)
        if v is None:
            return None
        return list(struct.unpack('>%dI' % (len(v) // 4), v))

    def strings(self, pname):
        v = self.get(pname)
        if v is None:
            return None
        return [s.decode() for s in v.split(b'\0') if s]

    def phandle(self):
        v = self.u32s('phandle')
        return v[0] if v else None


class FDT:
    def __init__(self, data=None):
        self.root = Node('')
        self.mem_rsvmap = []
        self.boot_cpuid = 0
        self.version = 17
        self.last_comp_version = 16
        if data is not None:
            self._parse(data)

    # ---- parsing -------------------------------------------------------
    def _parse(self, d):
        (magic, totalsize, off_struct, off_strings, off_rsvmap, version,
         last_comp, boot_cpuid, size_strings, size_struct) = struct.unpack('>10I', d[:40])
        if magic != FDT_MAGIC:
            raise ValueError('bad magic %#x' % magic)
        self.version, self.last_comp_version, self.boot_cpuid = version, last_comp, boot_cpuid
        pos = off_rsvmap
        while True:
            a, s = struct.unpack('>QQ', d[pos:pos + 16])
            pos += 16
            if a == 0 and s == 0:
                break
            self.mem_rsvmap.append((a, s))
        strings = d[off_strings:off_strings + size_strings]
        pos = off_struct
        stack = []
        cur = None
        while True:
            tok, = struct.unpack('>I', d[pos:pos + 4])
            pos += 4
            if tok == FDT_BEGIN_NODE:
                end = d.index(b'\0', pos)
                name = d[pos:end].decode()
                pos = (end + 1 + 3) & ~3
                if cur is None:
                    node = self.root
                    node.name = name
                else:
                    node = cur.add_child(name)
                stack.append(cur)
                cur = node
            elif tok == FDT_END_NODE:
                cur = stack.pop()
            elif tok == FDT_PROP:
                length, nameoff = struct.unpack('>II', d[pos:pos + 8])
                pos += 8
                val = d[pos:pos + length]
                pos = (pos + length + 3) & ~3
                pend = strings.index(b'\0', nameoff)
                cur.props.append((strings[nameoff:pend].decode(), bytes(val)))
            elif tok == FDT_NOP:
                continue
            elif tok == FDT_END:
                break
            else:
                raise ValueError('bad token %d at %d' % (tok, pos - 4))

    # ---- serialisation -------------------------------------------------
    def to_bytes(self):
        strtab = bytearray()
        stroff = {}

        def s_off(name):
            if name not in stroff:
                stroff[name] = len(strtab)
                strtab.extend(name.encode() + b'\0')
            return stroff[name]

        out = bytearray()

        def emit(node):
            out.extend(struct.pack('>I', FDT_BEGIN_NODE))
            nb = node.name.encode() + b'\0'
            out.extend(nb + b'\0' * ((-len(nb)) % 4))
            for n, v in node.props:
                out.extend(struct.pack('>III', FDT_PROP, len(v), s_off(n)))
                out.extend(v + b'\0' * ((-len(v)) % 4))
            for c in node.children:
                emit(c)
            out.extend(struct.pack('>I', FDT_END_NODE))

        emit(self.root)
        out.extend(struct.pack('>I', FDT_END))
        rsv = bytearray()
        for a, s in self.mem_rsvmap:
            rsv.extend(struct.pack('>QQ', a, s))
        rsv.extend(struct.pack('>QQ', 0, 0))
        off_rsvmap = 40
        off_struct = off_rsvmap + len(rsv)
        off_struct += (-off_struct) % 4
        off_strings = off_struct + len(out)
        total = off_strings + len(strtab)
        total += (-total) % 4
        hdr = struct.pack('>10I', FDT_MAGIC, total, off_struct, off_strings, off_rsvmap,
                          self.version, self.last_comp_version, self.boot_cpuid,
                          len(strtab), len(out))
        blob = bytearray(hdr)
        blob.extend(rsv)
        blob.extend(b'\0' * (off_struct - len(blob)))
        blob.extend(out)
        blob.extend(strtab)
        blob.extend(b'\0' * (total - len(blob)))
        return bytes(blob)

    # ---- lookups -------------------------------------------------------
    def node(self, path):
        if path == '/':
            return self.root
        cur = self.root
        for part in path.strip('/').split('/'):
            cur = cur.child(part)
            if cur is None:
                return None
        return cur

    def by_phandle(self, ph):
        for n in self.root.walk():
            if n.phandle() == ph:
                return n
        return None

    def symbol(self, label):
        sym = self.node('/__symbols__')
        if sym is None:
            return None
        v = sym.get(label)
        return self.node(v.rstrip(b'\0').decode()) if v else None

    def max_phandle(self):
        return max((n.phandle() or 0) for n in self.root.walk())

    def props_flat(self):
        d = {}
        for n in self.root.walk():
            p = n.path()
            for name, v in n.props:
                d[(p if p != '/' else '') + '/' + name] = v
        return d


def diff(a, b):
    """Return {path/prop: {'before': hex|None, 'after': hex|None}} for two FDTs."""
    fa, fb = a.props_flat(), b.props_flat()
    out = {}
    for k in sorted(set(fa) | set(fb)):
        if fa.get(k) != fb.get(k):
            out[k] = {'before': fa[k].hex() if k in fa else None,
                      'after': fb[k].hex() if k in fb else None}
    return out
