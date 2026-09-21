import csv

import ida_auto
import ida_funcs
import idaapi
import idc

ida_auto.auto_wait()
src, out = idc.ARGV[1], idc.ARGV[2]
SN = idc.SN_NOWARN | 0x800
PLACEHOLDER = ("sub_", "helper_", "nullsub_", "off_", "unk_", "dword_", "byte_", "word_", "flt_", "qword_", "GameLogic_", "loc_", "asc_")

n_ok = n_skip = n_fail = 0
with open(out, "w", encoding="utf-8") as rep:
    for row in csv.reader(open(src, encoding="utf-8")):
        if not row or row[0].startswith("#"):
            continue
        ea = int(row[0], 16)
        name = row[1].strip()
        force = len(row) > 2 and row[2].strip().lower() in ("1", "true", "force")
        fn = ida_funcs.get_func(ea)
        if fn and fn.start_ea != ea:
            rep.write("%#x %s: inside %s, skipped\n" % (ea, name, ida_funcs.get_func_name(fn.start_ea)))
            n_skip += 1
            continue
        cur = idc.get_name(ea) or ""
        if cur.endswith(("_shared", "_folded")) and not force:
            rep.write("%#x %s: keeps %s (shared body)\n" % (ea, name, cur))
            n_skip += 1
            continue
        if cur and not force and not cur.startswith(PLACEHOLDER):
            rep.write("%#x %s: keeps %s\n" % (ea, name, cur))
            n_skip += 1
            continue
        ok = idc.set_name(ea, name, SN) or idc.set_name(ea, "%s_%X" % (name, ea & 0xFFFF), SN)
        rep.write("%#x %s -> %s %s\n" % (ea, cur, name, "OK" if ok else "FAIL"))
        n_ok += ok
        n_fail += not ok
    rep.write("applied %d, skipped %d, failed %d\n" % (n_ok, n_skip, n_fail))
print("done apply_names", n_ok, n_skip, n_fail)
idc.qexit(0)
