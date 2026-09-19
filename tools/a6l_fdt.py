"""Small read-only FDT reader for comparing captured and compiled board wiring."""
import struct


def cells(value):
    if len(value) % 4:
        raise ValueError('Property is not a cell array')
    return list(struct.unpack('>' + 'I' * (len(value) // 4), value))


def strings(value):
    return value.rstrip(b'\0').decode('utf-8').split('\0')


def read_fdt(data):
    if len(data) < 40:
        raise ValueError('Truncated FDT header')
    magic, total, off, str_off, _, version, _, _, str_size, size = struct.unpack_from('>10I', data)
    if magic != 0xd00dfeed or version < 17 or total > len(data):
        raise ValueError('Unsupported FDT header')
    end = off + size
    if end > total or str_off + str_size > total:
        raise ValueError('FDT block outside declared bounds')
    names = data[str_off:str_off + str_size]
    nodes, stack = {}, []
    while off + 4 <= end:
        token = struct.unpack_from('>I', data, off)[0]
        off += 4
        if token == 1:
            stop = data.index(b'\0', off, end)
            stack.append(data[off:stop].decode())
            path = '/' + '/'.join(stack[1:])
            if path in nodes:
                raise ValueError('Duplicate node')
            nodes[path] = {}
            off = (stop + 4) & ~3
        elif token == 2:
            if not stack:
                raise ValueError('Unbalanced node end')
            stack.pop()
        elif token == 3:
            if not stack or off + 8 > end:
                raise ValueError('Invalid property header')
            length, name_off = struct.unpack_from('>II', data, off)
            off += 8
            if off + length > end or name_off >= len(names):
                raise ValueError('Invalid property bounds')
            name = names[name_off:names.index(b'\0', name_off)].decode()
            props = nodes['/' + '/'.join(stack[1:])]
            if name in props:
                raise ValueError('Duplicate property')
            props[name] = data[off:off + length]
            off = (off + length + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            if stack:
                raise ValueError('Unclosed nodes')
            return nodes
        else:
            raise ValueError(f'Invalid FDT token {token}')
    raise ValueError('Missing FDT end token')
