import math, mmap, os, struct, sys

root = os.environ.get("EOT_EXTRACT", r"D:\EOT_Extract\extracted")
pack = sys.argv[1]
near = [float(v) for v in sys.argv[2].split(",")] if len(sys.argv) > 2 else None
radius = float(sys.argv[3]) if len(sys.argv) > 3 else float("inf")

def children(m, start, end):
    o = start
    while o + 12 <= end:
        t, a, s = struct.unpack_from(">IIi", m, o)
        if s < 0 or o + 12 + s > end:
            return
        yield t, a, o + 12, o + 12 + s
        o += 12 + s

path = pack if os.path.isfile(pack) else os.path.join(root, pack + ".ext")
f = open(path, "rb")
m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
t, a, s = struct.unpack_from(">IIi", m, 0)
count = 0
for tt, ta, ts, te in children(m, 12, 12 + s):
    if tt != 4:
        continue
    for kt, ka, ks, ke in children(m, ts, te):
        if kt != 5005:
            continue
        rtype = None; name = "?"; body = None
        for st, sa, ss, se in children(m, ks, ke):
            if st == 5006:
                rtype = struct.unpack_from(">I", m, ss + 4)[0]
                name = m[ss + 24:ss + 88].split(b"\0")[0].decode("ascii", "replace")
            elif st == 500:
                body = (ss, se)
        if rtype != 0x104 or body is None:
            continue
        count += 1
        b = m[body[0]:body[1]]
        F = lambda o: struct.unpack_from(">f", b, o)[0]
        U = lambda o: struct.unpack_from(">I", b, o)[0]
        rows = [[F(0x1C + 16 * r + 4 * c) for c in range(3)] for r in range(4)]
        pos, axis = rows[3], rows[2]
        dist = math.dist(pos, near) if near else 0.0
        if dist > radius:
            continue
        fl = U(0x6C)
        kind = "spot" if fl & 0x20 else "omni" if fl & 0x10 else "dir" if fl & 0x40 else "other"
        print(f"{name:48s} {kind:5s} fl={fl:#07x} pos=({pos[0]:8.2f},{pos[1]:7.2f},{pos[2]:8.2f})"
              + (f" d={dist:6.1f}" if near else "")
              + f" axis=({axis[0]:6.3f},{axis[1]:6.3f},{axis[2]:6.3f}) col={U(0x70):08x}/{U(0x74):08x}"
              f" int={F(0x7C):.3g}/{F(0x80):.3g} att={F(0x88):.4g}/{F(0x8C):.4g}"
              f" cone={F(0x90):.4g}/{F(0x94):.4g} cookie={U(0x9C):08x}")
print(count, "lights in pack")
