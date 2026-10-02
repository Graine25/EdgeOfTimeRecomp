import glob
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

COLUMNS = 8
ROWS = 6
THUMBNAIL_CELL = 64
PANEL_CELL = 256
MODEL_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "models")


def load_model():
    import cv2

    models = sorted(glob.glob(os.path.join(MODEL_DIR, "*.onnx")))
    if not models:
        print("  no model in tools/pkz/models; upscaling with Lanczos + unsharp")
        return None
    try:
        net = cv2.dnn.readNetFromONNX(models[0])
    except Exception as error:
        print(f"  {os.path.basename(models[0])} would not load ({error}); falling back to Lanczos")
        return None
    print(f"  upscaling with {os.path.basename(models[0])}")
    return net


def run_model(net, rgb, size):
    import cv2

    while rgb.shape[0] < size:
        net.setInput(cv2.dnn.blobFromImage(rgb, scalefactor=1.0 / 255.0, swapRB=False))
        out = net.forward()[0].transpose(1, 2, 0)
        grown = np.clip(out * 255.0, 0, 255).astype(np.uint8)
        if grown.shape[0] <= rgb.shape[0]:
            break
        rgb = grown
    if rgb.shape[0] != size:
        rgb = cv2.resize(rgb, (size, size), interpolation=cv2.INTER_LANCZOS4)
    return rgb


def run_lanczos(rgb, size):
    import cv2

    big = cv2.resize(rgb, (size, size), interpolation=cv2.INTER_LANCZOS4)
    blurred = cv2.GaussianBlur(big, (0, 0), sigmaX=size / float(THUMBNAIL_CELL))
    return cv2.addWeighted(big, 1.55, blurred, -0.55, 0)


def upscale(net, image, size):
    import cv2

    rgba = np.array(image.convert("RGBA"))
    colour = run_model(net, rgba[:, :, :3], size) if net is not None else run_lanczos(rgba[:, :, :3], size)
    alpha = cv2.resize(rgba[:, :, 3], (size, size), interpolation=cv2.INTER_LANCZOS4)
    return Image.fromarray(np.dstack([colour, alpha]).astype(np.uint8), "RGBA")


def secret_plate(size):
    plate = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(plate)
    inset = size // 10
    draw.rounded_rectangle([inset, inset, size - inset, size - inset], radius=size // 9,
                           fill=(14, 16, 26, 236), outline=(126, 136, 172, 255),
                           width=max(1, size // 28))
    try:
        font = ImageFont.truetype("arialbd.ttf", int(size * 0.58))
    except OSError:
        font = ImageFont.load_default(int(size * 0.58))
    box = draw.textbbox((0, 0), "?", font=font)
    draw.text(((size - box[2] - box[0]) / 2, (size - box[3] - box[1]) / 2), "?",
              fill=(158, 168, 208, 255), font=font)
    return plate


def build_sheet(icons, cell, net):
    sheet = Image.new("RGBA", (COLUMNS * cell, ROWS * cell), (0, 0, 0, 0))
    for slot, path in enumerate(icons):
        image = Image.open(path)
        image = image.convert("RGBA") if image.width == cell else upscale(net, image, cell)
        sheet.paste(image, ((slot % COLUMNS) * cell, (slot // COLUMNS) * cell))
    last = COLUMNS * ROWS - 1
    sheet.paste(secret_plate(cell), ((last % COLUMNS) * cell, (last // COLUMNS) * cell))
    return sheet


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    icons_dir, out_dir = sys.argv[1], sys.argv[2]

    icons = []
    for image_id in range(1, COLUMNS * ROWS):
        path = os.path.join(icons_dir, f"{image_id}.png")
        if not os.path.exists(path):
            break
        icons.append(path)
    if not icons:
        sys.exit(f"no <image_id>.png icons in {icons_dir}")

    os.makedirs(out_dir, exist_ok=True)
    thumbnails = os.path.join(out_dir, "achievement_icons_64.png")
    panel = os.path.join(out_dir, "achievement_icons_4x.dds")

    build_sheet(icons, THUMBNAIL_CELL, None).save(thumbnails)
    build_sheet(icons, PANEL_CELL, load_model()).save(panel, format="DDS", pixel_format="DXT5")
    for path, cell in ((thumbnails, THUMBNAIL_CELL), (panel, PANEL_CELL)):
        print(f"  {os.path.basename(path):22} {COLUMNS * cell}x{ROWS * cell}"
              f"  {len(icons)} icons + the secret plate at cell {COLUMNS * ROWS}"
              f"  {os.path.getsize(path) / 1024.0:.0f} KiB")


if __name__ == "__main__":
    main()
