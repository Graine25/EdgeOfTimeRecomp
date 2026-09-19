import sys, os, time
import numpy as np
from scipy.spatial import cKDTree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from retail_ref import load as load_retail
from skin import fk, rotvec_matrix, skin_matrices, blend, apply
from s99_pose import retail_sample_mask
from quickrender import render

def surface_samples(P, tris, J, W, per_area=400000.0, rng=None, vmask=None):
    rng = rng or np.random.default_rng(2)
    a, b, c = P[tris[:, 0]], P[tris[:, 1]], P[tris[:, 2]]
    fn = np.cross(b - a, c - a)
    area = 0.5 * np.linalg.norm(fn, axis=1)
    fn = fn / (2 * area[:, None] + 1e-12)
    counts = np.maximum(1, np.round(area * per_area)).astype(int)
    tri_idx = np.repeat(np.arange(len(tris)), counts)
    r1 = np.sqrt(rng.random(len(tri_idx))); r2 = rng.random(len(tri_idx))
    bary = np.stack([1 - r1, r1 * (1 - r2), r1 * r2], 1)
    pts = (bary[:, :1] * a[tri_idx] + bary[:, 1:2] * b[tri_idx] + bary[:, 2:3] * c[tri_idx])
    SJ = np.concatenate([J[tris[tri_idx, k]] for k in range(3)], 1)
    SW = np.concatenate([W[tris[tri_idx, k]] * bary[:, k:k + 1] for k in range(3)], 1)
    SN = fn[tri_idx]
    VN = np.zeros_like(P)
    for k in range(3):
        np.add.at(VN, tris[:, k], fn)
    VN /= np.linalg.norm(VN, axis=1, keepdims=True) + 1e-12
    if vmask is None:
        vmask = np.ones(len(P), bool)
    Pm, Jm, Wm, VNm = P[vmask], J[vmask], W[vmask], VN[vmask]
    pts = np.concatenate([pts, Pm]); SJ = np.concatenate([SJ, np.concatenate([Jm, Jm, Jm], 1)])
    SW = np.concatenate([SW, np.concatenate([Wm, Wm * 0, Wm * 0], 1)]); SN = np.concatenate([SN, VNm])
    return pts, SJ, SW, SN

def nearest_facing(tree, SN, FP, FN, k=12, min_dot=0.2, slack=0.012):
    d, nn = tree.query(FP, k=k)
    dots = np.einsum('nkj,nj->nk', SN[nn], FN)
    ok = dots > min_dot
    first = np.argmax(ok, axis=1)
    rows = np.arange(len(FP))
    use = ok.any(1) & (d[rows, first] <= d[:, 0] + slack)
    pick = np.where(use, first, 0)
    return d[rows, pick], nn[rows, pick], use.mean()

def top4(SJ, SW):
    n = len(SJ)
    J = np.zeros((n, 4), np.int64); W = np.zeros((n, 4))
    for i in range(n):
        acc = {}
        for j, w in zip(SJ[i], SW[i]):
            if w > 0: acc[j] = acc.get(j, 0.0) + w
        items = sorted(acc.items(), key=lambda kv: -kv[1])[:4]
        s = sum(w for _, w in items)
        for k, (j, w) in enumerate(items):
            J[i, k] = j; W[i, k] = w / s
    return J, W

def adjacency(n, tris):
    import scipy.sparse as sp
    i = np.concatenate([tris[:, 0], tris[:, 1], tris[:, 2], tris[:, 1], tris[:, 2], tris[:, 0]])
    j = np.concatenate([tris[:, 1], tris[:, 2], tris[:, 0], tris[:, 0], tris[:, 1], tris[:, 2]])
    A = sp.coo_matrix((np.ones(len(i)), (i, j)), shape=(n, n)).tocsr()
    A.data[:] = 1.0
    return A

