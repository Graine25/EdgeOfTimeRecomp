import struct, glob, os, mmap, re, csv, collections, sys

root = os.environ.get('EOT_EXTRACT', r'D:\EOT_Extract\extracted')
out = os.path.join(os.getcwd(), 'materials.csv')

def children(m, start, end):
    o = start
    while o + 12 <= end:
        t, a, s = struct.unpack_from('>IIi', m, o)
        if s < 0 or o + 12 + s > end:
            return
        yield t, a, o + 12, o + 12 + s
        o += 12 + s

FEATURES = ['LightPrePassColor', 'ShadowMap', 'irradianceTexObj', 'kPS_LightMapKonsts', 'SceneColorSampler', 'DropShadowSamp',
            'kPS_Light0Att', 'kPS_Light2Att', 'kPS_Light4Att', 'kPS_LightGridBNeg', 'kPS_Distortion', 'kPS_EncodeHDRAlphaRef',
            'kPS_RimLightColor', 'kPS_EmissiveMapFactor', 'kPS_SpecColor', 'kPS_BumpFactor', 'kPS_FogColor', 'kPS_StageColor1',
            'kVS_Bones', 'kVS_InstanceParams', 'kVS_UVTransform00', 'kVS_Extrude', 'kVS_DropShadows_Color', 'kPS_EdgeExtrusionColor']
FEAT_B = [f.encode() for f in FEATURES]
path_re = re.compile(rb'[A-Za-z]:\\[^\x00]{4,200}?\.(?:dds|DDS|Dds|tga|TGA|png|PNG)')

def role(basename):
    n = re.sub(r'\.(dds|DDS|Dds|tga|TGA|png|PNG)$', '', basename)
    n = re.sub(r'\[(Hi|HI|hi|Lo|Med|Mid|mid|lo)\]$', '', n)
    n = re.sub(r'\d+$', '', n)
    if 'CLUT' in n.upper(): return 'clut'
    m = re.search(r'_([A-Za-z]{1,3})$', n)
    if not m: return 'other'
    s = m.group(1).upper()
    return {'LM': 'LM', 'MK': 'MK', 'N': 'N', 'NM': 'N', 'S': 'S', 'SP': 'S', 'D': 'D', 'DA': 'DA', 'E': 'E', 'EM': 'E', 'G': 'G',
            'O': 'O', 'H': 'H', 'A': 'A', 'R': 'R', 'C': 'C', 'CM': 'CM', 'IM': 'IM', 'M': 'MK'}.get(s, 'other')

def find_804(m, start, end, depth, acc):
    for t, a, s, e in children(m, start, end):
        if t == 804:
            acc.append((a, s, e))
        elif depth < 4 and e - s > 12:
            t2, a2, s2 = struct.unpack_from('>IIi', m, s)
            if 0 < t2 < 10000 and 0 <= s2 <= e - s - 12:
                find_804(m, s, e, depth + 1, acc)

w = csv.writer(open(out, 'w', newline='', encoding='utf-8'))
w.writerow(['pack', 'owner_kind', 'owner', 'material', 'attr', 'size', 'n_vs', 'n_ps', 'textures', 'roles'] + FEATURES)
tot = collections.Counter()
for path in sorted(glob.glob(os.path.join(root, '*.ext'))):
    pack = os.path.basename(path)[:-4]
    f = open(path, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    t, a, s = struct.unpack_from('>IIi', m, 0)
    for tt, ta, ts, te in children(m, 12, 12 + s):
        if tt not in (7, 48):
            continue
        for kt, ka, ks, ke in children(m, ts, te):
            if kt != 5005:
                continue
            owner = '?'; mats = []
            for st, sa, ss, se in children(m, ks, ke):
                if st == 5006:
                    owner = m[ss + 24:ss + 88].split(b'\0')[0].decode('ascii', 'replace')
                elif st == 801 or st == 3003:
                    find_804(m, ss, se, 0, mats)
            kind = 'model' if tt == 7 else 'env'
            for a4, s4, e4 in mats:
                b = m[s4:e4]
                n_ps = b.count(b'\x10\x2a\x11\x00'); n_vs = b.count(b'\x10\x2a\x11\x01')
                strs = re.findall(rb'[\x20-\x7e]{4,}', b[:512])
                name = strs[0].decode() if strs else '?'
                paths = [os.path.basename(p.decode('ascii', 'replace')) for p in path_re.findall(b)]
                seen = []
                for p in paths:
                    if p not in seen: seen.append(p)
                roles = '+'.join(sorted(set(role(p) for p in seen)))
                feats = [1 if fb in b else 0 for fb in FEAT_B]
                w.writerow([pack, kind, owner, name, f'{a4:#x}', e4 - s4, n_vs, n_ps, '|'.join(seen), roles] + feats)
                tot['materials'] += 1; tot['mat_' + kind] += 1; tot['ps'] += n_ps; tot['vs'] += n_vs
                for fn, fv in zip(FEATURES, feats):
                    if fv: tot['f_' + fn] += 1
                tot['roles ' + roles] += 1
    m.close(); f.close()
print({k: v for k, v in tot.items() if not k.startswith('roles ')})
print('top role combos:')
for k, v in sorted(((k, v) for k, v in tot.items() if k.startswith('roles ')), key=lambda x: -x[1])[:25]:
    print(f'  {v:>6} {k[6:]}')
