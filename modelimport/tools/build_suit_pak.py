import struct, zlib, sys, os, re, json
import numpy as np

sys.path.insert(0, r'C:\Users\rieng\Documents\GitHub\reeot-dni\reference\eot_tools\SPIDER-MAN EDGE OF TIME TOOLS\tools')
import x360_geo as G

SCR = os.path.dirname(os.path.abspath(__file__))
BG = 'D:/EOT_Extract/extracted/BaseGameplay.ext'
GAL = 'D:/EOT_Extract/extracted/Gallery.ext'
DLC1 = 'C:/Users/rieng/Documents/GitHub/reeot-dni/reference/model_import/dlc/_DLC001.ext'
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(SCR, '_DLC002.pak')

SUIT_NAME = 'ReeotCasualMask[Hi]'
SUIT_ID = 42
HERO_AMAZING = 1
PACKAGE_ID = 0xBBA
STRING_NAME = 'REEOT_SUIT_CASUALMASK_NAME'
STRING_DESC = 'REEOT_SUIT_CASUALMASK_DESC'
SUIT_TITLE = 'OFFICE HOURS'
SUIT_DESC = ("Peter never made it home before the Alchemax alarms went off. A mask pulled from a coat "
             "pocket, a shirt that still smells like the Bugle's break room and a tie he insists is "
             "'a classic'. Not every crisis waits for a costume change.")
CARD_LARGE_SRC = 'Gallery_AltSuit_SelectionAmazingPeterParkerCivilian_DA[Hi]'
CARD_SMALL_SRC = 'Gallery_AltSuit_SelectedAmazingPeterParkerCivilian_DA[Hi]'
CARD_LARGE = 'Reeot_AltSuit_SelectionCasualMask_DA[Hi]'
CARD_SMALL = 'Reeot_AltSuit_SelectedCasualMaskSmall_DA[Hi]'

def crc_name(name):
    stem = re.sub(r'\[.*?\]', '', name)
    return zlib.crc32(stem.upper().encode()) & 0xFFFFFFFF

def chunk(cid, ver, payload=b'', children=None):
    if children is not None:
        body = b''.join(children); kids = 1
    else:
        body = payload; kids = 0
    return struct.pack('>IHHI', cid, ver, kids, len(body)) + body

def hdr(d, o):
    return struct.unpack_from('>IHHI', d, o)

def kids(d, off, end):
    p, out = off, []
    while p + 12 <= end:
        t, a, b, size = hdr(d, p)
        if p + 12 + size > end:
            break
        out.append((p, t, a, b, size)); p += 12 + size
    return out

def whole(d, p):
    t, a, b, size = hdr(d, p)
    return d[p:p + 12 + size]

def res_header(payload_off, d):
    crc, typ, lang, dataoff, qual, plcrc = struct.unpack_from('>6I', d, payload_off)
    name = d[payload_off + 24:payload_off + 88].split(b'\0')[0].decode('ascii', 'replace')
    return dict(crc=crc, type=typ, lang=lang, dataoff=dataoff, qual=qual, plcrc=plcrc, name=name)

def build_header(crc, typ, lang, dataoff, qual, plcrc, name):
    nm = name.encode('ascii')[:63].ljust(64, b'\0')
    return chunk(0x138E, 5, struct.pack('>6I', crc, typ, lang, dataoff, qual, plcrc) + nm)

class Pak:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        self.root = hdr(self.d, 0)
        self.top = kids(self.d, 12, 12 + self.root[3])
    def library(self, lib_id):
        return [k for k in self.top if k[1] == lib_id]
    def entries(self, lib_id):
        out = []
        for (p, t, a, b, size) in self.library(lib_id):
            for (ep, et, ea, eb, es) in kids(self.d, p + 12, p + 12 + size):
                if et != 0x138D:
                    continue
                h = next(k for k in kids(self.d, ep + 12, ep + 12 + es) if k[1] == 0x138E)
                out.append((ep, es, res_header(h[0] + 12, self.d)))
        return out
    def entry(self, lib_id, name=None, crc=None):
        for (ep, es, h) in self.entries(lib_id):
            if (name and h['name'] == name) or (crc is not None and h['crc'] == crc):
                return ep, es, h
        raise KeyError(name or ('%08X' % crc))
    def postload(self, dataoff):
        t, a, b, size = hdr(self.d, dataoff)
        assert t == 0x26, 'no Gen_PostLoadData at %#x' % dataoff
        return self.d[dataoff:dataoff + 12 + size]

