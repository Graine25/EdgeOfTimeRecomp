import struct, glob, os, csv, io

root = r'C:\Users\rieng\Documents\GitHub\reeot-dni'


def children(buf, start, end):
    o = start
    while o + 12 <= end:
        t, a, s = struct.unpack_from('>IIi', buf, o)
        if s < 0 or o + 12 + s > end:
            return
        yield t, a, o + 12, o + 12 + s
        o += 12 + s


f = io.open(os.path.join(root, 'tools/pkz/env_occluders.csv'), 'w', newline='', encoding='utf-8')
out = csv.writer(f)
out.writerow(['package', 'environment', 'crc', 'octree', 'collision', 'occluder_prims', 'audio_occlusion', 'navmesh'])
for path in sorted(glob.glob(os.path.join(root, 'shader_notes/EOT_pak/*.pak'))):
    buf = open(path, 'rb').read()
    ot, oa, osz = struct.unpack_from('>IIi', buf, 0)
    for t, a, s, e in children(buf, 12, 12 + osz):
        if t != 48:
            continue
        kids = list(children(buf, s, e))
        for et, ea, es, ee in kids[1:]:
            name = '?'; crc = ''; oc = col = au = nm = 0; prims = 0
            for ct, ca, cs, ce in children(buf, es, ee):
                if ct == 5006:
                    crc = buf[cs:cs + 4].hex()
                    name = buf[cs + 24:cs + 88].split(b'\0')[0].decode('ascii', 'replace')
                elif ct == 3003:
                    oc = 1
                elif ct == 3004:
                    col = 1
                elif ct == 3005:
                    cc = list(children(buf, cs, ce))
                    prims = (cc[0][3] - cc[0][2]) // 68 if cc else 0
                elif ct == 3006:
                    au = 1
                elif ct == 3007:
                    nm = 1
            out.writerow([os.path.basename(path), name, crc, oc, col, prims, au, nm])
f.close()
print('written')
