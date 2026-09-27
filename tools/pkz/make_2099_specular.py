import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dxt import mip_chain, write_dds  # noqa: E402

SIZE = 512
LEVELS = 3
EMBLEM_SPECULAR = 0.5
OUT = os.path.join(ROOT, "res", "suits", "2099", "Reeot_SM2099Body_S.dds")
DIFFUSE = os.path.join(ROOT, "res", "suits", "2099", "source", "SM2099Body_Diffuse.png")


def main(extract):
    retail = os.path.join(extract, "BaseGameplay_textures", "SM99_Spiderman_S_Hi.png")
    if not os.path.exists(retail):
        sys.exit(f"{retail} is not there; the retail extract holds the map this is derived from")
    spec = np.asarray(Image.open(retail).convert("L").resize((SIZE, SIZE), Image.LANCZOS)).astype(np.float32)
    d = np.asarray(Image.open(DIFFUSE).convert("RGB").resize((SIZE, SIZE), Image.LANCZOS)).astype(np.int32)
    r, g, b = d[..., 0], d[..., 1], d[..., 2]
    red = (r > b + 25) & (r > 40) & (g < 90)
    before = spec[red].mean()
    spec[red] *= EMBLEM_SPECULAR
    out = np.repeat(np.clip(np.round(spec), 0, 255).astype(np.uint8)[..., None], 3, axis=2)
    write_dds(OUT, mip_chain(out, LEVELS), b"DXT1")
    print(f"{OUT}: {SIZE}x{SIZE} DXT1 x{LEVELS}, {os.path.getsize(OUT)} bytes")
    print(f"  red is {100*red.mean():.1f}% of the sheet; its specular {before:.0f} -> {spec[red].mean():.0f}, "
          f"the rest unchanged at {spec[~red].mean():.0f}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else r"D:\EOT_Extract\extracted")
