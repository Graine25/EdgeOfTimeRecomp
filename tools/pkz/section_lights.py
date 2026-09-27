import glob, mmap, os, struct

root = os.environ.get("EOT_EXTRACT", r"D:\EOT_Extract\extracted")
SECTION_CONTROL_CLASS = 0xDD13F139
NONE = 0xFFFFFFFF

def children(m, start, end):
    o = start
    while o + 12 <= end:
        t, a, s = struct.unpack_from(">IIi", m, o)
        if s < 0 or o + 12 + s > end:
            return
        yield t, a, o + 12, o + 12 + s
        o += 12 + s

def param_block(m, start, end, depth=0):
    for t, a, s, e in children(m, start, end):
        if t == 5009:
            return s
        if depth < 3:
            found = param_block(m, s, e, depth + 1)
            if found is not None:
                return found
    return None

names, controls, managers = {}, [], []
for path in sorted(glob.glob(os.path.join(root, "*.ext"))):
    pack = os.path.basename(path)[:-4]
    f = open(path, "rb")
    m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    t, a, s = struct.unpack_from(">IIi", m, 0)
    for tt, ta, ts, te in children(m, 12, 12 + s):
        if tt != 4:
            continue
        for kt, ka, ks, ke in children(m, ts, te):
            if kt != 5005:
                continue
            crc = rtype = body = None
            name = "?"
            for st, sa, ss, se in children(m, ks, ke):
                if st == 5006:
                    crc, rtype = struct.unpack_from(">II", m, ss)
                    name = m[ss + 24:ss + 88].split(b"\0")[0].decode("ascii", "replace")
                elif st == 500:
                    body = (ss, se)
            if crc is None:
                continue
            names[crc] = name
            if rtype != 0x101 or body is None or body[1] - body[0] < 0x88:
                continue
            block = param_block(m, body[0], body[1])
            if block is None:
                continue
            params = block + 4
            logic_class = struct.unpack_from(">I", m, body[0] + 0x84)[0]
            if logic_class == SECTION_CONTROL_CLASS:
                u32 = lambda o: struct.unpack_from(">I", m, params + o)[0]
                controls.append((u32(60), name, pack, u32(20)))
            elif name.startswith("LightManager"):
                u32 = lambda o: struct.unpack_from(">I", m, params + o)[0]
                count = struct.unpack_from(">H", m, params + 14)[0]
                lights = [u32(36 + 4 * i) for i in range(count)]
                managers.append((name, pack, u32(32), lights))

label = lambda crc: "-" if crc == NONE else names.get(crc, f"{crc:#010x}")
for section, name, pack, lm in sorted(controls):
    print(f"{section:4d}  {name:28s} {pack:45s} {label(lm)}")
print()
for name, pack, fade_others, lights in managers:
    print(f"{name:34s} {pack:24s} fade_others={fade_others} lights={[label(l) for l in lights]}")
