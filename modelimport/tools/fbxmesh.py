import sys, os, math, collections
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fbxparse import parse

def rot_xyz(deg):
    x, y, z = [math.radians(a) for a in deg]
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return Rz @ Ry @ Rx

def local_matrix(model):
    t = np.zeros(3); r = np.zeros(3); s = np.ones(3); pre = np.zeros(3); post = np.zeros(3)
    p70 = model.first('Properties70')
    if p70:
        for p in p70.children:
            k = p.props[0]
            if k == 'Lcl Translation': t = np.array(p.props[4:7], float)
            elif k == 'Lcl Rotation': r = np.array(p.props[4:7], float)
            elif k == 'Lcl Scaling': s = np.array(p.props[4:7], float)
            elif k == 'PreRotation': pre = np.array(p.props[4:7], float)
            elif k == 'PostRotation': post = np.array(p.props[4:7], float)
    R = rot_xyz(pre) @ rot_xyz(r) @ np.linalg.inv(rot_xyz(post))
    M = np.eye(4)
    M[:3, :3] = R @ np.diag(s)
    M[:3, 3] = t
    return M

def load(path, verbose=True):
    v, root = parse(path)
    objects = root.first('Objects'); conns = root.first('Connections')
    by_id = {o.props[0]: o for o in objects.children if o.props}
    parent_of = {}; kids = collections.defaultdict(list)
    for c in conns.children:
        if c.name == 'C' and c.props[0] == 'OO':
            parent_of[c.props[1]] = c.props[2]; kids[c.props[2]].append(c.props[1])
    def name_of(o): return o.props[1].split('\x00')[0]
    def world(mid):
        M = np.eye(4); cur = mid
        chain = []
        while cur in by_id and by_id[cur].name == 'Model':
            chain.append(cur); cur = parent_of.get(cur, 0)
        for m in chain:
            pass
        for m in reversed(chain):
            M = M @ local_matrix(by_id[m])
        return M
    meshes = []
    for o in objects.children:
        if o.name != 'Model' or o.props[2] != 'Mesh':
            continue
        mid = o.props[0]
        geos = [by_id[c] for c in kids.get(mid, []) if c in by_id and by_id[c].name == 'Geometry']
        mats = [by_id[c] for c in kids.get(mid, []) if c in by_id and by_id[c].name == 'Material']
        if not geos:
            continue
        g = geos[0]
        M = world(mid)
        V = g.first('Vertices').props[0].reshape(-1, 3)
        idx = g.first('PolygonVertexIndex').props[0]
        tris = []; corner = []
        poly = []
        for i, ix in enumerate(idx):
            poly.append((i, ix if ix >= 0 else ~ix))
            if ix < 0:
                for k in range(1, len(poly) - 1):
                    tris.append((poly[0][1], poly[k][1], poly[k + 1][1]))
                    corner.append((poly[0][0], poly[k][0], poly[k + 1][0]))
                poly = []
        tris = np.array(tris, np.int64); corner = np.array(corner, np.int64)
        def layer(kind, key):
            L = g.first(kind)
            if not L: return None, None, None
            data = L.first(key).props[0]
            mapping = L.first('MappingInformationType').props[0]
            ref = L.first('ReferenceInformationType').props[0]
            index = L.first(key + 'Index')
            return data, mapping, (index.props[0] if index else None), ref
        n = layer('LayerElementNormal', 'Normals'); uv = layer('LayerElementUV', 'UV')
        def per_corner(lay, width):
            data, mapping, index, ref = lay
            if data is None: return None
            data = data.reshape(-1, width)
            if index is not None and ref == 'IndexToDirect':
                data = data[index]
            if mapping == 'ByPolygonVertex':
                return data[corner]
            if mapping == 'ByControlPoint':
                return data[tris]
            return None
        Nc = per_corner(n, 3); UVc = per_corner(uv, 2)
        Pw = (M[:3, :3] @ V.T).T + M[:3, 3]
        Rn = M[:3, :3] / np.cbrt(abs(np.linalg.det(M[:3, :3])))
        keys = {}; outP = []; outN = []; outUV = []; outT = np.zeros_like(tris)
        for t in range(len(tris)):
            for c in range(3):
                vi = tris[t, c]
                nn = tuple(np.round(Nc[t, c], 3)) if Nc is not None else ()
                uu = tuple(np.round(UVc[t, c], 5)) if UVc is not None else ()
                key = (vi, nn, uu)
                j = keys.get(key)
                if j is None:
                    j = len(outP); keys[key] = j
                    outP.append(Pw[vi])
                    outN.append((Rn @ Nc[t, c]) if Nc is not None else np.zeros(3))
                    outUV.append(UVc[t, c] if UVc is not None else np.zeros(2))
                outT[t, c] = j
        m = dict(name=name_of(o), P=np.array(outP), N=np.array(outN), UV=np.array(outUV), tris=outT,
                 material=name_of(mats[0]) if mats else '', srcverts=len(V))
        meshes.append(m)
        if verbose:
            lo, hi = m['P'].min(0), m['P'].max(0)
            print('%-20s %6d src verts -> %6d split, %6d tris, mat %-24s bbox %s .. %s' % (
                m['name'], len(V), len(outP), len(outT), m['material'][:24], np.round(lo, 3), np.round(hi, 3)))
    return meshes

if __name__ == '__main__':
    load(sys.argv[1])
