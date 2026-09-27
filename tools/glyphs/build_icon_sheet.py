import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dds import write_dxt5  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ART = os.path.join(ROOT, "res", "textures", "prompts")
OUT_PNG = os.path.join(ROOT, "res", "textures", "Reeot_Icons.png")
OUT_DDS = os.path.join(ROOT, "res", "textures", "Reeot_Icons.dds")
OUT_TABLE = os.path.join(HERE, "icons.txt")

SCALE = 3
INK_TOP = 0.04
INK_HEIGHT = 0.72
INK_WIDTH = 0.90
CAP_WIDEST = 3
SHEET_WIDTH = 1024
PAD = 2

SLOTS = (
    (0x00, "A", 36, 35),
    (0x01, "B", 35, 35),
    (0x02, "Y", 35, 34),
    (0x03, "X", 35, 35),
    (0x04, "RT", 43, 44),
    (0x05, "LT", 45, 45),
    (0x06, "RS", 46, 42),
    (0x07, "LS", 46, 43),
    (0x08, "BACK", 43, 29),
    (0x09, "START", 43, 29),
    (0x0A, "RB", 49, 35),
    (0x0B, "LB", 53, 36),
    (0x0C, "DPADPLAIN", 45, 45),
    (0x12, "AOFF", 30, 29),
    (0x13, "BOFF", 29, 29),
    (0x14, "YOFF", 29, 29),
    (0x15, "XOFF", 29, 29),
    (0x1B, "LSDOWN", 44, 52),
    (0x1E, "DPADUP", 45, 45),
    (0x1F, "DPADLEFT", 46, 46),
    (0x20, "DPADDOWN", 47, 47),
    (0x21, "DPADRIGHT", 46, 46),
)

SETS = (
    ("switch", {0x00: ("alias", 0x01), 0x01: ("alias", 0x00),
                0x02: ("alias", 0x03), 0x03: ("alias", 0x02)}),
    ("playstation", {0x00: "playstation_Cross", 0x01: "playstation_Circle",
                     0x02: "playstation_Triangle", 0x03: "playstation_Square",
                     0x04: "playstation_R2", 0x05: "playstation_L2",
                     0x08: ("sized", "playstation_Share", 0x04),
                     0x09: ("sized", "playstation_Options", 0x04),
                     0x0A: "playstation_R1", 0x0B: "playstation_L1",
                     0x0C: "playstation_DPad",
                     0x12: "playstation_CrossInv", 0x13: "playstation_CircleInv",
                     0x14: "playstation_TriangleInv", 0x15: "playstation_SquareInv",
                     0x1E: "playstation_DPadUp", 0x1F: "playstation_DPadLeft",
                     0x20: "playstation_DPadDown", 0x21: "playstation_DPadRight"}),
)

KEYS = ("A B C D E F G H I J K L M N O P Q R S T U V W X Y Z 0 1 2 3 4 5 6 7 8 9 "
        "F1 F2 F3 F4 F5 F6 F7 F8 F9 F10 F11 F12 Escape Return Space Tab Shift Control Alt "
        "Backspace Delete Insert Home End PageUp PageDown Left Right Up Down "
        "Backtick Minus Plus Comma Period Semicolon Slash Backslash LBracket RBracket Quote "
        "Numpad0 Numpad1 Numpad2 Numpad3 Numpad4 Numpad5 Numpad6 Numpad7 Numpad8 Numpad9 "
        "NumpadEnter NumpadPlus NumpadMinus NumpadStar NumpadSlash PrintScreen Pause CapsLock "
        "NumLock LMB RMB MMB").split()
CLUSTERS = ("WASD", "MOUSE")
ALIASES = {f"Numpad{n}": str(n) for n in range(10)}


def art(name):
    path = os.path.join(ART, f"{ALIASES.get(name, name)}.png")
    if not os.path.exists(path):
        return None
    image = Image.open(path).convert("RGBA")
    box = image.getchannel("A").getbbox()
    return image.crop(box) if box else image


