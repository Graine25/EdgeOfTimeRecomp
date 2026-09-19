import sys, os, time
import numpy as np
from PIL import Image, ImageFilter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, r'C:\Users\rieng\Documents\GitHub\reeot-dni\reference\eot_tools\SPIDER-MAN EDGE OF TIME TOOLS\tools')

S99 = 's99/shit in kettle/images/'

def raster(P, N, UV, tris, mat_of_tri, textures, W, H, view):
    c, ext, yaw = view
    a = np.radians(yaw)
    R = np.array([[np.cos(a), 0, np.sin(a)], [0, 1, 0], [-np.sin(a), 0, np.cos(a)]])
    Q = (P - c) @ R.T
    Nn = N @ R.T
    scale = H / ext
    sx = Q[:, 0] * scale + W / 2; sy = -Q[:, 1] * scale + H / 2; sz = -Q[:, 2]
    zbuf = np.full((H, W), np.inf)
    col = np.zeros((H, W, 3)); alpha = np.zeros((H, W))
    L1 = np.array([-0.45, 0.55, 0.7]); L1 /= np.linalg.norm(L1)
    L2 = np.array([0.7, 0.1, 0.4]); L2 /= np.linalg.norm(L2)
    V = np.array([0.0, 0.0, 1.0])
    Hh = (L1 + V); Hh /= np.linalg.norm(Hh)
    T = tris
    ax, ay = sx[T[:, 0]], sy[T[:, 0]]; bx, by = sx[T[:, 1]], sy[T[:, 1]]; cx, cy = sx[T[:, 2]], sy[T[:, 2]]
    area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay)
    order = np.argsort(-(sz[T].mean(1)))
    for t in order:
        if abs(area[t]) < 1e-6:
            continue
        xs = sx[T[t]]; ys = sy[T[t]]; zs = sz[T[t]]
        x0, x1 = int(max(0, np.floor(xs.min()))), int(min(W - 1, np.ceil(xs.max())))
        y0, y1 = int(max(0, np.floor(ys.min()))), int(min(H - 1, np.ceil(ys.max())))
        if x1 < x0 or y1 < y0:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        d = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (ys[1] - ys[0])
        l1 = ((xs[1] - gx) * (ys[2] - gy) - (xs[2] - gx) * (ys[1] - gy)) / d
        l2 = ((xs[2] - gx) * (ys[0] - gy) - (xs[0] - gx) * (ys[2] - gy)) / d
        l3 = 1 - l1 - l2
        inside = (l1 >= 0) & (l2 >= 0) & (l3 >= 0)
        if not inside.any():
            continue
        z = l1 * zs[0] + l2 * zs[1] + l3 * zs[2]
        sub = zbuf[y0:y1 + 1, x0:x1 + 1]
        upd = inside & (z < sub)
        if not upd.any():
            continue
        sub[upd] = z[upd]
        uv = (l1[..., None] * UV[T[t, 0]] + l2[..., None] * UV[T[t, 1]] + l3[..., None] * UV[T[t, 2]])[upd]
        n = (l1[..., None] * Nn[T[t, 0]] + l2[..., None] * Nn[T[t, 1]] + l3[..., None] * Nn[T[t, 2]])[upd]
        n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-9
        tex = textures[mat_of_tri[t]]
        th, tw = tex.shape[:2]
        u = np.clip((uv[:, 0] * tw).astype(int), 0, tw - 1); v = np.clip((uv[:, 1] * th).astype(int), 0, th - 1)
        base = tex[v, u].astype(np.float64) / 255.0
        ndl1 = np.clip(n @ L1, 0, 1); ndl2 = np.clip(n @ L2, 0, 1)
        spec = np.clip(n @ Hh, 0, 1) ** 40
        rim = np.clip(1 - np.abs(n @ V), 0, 1) ** 3
        shade = base * (0.28 + 0.85 * ndl1 + 0.35 * ndl2)[:, None] + (0.55 * spec + 0.45 * rim)[:, None] * np.array([1.0, 0.97, 0.95])
        col[y0:y1 + 1, x0:x1 + 1][upd] = shade
        alpha[y0:y1 + 1, x0:x1 + 1][upd] = 1.0
    return col, alpha

