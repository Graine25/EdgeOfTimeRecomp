import csv
import sys

import ida_auto
import ida_funcs
import ida_name
import idaapi
import idautils
import idc

ida_auto.auto_wait()
out = idc.ARGV[1] if len(idc.ARGV) > 1 else "names.csv"
with open(out, "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    w.writerow(["kind", "ea", "size", "name", "demangled"])
    for ea in idautils.Functions():
        fn = ida_funcs.get_func(ea)
        name = ida_funcs.get_func_name(ea)
        dem = ida_name.demangle_name(name, idc.get_inf_attr(idc.INF_SHORT_DEMNAMES)) or ""
        w.writerow(["func", f"{ea:#x}", fn.end_ea - fn.start_ea, name, dem])
    for ea, name in idautils.Names():
        if ida_funcs.get_func(ea) and ida_funcs.get_func(ea).start_ea == ea:
            continue
        dem = ida_name.demangle_name(name, idc.get_inf_attr(idc.INF_SHORT_DEMNAMES)) or ""
        w.writerow(["name", f"{ea:#x}", idc.get_item_size(ea), name, dem])
print("exported to", out)
idc.qexit(0)
