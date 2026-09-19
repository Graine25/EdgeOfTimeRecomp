import struct
import numpy as np

def a32(v):
    return (v + 31) & ~31

def rgb565(rgb):
    r = (rgb[..., 0] >> 3).astype(np.uint32); g = (rgb[..., 1] >> 2).astype(np.uint32); b = (rgb[..., 2] >> 3).astype(np.uint32)
    return (r << 11) | (g << 5) | b

def from565(c):
    r = ((c >> 11) & 31).astype(np.float32) * (255.0 / 31.0)
    g = ((c >> 5) & 63).astype(np.float32) * (255.0 / 63.0)
    b = (c & 31).astype(np.float32) * (255.0 / 31.0)
    return np.stack([r, g, b], -1)

def blocks_of(img, channels):
    h, w = img.shape[:2]
    assert h % 4 == 0 and w % 4 == 0
    return img.reshape(h // 4, 4, w // 4, 4, channels).transpose(0, 2, 1, 3, 4).reshape(-1, 16, channels)

def _pca_axis(X):
    mean = X.mean(1, keepdims=True)
    Y = X - mean
    C = np.einsum('nki,nkj->nij', Y, Y)
    v = np.ones((len(X), 3)) / np.sqrt(3)
    for _ in range(6):
        v = np.einsum('nij,nj->ni', C, v)
        nrm = np.linalg.norm(v, axis=1, keepdims=True)
        v = np.where(nrm > 1e-6, v / np.maximum(nrm, 1e-6), np.array([1.0, 0, 0]))
    return v, mean[:, 0]

def _index_colours(X, c0, c1):
    pal = np.stack([c0, c1, (2 * c0 + c1) / 3, (c0 + 2 * c1) / 3], 1)
    d = ((X[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)
    idx = d.argmin(-1)
    return idx, d.min(-1).sum(1)

def encode_colour_blocks(rgb_blocks):
    X = rgb_blocks.astype(np.float32)
    axis, mean = _pca_axis(X)
    t = np.einsum('nki,ni->nk', X - mean[:, None, :], axis)
    lo = mean + axis * t.min(1)[:, None]; hi = mean + axis * t.max(1)[:, None]
    lo = np.clip(lo, 0, 255); hi = np.clip(hi, 0, 255)

    def evaluate(hi, lo):
        q0 = rgb565(np.round(hi).astype(np.uint8)); q1 = rgb565(np.round(lo).astype(np.uint8))
        idx, err = _index_colours(X, from565(q0), from565(q1))
        swap = q0 < q1
        q0s = np.where(swap, q1, q0); q1s = np.where(swap, q0, q1)
        idxs = np.where(swap[:, None], idx ^ 1, idx)
        idxs = np.where((q0s == q1s)[:, None], 0, idxs)
        return q0s, q1s, idxs, err, idx

    q0, q1, idx, err, raw_idx = evaluate(hi, lo)
    wts = np.array([[1.0, 0.0], [0.0, 1.0], [2 / 3, 1 / 3], [1 / 3, 2 / 3]])[raw_idx]
    A = np.einsum('nki,nkj->nij', wts, wts)
    B = np.einsum('nki,nkc->nic', wts, X)
    det = A[:, 0, 0] * A[:, 1, 1] - A[:, 0, 1] * A[:, 1, 0]
    ok = np.abs(det) > 1e-6
    inv = np.zeros_like(A)
    inv[ok, 0, 0] = A[ok, 1, 1] / det[ok]; inv[ok, 1, 1] = A[ok, 0, 0] / det[ok]
    inv[ok, 0, 1] = -A[ok, 0, 1] / det[ok]; inv[ok, 1, 0] = -A[ok, 1, 0] / det[ok]
    E = np.einsum('nij,njc->nic', inv, B)
    hi2 = np.where(ok[:, None], np.clip(E[:, 0], 0, 255), hi); lo2 = np.where(ok[:, None], np.clip(E[:, 1], 0, 255), lo)
    q0b, q1b, idxb, errb, _ = evaluate(hi2, lo2)
    better = errb < err
    q0 = np.where(better, q0b, q0); q1 = np.where(better, q1b, q1); idx = np.where(better[:, None], idxb, idx)
    bits = np.zeros(len(X), np.uint32)
    for k in range(16):
        bits |= (idx[:, k].astype(np.uint32) & 3) << (2 * k)
    return q0.astype(np.uint16), q1.astype(np.uint16), bits

def encode_dxt1(rgb, alpha=None):
    q0, q1, bits = encode_colour_blocks(blocks_of(rgb, 3))
    if alpha is not None:
        ab = blocks_of(alpha[..., None], 1)[:, :, 0] < 128
        punch = ab.any(1)
        if punch.any():
            X = blocks_of(rgb, 3)[punch].astype(np.float32)
            keep = ~ab[punch]
            lo = np.where(keep[..., None], X, 255).min(1); hi = np.where(keep[..., None], X, 0).max(1)
            a0 = rgb565(np.round(lo).astype(np.uint8)); a1 = rgb565(np.round(hi).astype(np.uint8))
            c0 = np.minimum(a0, a1); c1 = np.maximum(a0, a1)
            pal = np.stack([from565(c0), from565(c1), (from565(c0) + from565(c1)) / 2], 1)
            dist = ((X[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)
            idx = dist.argmin(-1)
            idx = np.where(keep, idx, 3)
            b = np.zeros(len(X), np.uint32)
            for k in range(16):
                b |= (idx[:, k].astype(np.uint32) & 3) << (2 * k)
            q0 = q0.copy(); q1 = q1.copy(); bits = bits.copy()
            q0[punch] = c0.astype(np.uint16); q1[punch] = c1.astype(np.uint16); bits[punch] = b
    out = np.zeros((len(q0), 8), np.uint8)
    out[:, 0:2] = q0.astype('<u2').view(np.uint8).reshape(-1, 2)
    out[:, 2:4] = q1.astype('<u2').view(np.uint8).reshape(-1, 2)
    out[:, 4:8] = bits.astype('<u4').view(np.uint8).reshape(-1, 4)
    return out.tobytes()

def encode_alpha_blocks(a_blocks):
    A = a_blocks.astype(np.float32)
    a0 = A.max(1); a1 = A.min(1)
    flat = a0 == a1
    a0 = np.where(flat, np.minimum(a0 + 1, 255), a0); a1 = np.where(flat, np.minimum(a1, 254), a1)
    pal = np.stack([a0, a1] + [((7 - i) * a0 + i * a1) / 7 for i in range(1, 7)], 1)
    idx = np.abs(A[:, :, None] - pal[:, None, :]).argmin(-1)
    bits = np.zeros(len(A), np.uint64)
    for k in range(16):
        bits |= (idx[:, k].astype(np.uint64) & 7) << np.uint64(3 * k)
    out = np.zeros((len(A), 8), np.uint8)
    out[:, 0] = a0.astype(np.uint8); out[:, 1] = a1.astype(np.uint8)
    out[:, 2:8] = bits.astype('<u8').view(np.uint8).reshape(-1, 8)[:, :6]
    return out

def encode_dxt5(rgba):
    b = blocks_of(rgba, 4)
    alpha = encode_alpha_blocks(b[:, :, 3])
    q0, q1, bits = encode_colour_blocks(b[:, :, :3])
    out = np.zeros((len(q0), 16), np.uint8)
    out[:, 0:8] = alpha
    out[:, 8:10] = q0.astype('<u2').view(np.uint8).reshape(-1, 2)
    out[:, 10:12] = q1.astype('<u2').view(np.uint8).reshape(-1, 2)
    out[:, 12:16] = bits.astype('<u4').view(np.uint8).reshape(-1, 4)
    return out.tobytes()

def mip_chain(img, levels):
    out = [img]
    cur = img.astype(np.float32)
    for _ in range(levels - 1):
        h, w = cur.shape[:2]
        if h < 8 or w < 8:
            break
        cur = (cur[0::2, 0::2] + cur[1::2, 0::2] + cur[0::2, 1::2] + cur[1::2, 1::2]) / 4
        out.append(np.round(cur).astype(np.uint8))
    return out

def tiled_offsets(bw, bh, bpe):
    aw = a32(bw)
    logbpp = (bpe >> 2) + ((bpe >> 1) >> (bpe >> 2))
    y, x = np.meshgrid(np.arange(bh, dtype=np.int64), np.arange(bw, dtype=np.int64), indexing='ij')
    macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (logbpp + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << logbpp
    off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + logbpp)) + ((y & 1) << 4)
    return (((off & ~0x1FF) << 3) + ((off & 0x1C0) << 2) + (off & 0x3F) +
            ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> logbpp

def tile(linear_blocks, bw, bh, bpe):
    src = np.frombuffer(linear_blocks, np.uint8).reshape(bh, bw, bpe)
    dst = np.zeros((a32(bw) * a32(bh), bpe), np.uint8)
    idx = tiled_offsets(bw, bh, bpe)
    dst[idx.ravel()] = src.reshape(-1, bpe)
    return dst.tobytes()

def swap16(buf):
    a = np.frombuffer(buf, np.uint8).reshape(-1, 2)[:, ::-1]
    return a.tobytes()

FMT_DXT1_GAMMA = 0x1A207F52
FMT_DXT1_LINEAR = 0x1A200152
FMT_DXT5_NORMAL = 0x1A215554

FMT_DXT5_GAMMA = 0x1A207F54

def texture_data(levels_rgba, fmt, normal_map=False, punch_alpha=False):
    dxt5 = fmt in (FMT_DXT5_NORMAL, FMT_DXT5_GAMMA)
    bpe = 16 if dxt5 else 8
    body = b''
    sizes = []
    for lv in levels_rgba:
        h, w = lv.shape[:2]
        blocks = encode_dxt5(lv) if dxt5 else encode_dxt1(np.ascontiguousarray(lv[..., :3]), lv[..., 3] if punch_alpha else None)
        bw, bh = w // 4, h // 4
        t = swap16(tile(blocks, bw, bh, bpe))
        body += t; sizes.append(len(t))
    W, H = levels_rgba[0].shape[1], levels_rgba[0].shape[0]
    mips = len(levels_rgba)
    chain_off = sizes[0] + (sizes[1] if mips > 1 else 0)
    lvl = min(2, mips - 1)
    tail = struct.pack('>3I', chain_off if mips > 1 else 0xFFFFFFFF, max(1, mips - 2), ((W >> lvl) << 16) | (H >> lvl))
    if normal_map:
        desc = struct.pack('>2I', 0, 5) + struct.pack('>f', 1.0) + struct.pack('>4I', W, H, mips, 0) + struct.pack('>I', fmt) + struct.pack('>5I', 0, 0, 0, 0xFFFFFFFF, 0) + tail
    else:
        desc = struct.pack('>2I', 0, 1) + struct.pack('>4I', W, H, mips, 0) + struct.pack('>I', fmt) + struct.pack('>5I', 0, 0, 0, 0xFFFFFFFF, 0) + tail
    return desc + body

def texture_chunks(name, crc, payload, chunk, build_header):
    import zlib
    plcrc = zlib.crc32(payload) & 0xFFFFFFFF
    entry = chunk(0x138D, 1, children=[build_header(crc, 4, 0x1F, 0, 0, plcrc, name), chunk(0x191, 6, b'')])
    post = chunk(0x26, 0, children=[build_header(crc, 4, 0x1F, 0, 0, plcrc, name), chunk(0x195, 7, payload)])
    return entry, post
