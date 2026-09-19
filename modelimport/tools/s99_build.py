import sys, os, re, struct, zlib, time
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pakwriter import Pak, assemble, chunk, whole, build_header, kids, patch_header_in, check_alignment
from geo_pack import pack_vertices, stripify, split_palettes, orient, FMT_SKINNED_1UV, STRIDE, format_for, stride_for
from tex_pack import texture_data, texture_chunks, mip_chain, FMT_DXT1_GAMMA, FMT_DXT1_LINEAR, FMT_DXT5_NORMAL, FMT_DXT5_GAMMA
from retail_ref import load as load_retail
import build_suit_pak as OLD
from quickrender import render

DLC1 = 'C:/Users/rieng/Documents/GitHub/reeot-dni/reference/model_import/dlc/_DLC001.ext'
BG = 'D:/EOT_Extract/extracted/BaseGameplay.ext'
GAL = 'D:/EOT_Extract/extracted/Gallery.ext'
S99 = 's99/shit in kettle/images/'

SUIT_NAME = 'S99Miguel[Hi]'
SUIT_ID = 32
HERO = 0
PACKAGE_ID = 0xBBA
DONOR_GEO = 'SpiderMan2099_CEO_No_Suit_Civilian[Hi]'
DONOR_MATERIAL = 'SM99_CEO_No_Suit_CivilianCostar'
CIRCUITS = True
CIRCUIT_GEO = 'SpiderMan2099[Hi]'
CIRCUIT_MATERIAL = 'SMFuturBody'
BODY_MATERIAL = 'SMFuturBody'
TEX_BODY_CIRC = 'S99_Miguel_Body_Circuits_MK[Hi]'
TEX_RED_CIRC = 'S99_Miguel_Red_Circuits_MK[Hi]'
TEX_PULSE = 'S99_Miguel_Pulse_LM[Hi]'
SPIDER_MK = 'SM99_SpidermanSpider_MK[Hi]'
STRING_NAME = 'REEOT_SUIT_S99_NAME'
STRING_DESC = 'REEOT_SUIT_S99_DESC'
TITLE = 'ACROSS THE SPIDER-VERSE'
DESC = ("Miguel O'Hara as the Spider-Society knows him: talons out, the suit's living circuitry humming under "
        "the blue, and no patience left for anomalies. Nueva York's future never looked this sharp.")
CARD_SRC = ('Gallery_AltSuit_Selection2099Original_DA[Hi]', 'Gallery_AltSuit_Selected2099OriginalSmall_DA[Hi]')
CARDS = ('Reeot_AltSuit_SelectionS99_DA[Hi]', 'Reeot_AltSuit_SelectedS99Small_DA[Hi]')
TEX_BODY = 'S99_Miguel_Body_D[Hi]'
TEX_RED = 'S99_Miguel_Red_D[Hi]'
TEX_FLAT_N = 'S99_Flat_N[Hi]'
TEX_FLAT_S = 'S99_Flat_S[Hi]'
TEX_RED_S = 'S99_Red_S[Hi]'
RIM = 'RimLightCarFrame2_[Hi]'
SPEC_BODY = 96
SPEC_RED = 128
BODY_GAIN = 1.3
RED_SAT = 1.3
SPEC_POWER = 25.0
DIFFUSE_SIZE = 2048
FLIP_V = True
PULSE_GAIN = 2.0
MASK_BASE_BODY = 0.18
MASK_BASE_RED = 0.12
RED_TRACE_GAIN = 2.0
RING_WIDEN = 0.4
BLADE_KEEP = 0.14
RING_COLOUR = (0.0, 0.0, 40.0)

def crc_name(name):
    return zlib.crc32(re.sub(r'\[.*?\]', '', name).upper().encode()) & 0xFFFFFFFF

def load_rgba(path, size):
    im = Image.open(path).convert('RGBA')
    if im.size != (size, size):
        im = im.resize((size, size), Image.LANCZOS)
    return np.asarray(im)

