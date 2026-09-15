import struct, glob, os, mmap, collections, sys, csv

root = os.environ.get('EOT_EXTRACT', r'D:\EOT_Extract\extracted')
MAXDEPTH = int(sys.argv[1]) if len(sys.argv) > 1 else 3

def children(m, start, end):
    o = start
    while o + 12 <= end:
        t, a, s = struct.unpack_from('>IIi', m, o)
        if s < 0 or o + 12 + s > end:
            return
        yield t, a, o + 12, o + 12 + s
        o += 12 + s

stats = collections.defaultdict(lambda: [0, 0, set()])
top_seq = {}
bad = []

def walk(m, start, end, depth, parent, pack):
    for t, a, s, e in children(m, start, end):
        k = (depth, parent, t)
        st = stats[k]
        st[0] += 1; st[1] += e - s; st[2].add(pack)
        if depth < MAXDEPTH and e - s >= 12:
            t2, a2, s2 = struct.unpack_from('>IIi', m, s)
            if 0 < t2 < 0x10000 and 0 <= s2 <= e - s - 12:
                walk(m, s, e, depth + 1, t, pack)

files = sorted(glob.glob(os.path.join(root, '*.ext')))
for path in files:
    pack = os.path.basename(path)[:-4]
    with open(path, 'rb') as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        t, a, s = struct.unpack_from('>IIi', m, 0)
        if t != 1 or 12 + s != len(m):
            bad.append((pack, t, s, len(m))); m.close(); continue
        seq = [c[0] for c in children(m, 12, 12 + s)]
        top_seq[pack] = seq
        walk(m, 12, 12 + s, 1, 1, pack)
        m.close()

print('packs', len(files), 'bad', bad[:5])
out = csv.writer(open(os.path.join(os.getcwd(), 'pak_census.csv'), 'w', newline=''))
out.writerow(['depth', 'parent', 'type', 'count', 'bytes', 'packs'])
for k in sorted(stats):
    c, b, p = stats[k]
    out.writerow([k[0], k[1], k[2], c, b, len(p)])
print('depth1 types:')
for k in sorted(stats):
    if k[0] == 1:
        c, b, p = stats[k]
        print(f'  type {k[2]:>5}  n={c:>7}  bytes={b:>12,}  packs={len(p)}')
print('depth2 by parent:')
for k in sorted(stats):
    if k[0] == 2:
        c, b, p = stats[k]
        print(f'  {k[1]:>5} -> {k[2]:>5}  n={c:>7}  bytes={b:>12,}  packs={len(p)}')
sigs = collections.Counter(tuple(v) for v in top_seq.values())
print('distinct top sequences', len(sigs))
for s, n in sigs.most_common(5):
    print(n, s[:40])
