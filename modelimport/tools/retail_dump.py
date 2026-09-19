import sys, struct, re
sys.path.insert(0, r'C:\Users\rieng\Documents\GitHub\reeot-dni\reference\eot_tools\SPIDER-MAN EDGE OF TIME TOOLS\tools')
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import x360_geo as G
from pakwriter import Pak, kids, whole

def material_name(d, mp):
    for (p, t, a, b, size) in kids(d, mp + 12, mp + 12 + struct.unpack_from('>I', d, mp + 8)[0]):
        if t == 0x0329 or size > 64:
            return d[p + 12:p + 12 + 64].split(b'\0')[0].decode('ascii', 'replace'), (p, t, a, size)
    return '?', None

def dump(pak, name):
    m = {mm[0]: mm for mm in G.get_models(pak.d)}[name]
    _, vb, vbsize, ib, ibsize = m
    descs = G.get_descriptors(pak.d, name)
    ep, es, h = pak.entry(0x07, name=name)
    ek = kids(pak.d, ep + 12, ep + 12 + es)
    geo = next(k for k in ek if k[1] == 0x321)
    gk = kids(pak.d, geo[0] + 12, geo[0] + 12 + geo[4])
    gi = pak.d[gk[0][0] + 12:gk[0][0] + 12 + gk[0][4]]
    print('%s: VB %d IB %d verts %d, geominfo %s' % (name, vbsize, ibsize, sum(d['nv'] for d in descs), struct.unpack_from('>4f4I4f4I', gi)))
    for i, d in enumerate(descs):
        print('  mesh %d: mat %d fmt %#x stride %d nv %d nidx %d pal %d' % (i, d['mat'], d['fmt'], d['stride'], d['nv'], d['nidx'], len(d['pal'])))
    for mi, k in enumerate([k for k in gk if k[1] == 0x324]):
        nm, info = material_name(pak.d, k[0])
        print('  material %d %r (chunk %d bytes, info %s)' % (mi, nm, k[4], info))
        seg = pak.d[k[0] + 12:k[0] + 12 + k[4]]
        for mm in re.finditer(rb'[ -~]{4,}\.(?:dds|tga)', seg):
            o = mm.start(); kind, crc = struct.unpack_from('>2I', seg, o - 16)
            print('    +%05x kind %d crc %08X %s' % (o, kind, crc, mm.group().decode()[-44:]))

if __name__ == '__main__':
    args = sys.argv[1:]
    path = 'D:/EOT_Extract/extracted/BaseGameplay.ext'
    if args and args[0].endswith('.ext'):
        path = args.pop(0)
    pak = Pak(path)
    for n in args:
        dump(pak, n)
