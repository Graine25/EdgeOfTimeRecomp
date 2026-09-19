import sys
import numpy as np
from PIL import Image
sys.path.insert(0, 'tools')
import capture_pose as cp

X0, Y0, X1, Y1 = 1000, 560, 1440, 1320
SCALE = 2

def rasterize(sx, sy, z, w, tris, cull=True):
    Wd, Hd = (X1 - X0) * SCALE, (Y1 - Y0) * SCALE
    zb = np.full((Hd, Wd), -np.inf); ib = np.full((Hd, Wd), -1, np.int64); fb = np.zeros((Hd, Wd), bool)
    X = (sx - X0) * SCALE; Y = (sy - Y0) * SCALE
    A = np.stack([X[tris[:, 0]], Y[tris[:, 0]]], 1); B = np.stack([X[tris[:, 1]], Y[tris[:, 1]]], 1); C = np.stack([X[tris[:, 2]], Y[tris[:, 2]]], 1)
    area = (B[:, 0] - A[:, 0]) * (C[:, 1] - A[:, 1]) - (B[:, 1] - A[:, 1]) * (C[:, 0] - A[:, 0])
    front = area < 0
    ok = (w[tris].min(1) > 0) & (np.abs(area) > 1e-9)
    zt = z[tris]
    for t in np.where(ok)[0]:
        a, b, c = A[t], B[t], C[t]
        x0 = max(int(np.floor(min(a[0], b[0], c[0]))), 0); x1 = min(int(np.ceil(max(a[0], b[0], c[0]))), Wd - 1)
        y0 = max(int(np.floor(min(a[1], b[1], c[1]))), 0); y1 = min(int(np.ceil(max(a[1], b[1], c[1]))), Hd - 1)
        if x1 < x0 or y1 < y0:
            continue
        xs = np.arange(x0, x1 + 1) + 0.5; ys = np.arange(y0, y1 + 1) + 0.5
        px, py = np.meshgrid(xs, ys)
        e0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0])
        e1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0])
        e2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0])
        if area[t] > 0:
            inside = (e0 >= 0) & (e1 >= 0) & (e2 >= 0)
        else:
            inside = (e0 <= 0) & (e1 <= 0) & (e2 <= 0)
        if not inside.any():
            continue
        l1 = e1 / area[t]; l2 = e2 / area[t]; l0 = 1 - l1 - l2
        depth = l0 * zt[t, 0] + l1 * zt[t, 1] + l2 * zt[t, 2]
        sub = zb[y0:y1 + 1, x0:x1 + 1]
        upd = inside & (depth >= sub)
        sub[upd] = depth[upd]
        ib[y0:y1 + 1, x0:x1 + 1][upd] = t
        fb[y0:y1 + 1, x0:x1 + 1][upd] = front[t]
    return zb, ib, fb

def front_mask(sx, sy, tris):
    A = np.stack([sx[tris[:, 0]], sy[tris[:, 0]]], 1); B = np.stack([sx[tris[:, 1]], sy[tris[:, 1]]], 1); C = np.stack([sx[tris[:, 2]], sy[tris[:, 2]]], 1)
    return ((B[:, 0] - A[:, 0]) * (C[:, 1] - A[:, 1]) - (B[:, 1] - A[:, 1]) * (C[:, 0] - A[:, 0])) < 0

def pose_test(P, J, W, tris, tri_mat, out=None, double_sided=False, label='', M=None):
    M = M if M is not None else cp.bone_matrices()
    S = cp.skin(P, J, W, M)
    sx, sy, z, w = cp.project(S)
    zb, ib, fb = rasterize(sx, sy, z, w, tris)
    zf, _, _ = rasterize(sx, sy, z, w, tris[front_mask(sx, sy, tris)])
    vis = ib >= 0
    hole = vis & ~fb & ((zb > zf + 3e-4) | ~np.isfinite(zf))
    n_vis = int(vis.sum()); n_hole = int(hole.sum())
    print(f'{label}visible {n_vis} px, nearest face back-facing (see-through) {n_hole} px ({100.0 * n_hole / max(n_vis, 1):.2f}%)')
    if out:
        img = np.zeros(zb.shape + (3,), np.uint8); img[:] = (20, 20, 30)
        d = zb[vis]; lo, hi = np.percentile(d, 1), np.percentile(d, 99)
        g = (60 + 180 * np.clip((zb - lo) / (hi - lo + 1e-9), 0, 1)).astype(np.uint8)
        mat = np.where(vis, tri_mat[np.maximum(ib, 0)], -1)
        img[vis & (mat == 0)] = np.stack([g[vis & (mat == 0)] // 2, g[vis & (mat == 0)] // 2, g[vis & (mat == 0)]], 1)
        img[vis & (mat == 1)] = np.stack([g[vis & (mat == 1)], g[vis & (mat == 1)] // 4, g[vis & (mat == 1)] // 4], 1)
        img[hole] = (40, 255, 40)
        Image.fromarray(img).save(out)
    return n_vis, n_hole, (zb, ib, fb)

if __name__ == '__main__':
    src = sys.argv[1] if len(sys.argv) > 1 else 'build/s99_rigged.npz'
    out = sys.argv[2] if len(sys.argv) > 2 else 'build/posetest.png'
    rig = np.load(src, allow_pickle=True)
    names = list(rig['mesh_names'])
    mat_of_mesh = {names.index('GeometryNode_5'): 0, names.index('GeometryNode_41'): 0,
                   names.index('GeometryNode_23'): 1, names.index('GeometryNode_56'): 1}
    tris = rig['tris'][:, [0, 2, 1]]; mesh_of = rig['mesh_of']
    tri_mat = np.array([mat_of_mesh[m] for m in mesh_of[tris[:, 0]]])
    order = np.argsort(tri_mat, kind='stable')
    tris = tris[order]; tri_mat = tri_mat[order]
    pose_test(rig['P'], rig['J'], rig['W'], tris, tri_mat, out=out)
