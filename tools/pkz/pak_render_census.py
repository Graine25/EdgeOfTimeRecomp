import struct, glob, os, mmap, collections, csv, re

root = os.environ.get('EOT_EXTRACT', r'D:\EOT_Extract\extracted')
here = os.getcwd()

def children(m, start, end):
    o = start
    while o + 12 <= end:
        t, a, s = struct.unpack_from('>IIi', m, o)
        if s < 0 or o + 12 + s > end:
            return
        yield t, a, o + 12, o + 12 + s
        o += 12 + s

SUFFIX = [('_LM', 'lightmap'), ('_MK', 'mask'), ('_CLUT', 'clut'), ('CLUT', 'clut'), ('_N', 'normal'), ('_S', 'specular'),
          ('_D', 'diffuse'), ('_DA', 'diffuse_alpha'), ('_E', 'emissive'), ('_G', 'glow'), ('_O', 'occlusion'), ('_H', 'height'), ('_SP', 'specular'), ('_NM', 'normal')]

def texclass(name):
    n = re.sub(r'\[(Hi|HI|Lo|Med)\]$', '', name)
    n = re.sub(r'\d+$', '', n)
    if 'CLUT' in n.upper(): return 'clut'
    m = re.search(r'_([A-Za-z]{1,3})$', n)
    if not m: return 'other'
    s = m.group(1).upper()
    return {'LM': 'lightmap', 'MK': 'mask', 'N': 'normal', 'NM': 'normal', 'S': 'specular', 'SP': 'specular', 'D': 'diffuse', 'DA': 'diffuse_alpha',
            'E': 'emissive', 'G': 'glow', 'O': 'occlusion', 'H': 'height', 'A': 'alpha', 'R': 'reflection', 'C': 'cube', 'M': 'mask'}.get(s, 'other')

percsv = []
tot = collections.Counter()
texfmt = collections.Counter()
lightkinds = collections.Counter()
lm_by_pack = {}
for path in sorted(glob.glob(os.path.join(root, '*.ext'))):
    pack = os.path.basename(path)[:-4]
    f = open(path, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    t, a, s = struct.unpack_from('>IIi', m, 0)
    c = collections.Counter()
    for tt, ta, ts, te in children(m, 12, 12 + s):
        if tt == 9:
            for kt, ka, ks, ke in children(m, ts, te):
                if kt != 5005: continue
                name = None
                for st, sa, ss, se in children(m, ks, ke):
                    if st == 5006:
                        name = m[ss + 24:ss + 88].split(b'\0')[0].decode('ascii', 'replace')
                    elif st == 401:
                        w = struct.unpack_from('>IIIIII', m, ss)
                        texfmt[(se - ss, w[0], w[1])] += 1
                if name:
                    c['tex_' + texclass(name)] += 1
                    c['tex_total'] += 1
        elif tt == 7:
            for kt, ka, ks, ke in children(m, ts, te):
                if kt != 5005: continue
                b = m[ks:ke]
                c['models'] += 1
                c['model_shaders'] += b.count(b'\x10\x2a\x11\x00') + b.count(b'\x10\x2a\x11\x01')
                c['model_bytes'] += ke - ks
        elif tt == 48:
            for kt, ka, ks, ke in children(m, ts, te):
                if kt != 5005: continue
                c['environments'] += 1
                for st, sa, ss, se in children(m, ks, ke):
                    if st in (3003, 3004, 3005, 3006, 3007, 3050):
                        c[f'env_{st}'] += 1
        elif tt == 4:
            for kt, ka, ks, ke in children(m, ts, te):
                if kt != 5005: continue
                rtype = None; name = '?'; body = None
                for st, sa, ss, se in children(m, ks, ke):
                    if st == 5006:
                        rtype = struct.unpack_from('>I', m, ss + 4)[0]
                        name = m[ss + 24:ss + 88].split(b'\0')[0].decode('ascii', 'replace')
                    elif st == 500:
                        body = (ss, se)
                if rtype == 0x104 and body:
                    for c1t, c1a, c1s, c1e in children(m, body[0], body[1]):
                        if c1t == 511:
                            for c2t, c2a, c2s, c2e in children(m, c1s, c1e):
                                if c2t == 512 and c2e - c2s >= 0xfc:
                                    fl = struct.unpack_from('>I', m, c2s + 0x54)[0]
                                    kind = 'omni' if fl & 0x10 else 'spot' if fl & 0x20 else 'dir' if fl & 0x40 else 'none'
                                    c['lights'] += 1; c['light_' + kind] += 1
                                    if fl & 0x200: c['light_shadowmap'] += 1
                                    if fl & 0x2000: c['light_shadowmap_priority'] += 1
                                    if fl & 0x100: c['light_lightshadowcast'] += 1
                                    if fl & 0x80: c['light_objshadow'] += 1
                                    if struct.unpack_from('>i', m, c2s + 0x84)[0] not in (-1, 0): c['light_cookie'] += 1
                                    if struct.unpack_from('>i', m, c2s + 0xf8)[0] not in (-1, 0): c['light_tex2'] += 1
                                    lightkinds[(kind, fl & 0x2380)] += 1
                elif rtype == 0x101 and 'postfxinit' in name.lower():
                    c['postfx_initializers'] += 1
                elif rtype == 0x106:
                    c['zones'] += 1
                elif rtype == 0x103:
                    c['vfx_emitters'] += 1
    m.close(); f.close()
    c['pack'] = pack
    percsv.append(c)
    for k, v in c.items():
        if k != 'pack': tot[k] += v

keys = ['pack'] + sorted(k for k in tot)
w = csv.DictWriter(open(os.path.join(here, 'pak_render_census.csv'), 'w', newline='', encoding='utf-8'), fieldnames=keys)
w.writeheader()
for c in percsv:
    w.writerow({k: c.get(k, '') for k in keys})
print('TOTALS')
for k in keys[1:]:
    print(f'  {k:<28} {tot[k]:>10,}')
print('light kind x shadow flags:', lightkinds.most_common(12))
print('texture 401 (size, w0, w1) top:', texfmt.most_common(12))
