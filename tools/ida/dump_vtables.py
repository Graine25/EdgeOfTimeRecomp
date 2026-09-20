import csv, sys
import ida_auto, ida_funcs, ida_name, idaapi, idautils, idc

ida_auto.auto_wait()
out = idc.ARGV[1]
ptr = 8 if idaapi.inf_is_64bit() else 4
rd = idc.get_qword if ptr == 8 else idc.get_wide_dword

def dname(ea):
    n = ida_funcs.get_func_name(ea) or idc.get_name(ea)
    d = ida_name.demangle_name(n, idc.get_inf_attr(idc.INF_SHORT_DEMNAMES))
    return d or n

rows = []
for ea, name in idautils.Names():
    d = ida_name.demangle_name(name, idc.get_inf_attr(idc.INF_SHORT_DEMNAMES)) or name
    if not d.startswith("`vtable for'"):
        continue
    cls = d[len("`vtable for'"):]
    p = ea + 2 * ptr
    slot = 0
    while True:
        v = rd(p)
        f = ida_funcs.get_func(v)
        if v == 0:
            rows.append((cls, slot, "0", "-"))
        elif not f:
            break
        else:
            rows.append((cls, slot, "%#x" % v, dname(v) if f.start_ea == v else dname(f.start_ea) + "+%d" % (v - f.start_ea)))
        slot += 1
        p += ptr
        if slot > 200: break
        n2 = idc.get_name(p)
        if n2 and (ida_name.demangle_name(n2, idc.get_inf_attr(idc.INF_SHORT_DEMNAMES)) or n2).startswith("`vtable for'"):
            break
with open(out, "w", newline="", encoding="utf-8") as fh:
    w = csv.writer(fh); w.writerow(["class", "slot", "ea", "name"]); w.writerows(rows)
print("done", len(rows))
idc.qexit(0)
