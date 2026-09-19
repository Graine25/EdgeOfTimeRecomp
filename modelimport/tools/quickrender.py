import numpy as np

def render(meshes, out, W=1600, H=800, views=('front', 'side', 'back')):
    from PIL import Image
    img = np.zeros((H, W, 3), np.uint8) + 25
    allP = np.concatenate([m[0] for m in meshes])
    c = (allP.min(0) + allP.max(0)) / 2
    ext = (allP.max(0) - allP.min(0)).max()
    vw = W // len(views)
    for vi, view in enumerate(views):
        zbuf = np.full((H, vw), np.inf)
        col = np.zeros((H, vw, 3), np.uint8) + 25
        for (P, tris, color) in meshes:
            color = np.asarray(color)
            per_vertex = color.ndim == 2
            Q = P - c
            if view == 'side': Q = Q[:, [2, 1, 0]] * np.array([1, 1, -1])
            if view == 'back': Q = Q * np.array([-1, 1, -1])
            if view == 'top': Q = Q[:, [0, 2, 1]]
            scale = 0.9 * min(vw, H) / ext
            sx = Q[:, 0] * scale + vw / 2; sy = -Q[:, 1] * scale + H / 2; sz = -Q[:, 2]
            T = tris
            a, b, cc = Q[T[:, 0]], Q[T[:, 1]], Q[T[:, 2]]
            n = np.cross(b - a, cc - a); n /= (np.linalg.norm(n, axis=1, keepdims=True) + 1e-9)
            shade = np.clip(0.35 + 0.65 * abs(n @ np.array([0.3, 0.5, 0.8])), 0, 1)
            for t in range(len(T)):
                xs = sx[T[t]]; ys = sy[T[t]]; zs = sz[T[t]]
                x0, x1 = int(max(0, xs.min())), int(min(vw - 1, xs.max()))
                y0, y1 = int(max(0, ys.min())), int(min(H - 1, ys.max()))
                if x1 < x0 or y1 < y0: continue
                gx, gy = np.meshgrid(np.arange(x0, x1 + 1), np.arange(y0, y1 + 1))
                d = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (ys[1] - ys[0])
                if abs(d) < 1e-9: continue
                l1 = ((xs[1] - gx) * (ys[2] - gy) - (xs[2] - gx) * (ys[1] - gy)) / d
                l2 = ((xs[2] - gx) * (ys[0] - gy) - (xs[0] - gx) * (ys[2] - gy)) / d
                l3 = 1 - l1 - l2
                inside = (l1 >= 0) & (l2 >= 0) & (l3 >= 0)
                if not inside.any(): continue
                z = l1 * zs[0] + l2 * zs[1] + l3 * zs[2]
                sub = zbuf[y0:y1 + 1, x0:x1 + 1]
                upd = inside & (z < sub)
                sub[upd] = z[upd]
                cc = color[T[t]].mean(0) if per_vertex else color
                col[y0:y1 + 1, x0:x1 + 1][upd] = (cc * shade[t]).astype(np.uint8)
        img[:, vi * vw:(vi + 1) * vw] = col
    Image.fromarray(img).save(out)
    return out