def propagate_from_fitted(SJ, SW, d, tris, good_dist):
    n = len(d)
    A = adjacency(n, tris)
    src = np.where(d <= good_dist, np.arange(n), -1)
    frontier = np.where(src >= 0)[0]
    indptr, indices = A.indptr, A.indices
    rounds = 0
    while len(frontier):
        nxt = []
        for v in frontier:
            for u in indices[indptr[v]:indptr[v + 1]]:
                if src[u] < 0:
                    src[u] = src[v]; nxt.append(u)
        frontier = np.array(nxt, np.int64); rounds += 1
    unreached = src < 0
    src[unreached] = np.arange(n)[unreached]
    print('propagation: %d vertices beyond %.3f re-assigned in %d rounds, %d unreachable' % ((d > good_dist).sum(), good_dist, rounds, unreached.sum()))
    return SJ[src], SW[src]

def weld_clusters(P, tol=1e-4):
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components
    pairs = cKDTree(P).query_pairs(tol, output_type='ndarray')
    n = len(P)
    g = coo_matrix((np.ones(len(pairs)), (pairs[:, 0], pairs[:, 1])), shape=(n, n))
    nc, lab = connected_components(g, directed=False)
    return lab, nc

def weight_matrix(J, W):
    import scipy.sparse as sp
    n = len(J)
    bones = np.unique(J[W > 0])
    col = {b: k for k, b in enumerate(bones)}
    nz = W.ravel() > 0
    rows = np.repeat(np.arange(n), J.shape[1])[nz]; cols = np.array([col[b] for b in J.ravel()[nz]]); vals = W.ravel()[nz]
    return sp.coo_matrix((vals, (rows, cols)), shape=(n, len(bones))).tocsr(), bones

def weights_from_matrix(D, bones, k=4):
    D = D.tocsr()
    n = D.shape[0]
    J2 = np.zeros((n, k), np.int64); W2 = np.zeros((n, k))
    for i in range(n):
        row = D.getrow(i); idx = row.indices; w = row.data
        order = np.argsort(-w)[:k]
        ww = w[order]; ww /= ww.sum()
        J2[i, :len(order)] = bones[idx[order]]; W2[i, :len(order)] = ww
    return J2, W2

def share_weights(J, W, cluster, nc):
    import scipy.sparse as sp
    D, bones = weight_matrix(J, W)
    n = len(J)
    C = sp.coo_matrix((np.ones(n), (cluster, np.arange(n))), shape=(nc, n)).tocsr()
    cnt = np.asarray(C.sum(1)).ravel()
    Dc = sp.diags(1.0 / cnt) @ (C @ D)
    return weights_from_matrix(C.T @ Dc, bones)

def smooth_weights(J, W, tris, iters=3, lam=0.5, cluster=None, nc=None):
    import scipy.sparse as sp
    n = len(J)
    D, bones = weight_matrix(J, W)
    if cluster is None:
        cluster = np.arange(n); nc = n
    C = sp.coo_matrix((np.ones(n), (cluster, np.arange(n))), shape=(nc, n)).tocsr()
    cnt = np.asarray(C.sum(1)).ravel()
    D = sp.diags(1.0 / cnt) @ (C @ D)
    A = adjacency(nc, cluster[tris])
    deg = np.asarray(A.sum(1)).ravel(); deg[deg == 0] = 1
    for _ in range(iters):
        D = (1 - lam) * D + lam * sp.diags(1.0 / deg) @ (A @ D)
    return weights_from_matrix(C.T @ D, bones)

SHELL_MESHES = ('GeometryNode_23', 'GeometryNode_56')
BODY_MESHES = ('GeometryNode_5', 'GeometryNode_41')

