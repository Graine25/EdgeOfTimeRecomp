import struct, os, mmap, sys, re

path = sys.argv[1]
want = int(sys.argv[2], 0)
maxn = int(sys.argv[3]) if len(sys.argv) > 3 else 2
namefilter = sys.argv[4] if len(sys.argv) > 4 else None

def children(m, start, end):
    o = start
    while o + 12 <= end:
        t, a, s = struct.unpack_from('>IIi', m, o)
        if s < 0 or o + 12 + s > end:
            return
        yield t, a, o + 12, o + 12 + s
        o += 12 + s

def fmt_dword(b):
    u = struct.unpack('>I', b)[0]
    f = struct.unpack('>f', b)[0]
    s = ''
    if all(32 <= c < 127 for c in b):
        s = b.decode()
    if f == f and 1e-6 < abs(f) < 1e7:
        return f'{u:08x} f={f:<12.5g} {s}'
    return f'{u:08x} i={u if u < 0x80000000 else u - 0x100000000:<12} {s}'

f = open(path, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
t, a, s = struct.unpack_from('>IIi', m, 0)
n = 0
for tt, ta, ts, te in children(m, 12, 12 + s):
    if tt != 4:
        continue
    for kt, ka, ks, ke in children(m, ts, te):
        if kt != 5005:
            continue
        rtype = None; name = '?'; body = None
        for st, sa, ss, se in children(m, ks, ke):
            if st == 5006:
                rtype = struct.unpack_from('>I', m, ss + 4)[0]
                name = m[ss + 24:ss + 88].split(b'\0')[0].decode('ascii', 'replace')
                hdr = m[ss:ss + 24]
            elif st == 500:
                body = (sa, ss, se)
        if rtype != want or body is None:
            continue
        if namefilter and namefilter.lower() not in name.lower():
            continue
        sa, ss, se = body
        print(f'\n##### {name}  rtype={rtype:#x} 500-attr={sa:#x} size={se-ss}  5006hdr={hdr.hex()}')
        b = m[ss:se]
        for o in range(0, len(b) - 3, 4):
            print(f'  +{o:04x}  {fmt_dword(b[o:o+4])}')
        rem = len(b) % 4
        if rem:
            print(f'  tail {b[-rem:].hex()}')
        n += 1
        if n >= maxn:
            sys.exit(0)
