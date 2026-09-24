import os
import sys
from collections import deque

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dxt import console_normal, mip_chain, write_dds  # noqa: E402

LEVELS = 5
STATES = ["00", "03", "05", "07"]
LUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sma_specular_lut.png")
LUT_BINS = 32
FLESH_SPECULAR = 104.0
EXTRACT = r"D:\EOT_Extract\extracted"
EAR_U = (0.80, 1.0)
EAR_V = (0.17, 0.31)
EAR_RING = 4


def box(h, w):
    return int(EAR_U[0] * w), int(EAR_V[0] * h), int(EAR_U[1] * w), int(EAR_V[1] * h)


def flood(passable, seeds):
    out = np.zeros_like(passable)
    q = deque()
    for y, x in seeds:
        if passable[y, x] and not out[y, x]:
            out[y, x] = True
            q.append((y, x))
    h, w = passable.shape
    while q:
        y, x = q.popleft()
        for ny, nx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
            if 0 <= ny < h and 0 <= nx < w and passable[ny, nx] and not out[ny, nx]:
                out[ny, nx] = True
                q.append((ny, nx))
    return out


def ear_island(diffuse):
    x0, y0, x1, y1 = box(*diffuse.shape[:2])
    d = diffuse[y0:y1, x0:x1].astype(np.int32)
    r, g, b = d[..., 0], d[..., 1], d[..., 2]
    blue = b > r
    red = g < 0.45 * r
    skin = (r > 170) & (g > 110) & (b > 100)
    ys, xs = np.nonzero(skin)
    if not len(ys):
        sys.exit("no ear in the retail map's box: the extract is not the retail Amazing suit")
    island = flood(~red & ~blue, [(int(np.median(ys)), int(np.median(xs)))])
    h, w = island.shape
    edge = [(y, x) for y in range(h) for x in (0, w - 1)] + [(y, x) for x in range(w) for y in (0, h - 1)]
    island |= ~flood(~island, edge)
    grown = island.copy()
    for _ in range(EAR_RING):
        step = grown.copy()
        step[1:] |= grown[:-1]
        step[:-1] |= grown[1:]
        step[:, 1:] |= grown[:, :-1]
        step[:, :-1] |= grown[:, 1:]
        grown = step
    return grown & (island | red) & ~blue


def carry_ear(target, source, mask, resample):
    tx0, ty0, tx1, ty1 = box(*target.shape[:2])
    sx0, sy0, sx1, sy1 = box(*source.shape[:2])
    size = (tx1 - tx0, ty1 - ty0)
    patch = np.asarray(Image.fromarray(source[sy0:sy1, sx0:sx1]).resize(size, resample))
    m = np.asarray(Image.fromarray(mask.astype(np.uint8) * 255).resize(size, Image.NEAREST)) > 127
    region = target[ty0:ty1, tx0:tx1]
    region[m] = patch[m][..., : region.shape[-1]]


def retail_maps(extract, state):
    path = os.path.join(extract, "BaseGameplay_textures", f"SMA_SMAmazingState{state}_%s_Hi.png")
    if not os.path.exists(path % "D"):
        sys.exit(f"{path % 'D'} is not there; the retail extract holds the ear the remaster leaves out")
    return (np.asarray(Image.open(path % "D").convert("RGB")),
            np.asarray(Image.open(path % "N").convert("RGBA")),
            np.asarray(Image.open(path % "S").convert("RGB")))


def classes(diffuse):
    d = diffuse.astype(np.int32)
    r, g, b = d[..., 0], d[..., 1], d[..., 2]
    sat = d.max(-1) - d.min(-1)
    lum = (r * 3 + g * 6 + b) // 10
    lens = (sat < 45) & (lum > 95)
    return {
        "lum": lum,
        "lens": lens,
        "interior": lens & (lum > 150),
        "web": (sat < 40) & (lum < 45),
        "flesh": (r > 120) & (r - b > 25) & (r - b < 110) & (g > 70) & (b > 60) & (abs(g - b) < 40) & ~lens,
    }


def retail_levels(diffuse, specular):
    c = classes(diffuse)
    s = specular[..., 0].astype(np.float32)
    return {"web": float(s[c["web"]].mean()), "interior": float(s[c["interior"]].mean())}


def specular_from_diffuse(diffuse, levels):
    lut = np.asarray(Image.open(LUT)).astype(np.float32).ravel()
    q = (diffuse.astype(np.int32) * LUT_BINS) // 256
    spec = lut[(q[..., 0] * LUT_BINS + q[..., 1]) * LUT_BINS + q[..., 2]]
    c = classes(diffuse)
    lum, lens, interior, web, flesh = c["lum"], c["lens"], c["interior"], c["web"], c["flesh"]
    if web.any():
        spec[web] *= levels["web"] / spec[web].mean()
    if lens.any():
        spec[lens] = lum[lens].astype(np.float32)
        if interior.any():
            spec[lens] *= levels["interior"] / spec[interior].mean()
    if flesh.sum() > 1000:
        spec[flesh] *= FLESH_SPECULAR / spec[flesh].mean()
    return np.clip(np.round(spec), 0, 255).astype(np.uint8)


def load(folder, state, kind, mode):
    return np.asarray(Image.open(os.path.join(folder, f"SMA_State{state}_{kind}.png")).convert(mode), dtype=np.uint8)


def main(src, out, preview=None, extract=EXTRACT):
    os.makedirs(out, exist_ok=True)
    if preview:
        os.makedirs(preview, exist_ok=True)
    for state in STATES:
        diffuse = load(src, state, "Diffuse", "RGB").copy()
        rd, rn, rs = retail_maps(extract, state)
        levels = retail_levels(rd, rs)
        specular = np.repeat(specular_from_diffuse(diffuse, levels)[..., None], 3, axis=2)
        print(f"state {state}: retail's webbing {levels['web']:.0f}, lens interior {levels['interior']:.0f}")
        nm = console_normal(load(src, state, "Normal", "RGB")).copy()
        island = ear_island(rd)
        carry_ear(diffuse, rd, island, Image.LANCZOS)
        carry_ear(specular, rs, island, Image.LANCZOS)
        carry_ear(nm, rn, island, Image.BILINEAR)
        print(f"state {state}: retail's ear island carried in ({int(island.sum())} texels at retail's size)")
        for suffix, img, fourcc in (("D", diffuse, b"DXT1"), ("S", specular, b"DXT1"), ("N", nm, b"DXT5")):
            path = os.path.join(out, f"Reeot_SMA_State{state}_{suffix}.dds")
            write_dds(path, mip_chain(img, LEVELS), fourcc)
            print(f"{path}: {img.shape[1]}x{img.shape[0]} {fourcc.decode()} x{LEVELS}, {os.path.getsize(path)} bytes")
        if preview:
            Image.fromarray(specular[..., 0]).save(os.path.join(preview, f"Reeot_SMA_State{state}_S.png"))


if __name__ == "__main__":
    if len(sys.argv) > 5:
        sys.exit("usage: make_suit_textures.py [<source dir> [<amazing dir> [<preview dir> [<retail extract>]]]]")
    amazing = os.path.join(ROOT, "res", "suits", "amazing")
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(amazing, "source")
    out = sys.argv[2] if len(sys.argv) > 2 else amazing
    main(src, out, sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] else None,
         sys.argv[4] if len(sys.argv) > 4 else EXTRACT)
