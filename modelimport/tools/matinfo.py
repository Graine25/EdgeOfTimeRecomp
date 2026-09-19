import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pakwriter import Pak, kids

DECL_TYPES = {0x2A23B9: 'FLOAT3', 0x1A23A6: 'FLOAT2', 0x1A25B6: 'SHORT2N?', 0x2A2287: 'UBYTE4', 0x1A2286: 'UBYTE4N?',
              0x2A23A5: 'FLOAT4', 0x1A2373: 'DEC3N?', 0x1A2286: 'UBYTE4N', 0x2A2372: 'UDEC3?'}
USAGES = {0: 'POSITION', 1: 'BLENDWEIGHT', 2: 'BLENDINDICES', 3: 'NORMAL', 4: 'PSIZE', 5: 'TEXCOORD', 6: 'TANGENT', 7: 'BINORMAL',
          8: 'TESSFACTOR', 9: 'POSITIONT', 10: 'COLOR', 11: 'FOG', 12: 'DEPTH', 13: 'SAMPLE'}

class Reader:
    def __init__(self, d, off):
        self.d = d; self.p = off
    def bytes(self, n):
        b = self.d[self.p:self.p + n]; self.p += n; return b
    def u32(self):
        return struct.unpack('>I', self.bytes(4))[0]
    def align(self):
        if self.p & 3: self.p += 4 - (self.p & 3)

def parse_decl(blob):
    out = []
    for i in range(0, len(blob) - 11, 12):
        stream, offset, typ = struct.unpack_from('>HHI', blob, i)
        method, usage, uidx = blob[i + 8], blob[i + 9], blob[i + 10]
        if stream == 0xFF:
            break
        out.append((stream, offset, typ, usage, uidx))
    return out

def parse_material(d, info_payload_off, version):
    r = Reader(d, info_payload_off)
    hdr = r.bytes(244)
    name = hdr[:64].split(b'\0')[0].decode('ascii', 'replace')
    ntex = struct.unpack_from('>I', hdr, 96)[0]
    def hi(k): return struct.unpack_from('>I', hdr, 8 * k)[0]
    def lo(k): return struct.unpack_from('>I', hdr, 8 * k + 4)[0]
    ntex = lo(12); nsamp = hi(13); nvar = lo(13); npass = hi(14)
    flags = hi(4)
    texs = []
    for i in range(ntex):
        rec = r.bytes(140)
        texs.append(rec)
    for i in range(nsamp):
        r.bytes(32)
    passes = []
    for p in range(npass):
        for v in range(nvar):
            rec = r.bytes(176)
            r.p += 4
            var = r.u32(); pas = r.u32()
            if version <= 0x3D and var >= 4: var += 1
            declsize = r.u32(); decl = r.bytes(declsize); r.align()
            key = r.u32()
            if key == 0: key = r.bytes(8)
            vsz = r.u32(); psz = r.u32(); r.bytes(vsz); r.align(); r.bytes(psz); r.align()
            pkey = r.u32()
            if pkey == 0: pkey = r.bytes(8)
            pvsz = r.u32(); ppsz = r.u32(); r.bytes(pvsz); r.align(); r.bytes(ppsz); r.align()
            passes.append(dict(pas=pas, var=var, decl=parse_decl(decl), declsize=declsize, key=key, vs=(vsz, psz), pkey=pkey, ps=(pvsz, ppsz)))
    return dict(name=name, ntex=ntex, nsamp=nsamp, nvar=nvar, npass=npass, flags=flags, passes=passes, end=r.p)

def materials_of(pak, geo_name):
    ep, es, h = pak.entry(0x07, name=geo_name)
    ek = kids(pak.d, ep + 12, ep + 12 + es)
    geo = next(k for k in ek if k[1] == 0x321)
    gk = kids(pak.d, geo[0] + 12, geo[0] + 12 + geo[4])
    out = []
    for k in gk:
        if k[1] != 0x324: continue
        for (p, t, a, b, size) in kids(pak.d, k[0] + 12, k[0] + 12 + k[4]):
            if t == 0x331:
                out.append((k, (p, t, a, size)))
    return out

if __name__ == '__main__':
    pak = Pak('D:/EOT_Extract/extracted/BaseGameplay.ext')
    for geo in sys.argv[1:]:
        for (mk, (p, t, ver, size)) in materials_of(pak, geo):
            m = parse_material(pak.d, p + 12, ver)
            print('%s: material %r v%d ntex %d nsamp %d nvar %d npass %d flags %#x; parsed to +%#x of %#x' % (
                geo, m['name'], ver, m['ntex'], m['nsamp'], m['nvar'], m['npass'], m['flags'], m['end'] - (p + 12), size))
            seen = set()
            for ps in m['passes']:
                key = (ps['pas'], tuple(ps['decl']))
                if key in seen: continue
                seen.add(key)
                print('   pass %d var %d decl(%d B): %s' % (ps['pas'], ps['var'], ps['declsize'],
                      ', '.join('%s%d@%d:%06x' % (USAGES.get(u, str(u)), ui, off, typ) for (s, off, typ, u, ui) in ps['decl'])))
