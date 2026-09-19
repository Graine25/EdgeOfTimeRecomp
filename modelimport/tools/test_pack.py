import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from retail_ref import load
from geo_pack import pack_vertices, stripify
from pakwriter import Pak
import x360_geo as G

name = 'SpiderMan2099_CEO_No_Suit_Civilian[Hi]'
ref = load(name)
mi = 0
sel = ref['mesh_of'] == mi
d = ref['descs'][mi]
bg = Pak('D:/EOT_Extract/extracted/BaseGameplay.ext')
m = {mm[0]: mm for mm in G.get_models(bg.d)}[name]
_, vb, vbsize, ib, ibsize = m
raw = bg.d[vb + d['vboff']: vb + d['vboff'] + d['nv'] * d['stride']]
pal = {b: i for i, b in enumerate(d['pal'])}
J = np.vectorize(lambda b: pal.get(int(b), 0))(ref['J'][sel])
TW = np.where(ref['TW'][sel] == 3, -1, 1)
packed = pack_vertices(ref['P'][sel], ref['N'][sel], ref['T'][sel], TW, ref['UV'][sel], J, ref['W'][sel])
a = np.frombuffer(raw, np.uint8).reshape(-1, 32); b = np.frombuffer(packed, np.uint8).reshape(-1, 32)
print('vertex bytes identical: %.4f' % (a == b).mean())
pairs = lambda m: [sorted(zip(m[i, 24:28].tolist(), m[i, 28:32].tolist())) for i in range(len(m))]
same = np.mean([pa == pb for pa, pb in zip(pairs(a), pairs(b))])
print('skin (joint, weight) pair sets identical: %.4f; byte 0 zero in retail: %.4f, ours: %.4f' % (same, (a[:, 28] == 0).mean(), (b[:, 28] == 0).mean()))
for nm, sl in (('pos', slice(0, 12)), ('nrm', slice(12, 16)), ('tan', slice(16, 20)), ('uv', slice(20, 24)), ('joints', slice(24, 28)), ('weights', slice(28, 32))):
    rows = (a[:, sl] == b[:, sl]).all(1)
    print('  %-8s rows identical %.4f' % (nm, rows.mean()))
    for k in np.where(~rows)[0][:3]:
        print('     v%d retail %s ours %s' % (k, a[k, sl].tobytes().hex(), b[k, sl].tobytes().hex()))
first = np.where(sel)[0][0]
tris = ref['tris'][sel[ref['tris'][:, 0]]] - first
strip = stripify(tris)
back = np.array(G.strip_to_tris(strip)).reshape(-1, 3)
canon = lambda T: set(tuple(np.roll(t, -np.argmin(t))) for t in T)
print('strip: %d tris -> %d indices (%.2f/tri, retail %d), decoded set equal: %s' % (
    len(tris), len(strip), len(strip) / len(tris), d['nidx'], canon(tris) == canon(back)))