def patch_header_in(chunk_bytes, dataoff=None, crc=None, name=None):
    d = bytearray(chunk_bytes)
    t, a, b, size = hdr(d, 0)
    for (p, ct, ca, cb, cs) in kids(d, 12, 12 + size):
        if ct == 0x138E:
            if dataoff is not None: struct.pack_into('>I', d, p + 12 + 12, dataoff)
            if crc is not None: struct.pack_into('>I', d, p + 12, crc)
            if name is not None: d[p + 12 + 24:p + 12 + 88] = name.encode('ascii')[:63].ljust(64, b'\0')
            return bytes(d)
    raise ValueError('no header')

NAMES = None
def bone_names():
    global NAMES
    if NAMES is None:
        from sd_export_gltf import load_skeletons, parse_skeleton
        sk = load_skeletons(BG)
        NAMES = parse_skeleton(*sk['SpiderManAmazing[Hi]']).names
    return NAMES

class Source:
    def __init__(self, pak, name):
        self.pak = pak; self.d = pak.d; self.name = name
        m = {mm[0]: mm for mm in G.get_models(self.d)}[name]
        _, self.vb, self.vbsize, self.ib, self.ibsize = m
        self.descs = G.get_descriptors(self.d, name)
        ep, es, h = pak.entry(0x0007, name=name)
        self.entry, self.entry_size, self.header = ep, es, h
        ek = kids(self.d, ep + 12, ep + 12 + es)
        self.paramblock = whole(self.d, next(k for k in ek if k[1] == 0x1391)[0])
        geo = next(k for k in ek if k[1] == 0x0321)
        gk = kids(self.d, geo[0] + 12, geo[0] + 12 + geo[4])
        self.geominfo = self.d[gk[0][0] + 12:gk[0][0] + 12 + gk[0][4]]
        self.materials = [whole(self.d, k[0]) for k in gk if k[1] == 0x0324]
        self.material_names = [self.d[k[0] + 12 + 12 + 12 + 4:k[0] + 12 + 12 + 12 + 4 + 64].split(b'\0')[0].decode() for k in gk if k[1] == 0x0324]
        lod = next(k for k in gk if k[1] == 0x0325)
        lk = kids(self.d, lod[0] + 12, lod[0] + 12 + lod[4])
        self.lodinfo = self.d[lk[0][0] + 12:lk[0][0] + 12 + lk[0][4]]
        self.mesh_raw = [struct.unpack_from('>12I', self.d, k[0] + 12) for k in lk if k[1] == 0x0327]
    def vertex_bytes(self, k):
        dd = self.descs[k]
        s = self.vb + dd['vboff']
        return self.d[s:s + dd['nv'] * dd['stride']]
    def index_words(self, k):
        dd = self.descs[k]
        return list(struct.unpack_from('>%dH' % dd['nidx'], self.d, self.ib + dd['idxoff'] * 2))

def material_texture_crcs(mat_chunk):
    out = []
    for m in re.finditer(rb'[ -~]{4,}\.(?:dds|tga)', mat_chunk):
        o = m.start()
        if o >= 12:
            out.append(struct.unpack_from('>I', mat_chunk, o - 12)[0])
    return out

def mask_subset(src, k, ycut=1.555, ytop=1.66, rmax=0.095):
    dd = src.descs[k]
    stride = dd['stride']; o = G.offsets(dd['fmt'])
    raw = src.vertex_bytes(k)
    nv = dd['nv']
    P = np.array([struct.unpack_from('>3f', raw, v * stride) for v in range(nv)])
    r = np.sqrt(P[:, 0] ** 2 + P[:, 2] ** 2)
    keep = (P[:, 1] >= ytop) | ((P[:, 1] >= ycut) & (r <= rmax))
    tris = np.array(G.strip_to_tris(src.index_words(k))).reshape(-1, 3)
    tkeep = keep[tris].all(axis=1)
    tris = tris[tkeep]
    used = sorted(set(tris.ravel().tolist()))
    remap = {old: new for new, old in enumerate(used)}
    pal_old = dd['pal']
    bones_used = set()
    for v in used:
        js = raw[v * stride + o['skin']:v * stride + o['skin'] + 4]
        ws = raw[v * stride + o['skin'] + 4:v * stride + o['skin'] + 8]
        for c in range(4):
            if ws[c]:
                bones_used.add(pal_old[js[c]])
    pal_new = sorted(bones_used)
    pal_index = {b: i for i, b in enumerate(pal_new)}
    out = bytearray()
    for v in used:
        vb = bytearray(raw[v * stride:(v + 1) * stride])
        js = vb[o['skin']:o['skin'] + 4]; ws = vb[o['skin'] + 4:o['skin'] + 8]
        for c in range(4):
            vb[o['skin'] + c] = pal_index[pal_old[js[c]]] if ws[c] else 0
        out += vb
    idx = []
    for (a, b, c) in tris:
        idx += [remap[b], remap[a], remap[c], 0xFFFF]
    return bytes(out), idx, pal_new, len(used), len(tris)

