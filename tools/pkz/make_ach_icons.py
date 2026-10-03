import math
import os
import struct
import sys
import zlib

COLUMNS = 8
ROWS = 6
CELL = 64
SUBSAMPLES = 4
PLATE_FILL = (14, 16, 26, 236)
PLATE_EDGE = (126, 136, 172, 255)
GLYPH = (158, 168, 208, 255)


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, palette, alpha = 8, b"", None, None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b"tRNS":
            alpha = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if depth != 8 or interlace:
        raise ValueError(f"{path}: only 8-bit, non-interlaced PNGs are read")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, previous, at = [], bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[at], bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + up) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 0xFF
            elif kind == 4:
                guess = left + up - corner
                pa, pb, pc = abs(guess - left), abs(guess - up), abs(guess - corner)
                line[i] = (line[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 0xFF
        rows.append(line)
        previous = line
    pixels = []
    for line in rows:
        for x in range(width):
            p = line[x * channels:(x + 1) * channels]
            if colour == 6:
                pixels.append(tuple(p))
            elif colour == 2:
                pixels.append((p[0], p[1], p[2], 255))
            elif colour == 4:
                pixels.append((p[0], p[0], p[0], p[1]))
            elif colour == 0:
                pixels.append((p[0], p[0], p[0], 255))
            else:
                r, g, b = palette[p[0]]
                pixels.append((r, g, b, alpha[p[0]] if alpha and p[0] < len(alpha) else 255))
    return width, height, pixels


def write_png(path, width, height, pixels):
    rows = b"".join(b"\x00" + bytes(c for p in pixels[y * width:(y + 1) * width] for c in p) for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n")
        out.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        out.write(chunk(b"IDAT", zlib.compress(rows, 9)))
        out.write(chunk(b"IEND", b""))


def resize(width, height, pixels, size):
    if width == size and height == size:
        return pixels
    out = []
    for y in range(size):
        for x in range(size):
            out.append(pixels[(y * height // size) * width + x * width // size])
    return out


def rounded_box_distance(x, y, low, high, radius):
    cx, cy = (low + high) / 2.0, (low + high) / 2.0
    half = (high - low) / 2.0 - radius
    qx, qy = abs(x - cx) - half, abs(y - cy) - half
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - radius


def in_glyph(x, y):
    cx, cy, outer, thickness = 32.0, 24.0, 10.0, 5.0
    inner = outer - thickness
    distance = math.hypot(x - cx, y - cy)
    angle = math.degrees(math.atan2(cy - y, x - cx))
    if inner <= distance <= outer and (angle >= -90.0 or angle <= -160.0):
        return True
    middle = outer - thickness / 2.0
    if abs(x - cx) <= thickness / 2.0 and cy + middle - 2.0 <= y <= cy + middle + 7.5:
        return True
    return math.hypot(x - cx, y - 45.5) <= 3.3


def secret_plate():
    inset, radius, edge = CELL // 10, CELL // 9, max(1, CELL // 28)
    pixels = []
    for y in range(CELL):
        for x in range(CELL):
            total = [0.0, 0.0, 0.0, 0.0]
            for sy in range(SUBSAMPLES):
                for sx in range(SUBSAMPLES):
                    px, py = x + (sx + 0.5) / SUBSAMPLES, y + (sy + 0.5) / SUBSAMPLES
                    distance = rounded_box_distance(px, py, inset, CELL - inset, radius)
                    if distance > 0.0:
                        continue
                    colour = GLYPH if in_glyph(px, py) else PLATE_EDGE if distance > -edge else PLATE_FILL
                    a = colour[3] / 255.0
                    total[0] += colour[0] * a
                    total[1] += colour[1] * a
                    total[2] += colour[2] * a
                    total[3] += a
            count = SUBSAMPLES * SUBSAMPLES
            a = total[3] / count
            if a <= 0.0:
                pixels.append((0, 0, 0, 0))
                continue
            pixels.append(tuple(round(total[i] / total[3]) for i in range(3)) + (round(a * 255),))
    return pixels


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: make_ach_icons.py <icons dir> <out.png>")
    icons_dir, destination = sys.argv[1], sys.argv[2]
    icons = []
    for image_id in range(1, COLUMNS * ROWS):
        path = os.path.join(icons_dir, f"{image_id}.png")
        if not os.path.exists(path):
            break
        icons.append(path)
    if not icons:
        sys.exit(f"no <image_id>.png icons in {icons_dir}")
    width, height = COLUMNS * CELL, ROWS * CELL
    sheet = [(0, 0, 0, 0)] * (width * height)

    def paste(slot, pixels):
        ox, oy = (slot % COLUMNS) * CELL, (slot // COLUMNS) * CELL
        for y in range(CELL):
            sheet[(oy + y) * width + ox:(oy + y) * width + ox + CELL] = pixels[y * CELL:(y + 1) * CELL]

    for slot, path in enumerate(icons):
        w, h, pixels = read_png(path)
        paste(slot, resize(w, h, pixels, CELL))
    paste(COLUMNS * ROWS - 1, secret_plate())
    os.makedirs(os.path.dirname(os.path.abspath(destination)), exist_ok=True)
    write_png(destination, width, height, sheet)
    print(f"{destination}: {width}x{height}, {len(icons)} icons and the secret plate")


if __name__ == "__main__":
    main()
