import struct, glob, os, mmap, csv, collections

root = os.environ.get('EOT_EXTRACT', r'D:\EOT_Extract\extracted')
out = os.path.join(os.getcwd(), 'postfx_initializers.csv')

def children(m, start, end):
    o = start
    while o + 12 <= end:
        t, a, s = struct.unpack_from('>IIi', m, o)
        if s < 0 or o + 12 + s > end:
            return
        yield t, a, o + 12, o + 12 + s
        o += 12 + s

def F(m, b, off): return struct.unpack_from('>f', m, b + off)[0]
def U(m, b, off): return struct.unpack_from('>I', m, b + off)[0]
def I(m, b, off): return struct.unpack_from('>i', m, b + off)[0]

rows = []
classes = collections.Counter()
for path in sorted(glob.glob(os.path.join(root, '*.ext'))):
    pack = os.path.basename(path)[:-4]
    f = open(path, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    t, a, s = struct.unpack_from('>IIi', m, 0)
    for tt, ta, ts, te in children(m, 12, 12 + s):
        if tt != 4:
            continue
        for kt, ka, ks, ke in children(m, ts, te):
            if kt != 5005:
                continue
            rtype = None; name = '?'; body = None
            for st, sa, ss, se in children(m, ks, ke):
                if st == 5006:
                    rtype = U(m, ss, 4)
                    name = m[ss + 24:ss + 88].split(b'\0')[0].decode('ascii', 'replace')
                elif st == 500:
                    body = (ss, se)
            if rtype != 0x101 or body is None:
                continue
            pb = None
            for ct, ca, cs, ce in children(m, body[0], body[1]):
                if ct == 5009:
                    pb = (cs, ce)
                elif ct == 502:
                    for dt, da, ds, de in children(m, cs, ce):
                        if dt == 5009:
                            pb = (ds, de)
            if pb is None:
                continue
            cls = (U(m, pb[0], 0), U(m, pb[0], 4), U(m, pb[0], 8), U(m, pb[0], 12))
            if 'postfxinit' not in name.lower():
                continue
            classes[cls[1:]] += 1
            b = pb[0] + 4
            size = U(m, b, 0)
            r = {
                'pack': pack, 'name': name, 'size': size,
                'enabled': U(m, b, 268), 'blend_time': F(m, b, 308),
                'far_clip': F(m, b, 324), 'fov_deg': F(m, b, 360),
                'lightshaft_color_on': U(m, b, 348), 'lightshaft_color': [round(F(m, b, o), 4) for o in (332, 336, 340, 344)],
                'dof_on': U(m, b, 32), 'dof_near_focus_far_amount': [round(F(m, b, o), 4) for o in (100, 12, 16, 60)],
                'fog_on': U(m, b, 56), 'fog_dome': U(m, b, 36), 'fog_near_far_rgb_density_gamma': [round(F(m, b, o), 4) for o in (96, 20, 76, 80, 84, 88, 352)],
                'bloom_on': U(m, b, 240), 'tonemap_enable': U(m, b, 72), 'bloom_enable': U(m, b, 244), 'bloom_threshold_toneA_toneB_int5x5_int9x9': [round(F(m, b, o), 4) for o in (48, 24, 28, 92, 40)],
                'heathaze_on': U(m, b, 120), 'heathaze': [round(F(m, b, o), 4) for o in (104, 108, 112, 116)],
                'clut_on': U(m, b, 172), 'clut_tex': f"{U(m, b, 164):08x}", 'clut_weight': round(F(m, b, 168), 4),
                'halo_on': U(m, b, 248), 'halo': [round(F(m, b, o), 4) for o in (252, 256, 356)],
                'grain_on': U(m, b, 176), 'grain_tex': f"{U(m, b, 184):08x}", 'grain': [round(F(m, b, o), 4) for o in (180, 188, 192, 196, 200, 204)], 'grain_flag208': U(m, b, 208), 'grain_224': U(m, b, 224),
                'edge_on': U(m, b, 212), 'edge': [round(F(m, b, o), 4) for o in (312, 316, 228, 232, 276, 280, 236, 328)],
            }
            rows.append(r)
    m.close(); f.close()

print('5009 header dwords 1..3 among PostFXInitializers:', classes.most_common(8))
print('PostFXInitializers:', len(rows))
keys = list(rows[0].keys())
w = csv.DictWriter(open(out, 'w', newline='', encoding='utf-8'), fieldnames=keys)
w.writeheader()
for r in rows:
    w.writerow(r)
def on(k): return sum(1 for r in rows if r[k])
print({k: on(k) for k in ('enabled', 'dof_on', 'fog_on', 'bloom_on', 'heathaze_on', 'clut_on', 'halo_on', 'grain_on', 'edge_on', 'lightshaft_color_on')})
print('sizes', collections.Counter(r['size'] for r in rows))
print('far_clip', collections.Counter(r['far_clip'] for r in rows).most_common(6))
print('fov', collections.Counter(r['fov_deg'] for r in rows).most_common(6))
print('bloom vectors (on):', collections.Counter(tuple(r['bloom_threshold_toneA_toneB_int5x5_int9x9']) for r in rows if r['bloom_on']).most_common(8))
print('fog vectors (on):', collections.Counter(tuple(r['fog_near_far_rgb_density_gamma']) for r in rows if r['fog_on']).most_common(8))
print('dof vectors (on):', collections.Counter(tuple(r['dof_near_focus_far_amount']) for r in rows if r['dof_on']).most_common(8))
print('grain (on):', collections.Counter((r['grain_tex'], tuple(r['grain'])) for r in rows if r['grain_on']).most_common(6))
print('edge (on):', collections.Counter(tuple(r['edge']) for r in rows if r['edge_on']).most_common(6))
print('halo (on):', collections.Counter(tuple(r['halo']) for r in rows if r['halo_on']).most_common(6))
print('heathaze (on):', collections.Counter(tuple(r['heathaze']) for r in rows if r['heathaze_on']).most_common(6))
