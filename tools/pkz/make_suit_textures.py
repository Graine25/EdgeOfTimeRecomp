import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dxt import console_normal, mip_chain, write_dds  # noqa: E402

LEVELS = 5
STATES = [("", "00"), ("damage1", "03"), ("damage1.5", "05"), ("damage2", "07")]
LUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sma_specular_lut.png")
LUT_BINS = 32
LENS_SPECULAR = 201.0
FLESH_SPECULAR = 104.0


def specular_from_diffuse(diffuse):
    lut = np.asarray(Image.open(LUT)).astype(np.float32).ravel()
    q = (diffuse.astype(np.int32) * LUT_BINS) // 256
    spec = lut[(q[..., 0] * LUT_BINS + q[..., 1]) * LUT_BINS + q[..., 2]]
    d = diffuse.astype(np.int32)
    r, g, b = d[..., 0], d[..., 1], d[..., 2]
    sat = d.max(-1) - d.min(-1)
    lum = (r * 3 + g * 6 + b) // 10
    lens = (sat < 45) & (lum > 95)
    flesh = (r > 120) & (r - b > 25) & (r - b < 110) & (g > 70) & (b > 60) & (abs(g - b) < 40) & ~lens
    if lens.any():
        spec[lens] = lum[lens] * (LENS_SPECULAR / lum[lens].mean())
    if flesh.sum() > 1000:
        spec[flesh] *= FLESH_SPECULAR / spec[flesh].mean()
    return np.clip(np.round(spec), 0, 255).astype(np.uint8)


def load(folder, kind, mode):
    return np.asarray(Image.open(os.path.join(folder, f"Model_SMA_State_00_{kind}.png")).convert(mode), dtype=np.uint8)


def main(src, out, preview=None):
    os.makedirs(out, exist_ok=True)
    if preview:
        os.makedirs(preview, exist_ok=True)
    for sub, state in STATES:
        folder = os.path.join(src, sub)
        diffuse = load(folder, "Diffuse", "RGB")
        specular = np.repeat(specular_from_diffuse(diffuse)[..., None], 3, axis=2)
        nm = console_normal(load(folder, "Normal", "RGB"))
        for suffix, img, fourcc in (("D", diffuse, b"DXT1"), ("S", specular, b"DXT1"), ("N", nm, b"DXT5")):
            path = os.path.join(out, f"Reeot_SMA_State{state}_{suffix}.dds")
            write_dds(path, mip_chain(img, LEVELS), fourcc)
            print(f"{path}: {img.shape[1]}x{img.shape[0]} {fourcc.decode()} x{LEVELS}, {os.path.getsize(path)} bytes")
        if preview:
            Image.fromarray(specular[..., 0]).save(os.path.join(preview, f"Reeot_SMA_State{state}_S.png"))


if __name__ == "__main__":
    if len(sys.argv) > 4:
        sys.exit(__doc__)
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "res", "textures", "remasted", "remasted")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "res", "textures", "suits")
    main(src, out, sys.argv[3] if len(sys.argv) > 3 else None)
