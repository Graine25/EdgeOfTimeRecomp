import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dds import write_dxt5  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ART = os.path.join(ROOT, "thirdparty", "xelu-prompts")
OUT_PNG = os.path.join(ROOT, "res", "textures", "Reeot_Icons.png")
OUT_DDS = os.path.join(ROOT, "res", "textures", "Reeot_Icons.dds")
OUT_TABLE = os.path.join(HERE, "icons.txt")

SCALE = 3
CAP_FILL = 0.86
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
    ("xboxseries", ("XboxSeriesX_A", "XboxSeriesX_B", "XboxSeriesX_Y", "XboxSeriesX_X",
                    "XboxSeriesX_RT", "XboxSeriesX_LT", "XboxSeriesX_Right_Stick",
                    "XboxSeriesX_Left_Stick", "XboxSeriesX_View", "XboxSeriesX_Menu",
                    "XboxSeriesX_RB", "XboxSeriesX_LB", "XboxSeriesX_Left_Stick",
                    "XboxSeriesX_Dpad")),
    ("playstation", ("PS5_Cross", "PS5_Circle", "PS5_Triangle", "PS5_Square",
                     "PS5_R2", "PS5_L2", "PS5_Right_Stick", "PS5_Left_Stick",
                     "PS5_Share", "PS5_Options", "PS5_R1", "PS5_L1", "PS5_Left_Stick",
                     "PS5_Dpad")),
    ("switch", ("Switch_B", "Switch_A", "Switch_X", "Switch_Y",
                "Switch_RT", "Switch_LT", "Switch_Right_Stick", "Switch_Left_Stick",
                "Switch_Minus", "Switch_Plus", "Switch_RB", "Switch_LB", "Switch_Left_Stick",
                "Switch_Dpad")),
    ("steamdeck", ("SteamDeck_A", "SteamDeck_B", "SteamDeck_Y", "SteamDeck_X",
                   "SteamDeck_R2", "SteamDeck_L2", "SteamDeck_Right_Stick",
                   "SteamDeck_Left_Stick", "SteamDeck_Square", "SteamDeck_Menu",
                   "SteamDeck_R1", "SteamDeck_L1", "SteamDeck_Left_Stick",
                   "SteamDeck_Dpad")),
)

KEYS = ("F1 F2 F3 F4 F5 F6 F7 F8 F9 F10 F11 F12 A B C D E F G H I J K L M N O P Q R S T U V W "
        "X Y Z 0 1 2 3 4 5 6 7 8 9 Backtick Minus Plus Comma Period Semicolon Slash "
        "LBracket RBracket Quote Escape Return Space Tab Backspace Delete Insert Home End PageUp "
        "PageDown Left Right Up Down Shift Control Alt Numpad0 Numpad1 Numpad2 Numpad3 Numpad4 "
        "Numpad5 Numpad6 Numpad7 Numpad8 Numpad9 NumpadEnter NumpadPlus NumpadMinus NumpadStar "
        "NumpadSlash PrintScreen CapsLock NumLock LMB RMB MMB").split()
CAP_STEMS = {
    "LMB": "Mouse_Left", "RMB": "Mouse_Right", "MMB": "Mouse_Middle",
    "Return": "Enter", "Escape": "Esc", "Backtick": "Tilda", "Control": "Ctrl",
    "Comma": "Mark_Left", "Period": "Mark_Right",
    "LBracket": "Bracket_Left", "RBracket": "Bracket_Right",
    "PageUp": "Page_Up", "PageDown": "Page_Down", "CapsLock": "Caps_Lock",
    "NumLock": "Num_Lock", "PrintScreen": "Print_Screen", "Delete": "Del", "Shift": "Shift_Alt",
    "Left": "Arrow_Left", "Right": "Arrow_Right", "Up": "Arrow_Up", "Down": "Arrow_Down",
    "NumpadEnter": "Enter", "NumpadPlus": "Plus", "NumpadMinus": "Minus",
    "NumpadStar": "Asterisk", "NumpadSlash": "Slash",
    **{f"Numpad{n}": str(n) for n in range(10)},
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
    return art(f"{CAP_STEMS.get(name, name)}_Key_Light")


def fit(image, width, height):
    scale = min(width / image.width, height / image.height)
    size = (max(1, round(image.width * scale)), max(1, round(image.height * scale)))
    scaled = image.resize(size, Image.LANCZOS)
    cell = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    cell.alpha_composite(scaled, ((width - size[0]) // 2, (height - size[1]) // 2))
    return cell


def inset(image, width, height):
    cell = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    cell.alpha_composite(image, ((width - image.width) // 2, (height - image.height) // 2))
    return cell


def cluster(name, key):
    if name == "MOUSE":
        return art("Mouse_Simple_Key_Light")
    up, left, down, right = (("W", "A", "S", "D") if name == "WASD"
                             else ("Arrow_Up", "Arrow_Left", "Arrow_Down", "Arrow_Right"))
    out = Image.new("RGBA", (key * 3, key * 2), (0, 0, 0, 0))
    for stem, (cx, cy) in ((up, (key, 0)), (left, (0, key)), (down, (key, key)), (right, (2 * key, key))):
        cap = fit(art(f"{stem}_Key_Light"), key, key)
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
            image = art(stem)
            if image is None:
                missing.append(stem)
                continue
            x0, y0, x1, y1 = shelf.put(fit(image, w * SCALE, h * SCALE))
            lines.append(f"cell {slot:02X} {x0} {y0} {x1} {y1}")
    lines.append("caps")
    key = SLOTS[0][3] * SCALE
    for name in KEYS:
        image = cap_art(name)
        if image is None:
            missing.append(name)
            continue
        width = min(2 * key, max(key, round(key * image.width / image.height)))
        x0, y0, x1, y1 = shelf.put(inset(fit(image, round(width * CAP_FILL), round(key * CAP_FILL)), width, key))
        lines.append(f"cap {name} {x0} {y0} {x1} {y1}")
    for name in CLUSTERS:
        image = cluster(name, key)
        width = key * 3 // 2 if name != "MOUSE" else key
        cell = inset(fit(image, round(width * CAP_FILL), round(key * CAP_FILL)), width, key)
        x0, y0, x1, y1 = shelf.put(cell)
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
