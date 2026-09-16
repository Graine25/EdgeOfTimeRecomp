import struct, glob, os, mmap, collections, re, csv

root = os.environ.get('EOT_EXTRACT', r'D:\EOT_Extract\extracted')

def children(m, start, end):
    o = start
    while o + 12 <= end:
        t, a, s = struct.unpack_from('>IIi', m, o)
        if s < 0 or o + 12 + s > end:
            return
        yield t, a, o + 12, o + 12 + s
        o += 12 + s

def role(name):
    n = re.sub(r'\[(Hi|HI|hi|Lo|Med|Mid|mid|lo)\]$', '', name)
    n = re.sub(r'\d+$', '', n)
    if 'CLUT' in n.upper(): return 'clut'
    m = re.search(r'_([A-Za-z]{1,3})$', n)
    if not m: return 'other'
    s = m.group(1).upper()
    return {'LM': 'LM', 'MK': 'MK', 'N': 'N', 'NM': 'N', 'S': 'S', 'SP': 'S', 'D': 'D', 'DA': 'DA', 'E': 'E', 'EM': 'E', 'G': 'G',
            'O': 'O', 'H': 'H', 'A': 'A', 'R': 'R', 'C': 'C', 'CM': 'CM', 'IM': 'IM', 'M': 'MK'}.get(s, 'other')

FMT = {0:'1_REV',1:'1',2:'8',3:'1_5_5_5',4:'5_6_5',5:'6_5_5',6:'8_8_8_8',7:'2_10_10_10',8:'8_A',9:'8_B',10:'8_8',13:'16_16_EDRAM',14:'8_8_8_8_A',15:'4_4_4_4',
       16:'10_11_11',17:'11_11_10',18:'DXT1',19:'DXT2_3',20:'DXT4_5',21:'16_16_16_16_EDRAM',22:'24_8',23:'24_8_FLOAT',24:'16',25:'16_16',26:'16_16_16_16',
       30:'16_FLOAT',31:'16_16_FLOAT',32:'16_16_16_16_FLOAT',33:'32',36:'32_FLOAT',49:'DXN',50:'8_8_8_8_AS_16',51:'DXT1_AS_16',52:'DXT2_3_AS_16',53:'DXT4_5_AS_16',
       54:'2_10_10_10_AS_16',58:'DXT3A',59:'DXT5A',60:'CTX1',61:'DXT3A_AS_1111',62:'8_8_8_8_GAMMA_EDRAM',63:'2_10_10_10_FLOAT_EDRAM'}

rows = []
groups = collections.Counter(); samples = {}
hdrs = collections.Counter()
for path in sorted(glob.glob(os.path.join(root, '*.ext'))):
    pack = os.path.basename(path)[:-4]
    f = open(path, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    t, a, s = struct.unpack_from('>IIi', m, 0)
    for tt, ta, ts, te in children(m, 12, 12 + s):
        if tt != 38:
            continue
        name = None; blob = None
        for kt, ka, ks, ke in children(m, ts, te):
            if kt == 5006:
                name = m[ks + 24:ks + 88].split(b'\0')[0].decode('ascii', 'replace')
            elif kt == 405:
                blob = (ks, ke)
        if not name or not blob:
            continue
        ks, ke = blob
        nw = min(24, (ke - ks) // 4)
        u = struct.unpack_from('>%dI' % nw, m, ks)
        k = next((i for i in range(2, nw) if (u[i] >> 20) in (0x1A2, 0x182, 0x280, 0x1A0, 0x180)), None)
        if k is None:
            k = next((i for i in range(4, nw) if (u[i] & 0x3F) in FMT and (u[i] >> 24) in (0x1a, 0x18, 0x28, 0x1c)), None)
        if k is None:
            hdrs[('nok', u[0])] += 1
            continue
        fmt = u[k] & 0x3F; endian = (u[k] >> 6) & 3
        W, H, mips = u[k - 4], u[k - 3], u[k - 2]
        r = role(name)
        key = (u[0], FMT.get(fmt, fmt), r)
        groups[key] += 1
        if key not in samples:
            samples[key] = (name, ke - ks, W, H, mips, [f'{x:08x}' for x in u[max(0, k - 5):k + 6]])
        rows.append((pack, name, r, u[0], W, H, mips, FMT.get(fmt, fmt), endian, ke - ks, f'{u[k-1]:08x}', f'{u[k]:08x}', f'{u[k+1]:08x}' if k + 1 < nw else '', f'{u[k+2]:08x}' if k + 2 < nw else '', f'{u[k+3]:08x}' if k + 3 < nw else ''))
    m.close(); f.close()

print('textures', len(rows), 'unparsed', hdrs)
w = csv.writer(open(os.path.join(os.getcwd(), 'texture_headers.csv'), 'w', newline='', encoding='utf-8'))
w.writerow(['pack', 'name', 'role', 'type', 'width', 'height', 'mips', 'format', 'endian', 'blob_bytes', 'dw_km1', 'dw_k', 'dw_k1', 'dw_k2', 'dw_k3'])
w.writerows(rows)
print('by (type, format, role):')
for key, n in groups.most_common(45):
    print(f'  {n:>6} type={key[0]} fmt={key[1]:<10} role={key[2]:<6} e.g. {samples[key]}')