def build_geometry():
    bg = Pak(BG)
    casual = Source(bg, 'SpiderManAmazing_Casual[Hi]')
    classic = Source(bg, 'SpiderManAmazing[Hi]')
    names = bone_names()
    materials = [casual.materials[0], classic.materials[0], classic.materials[1], classic.materials[2]]
    mat_names = [casual.material_names[0], classic.material_names[0], classic.material_names[1], classic.material_names[2]]
    meshes = []
    for k in (5, 6):
        dd = casual.descs[k]
        meshes.append((0, dd['fmt'], dd['stride'], casual.vertex_bytes(k), casual.index_words(k), dd['pal'], dd['nv'], None))
    vbytes, idx, pal, nv, ntris = mask_subset(classic, 1)
    dd = classic.descs[1]
    meshes.append((1, dd['fmt'], dd['stride'], vbytes, idx, pal, nv, ntris))
    print('mask: %d verts, %d tris, palette %d bones: %s' % (nv, ntris, len(pal), [names[b] for b in pal]))
    VB = bytearray(); IB = []
    descs = []
    for (mat, fmt, stride, vb, idx, pal, nvv, _) in meshes:
        while len(VB) % 32: VB.append(0)
        vboff = len(VB); VB += vb
        while len(IB) % 2: IB.append(0xAAAA)
        idxoff = len(IB); IB += idx
        descs.append((mat, vboff, stride, fmt, nvv, idxoff, len(idx), pal))
    while len(IB) % 2: IB.append(0xAAAA)
    ib_bytes = struct.pack('>%dH' % len(IB), *IB)
    allP = []
    for (mat, fmt, stride, vb, idx, pal, nvv, _) in meshes:
        allP += [struct.unpack_from('>3f', vb, v * stride) for v in range(nvv)]
    allP = np.array(allP)
    c = (allP.min(0) + allP.max(0)) / 2
    rad = float(np.sqrt(((allP - c) ** 2).sum(1)).max())
    table = sorted((crc_name(n), i) for i, n in enumerate(mat_names))
    geominfo = struct.pack('>4f', c[0], c[1], c[2], rad) + struct.pack('>4I', 0, 1, len(materials), 0) + struct.pack('>4f', 0, 0, 0, 0)
    geominfo += struct.pack('>4I', 0, len(descs), sum(len(dsc[7]) for dsc in descs), 0)
    for crc, i in table:
        geominfo += struct.pack('>2I', crc, i)
    assert len(geominfo) == 64 + 8 * len(materials)
    lodinfo = struct.pack('>6I', len(descs), 0, 0, 0, 0x00020000, 0)
    mesh_chunks = []
    for (mat, vboff, stride, fmt, nvv, idxoff, nidx, pal) in descs:
        words = [mat, vboff, stride, fmt, nvv, idxoff, nidx, len(pal), 0xFFFFFFFF, 3, 4, 0] + list(pal)
        mesh_chunks.append(chunk(0x0327, 8, struct.pack('>%dI' % len(words), *words)))
    lod_lib = chunk(0x0325, 3, children=[chunk(0x0326, 6, lodinfo)] + mesh_chunks)
    geo = chunk(0x0321, 3, children=[chunk(0x0322, 7, geominfo)] + materials + [lod_lib, chunk(0x0335, 1, children=[])])
    lod_post = chunk(0x0325, 3, children=[chunk(0x0326, 6, lodinfo), chunk(0x1389, 2, ib_bytes), chunk(0x1388, 2, bytes(VB))])
    crc = crc_name(SUIT_NAME)
    plcrc = zlib.crc32(ib_bytes + bytes(VB)) & 0xFFFFFFFF
    def entry(dataoff):
        return chunk(0x138D, 1, children=[build_header(crc, 3, 0x1F, dataoff, 0, plcrc, SUIT_NAME), classic.paramblock, geo])
    def post(dataoff):
        return chunk(0x26, 0, children=[build_header(crc, 3, 0x1F, dataoff, 0, plcrc, SUIT_NAME), lod_post])
    texcrcs = []
    for m in materials:
        for t in material_texture_crcs(m):
            if t not in texcrcs:
                texcrcs.append(t)
    print('geometry: %d meshes, VB %d bytes, IB %d indices, sphere %s r=%.3f, textures %s' % (
        len(descs), len(VB), len(IB), np.round(c, 3), rad, ['%08X' % t for t in texcrcs]))
    return entry, post, crc, texcrcs