def build_textures():
    out = []
    t = time.time()
    for name, png, gain, sat in ((TEX_BODY, 'torso_c.png', BODY_GAIN, 1.0), (TEX_RED, 'techy3.png', 1.0, RED_SAT)):
        img = load_rgba(S99 + png, DIFFUSE_SIZE)
        if gain != 1.0 or sat != 1.0:
            rgb = img[..., :3].astype(np.float32) * gain
            lum = rgb @ np.array([0.299, 0.587, 0.114], np.float32)
            rgb = lum[..., None] + (rgb - lum[..., None]) * sat
            img = img.copy(); img[..., :3] = np.clip(rgb, 0, 255).astype(np.uint8)
        payload = texture_data(mip_chain(img, 6), FMT_DXT1_GAMMA)
        out.append((name,) + texture_chunks(name, crc_name(name), payload, chunk, build_header))
        print('texture %s: %dx%d DXT1, %d bytes' % (name, DIFFUSE_SIZE, DIFFUSE_SIZE, len(payload)))
    flat_n = np.zeros((256, 256, 4), np.uint8); flat_n[..., 0] = 255; flat_n[..., 1] = 128; flat_n[..., 2] = 0; flat_n[..., 3] = 128
    payload = texture_data(mip_chain(flat_n, 3), FMT_DXT5_NORMAL, normal_map=True)
    out.append((TEX_FLAT_N,) + texture_chunks(TEX_FLAT_N, crc_name(TEX_FLAT_N), payload, chunk, build_header))
    if CIRCUITS:
        for name, mask in ((TEX_BODY_CIRC, circuit_mask_body()), (TEX_RED_CIRC, circuit_mask_red())):
            rgba = np.dstack([mask, mask, mask, np.full_like(mask, 255)])
            payload = texture_data(mip_chain(rgba, 4), FMT_DXT1_LINEAR)
            out.append((name,) + texture_chunks(name, crc_name(name), payload, chunk, build_header))
            print('texture %s: %dx%d DXT1 mask, %.1f%% lit' % (name, mask.shape[1], mask.shape[0], 100 * (mask > 128).mean()))
        ring = pulse_ring()
        payload = texture_data(mip_chain(ring, 4), FMT_DXT1_GAMMA)
        out.append((TEX_PULSE,) + texture_chunks(TEX_PULSE, crc_name(TEX_PULSE), payload, chunk, build_header))
    for name, level in ((TEX_FLAT_S, SPEC_BODY), (TEX_RED_S, SPEC_RED)):
        flat_s = np.zeros((256, 256, 4), np.uint8); flat_s[..., :3] = level; flat_s[..., 3] = 255
        payload = texture_data(mip_chain(flat_s, 3), FMT_DXT1_LINEAR)
        out.append((name,) + texture_chunks(name, crc_name(name), payload, chunk, build_header))
    print('textures encoded in %.1fs' % (time.time() - t))
    return out

MASK_SIZE = 1024

def circuit_mask_body():
    rgb = np.asarray(Image.open(S99 + 'torso_c.png').convert('RGB').resize((MASK_SIZE, MASK_SIZE), Image.LANCZOS)).astype(np.float32)
    m = np.clip((rgb[..., 0] - np.maximum(rgb[..., 1], rgb[..., 2]) - 30) / 90.0, 0, 1)
    m = np.maximum(m, MASK_BASE_BODY)
    return (m * 255).astype(np.uint8)

def circuit_mask_red():
    from PIL import ImageFilter
    im = Image.open(S99 + 'techy3.png').convert('RGB').resize((MASK_SIZE, MASK_SIZE), Image.LANCZOS)
    rgb = np.asarray(im).astype(np.float32)
    lum = rgb @ np.array([0.299, 0.587, 0.114], np.float32)
    blur = np.asarray(Image.fromarray(lum.astype(np.uint8)).filter(ImageFilter.BoxBlur(4))).astype(np.float32)
    dark = lum < 130
    m = np.clip((lum - blur) / 18.0, 0, 1) * dark
    m = np.clip(m * RED_TRACE_GAIN, 0, 1) * dark
    m = np.maximum(m, MASK_BASE_RED)
    return (m * 255).astype(np.uint8)

