import csv
import glob
import os
import struct
import sys

root = os.environ.get("EOT_EXTRACT", r"D:\EOT_Extract\extracted")
filters = [a.lower() for a in sys.argv[1:]]


def children(m, start, end):
    o = start
    while o + 12 <= end:
        t, a, s = struct.unpack_from(">IIi", m, o)
        if s < 0 or o + 12 + s > end:
            return
        yield t, a, o + 12, o + 12 + s
        o += 12 + s


def name_of(m, s, e):
    for t, a, cs, ce in children(m, s, e):
        if t == 5006:
            blob = m[cs:ce]
            i = blob.find(b"\0", 16)
            txt = blob[16:i] if i > 16 else b""
            if 3 < len(txt) < 64 and all(32 <= c < 127 for c in txt):
                return txt.decode()
            import re
            mm = re.search(rb"[A-Za-z][A-Za-z0-9_]{3,60}", blob)
            return mm.group(0).decode() if mm else "?"
    return "?"


rows = []
for path in sorted(glob.glob(os.path.join(root, "*.ext"))):
    pack = os.path.basename(path)[:-4]
    if filters and not any(f in pack.lower() for f in filters):
        continue
    m = open(path, "rb").read()
    ot, oa, osz = struct.unpack_from(">IIi", m, 0)
    for t, a, s, e in children(m, 12, 12 + osz):
        if t != 48:
            continue
        for t2, a2, s2, e2 in list(children(m, s, e))[1:]:
            env = name_of(m, s2, e2)
            for t3, a3, s3, e3 in children(m, s2, e2):
                if t3 != 3003:
                    continue
                prims = []
                nobj = nmat = nnodes = 0
                vb = ib = 0
                for t4, a4, s4, e4 in children(m, s3, e3):
                    ver = struct.unpack_from(">H", m, s4 - 8)[0]
                    if t4 == 204:
                        rec = 76 if ver >= 8 else 72
                        for o in range(s4, e4 - rec + 1, rec):
                            if not any(m[o:o + rec]):
                                continue
                            nidx = struct.unpack_from(">I", m, o + 28)[0]
                            sect = struct.unpack_from(">I", m, o + 24)[0]
                            prims.append((nidx, sect))
                    elif t4 == 208:
                        nobj = (e4 - s4) // 140
                    elif t4 == 202:
                        nmat = sum(1 for t5, *_ in children(m, s4, e4) if t5 == 804)
                    elif t4 == 100:
                        nnodes = (e4 - s4)
                    elif t4 == 5014:
                        vb = e4 - s4
                    elif t4 == 5016:
                        ib = e4 - s4
                tris = [max(0, n - 2) for n, _ in prims]
                tris_sorted = sorted(tris)
                def q(p):
                    return tris_sorted[min(len(tris_sorted) - 1, int(p * len(tris_sorted)))] if tris_sorted else 0
                small = sum(1 for x in tris if x <= 64)
                rows.append({
                    "pack": pack, "env": env, "prims": len(prims), "tris": sum(tris),
                    "tri_p50": q(0.5), "tri_p90": q(0.9), "tri_max": max(tris) if tris else 0,
                    "prims_le64tri": small, "materials": nmat, "objects": nobj,
                    "node_bytes": nnodes, "vb_mb": round(vb / 1e6, 1), "ib_mb": round(ib / 1e6, 1),
                    "prims_per_material": round(len(prims) / nmat, 1) if nmat else 0,
                })
with open("env_prims.csv", "w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0].keys()) if rows else ["pack"])
    w.writeheader()
    w.writerows(rows)
rows.sort(key=lambda r: -r["prims"])
print("%-40s %-32s %6s %8s %5s %5s %6s %6s %5s %5s %7s" % ("pack", "env", "prims", "tris", "p50", "p90", "<=64", "mats", "objs", "vbMB", "prim/mat"))
for r in rows[:60]:
    print("%-40s %-32s %6d %8d %5d %5d %6d %6d %5d %5.1f %7.1f" % (r["pack"][:40], r["env"][:32], r["prims"], r["tris"], r["tri_p50"], r["tri_p90"], r["prims_le64tri"], r["materials"], r["objects"], r["vb_mb"], r["prims_per_material"]))
print("environments", len(rows), "total prims", sum(r["prims"] for r in rows), "total tris", sum(r["tris"] for r in rows))
