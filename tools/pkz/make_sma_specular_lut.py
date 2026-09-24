import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "tools", "pkz", "sma_specular_lut.png")
B = 32


def bins(rgb):
    q = (rgb.astype(np.int32) * B) // 256
    return (q[..., 0] * B + q[..., 1]) * B + q[..., 2]


def main(extract):
    total = np.zeros(B ** 3)
    count = np.zeros(B ** 3)
    for state in ("00", "03", "05", "07"):
        path = os.path.join(extract, "BaseGameplay_textures", f"SMA_SMAmazingState{state}_%s_Hi.png")
        d = np.asarray(Image.open(path % "D").convert("RGB"))
        s = np.asarray(Image.open(path % "S").convert("L")).astype(float)
        b = bins(d).ravel()
        total += np.bincount(b, weights=s.ravel(), minlength=B ** 3)
        count += np.bincount(b, minlength=B ** 3)
    lut = total / np.maximum(count, 1)
    filled = np.where(count > 0)[0]
    empty = np.where(count == 0)[0]
    idx = np.arange(B ** 3)
    centers = np.stack([idx // (B * B), (idx // B) % B, idx % B], 1).astype(float)
    for chunk in np.array_split(empty, max(1, len(empty) // 2048)):
        d2 = ((centers[chunk][:, None, :] - centers[filled][None, :, :]) ** 2).sum(-1)
        lut[chunk] = lut[filled[d2.argmin(1)]]
    Image.fromarray(np.clip(np.round(lut), 0, 255).astype(np.uint8).reshape(128, 256)).save(OUT)
    print(f"{OUT}: {len(filled)} of {B ** 3} bins from retail, the rest filled from their nearest")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else r"D:\EOT_Extract\extracted")