def pulse_ring():
    from card_render import retail_card
    from PIL import ImageFilter
    ring = retail_card('SM99_Spiderman_LM[Hi]', gallery=BG)[..., :3]
    inten = ring.max(2) / 255.0
    def dilate(m):
        return np.asarray(Image.fromarray((m * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3))).astype(np.float32) / 255.0
    for _ in range(int(RING_WIDEN)):
        inten = dilate(inten)
    frac = RING_WIDEN - int(RING_WIDEN)
    if frac > 0:
        inten = inten + frac * (dilate(inten) - inten)
    col = np.array(RING_COLOUR)
    rgba = np.zeros((ring.shape[0], ring.shape[1], 4), np.uint8)
    rgba[..., :3] = np.clip(inten[..., None] * col, 0, 255).astype(np.uint8)
    rgba[..., 3] = 255
    return rgba

def extra_uv_sets(P, UV):
    px, py, pz = P[:, 0], P[:, 1], P[:, 2]
    uv1 = np.stack([0.692 * py - 0.061 * pz - 0.139, -0.144 * py - 1.644 * pz + 0.682], 1)
    uv2 = np.full((len(P), 2), 0.02)
    uv3 = np.stack([1.052 * px - 0.046 * pz + 1.075, -0.455 * py + 0.283 * pz + 0.497], 1)
    uv4 = np.stack([1.509 * px + 0.5, 1.55 * py - 1.146 * pz - 1.2], 1)
    return [uv1, uv2, uv3, uv4]

def donor_material(pak, geo_name, mat_name):
    ep, es, h = pak.entry(0x07, name=geo_name)
    ek = kids(pak.d, ep + 12, ep + 12 + es)
    geo = next(k for k in ek if k[1] == 0x321)
    for k in kids(pak.d, geo[0] + 12, geo[0] + 12 + geo[4]):
        if k[1] != 0x324:
            continue
        for (p, t, a, b, size) in kids(pak.d, k[0] + 12, k[0] + 12 + k[4]):
            if t == 0x331 and pak.d[p + 12:p + 12 + 64].split(b'\0')[0].decode() == mat_name:
                return whole(pak.d, k[0])
    raise KeyError(mat_name)

def clone_material(mat_chunk, new_name, slots, spec_power=None):
    d = bytearray(mat_chunk)
    t, a, b, size = struct.unpack_from('>IHHI', d, 0)
    info = next(k for k in kids(d, 12, 12 + size) if k[1] == 0x331)
    base = info[0] + 12
    d[base:base + 32] = new_name.encode('ascii')[:31].ljust(32, b'\0')
    if spec_power is not None:
        struct.pack_into('>f', d, base + 48, spec_power)
    seg = bytes(d[base:base + info[4]])
    for m in re.finditer(rb'[ -~]{4,}\.(?:dds|tga)', seg):
        path = m.group().decode()
        stem = path.replace('\\', '/').rsplit('/', 1)[-1].rsplit('.', 1)[0]
        low = stem.lower()
        kind = ('D' if low.endswith('_d[hi]') else 'N' if low.endswith('_n[hi]') else 'S' if low.endswith('_s[hi]')
                else 'R' if 'rimlight' in low else 'CIRC' if 'circuits_mk' in low else 'LM' if low.endswith('_lm[hi]')
                else 'SPIDER' if 'spider_mk' in low else None)
        if kind not in slots:
            continue
        new = slots[kind]
        newpath = ('Textures\\Reeot\\' + new + '.dds').encode('ascii')
        field = seg[m.start():].split(b'\0')[0]
        assert len(newpath) <= len(field) + 8, 'path too long'
        o = base + m.start()
        d[o:o + len(field)] = b'\0' * len(field)
        d[o:o + len(newpath)] = newpath
        struct.pack_into('>I', d, o - 12, crc_name(new))
    return bytes(d)

