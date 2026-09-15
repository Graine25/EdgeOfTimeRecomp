import struct, glob, os, mmap, collections, re, csv, sys

root = os.environ.get('EOT_EXTRACT', r'D:\EOT_Extract\extracted')
out_csv = os.path.join(os.getcwd(), 'pak_objects.csv')

def children(m, start, end):
    o = start
    while o + 12 <= end:
        t, a, s = struct.unpack_from('>IIi', m, o)
        if s < 0 or o + 12 + s > end:
            return
        yield t, a, o + 12, o + 12 + s
        o += 12 + s

lists = collections.defaultdict(lambda: {'n': 0, 'rtypes': collections.Counter(), 'subs': collections.Counter(), 'names': [], 'packs': set(), 'bytes': 0})
objs = collections.defaultdict(lambda: {'n': 0, 'names': [], 'packs': set(), 'sizes': collections.Counter()})
blobs = collections.defaultdict(lambda: {'n': 0, 'bytes': 0, 'rtypes': collections.Counter()})
objrows = []

for path in sorted(glob.glob(os.path.join(root, '*.ext'))):
    pack = os.path.basename(path)[:-4]
    f = open(path, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    t, a, s = struct.unpack_from('>IIi', m, 0)
    for tt, ta, ts, te in children(m, 12, 12 + s):
        if tt == 38:
            name = rtype = None; ptype = None; psize = 0
            for kt, ka, ks, ke in children(m, ts, te):
                if kt == 5006:
                    rtype = struct.unpack_from('>I', m, ks + 4)[0]
                else:
                    ptype = kt; psize = ke - ks
            b = blobs[ptype]; b['n'] += 1; b['bytes'] += psize; b['rtypes'][rtype] += 1
            continue
        L = lists[tt]; L['packs'].add(pack)
        for kt, ka, ks, ke in children(m, ts, te):
            if kt != 5005:
                continue
            L['n'] += 1; L['bytes'] += ke - ks
            sub = list(children(m, ks, ke))
            rtype = None; name = '?'
            for st, sa, ss, se in sub:
                if st == 5006:
                    rtype = struct.unpack_from('>I', m, ss + 4)[0]
                    name = m[ss + 24:ss + 88].split(b'\0')[0].decode('ascii', 'replace')
                else:
                    L['subs'][st] += 1
            L['rtypes'][rtype] += 1
            if len(L['names']) < 8:
                L['names'].append(name)
            if tt == 4:
                O = objs[rtype]; O['n'] += 1; O['packs'].add(pack); O['sizes'][ke - ks] += 1
                if len(O['names']) < 10:
                    O['names'].append(name)
                objrows.append((pack, rtype, ke - ks, name))
    m.close(); f.close()

print('LIST TYPES')
for tt in sorted(lists):
    L = lists[tt]
    print(f'list {tt:>3}: recs={L["n"]:>6} bytes={L["bytes"]:>13,} packs={len(L["packs"]):>3} rtypes={dict(L["rtypes"].most_common(4))} subs={dict(L["subs"].most_common(5))}')
    print(f'          names={L["names"][:6]}')
print('\nLEVEL OBJECT TYPES (list 4)')
for rt in sorted(objs, key=lambda k: -objs[k]['n']):
    O = objs[rt]
    print(f'  type {rt:#06x}: n={O["n"]:>6} packs={len(O["packs"]):>3} sizes={O["sizes"].most_common(3)} names={O["names"][:6]}')
print('\nBLOB TYPES (type-38 tail)')
for pt in sorted(blobs, key=lambda k: -blobs[k]['bytes']):
    b = blobs[pt]
    print(f'  blob {pt:>5}: n={b["n"]:>6} bytes={b["bytes"]:>14,} rtypes={dict(b["rtypes"].most_common(3))}')
w = csv.writer(open(out_csv, 'w', newline=''))
w.writerow(['pack', 'objtype', 'size', 'name'])
w.writerows(objrows)
print('objects csv rows', len(objrows))
