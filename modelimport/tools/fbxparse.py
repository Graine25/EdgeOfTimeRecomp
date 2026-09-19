import struct, zlib, sys
import numpy as np

class Node:
    __slots__ = ('name', 'props', 'children')
    def __init__(self, name, props, children):
        self.name, self.props, self.children = name, props, children
    def find(self, name):
        return [c for c in self.children if c.name == name]
    def first(self, name, default=None):
        for c in self.children:
            if c.name == name:
                return c
        return default
    def path(self, *names):
        n = self
        for nm in names:
            n = n.first(nm)
            if n is None:
                return None
        return n

_ARRAY = {b'f': ('<f4', 4), b'd': ('<f8', 8), b'l': ('<i8', 8), b'i': ('<i4', 4), b'b': ('<u1', 1)}
_SCALAR = {b'Y': ('<h', 2), b'C': ('<?', 1), b'I': ('<i', 4), b'F': ('<f', 4), b'D': ('<d', 8), b'L': ('<q', 8)}

def _read_prop(d, p):
    t = d[p:p + 1]; p += 1
    if t in _SCALAR:
        fmt, n = _SCALAR[t]
        return struct.unpack_from(fmt, d, p)[0], p + n
    if t in _ARRAY:
        dt, esz = _ARRAY[t]
        n, enc, clen = struct.unpack_from('<III', d, p); p += 12
        raw = d[p:p + clen]; p += clen
        if enc == 1:
            raw = zlib.decompress(raw)
        return np.frombuffer(raw, dt, n).copy(), p
    if t in (b'S', b'R'):
        n = struct.unpack_from('<I', d, p)[0]; p += 4
        v = d[p:p + n]; p += n
        return (v.decode('utf-8', 'replace') if t == b'S' else v), p
    raise ValueError('unknown property type %r at %d' % (t, p))

def parse(path):
    d = open(path, 'rb').read()
    assert d[:20] == b'Kaydara FBX Binary  ', 'not a binary FBX'
    version = struct.unpack_from('<I', d, 23)[0]
    big = version >= 7500
    def read_node(p):
        if big:
            end, nprops, plen = struct.unpack_from('<QQQ', d, p); p += 24
        else:
            end, nprops, plen = struct.unpack_from('<III', d, p); p += 12
        nlen = d[p]; p += 1
        name = d[p:p + nlen].decode('ascii', 'replace'); p += nlen
        if end == 0:
            return None, p
        props = []
        for _ in range(nprops):
            v, p = _read_prop(d, p)
            props.append(v)
        children = []
        while p < end:
            c, p = read_node(p)
            if c is None:
                break
            children.append(c)
        return Node(name, props, children), end
    p = 27
    roots = []
    while p < len(d):
        n, p = read_node(p)
        if n is None:
            break
        roots.append(n)
    return version, Node('ROOT', [], roots)

def outline(node, depth=0, maxdepth=3, out=None):
    out = out if out is not None else []
    if depth > maxdepth:
        return out
    ps = []
    for v in node.props[:4]:
        if isinstance(v, np.ndarray):
            ps.append('%s[%d]' % (v.dtype, len(v)))
        elif isinstance(v, (bytes, bytearray)):
            ps.append('R[%d]' % len(v))
        else:
            ps.append(repr(v)[:60])
    out.append('  ' * depth + node.name + ' ' + ', '.join(ps))
    for c in node.children:
        outline(c, depth + 1, maxdepth, out)
    return out

if __name__ == '__main__':
    v, root = parse(sys.argv[1])
    print('FBX version', v)
    depth = int(sys.argv[2]) if len(sys.argv) > 2 else 2
    print('\n'.join(outline(root, 0, depth)[:400]))