def boost_pulse(mat_chunk, gain=PULSE_GAIN):
    d = bytearray(mat_chunk)
    t, ver, flags, size = struct.unpack_from('>IHHI', d, 0)
    info = next(k for k in kids(d, 12, 12 + size) if k[1] == 0x331)
    base = info[0] + 12
    ntex = struct.unpack_from('>I', d, base + 100)[0]
    nsamp = struct.unpack_from('>I', d, base + 104)[0]
    assert nsamp >= 2, 'no parameter 1 to boost'
    rec = base + 244 + 140 * ntex + 32 * 1
    struct.pack_into('>f', d, rec + 16, gain)
    return bytes(d)

def tangents(P, N, UV, tris):
    T = np.zeros_like(P); B = np.zeros_like(P)
    a, b, c = tris[:, 0], tris[:, 1], tris[:, 2]
    e1 = P[b] - P[a]; e2 = P[c] - P[a]
    d1 = UV[b] - UV[a]; d2 = UV[c] - UV[a]
    det = d1[:, 0] * d2[:, 1] - d2[:, 0] * d1[:, 1]
    r = np.where(np.abs(det) > 1e-12, 1.0 / np.where(np.abs(det) > 1e-12, det, 1), 0.0)[:, None]
    t = (e1 * d2[:, 1:2] - e2 * d1[:, 1:2]) * r
    bt = (e2 * d1[:, 0:1] - e1 * d2[:, 0:1]) * r
    for k in (a, b, c):
        np.add.at(T, k, t); np.add.at(B, k, bt)
    T -= N * (N * T).sum(1, keepdims=True)
    n = np.linalg.norm(T, axis=1, keepdims=True)
    bad = n[:, 0] < 1e-9
    alt = np.cross(N, np.where(np.abs(N[:, 0:1]) < 0.9, np.array([1.0, 0, 0]), np.array([0, 1.0, 0])))
    T = np.where(bad[:, None], alt, T / np.maximum(n, 1e-12))
    T /= np.linalg.norm(T, axis=1, keepdims=True)
    w = np.where((np.cross(N, T) * B).sum(1) < 0, -1, 1)
    return T, w

