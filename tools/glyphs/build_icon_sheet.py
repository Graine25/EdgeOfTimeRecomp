import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dds import write_dxt5  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ART = os.path.join(ROOT, "thirdparty", "kenney-input-prompts")
OUT_PNG = os.path.join(ROOT, "res", "textures", "Reeot_Icons.png")
OUT_DDS = os.path.join(ROOT, "res", "textures", "Reeot_Icons.dds")
OUT_TABLE = os.path.join(HERE, "icons.txt")

SCALE = 3
INK_TOP = 0.04
INK_HEIGHT = 0.72
INK_WIDTH = 0.90
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
    (0x1B, "LSDOWN", 44, 52),
    (0x1E, "DPAD", 45, 45),
)

SETS = (
    ("switch", (("alias", 0x01), ("alias", 0x00), ("alias", 0x03), ("alias", 0x02),
                "switch_button_zr", "switch_button_zl", None, None,
                "switch_button_minus", "switch_button_plus", "switch_button_r", "switch_button_l",
                None, None)),
)

KEYS = ("F1 F2 F3 F4 F5 F6 F7 F8 F9 F10 F11 F12 A B C D E F G H I J K L M N O P Q R S T U V W "
        "X Y Z 0 1 2 3 4 5 6 7 8 9 Backtick Minus Plus Comma Period Semicolon Slash Backslash "
        "LBracket RBracket Quote Escape Return Space Tab Backspace Delete Insert Home End PageUp "
        "PageDown Left Right Up Down Shift Control Alt Numpad0 Numpad1 Numpad2 Numpad3 Numpad4 "
        "Numpad5 Numpad6 Numpad7 Numpad8 Numpad9 NumpadEnter NumpadPlus NumpadMinus NumpadStar "
        "NumpadSlash PrintScreen Pause CapsLock NumLock LMB RMB MMB").split()
CAP_STEMS = {
    "LMB": "mouse_left", "RMB": "mouse_right", "MMB": "mouse_scroll",
    "Return": "keyboard_enter", "Backtick": "keyboard_tilde", "Control": "keyboard_ctrl",
    "Slash": "keyboard_slash_forward", "Backslash": "keyboard_slash_back",
    "LBracket": "keyboard_bracket_open", "RBracket": "keyboard_bracket_close",
    "Quote": "keyboard_apostrophe", "PageUp": "keyboard_page_up", "PageDown": "keyboard_page_down",
    "Left": "keyboard_arrow_left", "Right": "keyboard_arrow_right",
    "Up": "keyboard_arrow_up", "Down": "keyboard_arrow_down",
    "NumpadEnter": "keyboard_numpad_enter", "NumpadPlus": "keyboard_numpad_plus",
    "NumpadMinus": "keyboard_minus", "NumpadStar": "keyboard_asterisk",
    "NumpadSlash": "keyboard_slash_forward",
    **{f"Numpad{n}": f"keyboard_{n}" for n in range(10)},
}
CLUSTERS = ("WASD", "ARROWS", "MOUSE")


def art(stem):
    path = os.path.join(ART, f"{stem}.png")
    if not os.path.exists(path):
        return None
    image = Image.open(path).convert("RGBA")
    box = image.getchannel("A").getbbox()
    return image.crop(box) if box else image


def cap_art(name):
    return art(CAP_STEMS.get(name, f"keyboard_{name.lower()}"))


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


def cluster(name, key):
    if name == "MOUSE":
        return art("mouse")
    if name == "ARROWS":
        return art("keyboard_arrows")
    out = Image.new("RGBA", (key * 3, key * 2), (0, 0, 0, 0))
    for stem, (cx, cy) in (("w", (key, 0)), ("a", (0, key)), ("s", (key, key)), ("d", (2 * key, key))):
        cap = fit(art(f"keyboard_{stem}"), key, key)
        out.alpha_composite(cap, (cx, cy))
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
    for name, stems in SETS:
        lines.append(f"set {name}")
        for (slot, _, w, h), stem in zip(SLOTS, stems):
            if stem is None:
                continue
            if isinstance(stem, tuple):
                lines.append(f"alias {slot:02X} {stem[1]:02X}")
                continue
            image = art(stem)
            if image is None:
                missing.append(stem)
                continue
            x0, y0, x1, y1 = shelf.put(cell_image(image, w * SCALE, h * SCALE))
            lines.append(f"cell {slot:02X} {x0} {y0} {x1} {y1}")
    lines.append("caps")
    key = SLOTS[0][3] * SCALE
    for name in KEYS:
        image = cap_art(name)
        if image is None:
            missing.append(name)
            continue
        width = min(2 * key, max(key, round(key * image.width / image.height)))
        x0, y0, x1, y1 = shelf.put(cell_image(image, width, key))
        lines.append(f"cap {name} {x0} {y0} {x1} {y1}")
    for name in CLUSTERS:
        image = cluster(name, key)
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
    print(f"{OUT_PNG}: {SHEET_WIDTH}x{height}, {len(shelf.placed)} cells; {OUT_TABLE}: {len(lines) + 1} lines")
    if missing:
        print("no art for:", " ".join(missing))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
