import json
import os
import subprocess
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


def glyph_table(pkztool, pak, font):
    out = subprocess.run([pkztool, "font", pak, font], capture_output=True, text=True, check=True).stdout
    table = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 9 and parts[0].startswith("U+"):
            table[int(parts[0][2:], 16)] = tuple(float(v) for v in parts[2:6])
    if not table:
        sys.exit(f"no glyphs for {font} in {pak}")
    return table


def cell(table, cp, size):
    u0, v0, u1, v1 = table[cp]
    w, h = size
    return (int(round(u0 * w)), int(round(v0 * h)), int(round(u1 * w)), int(round(v1 * h)))


def tight(img):
    box = img.split()[3].getbbox()
    return img.crop(box) if box else img


def add_bar(glyph):
    g = glyph.copy()
    px = g.load()
    w, h = g.size
    cover = [sum(1 for x in range(w) if px[x, y][3] > 40) for y in range(h)]
    thick = 0
    while thick < h and cover[thick] >= w * 0.5:
        thick += 1
    thick = max(2, thick)
    y0 = (h - thick) // 2
    for y in range(thick):
        for x in range(int(w * 0.4), w):
            if px[x, y][3] > px[x, y0 + y][3]:
                px[x, y0 + y] = px[x, y]
    return g


class Packer:
    def __init__(self, width, top, row_height):
        self.width, self.top, self.row_height = width, top, row_height
        self.x, self.y = 1, top + 1
        self.bottom = top

    def place(self, w, h):
        if h + 2 > self.row_height:
            raise SystemExit(f"a {h}-row cell does not fit the {self.row_height}-row rows")
        if self.x + w + 1 > self.width:
            self.x = 1
            self.y += self.row_height
        box = (self.x, self.y, self.x + w, self.y + h)
        self.x += w + 1
        self.bottom = max(self.bottom, self.y + self.row_height)
        return box


def build(font, spec, pkztool, pak):
    table = glyph_table(pkztool, pak, font)
    atlas = Image.open(os.path.join(ROOT, spec["atlas"])).convert("RGBA")
    source = Image.open(os.path.join(ROOT, spec["source"])).convert("RGBA")
    width, retail_h = atlas.size
    classes = {}
    for name, c in spec["classes"].items():
        ref = cell(table, ord(c["retail"]), atlas.size)
        classes[name] = (ref[3] - ref[1]) / float(c["source_h"])

    shapes = []
    for code, g in sorted(spec["glyphs"].items()):
        if "box" in g:
            shape = tight(source.crop(tuple(g["box"])))
            scale = classes[g["class"]]
        else:
            shape = tight(atlas.crop(cell(table, ord(g["from"]), atlas.size)))
            scale = classes[g["class"]] * float(spec["classes"][g["class"]]["source_h"]) / shape.height
        if g.get("flip") == "v":
            shape = shape.transpose(Image.FLIP_TOP_BOTTOM)
        if g.get("mirror"):
            shape = shape.transpose(Image.FLIP_LEFT_RIGHT)
        if g.get("bar"):
            shape = add_bar(shape)
        w = max(1, int(round(shape.width * scale)))
        h = max(1, int(round(shape.height * scale)))
        if (w, h) != shape.size:
            shape = shape.resize((w, h), Image.LANCZOS)
        base = h - int(round(g.get("desc", 0) * scale))
        shapes.append((int(code, 16), g["letter"], shape, base))

    packer = Packer(width, retail_h, spec["row_height"])
    cells = [(cp, letter, shape, base, packer.place(shape.width, shape.height)) for cp, letter, shape, base in shapes]
    new_h = (packer.bottom + 15) // 16 * 16
    out = Image.new("RGBA", (width, new_h), (0, 0, 0, 0))
    out.paste(atlas, (0, 0))
    for cp, letter, shape, base, box in cells:
        out.alpha_composite(shape, (box[0], box[1]))
    out_path = os.path.join(ROOT, spec["out"])
    out.save(out_path)

    check = Image.new("RGBA", out.size, (0, 0, 0, 255))
    check.alpha_composite(out)
    draw = ImageDraw.Draw(check)
    for cp, letter, shape, base, box in cells:
        draw.rectangle((box[0], box[1], box[2] - 1, box[3] - 1), outline=(255, 0, 0, 255))
        draw.line((box[0], box[1] + base - 1, box[2] - 1, box[1] + base - 1), fill=(0, 255, 0, 255))
    check.save(out_path[:-4] + ".boxes.png")

    prefix = spec["prefix"]
    lines = [f"# {font}: the Cyrillic glyph table, written by tools/lang/build_cyrillic_atlas.py -- do not edit.",
             f"{prefix}_0000\tsize {width} {retail_h} {new_h}"]
    n = 1
    for cp, letter, shape, base, box in cells:
        lines.append(f"{prefix}_{n:04d}\tcell {cp:04X} {box[0]} {box[1]} {box[2]} {box[3]} {base}")
        n += 1
    for code, target in spec["alias"].items():
        target_cp = ord(target) if len(target) == 1 else int(target, 16)
        lines.append(f"{prefix}_{n:04d}\talias {int(code, 16):04X} {target_cp:04X}")
        n += 1
    table_path = os.path.join(ROOT, spec["table"])
    os.makedirs(os.path.dirname(table_path), exist_ok=True)
    with open(table_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{font}: {len(cells)} cells in rows {retail_h}..{packer.bottom} of {width}x{new_h}, "
          f"{len(spec['alias'])} aliases -> {spec['out']}, {spec['table']}")


def main():
    plan_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "cyrillic_glyphs.json")
    pkztool = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        ROOT, "out", "build", "win-amd64-relwithdebinfo", "thirdparty", "PKZLib", "pkztool.exe")
    pak = sys.argv[3] if len(sys.argv) > 3 else os.path.abspath(
        os.path.join(ROOT, "..", "reeot", "reference_pak", "Main.pak"))
    plan = json.load(open(plan_path, encoding="utf-8"))
    for font, spec in plan["fonts"].items():
        build(font, spec, pkztool, pak)


if __name__ == "__main__":
    main()