def trim_blades(rig, red_mesh, keep=BLADE_KEEP):
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components
    from scipy.spatial import cKDTree
    P, N, UV, J, W, tris, mesh_of = (rig[k].copy() for k in ('P', 'N', 'UV', 'J', 'W', 'tris', 'mesh_of'))
    rt = tris[mesh_of[tris[:, 0]] == red_mesh]
    ridx = np.unique(rt); inv = np.full(len(P), -1); inv[ridx] = np.arange(len(ridx))
    pairs = cKDTree(P[ridx]).query_pairs(1e-4, output_type='ndarray')
    edges = inv[np.concatenate([rt[:, [0, 1]], rt[:, [1, 2]], rt[:, [2, 0]]])]
    rows = np.concatenate([edges[:, 0], pairs[:, 0]]); cols = np.concatenate([edges[:, 1], pairs[:, 1]])
    nc, lab = connected_components(coo_matrix((np.ones(len(rows)), (rows, cols)), shape=(len(ridx),) * 2), directed=False)
    comp = np.full(len(P), -1); comp[ridx] = lab
    blades = [c for c in range(nc) if 150 < (lab == c).sum() < 500 and abs(P[ridx[lab == c]].mean(0)[0]) > 0.3
              and np.ptp(P[ridx[lab == c]][:, 2]) > 0.2]
    wrist = [c for c in range(nc) if 1000 < (lab == c).sum() < 1500 and abs(P[ridx[lab == c]].mean(0)[0]) > 0.45]
    assert len(blades) == 2, blades
    extraP, extraN, extraUV, extraJ, extraW, extraM = [], [], [], [], [], []
    cache = {}
    def cut_vertex(a, b, ta, tb, tcut):
        key = (min(a, b), max(a, b))
        if key in cache:
            return cache[key]
        f = (tcut - ta) / (tb - ta)
        n = N[a] + (N[b] - N[a]) * f
        inside = a if ta <= tcut else b
        extraP.append(P[a] + (P[b] - P[a]) * f); extraN.append(n / max(np.linalg.norm(n), 1e-9))
        extraUV.append(UV[a] + (UV[b] - UV[a]) * f); extraJ.append(J[inside]); extraW.append(W[inside]); extraM.append(mesh_of[inside])
        cache[key] = len(P) + len(extraP) - 1
        return cache[key]
    keep_tri = np.ones(len(tris), bool)
    new_tris = []
    for c in blades:
        vs = ridx[lab == c]
        side = np.sign(P[vs].mean(0)[0])
        wv = np.concatenate([ridx[lab == w] for w in wrist if np.sign(P[ridx[lab == w]].mean(0)[0]) == side])
        d, _ = cKDTree(P[wv]).query(P[vs])
        root = P[vs[np.argmin(d)]]
        tip = P[vs[np.argmax(np.linalg.norm(P[vs] - root, axis=1))]]
        axis = (tip - root) / np.linalg.norm(tip - root)
        t = (P - root) @ axis
        removed = split = 0
        for ti in np.where(comp[tris[:, 0]] == c)[0]:
            tri = tris[ti]; tv = t[tri]
            if (tv <= keep).all():
                continue
            keep_tri[ti] = False
            if (tv > keep).all():
                removed += 1
                continue
            split += 1
            poly = []
            for k in range(3):
                a, b = tri[k], tri[(k + 1) % 3]
                if t[a] <= keep:
                    poly.append(a)
                if (t[a] <= keep) != (t[b] <= keep):
                    poly.append(cut_vertex(a, b, t[a], t[b], keep))
            for k in range(1, len(poly) - 1):
                new_tris.append((poly[0], poly[k], poly[k + 1]))
        print('blade at x %.2f: root (%.2f %.2f %.2f), %.2f m long, %.2f kept: %d triangles cut, %d split' % (
            P[vs].mean(0)[0], *root, np.linalg.norm(tip - root), keep, removed, split))
    out = dict(rig)
    out.update(P=np.concatenate([P, np.array(extraP).reshape(-1, 3)]), N=np.concatenate([N, np.array(extraN).reshape(-1, 3)]),
               UV=np.concatenate([UV, np.array(extraUV).reshape(-1, 2)]), J=np.concatenate([J, np.array(extraJ).reshape(-1, J.shape[1])]),
               W=np.concatenate([W, np.array(extraW).reshape(-1, W.shape[1])]), mesh_of=np.concatenate([mesh_of, np.array(extraM, dtype=mesh_of.dtype)]),
               tris=np.concatenate([tris[keep_tri], np.array(new_tris, dtype=tris.dtype).reshape(-1, 3)]))
    return out

