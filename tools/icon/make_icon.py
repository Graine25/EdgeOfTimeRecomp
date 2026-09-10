import os
import sys

from PIL import Image

root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "res", "icon", "reeot_icon.png")
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "res", "reeot.ico")

im = Image.open(src).convert("RGBA")
if im.width != im.height:
    side = min(im.size)
    left = (im.width - side) // 2
    top = (im.height - side) // 2
    im = im.crop((left, top, left + side, top + side))
sizes = [s for s in (16, 24, 32, 48, 64, 128, 256) if s <= im.width]
im.save(out, sizes=[(s, s) for s in sizes])
print(f"{os.path.relpath(out, root)}: {', '.join(str(s) for s in sizes)} from {im.width}x{im.height}")
