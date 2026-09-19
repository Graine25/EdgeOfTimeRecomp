import struct

def chunk(cid, ver, payload=b'', children=None):
    if children is not None:
        body = b''.join(children); kids = 1
    else:
        body = payload + bytes(-len(payload) % 4); kids = 0
    return struct.pack('>IHHI', cid, ver, kids, len(body)) + body

def check_alignment(data):
    bad = []
    def walk(off, end):
        for (p, t, a, b, size) in kids(data, off, end):
            if p % 4 or size % 4:
                bad.append((p, t, size))
            if b:
                walk(p + 12, p + 12 + size)
    walk(0, len(data))
    return bad

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

def res_header(d, payload_off):
    crc, typ, lang, dataoff, qual, plcrc = struct.unpack_from('>6I', d, payload_off)
    name = d[payload_off + 24:payload_off + 88].split(b'\0')[0].decode('ascii', 'replace')
    return dict(crc=crc, type=typ, lang=lang, dataoff=dataoff, qual=qual, plcrc=plcrc, name=name)

def build_header(crc, typ, lang, dataoff, qual, plcrc, name):
    nm = name.encode('ascii')[:63].ljust(64, b'\0')
    return chunk(0x138E, 5, struct.pack('>6I', crc, typ, lang, dataoff, qual, plcrc) + nm)

def patch_header_in(chunk_bytes, dataoff=None, crc=None, name=None):
    d = bytearray(chunk_bytes)
    t, a, b, size = hdr(d, 0)
    for (p, ct, ca, cb, cs) in kids(d, 12, 12 + size):
        if ct == 0x138E:
            if dataoff is not None: struct.pack_into('>I', d, p + 12 + 12, dataoff)
            if crc is not None: struct.pack_into('>I', d, p + 12, crc)
            if name is not None: d[p + 12 + 24:p + 12 + 88] = name.encode('ascii')[:63].ljust(64, b'\0')
            return bytes(d)
    raise ValueError('no resource header in chunk %#x' % t)

class Pak:
    def __init__(self, path):
        self.path = path
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
                out.append((ep, es, res_header(self.d, h[0] + 12)))
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
    def item(self, lib_id, name=None, crc=None, rename=None):
        ep, es, h = self.entry(lib_id, name=name, crc=crc)
        entry = whole(self.d, ep)
        post = self.postload(h['dataoff']) if h['dataoff'] else None
        newcrc = rename[0] if rename else None; newname = rename[1] if rename else None
        entry = patch_header_in(entry, dataoff=0 if post else None, crc=newcrc, name=newname)
        if post:
            post = patch_header_in(post, dataoff=0, crc=newcrc, name=newname)
        return entry, post

LIBRARY_VERSIONS = {0x12: 4, 0x09: 2, 0x0C: 4, 0x0F: 2, 0x07: 3, 0x05: 3, 0x14: 4, 0x08: 2, 0x0A: 4, 0x0B: 4,
                    0x0D: 4, 0x0E: 4, 0x10: 2, 0x11: 4, 0x13: 4, 0x15: 4, 0x16: 4, 0x17: 4, 0x18: 4,
                    0x32: 2, 0x1A: 2, 0x1B: 2, 0x1C: 2, 0x1D: 1, 0x1E: 1, 0x04: 4}

def assemble(template, libs, fixed=None, counts=None):
    fixed = fixed or {}; counts = counts or {}
    order = [(t, a) for (p, t, a, b, size) in template.top if t != 0x26]
    top = {t: whole(template.d, p) for (p, t, a, b, size) in template.top}

    def pass_(offsets):
        children = []; posts = []
        i = 0
        for (t, ver) in order:
            if t in fixed:
                children.append(fixed[t])
            elif t in libs:
                ents = []
                for (entry, post) in libs[t]:
                    if post is not None:
                        off = offsets[i]; i += 1
                        ents.append(patch_header_in(entry, dataoff=off))
                        posts.append(patch_header_in(post, dataoff=off))
                    else:
                        ents.append(entry)
                children.append(chunk(t, ver, children=[chunk(0x139A, 1, struct.pack('>I', counts.get(t, len(ents))))] + ents))
            else:
                children.append(top[t])
        return children, posts

    nposts = sum(1 for t in libs for (e, p) in libs[t] if p is not None)
    children, posts = pass_([0] * nposts)
    off = 12 + sum(len(c) for c in children)
    offsets = []
    for p in posts:
        offsets.append(off); off += len(p)
    children, posts = pass_(offsets)
    return chunk(0x0001, 1, children=children + posts)
