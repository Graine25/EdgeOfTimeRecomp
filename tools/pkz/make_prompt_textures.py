import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dxt import write_dds  # noqa: E402

SOURCE = os.path.join(ROOT, "res", "textures", "mash_prompt", "source")
OUT = os.path.join(ROOT, "res", "textures", "mash_prompt")

PADS = (
    ("Reeot_MashPrompt_PlayStation_D", "PlayStation_Circle.dds"),
    ("Reeot_MashPromptY_PlayStation_D", "PlayStation_Triangle.dds"),
    ("Reeot_MashPrompt_Switch_D", "Switch_A.dds"),
)
KEYBOARD = "Reeot_MashPrompt_Keyboard_D"
CAP_W, CAP_H = 1024, 256
RAISED_TOP, FOOT = 16, 240
KEY_LEVELS = 3


def premultiply(rgba):
    a = rgba[..., 3:4].astype(np.float32) / 255.0
    return np.concatenate([rgba[..., :3].astype(np.float32) * a, rgba[..., 3:4].astype(np.float32)], -1)


def unpremultiply(pm):
    a = pm[..., 3:4]
    rgb = np.where(a > 0, pm[..., :3] * 255.0 / np.maximum(a, 1e-6), 0.0)
    return np.clip(np.concatenate([rgb, a], -1) + 0.5, 0, 255).astype(np.uint8)


def resize(rgba, w, h):
    pm = premultiply(rgba)
    chans = [np.asarray(Image.fromarray(pm[..., c]).resize((w, h), Image.LANCZOS)) for c in range(4)]
    return unpremultiply(np.clip(np.stack(chans, -1), 0.0, None))


def mips(rgba, levels):
    out = [rgba]
    pm = premultiply(rgba)
    for _ in range(1, levels):
        h, w = pm.shape[:2]
        pm = pm.reshape(h // 2, 2, w // 2, 2, 4).mean(axis=(1, 3))
        out.append(unpremultiply(pm))
    return out


def load(path):
    return np.asarray(Image.open(path).convert("RGBA"))


def pad_sheet(src):
    img = load(src)
    if img.shape[:2] != (128, 64):
        sys.exit(f"{src} is {img.shape[1]}x{img.shape[0]}; the prompt's sheets are 64x128")
    return resize(img, 128, 256)


def key_sheet(source):
    raised = load(os.path.join(source, "Space.png"))
    pressed = load(os.path.join(source, "Space_Pressed.png"))
    scale = (FOOT - RAISED_TOP) / raised.shape[0]
    sheet = np.zeros((2 * CAP_H, CAP_W, 4), np.uint8)
    for row, art in ((0, raised), (1, pressed)):
        w = round(art.shape[1] * scale)
        h = round(art.shape[0] * scale)
        if w > CAP_W:
            sys.exit(f"the cap comes out {w} wide at the raised one's scale; the cell is {CAP_W}")
        cap = resize(art, w, h)
        x = (CAP_W - w) // 2
        y = row * CAP_H + FOOT - h
        sheet[y:y + h, x:x + w] = cap
    return sheet


def main(source=SOURCE, out=OUT):
    os.makedirs(out, exist_ok=True)
    for name, src in PADS:
        path = os.path.join(out, name + ".dds")
        write_dds(path, [pad_sheet(os.path.join(source, src))], b"DXT5")
        print(f"{path}: 128x256, 1 level")
    path = os.path.join(out, KEYBOARD + ".dds")
    write_dds(path, mips(key_sheet(source), KEY_LEVELS), b"DXT5")
    print(f"{path}: {CAP_W}x{2 * CAP_H}, {KEY_LEVELS} levels")


if __name__ == "__main__":
    main(*sys.argv[1:3])
