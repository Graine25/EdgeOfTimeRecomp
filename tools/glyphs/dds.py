import struct

import numpy as np


def _rgb565(rgb):
    r = (rgb[..., 0] >> 3).astype(np.uint16)
    g = (rgb[..., 1] >> 2).astype(np.uint16)
    b = (rgb[..., 2] >> 3).astype(np.uint16)
    return (r << 11) | (g << 5) | b


def _from565(c):
    r = ((c >> 11) & 31).astype(np.float32) * (255.0 / 31.0)
    g = ((c >> 5) & 63).astype(np.float32) * (255.0 / 63.0)
    b = (c & 31).astype(np.float32) * (255.0 / 31.0)
    return np.stack([r, g, b], axis=-1)


def encode_dxt5(rgba):
    h, w, _ = rgba.shape
    assert h % 4 == 0 and w % 4 == 0, "DXT needs sides that are multiples of 4"
    bh, bw = h // 4, w // 4
    blocks = rgba.reshape(bh, 4, bw, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * bw, 16, 4)
    n = blocks.shape[0]
    out = np.zeros((n, 16), dtype=np.uint8)

    alpha = blocks[:, :, 3].astype(np.int32)
    a0 = alpha.max(axis=1)
    a1 = alpha.min(axis=1)
    flat = a0 == a1
    levels = np.empty((n, 8), dtype=np.float32)
    levels[:, 0] = a0
    levels[:, 1] = a1
    for i in range(1, 7):
        levels[:, i + 1] = ((7 - i) * a0 + i * a1) / 7.0
    dist = np.abs(alpha[:, :, None] - levels[:, None, :])
    aidx = dist.argmin(axis=2).astype(np.uint64)
    aidx[flat] = 0
    out[:, 0] = a0.astype(np.uint8)
    out[:, 1] = a1.astype(np.uint8)
    bits = np.zeros(n, dtype=np.uint64)
    for i in range(16):
        bits |= aidx[:, i] << np.uint64(3 * i)
    for i in range(6):
        out[:, 2 + i] = ((bits >> np.uint64(8 * i)) & np.uint64(0xFF)).astype(np.uint8)

    rgb = blocks[:, :, :3].astype(np.int32)
    opaque = alpha > 0
    weight = rgb.sum(axis=2)
    hi = np.where(opaque, weight, -1).argmax(axis=1)
    lo = np.where(opaque, weight, 1 << 20).argmin(axis=1)
    c0 = _rgb565(rgb[np.arange(n), hi])
    c1 = _rgb565(rgb[np.arange(n), lo])
    swap = c0 < c1
    c0s = np.where(swap, c1, c0)
    c1s = np.where(swap, c0, c1)
    p0 = _from565(c0s)
    p1 = _from565(c1s)
    palette = np.stack([p0, p1, (2 * p0 + p1) / 3.0, (p0 + 2 * p1) / 3.0], axis=1)
    d = ((rgb[:, :, None, :].astype(np.float32) - palette[:, None, :, :]) ** 2).sum(axis=3)
    cidx = d.argmin(axis=2).astype(np.uint32)
    cidx[c0s == c1s] = 0
    out[:, 8] = (c0s & 0xFF).astype(np.uint8)
    out[:, 9] = (c0s >> 8).astype(np.uint8)
    out[:, 10] = (c1s & 0xFF).astype(np.uint8)
    out[:, 11] = (c1s >> 8).astype(np.uint8)
    cbits = np.zeros(n, dtype=np.uint32)
    for i in range(16):
        cbits |= cidx[:, i] << np.uint32(2 * i)
    for i in range(4):
        out[:, 12 + i] = ((cbits >> np.uint32(8 * i)) & 0xFF).astype(np.uint8)
    return out.tobytes()


def write_dxt5(image, path):
    rgba = np.asarray(image.convert("RGBA"), dtype=np.uint8)
    h, w = rgba.shape[:2]
    data = encode_dxt5(rgba)
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000
    header = struct.pack("<4sI I I I I I I 11I", b"DDS ", 124, flags, h, w, len(data), 0, 1, *([0] * 11))
    pixel_format = struct.pack("<I I 4s I I I I I", 32, 0x4, b"DXT5", 0, 0, 0, 0, 0)
    caps = struct.pack("<I I I I I", 0x1000, 0, 0, 0, 0)
    with open(path, "wb") as out:
        out.write(header + pixel_format + caps + data)
    return w, h, len(data)