def build_string_table(strings):
    crcs = [crc_name(n) for n, _ in strings]
    data = b''
    offsets = []
    for _, text in strings:
        offsets.append(len(data) // 2)
        data += text.encode('utf-16-be') + b'\0\0'
    header = struct.pack('>I', len(strings)) + struct.pack('>%dI' % len(crcs), *crcs) + struct.pack('>I', len(strings))
    for off in offsets:
        header += struct.pack('>I', 1) + struct.pack('>Iff', off, 0.0, -1.0)
    table = chunk(0x5DC, 4, children=[chunk(0x5DD, 4, header), chunk(0x5E1, 2, data)])
    tname = 'ReeotDLC002'
    res = chunk(0x138D, 1, children=[build_header(crc_name(tname), 0x82, 0x1F, 0, 0, 0xFFFFFFFF, tname), table])
    return chunk(0x12, 4, children=[chunk(0x139A, 1, struct.pack('>I', 1)), res])

def build_master(go_type_chunk, suit_crc, card_large_crc, card_small_crc, ghost_geos, ghost_hiers):
    def rec(cls, fields):
        body = struct.pack('>%dI' % len(fields), *fields)
        return struct.pack('>3I', 12 + len(body), cls, 0) + body
    hud = rec(0x4E2AA277, [crc_name(STRING_NAME), SUIT_ID, card_large_crc, card_small_crc, crc_name(STRING_DESC), HERO_AMAZING, 0])
    cost = rec(0x2070BF8C, [SUIT_ID, suit_crc, crc_name('SpiderManGeneric_Ghost'), crc_name('SpiderManGeneric_TrailGhost'),
                            crc_name('SpiderMan2099_AbilityGhost'), crc_name('SpiderMan2099_Ghost'), 0xFFFFFFFF])
    assert len(hud) == 40 and len(cost) == 40
    dmg = b''.join(struct.pack('>3I', 20, 0x73DCB000, 0) + struct.pack('>IHH', state, 0, 0) for state in (1, 2, 3, 4))
    sub_base = 24
    arrays_start = sub_base + 24
    hud_off = arrays_start
    cost_off = hud_off + len(hud)
    dmg_off = cost_off + len(cost)
    sub = struct.pack('>3I', 0x18, 0x107B9460, 0)
    sub += struct.pack('>HH', hud_off - (sub_base + 12), 1)
    sub += struct.pack('>HH', cost_off - (sub_base + 16), 1)
    sub += struct.pack('>HH', dmg_off - (sub_base + 20), 4)
    body = sub + hud + cost + dmg
    data_size = 20 + len(body)
    block = struct.pack('>4I', crc_name('REEOT_DLC002_DATA'), 0x14, 0x6664E5DB, data_size) + struct.pack('>I', 0x71CA6973) + struct.pack('>HH', 4, 0) + body
    pb = chunk(0x1391, 6, block)
    refs = [(4, card_large_crc), (4, card_small_crc), (3, suit_crc)] + [(3, c) for c in ghost_geos] + [(2, c) for c in ghost_hiers]
    ref = chunk(0x139C, 1, b''.join(struct.pack('>2I', t, c) for t, c in refs))
    go = chunk(0x1F4, 2, children=[go_type_chunk, pb, ref])
    name = 'ReeotDLC002Master'
    res = chunk(0x138D, 1, children=[build_header(crc_name(name), 257, 0x1F, 0, 0, 0xFFFFFFFF, name), go])
    return chunk(0x04, 4, children=[chunk(0x139A, 1, struct.pack('>I', 1)), res])

def main():
    dlc = Pak(DLC1)
    bg = Pak(BG)
    gal = Pak(GAL)
    geo_entry, geo_post, suit_crc, texcrcs = build_geometry()

    tex_items = []
    for tc in texcrcs:
        ep, es, h = bg.entry(0x0009, crc=tc)
        entry = whole(bg.d, ep); post = bg.postload(h['dataoff'])
        tex_items.append((patch_header_in(entry, dataoff=0), patch_header_in(post, dataoff=0), h['name']))
    for src_name, new_name in ((CARD_LARGE_SRC, CARD_LARGE), (CARD_SMALL_SRC, CARD_SMALL)):
        ep, es, h = gal.entry(0x0009, name=src_name)
        entry = whole(gal.d, ep); post = gal.postload(h['dataoff'])
        ncrc = crc_name(new_name)
        tex_items.append((patch_header_in(entry, dataoff=0, crc=ncrc, name=new_name),
                          patch_header_in(post, dataoff=0, crc=ncrc, name=new_name), new_name))
    print('textures:', [t[2] for t in tex_items])

    ghost_geo_names = ['SpiderMan2099_AbilityGhost[Hi]', 'SpiderMan2099_Ghost[Hi]', 'SpiderManGeneric_TrailGhost[Hi]', 'SpiderManGeneric_Ghost[Hi]']
    ghost_items = []
    for gname in ghost_geo_names:
        ep, es, h = dlc.entry(0x0007, name=gname)
        ghost_items.append((patch_header_in(whole(dlc.d, ep), dataoff=0), patch_header_in(dlc.postload(h['dataoff']), dataoff=0), h['crc']))
    hier_items = []
    for (ep, es, h) in dlc.entries(0x0005):
        hier_items.append((whole(dlc.d, ep), h['crc']))
    print('ghosts:', ghost_geo_names, 'hierarchies:', [h['name'] for (_, _, h) in dlc.entries(0x0005)])
    assert [g[2] for g in ghost_items] == [0xCAF014F8, 0xB5763883, 0x875DD3B6, 0x7E78247F], 'ghost crcs'
    assert crc_name('SpiderManGeneric_Ghost') == 0x7E78247F, 'crc rule'

    top = {t: whole(dlc.d, p) for (p, t, a, b, size) in dlc.top}
    level = bytearray(top[0x11]); struct.pack_into('>I', level, 12, PACKAGE_ID); level = bytes(level)
    master_go_type = None
    ep, es, h = dlc.entry(0x04, name='DLC001Master')
    for (p, t, a, b, size) in kids(dlc.d, ep + 12, ep + 12 + es):
        if t == 0x1F4:
            for (gp, gt, ga, gb, gs) in kids(dlc.d, p + 12, p + 12 + size):
                if gt == 0x1FF:
                    master_go_type = whole(dlc.d, gp)
    assert master_go_type
    strings = build_string_table([(STRING_NAME, SUIT_TITLE), (STRING_DESC, SUIT_DESC)])
    master = build_master(master_go_type, suit_crc, crc_name(CARD_LARGE), crc_name(CARD_SMALL),
                          [g[2] for g in ghost_items], [hh[1] for hh in hier_items])

    order = [t for (p, t, a, b, size) in dlc.top if t != 0x26]

    def assemble(tex_offs, geo_offs, ghost_offs):
        children = []
        posts = []
        for t in order:
            if t == 0x11:
                children.append(level)
            elif t == 0x12:
                children.append(strings)
            elif t == 0x09:
                ents = [patch_header_in(e, dataoff=o) for (e, _, _), o in zip(tex_items, tex_offs)]
                children.append(chunk(0x09, 2, children=[chunk(0x139A, 1, struct.pack('>I', len(ents)))] + ents))
                posts += [patch_header_in(pp, dataoff=o) for (_, pp, _), o in zip(tex_items, tex_offs)]
            elif t == 0x07:
                ents = [geo_entry(geo_offs)] + [patch_header_in(e, dataoff=o) for (e, _, _), o in zip(ghost_items, ghost_offs)]
                children.append(chunk(0x07, 3, children=[chunk(0x139A, 1, struct.pack('>I', len(ents)))] + ents))
                posts += [geo_post(geo_offs)] + [patch_header_in(pp, dataoff=o) for (_, pp, _), o in zip(ghost_items, ghost_offs)]
            elif t == 0x05:
                ents = [e for (e, _) in hier_items]
                children.append(chunk(0x05, 3, children=[chunk(0x139A, 1, struct.pack('>I', len(ents)))] + ents))
            elif t == 0x04:
                children.append(master)
            else:
                children.append(top[t])
        return children, posts

    ntex, nghost = len(tex_items), len(ghost_items)
    children, posts = assemble([0] * ntex, 0, [0] * nghost)
    off = 12 + sum(len(c) for c in children)
    offs = []
    for p in posts:
        offs.append(off); off += len(p)
    tex_offs, geo_offs, ghost_offs = offs[:ntex], offs[ntex], offs[ntex + 1:]
    children, posts = assemble(tex_offs, geo_offs, ghost_offs)
    root = chunk(0x0001, 1, children=children + posts)
    open(OUT, 'wb').write(root)
    print('wrote %s: %d bytes, package %#x, suit %s crc %08X id %d' % (OUT, len(root), PACKAGE_ID, SUIT_NAME, suit_crc, SUIT_ID))

if __name__ == '__main__':
    main()