def retail_card(name, gallery='D:/EOT_Extract/extracted/Gallery.ext'):
    import x360_tex as X
    from pakwriter import Pak, kids
    gal = Pak(gallery)
    ep, es, h = gal.entry(0x09, name=name)
    post = gal.postload(h['dataoff'])
    q = next(k for k in kids(post, 12, len(post)) if k[1] == 0x195)
    (df, W, H, n, levels), err = X.parse_texture(post, q[0], q[4])
    return np.asarray(X.decode_level(df, levels[0][0], levels[0][1], levels[0][2])).astype(np.float32)

def main():
    z = np.load('build/s99_aligned.npz')
    P, N, UV, tris, mesh_of = z['P'], z['N'], z['UV'].copy(), z['tris'], z['mesh_of']
    UV[:, 1] = 1 - UV[:, 1]
    names = list(z['mesh_names'])
    mat_of_mesh = {names.index('GeometryNode_5'): 0, names.index('GeometryNode_41'): 0, names.index('GeometryNode_23'): 1, names.index('GeometryNode_56'): 1}
    mat_of_tri = np.array([mat_of_mesh[m] for m in mesh_of[tris[:, 0]]])
    body = np.asarray(Image.open(S99 + 'torso_c.png').convert('RGB').resize((1024, 1024), Image.LANCZOS)).astype(np.float32)
    body = np.clip(body * 1.3, 0, 255).astype(np.uint8)
    red = np.asarray(Image.open(S99 + 'techy3.png').convert('RGB').resize((1024, 1024), Image.LANCZOS))
    t = time.time()
    view = (np.array([0.05, 1.42, 0.0]), 0.92, -18.0)
    RW, RH = 640, 640
    col, alpha = raster(P, N, UV, tris, mat_of_tri, [body, red], RW, RH, view)
    print('rendered in %.0fs, coverage %.1f%%' % (time.time() - t, 100 * alpha.mean()))
    rgb = np.clip(col * 255, 0, 255).astype(np.uint8)
    fig = Image.fromarray(np.dstack([rgb, (alpha * 255).astype(np.uint8)]), 'RGBA')
    fig.save('build/card_figure.png')

    retail = retail_card('Gallery_AltSuit_Selection2099Original_DA[Hi]')
    W, H = 512, 256
    yy, xx = np.mgrid[0:H, 0:W]
    g = 0.5 * xx / W + 0.5 * yy / H
    bg = np.zeros((H, W, 3), np.float32)
    for k, (c0, c1) in enumerate(((212, 150), (210, 150), (216, 158))):
        bg[..., k] = c0 + (c1 - c0) * g
    band = np.exp(-((xx - 0.55 * W + 0.4 * yy) / 90.0) ** 2) * 22
    bg += band[..., None]
    card = np.zeros((H, W, 4), np.float32)
    card[..., :3] = bg
    card[..., 3] = retail[..., 3] if retail is not None else 255
    if retail is not None:
        card[:, :104, :3] = retail[:, :104, :3]
    figure = fig.resize((int(RW * H / RH * 1.08), int(H * 1.08)), Image.LANCZOS)
    fx = 120; fy = -6
    fa = np.asarray(figure).astype(np.float32)
    fh, fw = fa.shape[:2]
    x0, y0 = max(0, fx), max(0, fy)
    x1, y1 = min(W, fx + fw), min(H, fy + fh)
    src = fa[y0 - fy:y1 - fy, x0 - fx:x1 - fx]
    a = src[..., 3:4] / 255.0
    card[y0:y1, x0:x1, :3] = card[y0:y1, x0:x1, :3] * (1 - a) + src[..., :3] * a
    large = Image.fromarray(np.clip(card, 0, 255).astype(np.uint8), 'RGBA')
    large.save('build/card_large.png')
    small = large.resize((128, 64), Image.LANCZOS)
    small.save('build/card_small.png')
    print('wrote build/card_large.png and build/card_small.png')

if __name__ == '__main__':
    main()