def build_geometry(rig, materials, mat_of_mesh, paramblock):
    P, N, UV, J, W, tris, mesh_of = rig['P'], rig['N'], rig['UV'].copy(), rig['J'], rig['W'], rig['tris'], rig['mesh_of']
    if FLIP_V:
        UV[:, 1] = 1.0 - UV[:, 1]
    _, nflip = orient(P, N, tris)
    print('geometry: %d verts, %d tris (%d disagree with their vertex normals, kept as authored)' % (len(P), len(tris), nflip))
    tris = tris[:, [0, 2, 1]]
    T, TW = tangents(P, N, UV, tris)
    extra = extra_uv_sets(P, UV) if CIRCUITS else []
    fmt = format_for(1 + len(extra)); stride = stride_for(1 + len(extra))
    VB = bytearray(); IB = []
    descs = []
    tri_mat = np.array([mat_of_mesh[m] for m in mesh_of[tris[:, 0]]])
    for mi in range(len(materials)):
        mtris = tris[tri_mat == mi]
        if not len(mtris):
            continue
        groups = split_palettes(mtris, J, W)
        for gi, (members, pal) in enumerate(groups):
            gtris = mtris[members]
            used = np.unique(gtris)
            remap = np.full(len(P), -1, np.int64); remap[used] = np.arange(len(used))
            local = remap[gtris]
            pal_index = {b: i for i, b in enumerate(pal)}
            Jl = np.vectorize(lambda b: pal_index.get(int(b), 0))(J[used])
            vb = pack_vertices(P[used], N[used], T[used], TW[used], UV[used], Jl, W[used], [e[used] for e in extra])
            strip = stripify(local)
            while len(VB) % 32: VB.append(0)
            while len(IB) % 2: IB.append(0xFFFF)
            descs.append((mi, len(VB), stride, fmt, len(used), len(IB), len(strip), pal))
            VB += vb; IB += strip
            print('  mesh %d: material %d (%s) group %d/%d: %d verts, %d tris, %d indices, %d bones' % (
                len(descs) - 1, mi, materials[mi][0], gi + 1, len(groups), len(used), len(gtris), len(strip), len(pal)))
    while len(IB) % 2: IB.append(0xFFFF)
    ib_bytes = struct.pack('>%dH' % len(IB), *IB)
    c = (P.min(0) + P.max(0)) / 2
    rad = float(np.sqrt(((P - c) ** 2).sum(1)).max())
    table = sorted((crc_name(n), i) for i, (n, _) in enumerate(materials))
    geominfo = struct.pack('>4f', c[0], c[1], c[2], rad) + struct.pack('>4I', 0, 1, len(materials), 0) + struct.pack('>4f', 0, 0, 0, 0)
    geominfo += struct.pack('>4I', 0, len(descs), sum(len(d[7]) for d in descs), 0)
    for crc, i in table:
        geominfo += struct.pack('>2I', crc, i)
    lodinfo = struct.pack('>6I', len(descs), 0, 0, 0, 0x00020000, 0)
    mesh_chunks = []
    for (mat, vboff, stride, fmt, nv, idxoff, nidx, pal) in descs:
        words = [mat, vboff, stride, fmt, nv, idxoff, nidx, len(pal), 0xFFFFFFFF, 3, 4, 0] + list(pal)
        mesh_chunks.append(chunk(0x0327, 8, struct.pack('>%dI' % len(words), *words)))
    lod_lib = chunk(0x0325, 3, children=[chunk(0x0326, 6, lodinfo)] + mesh_chunks)
    geo = chunk(0x0321, 3, children=[chunk(0x0322, 7, geominfo)] + [m for _, m in materials] + [lod_lib, chunk(0x0335, 1, children=[])])
    lod_post = chunk(0x0325, 3, children=[chunk(0x0326, 6, lodinfo), chunk(0x1389, 2, ib_bytes), chunk(0x1388, 2, bytes(VB))])
    crc = crc_name(SUIT_NAME)
    plcrc = zlib.crc32(ib_bytes + bytes(VB)) & 0xFFFFFFFF
    entry = chunk(0x138D, 1, children=[build_header(crc, 3, 0x1F, 0, 0, plcrc, SUIT_NAME), paramblock, geo])
    post = chunk(0x26, 0, children=[build_header(crc, 3, 0x1F, 0, 0, plcrc, SUIT_NAME), lod_post])
    print('geometry: %d meshes, VB %d bytes, IB %d indices, sphere %s r=%.3f' % (len(descs), len(VB), len(IB), np.round(c, 3), rad))
    return entry, post, crc, (P, N, UV, tris, mesh_of)

