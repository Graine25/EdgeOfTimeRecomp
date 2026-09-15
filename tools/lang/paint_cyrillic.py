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


def erase_bar(glyph):
    g = glyph.copy()
    px = g.load()
    w, h = g.size
    cover = [sum(1 for x in range(w) if px[x, y][3] > 40) for y in range(h)]
    top, bottom = h // 3, 2 * h // 3
    for y in range(top, bottom + 1):
        above = cover[max(0, y - 3)]
        if cover[y] >= w * 0.8 and above < w * 0.8:
            yy = y
            while yy < h and cover[yy] >= w * 0.8:
                for x in range(w):
                    if px[x, yy][3] > 40 and w * 0.25 < x < w * 0.75:
                        px[x, yy] = (0, 0, 0, 0)
                yy += 1
            break
    return g


def main():
    plan_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "cyrillic_glyphs.json")
    pkztool = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "out", "pkzlib", "pkztool.exe")
    pak = sys.argv[3] if len(sys.argv) > 3 else os.path.join(ROOT, "..", "reeot", "reference_pak", "Main.pak")
    plan = json.load(open(plan_path, encoding="utf-8"))
    for font, spec in plan["fonts"].items():
        table = glyph_table(pkztool, pak, font)
        atlas = Image.open(os.path.join(ROOT, spec["atlas"])).convert("RGBA")
        source = Image.open(os.path.join(ROOT, spec["source"])).convert("RGBA")
        check = atlas.copy()
        draw = ImageDraw.Draw(check)
        painted = 0
        for code, g in spec["glyphs"].items():
            cp = int(code, 16)
            if cp not in table:
                print(f"  {font}: no cell for {code} ({g['letter']}); skipped")
                continue
            x0, y0, x1, y1 = cell(table, cp, atlas.size)
            if "box" in g:
                shape = tight(source.crop(tuple(g["box"])))
            else:
                shape = tight(atlas.crop(cell(table, ord(g["from"]), atlas.size)))
                if g.get("erase_bar"):
                    shape = erase_bar(shape)
            ref = cell(table, ord(g["ref"]), atlas.size)
            target_h = max(1, ref[3] - ref[1])
            scale = target_h / max(1, shape.height)
            new_w = max(1, int(round(shape.width * scale)))
            if new_w > (x1 - x0):
                scale = (x1 - x0) / max(1, shape.width)
                new_w = max(1, x1 - x0)
            new_h = max(1, int(round(shape.height * scale)))
            shape = shape.resize((new_w, new_h), Image.LANCZOS)
            atlas.paste((0, 0, 0, 0), (x0, y0, x1, y1))
            atlas.alpha_composite(shape, (x0, max(y0, y1 - new_h)))
            draw.rectangle((x0, y0, x1 - 1, y1 - 1), outline=(255, 0, 0, 255))
            painted += 1
        out = os.path.join(ROOT, spec["out"])
        atlas.save(out)
        check.alpha_composite(atlas)
        check.save(out[:-4] + ".boxes.png")
        print(f"{font}: {painted} letters painted -> {spec['out']}")


if __name__ == "__main__":
    main()
