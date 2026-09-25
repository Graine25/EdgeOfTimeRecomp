import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dxt import console_normal, mip_chain, write_dds  # noqa: E402

LEVELS = 5

FORMS = [
    ("remastd", "SMA_AntiVenom_%s_Hi", "Reeot_AntiVenom"),
    ("massiveremasted", "Untitled2_AntiVenomMassive_%s", "Reeot_AntiVenomMassive"),
]
MAPS = [("Diffuse", "D", b"DXT1"), ("Normal", "N", b"DXT5")]
SOURCE_NAMES = {"Diffuse": ("D", "Diffuse"), "Normal": ("N", "Normal")}


def load(folder, stem, kind):
    for candidate in SOURCE_NAMES[kind]:
        path = os.path.join(folder, (stem % candidate) + ".png")
        if os.path.exists(path):
            return np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)
    raise SystemExit(f"no {kind} map in {folder}")


def main(src, out):
    os.makedirs(out, exist_ok=True)
    for sub, stem, name in FORMS:
        folder = os.path.join(src, sub)
        for kind, suffix, fourcc in MAPS:
            img = load(folder, stem, kind)
            if kind == "Normal":
                img = console_normal(img)
            path = os.path.join(out, f"{name}_{suffix}.dds")
            write_dds(path, mip_chain(img, LEVELS), fourcc)
            print(f"{path}: {img.shape[1]}x{img.shape[0]} {fourcc.decode()} x{LEVELS}, "
                  f"{os.path.getsize(path)} bytes")


if __name__ == "__main__":
    if len(sys.argv) > 3:
        sys.exit(__doc__)
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "res", "textures", "antivenom")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "res", "textures", "characters")
    main(src, out)