def shell_follows_body(J, W, fb, reach=0.015):
    names = list(fb['mesh_names']); mesh_of = fb['mesh_of']; P = fb['P']
    body = np.isin(mesh_of, [names.index(n) for n in BODY_MESHES if n in names])
    shell = np.isin(mesh_of, [names.index(n) for n in SHELL_MESHES if n in names])
    bidx = np.where(body)[0]; sidx = np.where(shell)[0]
    tree = cKDTree(P[bidx])
    d, nn = tree.query(P[sidx])
    close = d <= reach
    J = J.copy(); W = W.copy()
    J[sidx[close]] = J[bidx[nn[close]]]; W[sidx[close]] = W[bidx[nn[close]]]
    src = np.full(len(P), -1, np.int64)
    src[sidx[close]] = sidx[close]
    A = adjacency(len(P), fb['tris'])
    indptr, indices = A.indptr, A.indices
    frontier = list(sidx[close]); rounds = 0
    while frontier:
        nxt = []
        for v in frontier:
            for u in indices[indptr[v]:indptr[v + 1]]:
                if shell[u] and src[u] < 0:
                    src[u] = src[v]; nxt.append(u)
        frontier = nxt; rounds += 1
    far = sidx[~close]
    reached = far[src[far] >= 0]
    J[reached] = J[src[reached]]; W[reached] = W[src[reached]]
    print('shell: %d of %d vertices within %.0f mm of the body took its weights, %d propagated in %d rounds, %d kept their own' % (
        close.sum(), len(sidx), reach * 1000, len(reached), rounds, len(far) - len(reached)))
    return J, W

def main():
    ref = load_retail('SpiderMan2099[Hi]')
    fb = np.load('build/s99_aligned.npz')
    deltas = np.load('build/s99_pose.npz')['deltas']
    names = ref['bone_names']
    world = fk(ref['local'], ref['parent'], {b: rotvec_matrix(deltas[b]) for b in range(len(names)) if np.any(deltas[b])})
    M = skin_matrices(world, ref['invbind'])
    RP = apply(blend(M, ref['J'], ref['W']), ref['P'])
    mask = retail_sample_mask(ref)
    tris = ref['tris'][mask[ref['tris']].all(1)]
    t = time.time()
    pts, SJ, SW, SN = surface_samples(RP, tris, ref['J'], ref['W'], vmask=mask)
    tree = cKDTree(pts)
    FP = fb['P']
    FN = fb['N'] / (np.linalg.norm(fb['N'], axis=1, keepdims=True) + 1e-12)
    d, nn, frac = nearest_facing(tree, SN, FP, FN)
    print('%d surface samples; facing match for %.1f%% of vertices; nearest distance: mean %.4f, p95 %.4f, max %.4f (%.1fs)' % (
        len(pts), 100 * frac, d.mean(), np.percentile(d, 95), d.max(), time.time() - t))
    SJn, SWn = propagate_from_fitted(SJ[nn], SW[nn], d, fb['tris'], 0.03)
    J, W = top4(SJn, SWn)
    cluster, nc = weld_clusters(FP)
    print('weld: %d vertices in %d clusters (%d coincident copies)' % (len(FP), nc, len(FP) - nc))
    J, W = smooth_weights(J, W, fb['tris'], iters=4, cluster=cluster, nc=nc)
    J, W = shell_follows_body(J, W, fb)
    J, W = share_weights(J, W, cluster, nc)
    print('weights: %.1f%% single-bone, mean nonzero %.2f' % (100 * (W[:, 1] == 0).mean(), (W > 0).sum(1).mean()))
    B = blend(M, J, W)
    Binv = np.linalg.inv(B)
    P_bind = apply(Binv, FP)
    N_bind = np.einsum('nji,nj->ni', B[:, :3, :3], fb['N'])
    N_bind /= np.linalg.norm(N_bind, axis=1, keepdims=True) + 1e-9
    print('bind-pose bbox', P_bind.min(0).round(3), P_bind.max(0).round(3), '(retail', ref['P'].min(0).round(3), ref['P'].max(0).round(3), ')')
    used = np.unique(J[W > 0])
    print('bones used: %d of %d' % (len(used), len(names)))
    np.savez('build/s99_rigged.npz', P=P_bind, N=N_bind, UV=fb['UV'], J=J, W=W, tris=fb['tris'], mesh_of=fb['mesh_of'], mesh_names=fb['mesh_names'], dist=d)
    render([(P_bind, fb['tris'], (200, 60, 60)), (ref['P'], ref['tris'], (60, 120, 220))], 'build/unposed.png', views=('front', 'side'))
    render([(P_bind, fb['tris'], (200, 60, 60))], 'build/unposed_solo.png', views=('front', 'side', 'back'))

if __name__ == '__main__':
    main()