def fit(image, width, height):
    scale = min(width / image.width, height / image.height)
    size = (max(1, round(image.width * scale)), max(1, round(image.height * scale)))
    scaled = image.resize(size, Image.LANCZOS)
    cell = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    cell.alpha_composite(scaled, ((width - size[0]) // 2, (height - size[1]) // 2))
    return cell


def cell_image(image, width, height):
    ink = fit(image, round(width * INK_WIDTH), round(height * INK_HEIGHT))
    cell = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    cell.alpha_composite(ink, ((width - ink.width) // 2, round(height * INK_TOP)))
    return cell


def cap_width(image, key):
    wanted = key * INK_HEIGHT * image.width / image.height / INK_WIDTH
    return round(min(CAP_WIDEST * key, max(key, wanted)))


def art_width(image, height):
    wanted = height * INK_HEIGHT * image.width / image.height / INK_WIDTH
    return round(min(CAP_WIDEST * height, max(height / 2, wanted)))


def mouse():
    whole = art("Mouse")
    if whole is not None:
        return whole
    left, right = art("RMB"), art("LMB")
    if left is None or right is None:
        return None
    out = left.copy()
    half = left.width // 2
    out.paste(right.crop((half, 0, right.width, right.height)), (half, 0))
    return out


def cluster(name, key):
    if name == "MOUSE":
        return mouse()
    caps = {stem: art(stem) for stem in "WASD"}
    if any(cap is None for cap in caps.values()):
        return None
    out = Image.new("RGBA", (key * 3, key * 2), (0, 0, 0, 0))
    for stem, (cx, cy) in (("W", (key, 0)), ("A", (0, key)), ("S", (key, key)), ("D", (2 * key, key))):
        out.alpha_composite(fit(caps[stem], key, key), (cx, cy))
    return out


class Shelf:
    def __init__(self, width):
        self.width = width
        self.x = 0
        self.y = 0
        self.row_height = 0
        self.placed = []

    def put(self, image):
        w, h = image.width + 2 * PAD, image.height + 2 * PAD
        if self.x + w > self.width:
            self.x = 0
            self.y += self.row_height
            self.row_height = 0
        x, y = self.x + PAD, self.y + PAD
        self.placed.append((image, x, y))
        self.x += w
        self.row_height = max(self.row_height, h)
        return x, y, x + image.width, y + image.height

    def height(self):
        return self.y + self.row_height


def main():
    shelf = Shelf(SHEET_WIDTH)
    lines = []
    missing = []
    heights = {slot: h for slot, _, _, h in SLOTS}
    for name, slots in SETS:
        lines.append(f"set {name}")
        for slot, button, w, h in SLOTS:
            entry = slots.get(slot)
            if entry is None:
                continue
            if isinstance(entry, tuple) and entry[0] == "alias":
                lines.append(f"alias {slot:02X} {entry[1]:02X}")
                continue
            sized = isinstance(entry, tuple)
            stem = entry[1] if sized else entry
            image = art(stem)
            if image is None:
                missing.append(stem)
                continue
            if sized:
                height = heights[entry[2]] * SCALE
                cell = cell_image(image, art_width(image, height), height)
            else:
                cell = cell_image(image, w * SCALE, h * SCALE)
            x0, y0, x1, y1 = shelf.put(cell)
            lines.append(f"cell {slot:02X} {x0} {y0} {x1} {y1}"
                         + (f" {entry[2]:02X}" if sized else ""))
    lines.append("caps")
    key = SLOTS[0][3] * SCALE
    caps = 0
    for name in KEYS:
        image = art(name)
        if image is None:
            continue
        x0, y0, x1, y1 = shelf.put(cell_image(image, cap_width(image, key), key))
        lines.append(f"cap {name} {x0} {y0} {x1} {y1}")
        caps += 1
    for name in CLUSTERS:
        image = cluster(name, key)
        if image is None:
            missing.append(name)
            continue
        width = key * 3 // 2 if name != "MOUSE" else key
        x0, y0, x1, y1 = shelf.put(cell_image(image, width, key))
        lines.append(f"cap {name} {x0} {y0} {x1} {y1}")

    height = shelf.height()
    height = (height + 3) & ~3
    sheet = Image.new("RGBA", (SHEET_WIDTH, height), (0, 0, 0, 0))
    for image, x, y in shelf.placed:
        sheet.alpha_composite(image, (x, y))
    sheet.save(OUT_PNG)
    write_dxt5(sheet, OUT_DDS)

    with open(OUT_TABLE, "w", encoding="utf-8", newline="\n") as out:
        out.write("# Button glyphs: the cells of res/textures/Reeot_Icons.png, written by "
                  "tools/glyphs/build_icon_sheet.py -- do not edit.\n")
        entries = [f"sheet {SHEET_WIDTH} {height}"] + lines
        for i, text in enumerate(entries):
            out.write(f"IC_{i:04d}\t{text}\n")
    print(f"{OUT_PNG}: {SHEET_WIDTH}x{height}, {len(shelf.placed)} cells ({caps} caps of {len(KEYS)} keys); "
          f"{OUT_TABLE}: {len(lines) + 1} lines")
    if missing:
        print("no art for:", " ".join(missing))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