def preview(P, UV, tris, mesh_of, mat_of_mesh):
    imgs = [np.asarray(Image.open(S99 + 'torso_c.png').convert('RGB').resize((1024, 1024))),
            np.asarray(Image.open(S99 + 'techy3.png').convert('RGB').resize((1024, 1024)))]
    col = np.zeros((len(P), 3))
    for v in range(len(P)):
        img = imgs[mat_of_mesh[mesh_of[v]]]
        u, w = UV[v]
        x = int(np.clip(u * 1023, 0, 1023)); y = int(np.clip(w * 1023, 0, 1023))
        col[v] = img[y, x]
    col = np.clip(col * 1.6 + 30, 0, 255)
    render([(P, tris, col)], 'build/textured.png', views=('front', 'back'))

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'build/_DLC002.pak'
    dlc = Pak(DLC1); bg = Pak(BG); gal = Pak(GAL)
    rig = dict(np.load('build/s99_rigged.npz'))
    mesh_names = list(rig['mesh_names'])
    if BLADE_KEEP is not None:
        rig = trim_blades(rig, mesh_names.index('GeometryNode_23'))
    mat_of_mesh = {mesh_names.index('GeometryNode_5'): 0, mesh_names.index('GeometryNode_41'): 0,
                   mesh_names.index('GeometryNode_23'): 1, mesh_names.index('GeometryNode_56'): 1}

    if CIRCUITS:
        donor = donor_material(bg, CIRCUIT_GEO, CIRCUIT_MATERIAL)
        materials = [(BODY_MATERIAL, boost_pulse(clone_material(donor, BODY_MATERIAL, {'D': TEX_BODY, 'CIRC': TEX_BODY_CIRC, 'LM': TEX_PULSE, 'SPIDER': SPIDER_MK, 'N': TEX_FLAT_N, 'S': TEX_FLAT_S, 'R': RIM}))),
                     ('S99RedBody', boost_pulse(clone_material(donor, 'S99RedBody', {'D': TEX_RED, 'CIRC': TEX_RED_CIRC, 'LM': TEX_PULSE, 'SPIDER': SPIDER_MK, 'N': TEX_FLAT_N, 'S': TEX_RED_S, 'R': RIM})))]
    else:
        donor = donor_material(bg, DONOR_GEO, DONOR_MATERIAL)
        materials = [('SMFuturBody', clone_material(donor, 'SMFuturBody', {'D': TEX_BODY, 'N': TEX_FLAT_N, 'S': TEX_FLAT_S, 'R': RIM}, SPEC_POWER)),
                     ('S99RedBody', clone_material(donor, 'S99RedBody', {'D': TEX_RED, 'N': TEX_FLAT_N, 'S': TEX_RED_S, 'R': RIM}, SPEC_POWER))]
    ep, es, h = bg.entry(0x07, name=DONOR_GEO)
    paramblock = next(whole(bg.d, k[0]) for k in kids(bg.d, ep + 12, ep + 12 + es) if k[1] == 0x1391)

    geo_entry, geo_post, suit_crc, (P, N, UV, tris, mesh_of) = build_geometry(rig, materials, mat_of_mesh, paramblock)
    preview(P, UV, tris, mesh_of, mat_of_mesh)

    textures = build_textures()
    tex_items = [(e, p) for (_, e, p) in textures]
    if os.path.exists('build/card_large.png'):
        large = np.asarray(Image.open('build/card_large.png').convert('RGBA'))
        small = np.asarray(Image.open('build/card_small.png').convert('RGBA'))
        tex_items.append(texture_chunks(CARDS[0], crc_name(CARDS[0]), texture_data([large], FMT_DXT5_GAMMA), chunk, build_header))
        tex_items.append(texture_chunks(CARDS[1], crc_name(CARDS[1]), texture_data([small], FMT_DXT1_GAMMA, punch_alpha=True), chunk, build_header))
        print('cards: build/card_large.png, build/card_small.png')
    else:
        for src_name, new_name in zip(CARD_SRC, CARDS):
            tex_items.append(gal.item(0x09, name=src_name, rename=(crc_name(new_name), new_name)))

    ghost_names = ['SpiderMan2099_AbilityGhost[Hi]', 'SpiderMan2099_Ghost[Hi]', 'SpiderManGeneric_TrailGhost[Hi]', 'SpiderManGeneric_Ghost[Hi]']
    geo_items = [(geo_entry, geo_post)] + [dlc.item(0x07, name=g) for g in ghost_names]
    ghost_crcs = [dlc.entry(0x07, name=g)[2]['crc'] for g in ghost_names]
    hier_items = [(whole(dlc.d, ep2), None) for (ep2, es2, h2) in dlc.entries(0x05)]
    hier_crcs = [h2['crc'] for (_, _, h2) in dlc.entries(0x05)]

    ep, es, h = dlc.entry(0x04, name='DLC001Master')
    go_type = None
    for (p, t, a, b, size) in kids(dlc.d, ep + 12, ep + 12 + es):
        if t == 0x1F4:
            for (gp, gt, ga, gb, gs) in kids(dlc.d, p + 12, p + 12 + size):
                if gt == 0x1FF: go_type = whole(dlc.d, gp)
    master = build_master(go_type, suit_crc, crc_name(CARDS[0]), crc_name(CARDS[1]), ghost_crcs, hier_crcs)
    strings = OLD.build_string_table([(STRING_NAME, TITLE), (STRING_DESC, DESC)])
    level = None
    for (p, t, a, b, size) in dlc.top:
        if t == 0x11:
            level = bytearray(whole(dlc.d, p)); struct.pack_into('>I', level, 12, PACKAGE_ID); level = bytes(level)

    libs = {0x09: tex_items, 0x07: geo_items, 0x05: hier_items}
    fixed = {0x11: level, 0x12: strings, 0x04: master}
    data = assemble(dlc, libs, fixed)
    bad = check_alignment(data)
    open(out, 'wb').write(data)
    print('wrote %s: %d bytes, package %#x, suit %s crc %08X id %d, %d unaligned chunks' % (out, len(data), PACKAGE_ID, SUIT_NAME, suit_crc, SUIT_ID, len(bad)))

