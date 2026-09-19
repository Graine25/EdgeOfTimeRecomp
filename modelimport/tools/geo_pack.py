import struct
from collections import defaultdict
import numpy as np

FMT_SKINNED_1UV = 0x5627
STRIDE = 32

def pack_hend3n(n):
    x = np.clip(np.round(n[:, 0] * 1023), -1023, 1023).astype(np.int64) & 0x7FF
    y = np.clip(np.round(n[:, 1] * 1023), -1023, 1023).astype(np.int64) & 0x7FF
    z = np.clip(np.round(n[:, 2] * 511), -511, 511).astype(np.int64) & 0x3FF
    return (x | (y << 11) | (z << 22)).astype(np.uint32)

def pack_dec3n(t, w):
    x = np.clip(np.round(t[:, 0] * 511), -511, 511).astype(np.int64) & 0x3FF
    y = np.clip(np.round(t[:, 1] * 511), -511, 511).astype(np.int64) & 0x3FF
    z = np.clip(np.round(t[:, 2] * 511), -511, 511).astype(np.int64) & 0x3FF
    ww = np.where(np.asarray(w) < 0, 3, 1).astype(np.int64)
    return (x | (y << 10) | (z << 20) | (ww << 30)).astype(np.uint32)

MAX_INFLUENCES = 3

def format_for(nuv):
    return (FMT_SKINNED_1UV & ~0xE0) | ((nuv & 7) << 5)

def stride_for(nuv):
    return 12 + 4 + 4 + 4 * nuv + 8

def pack_vertices(P, N, T, TW, UV, J, W, extra_uvs=()):
    n = len(P)
    nuv = 1 + len(extra_uvs)
    stride = stride_for(nuv)
    skin = 20 + 4 * nuv
    out = np.zeros((n, stride), np.uint8)
    out[:, 0:12] = np.ascontiguousarray(P.astype('>f4')).view(np.uint8).reshape(n, 12)
    out[:, 12:16] = pack_hend3n(N).astype('>u4').view(np.uint8).reshape(n, 4)
    out[:, 16:20] = pack_dec3n(T, TW).astype('>u4').view(np.uint8).reshape(n, 4)
    for k, uvk in enumerate((UV,) + tuple(extra_uvs)):
        uv = np.ascontiguousarray(np.clip(np.round(np.asarray(uvk) * 1024), -32768, 32767).astype('>i2'))
        out[:, 20 + 4 * k:24 + 4 * k] = uv.view(np.uint8).reshape(n, 4)
    order = np.argsort(-W, axis=1)[:, :MAX_INFLUENCES]
    Wl = np.take_along_axis(W, order, 1); Jl = np.take_along_axis(J, order, 1)
    Wl = Wl / np.maximum(Wl.sum(1, keepdims=True), 1e-12)
    wq = np.floor(Wl * 255 + 0.5).astype(np.int64)
    wq[:, 0] += 255 - wq.sum(1)
    jl = np.where(wq > 0, Jl, 0)
    mem_w = np.zeros((n, 4), np.int64); mem_j = np.zeros((n, 4), np.int64)
    mem_w[:, 3:0:-1] = wq
    mem_j[:, 3:0:-1] = jl
    out[:, skin:skin + 4] = mem_j.astype(np.uint8)
    out[:, skin + 4:skin + 8] = mem_w.astype(np.uint8)
    return out.tobytes()

def orient(P, N, tris):
    a, b, c = P[tris[:, 0]], P[tris[:, 1]], P[tris[:, 2]]
    fn = np.cross(b - a, c - a)
    vn = N[tris[:, 0]] + N[tris[:, 1]] + N[tris[:, 2]]
    flip = (fn * vn).sum(1) < 0
    t = tris.copy(); t[flip] = t[flip][:, [0, 2, 1]]
    return t, int(flip.sum())

def stripify(tris):
    tris = np.asarray(tris, np.int64)
    ntri = len(tris)
    edge_tris = defaultdict(list)
    for ti, (a, b, c) in enumerate(tris):
        for e in ((a, b), (b, c), (c, a)):
            edge_tris[(min(e), max(e))].append(ti)
    used = np.zeros(ntri, bool)
    out = []
    def third(ti, a, b):
        for v in tris[ti]:
            if v != a and v != b:
                return v
    def same_tri(dec, ti):
        t = tuple(tris[ti])
        return any(dec == (t[i], t[(i + 1) % 3], t[(i + 2) % 3]) for i in range(3))
    for start in range(ntri):
        if used[start]:
            continue
        used[start] = True
        a, b, c = tris[start]
        run = [a, b, c]
        while True:
            u, v = run[-2], run[-1]
            cands = [t for t in edge_tris[(min(u, v), max(u, v))] if not used[t]]
            if not cands:
                break
            k = len(run) - 2
            picked = None
            for ti in cands:
                w = third(ti, u, v)
                dec = (u, v, w) if k % 2 == 0 else (v, u, w)
                if same_tri(dec, ti):
                    picked = (ti, w); break
            if picked is None:
                break
            used[picked[0]] = True
            run.append(picked[1])
        out += run + [0xFFFF]
    return out

def strip_to_tris(strip):
    out = []; run = []
    for x in list(strip) + [0xFFFF]:
        if x == 0xFFFF:
            for k in range(len(run) - 2):
                a, b, c = run[k], run[k + 1], run[k + 2]
                if a == b or b == c or a == c:
                    continue
                out.append((a, b, c) if k % 2 == 0 else (b, a, c))
            run = []
        else:
            run.append(x)
    return np.array(out, np.int64).reshape(-1, 3)

def split_palettes(tris, J, W, max_bones=58):
    Jl = J.tolist(); Wl = W.tolist()
    tri_bones = []
    for t in tris:
        s = set()
        for v in t:
            for j, w in zip(Jl[v], Wl[v]):
                if w > 0: s.add(j)
        tri_bones.append(s)
    def dominant(i):
        acc = defaultdict(float)
        for v in tris[i]:
            for j, w in zip(Jl[v], Wl[v]):
                acc[j] += w
        return max(acc.items(), key=lambda kv: kv[1])[0]
    remaining = sorted(range(len(tris)), key=dominant)
    groups = []
    while remaining:
        pal = set(); members = []; rest = []
        for i in remaining:
            need = tri_bones[i] | pal
            if len(need) <= max_bones:
                pal = need; members.append(i)
            else:
                rest.append(i)
        groups.append((np.array(members), sorted(pal)))
        remaining = rest
    return groups