def build_master(go_type_chunk, suit_crc, card_large_crc, card_small_crc, ghost_geos, ghost_hiers):
    def rec(cls, fields):
        body = struct.pack('>%dI' % len(fields), *fields)
        return struct.pack('>3I', 12 + len(body), cls, 0) + body
    hud = rec(0x4E2AA277, [crc_name(STRING_NAME), SUIT_ID, card_large_crc, card_small_crc, crc_name(STRING_DESC), HERO, 0])
    generic_ghost, trail_ghost, ability_ghost, ghost_2099 = (crc_name('SpiderManGeneric_Ghost'), crc_name('SpiderManGeneric_TrailGhost'),
                                                             crc_name('SpiderMan2099_AbilityGhost'), crc_name('SpiderMan2099_Ghost'))
    if HERO == 0:
        cost = rec(0x2070BF8C, [SUIT_ID, 0xFFFFFFFF, generic_ghost, trail_ghost, ability_ghost, ghost_2099, suit_crc])
    else:
        cost = rec(0x2070BF8C, [SUIT_ID, suit_crc, generic_ghost, trail_ghost, ability_ghost, ghost_2099, 0xFFFFFFFF])
    dmg = b''.join(struct.pack('>3I', 20, 0x73DCB000, 0) + struct.pack('>IHH', state, val, 0)
                   for state, val in ((1, 0x40), (2, 0x2C), (3, 0x18), (4, 0x04)))
    sub_base = 24
    hud_off = sub_base + 24
    cost_off = hud_off + len(hud)
    dmg_off = cost_off + len(cost)
    sub = struct.pack('>3I', 0x18, 0x107B9460, 0)
    sub += struct.pack('>HH', hud_off - (sub_base + 12), 1)
    sub += struct.pack('>HH', cost_off - (sub_base + 16), 1)
    sub += struct.pack('>HH', dmg_off - (sub_base + 20), 4)
    body = sub + hud + cost + dmg
    block = struct.pack('>4I', crc_name('REEOT_DLC002_DATA'), 0x14, 0x6664E5DB, 20 + len(body)) + struct.pack('>I', 0x71CA6973) + struct.pack('>HH', 4, 0) + body
    pb = chunk(0x1391, 6, block)
    refs = [(4, card_large_crc), (4, card_small_crc)] + [(3, c) for c in ghost_geos] + [(2, c) for c in ghost_hiers] + [(3, suit_crc)]
    ref = chunk(0x139C, 1, b''.join(struct.pack('>2I', t, c) for t, c in refs))
    go = chunk(0x1F4, 2, children=[go_type_chunk, pb, ref])
    name = 'ReeotDLC002Master'
    res = chunk(0x138D, 1, children=[build_header(crc_name(name), 257, 0x1F, 0, 0, 0xFFFFFFFF, name), go])
    return chunk(0x04, 4, children=[chunk(0x139A, 1, struct.pack('>I', 1)), res])

if __name__ == '__main__':
    main()
