import csv
import re
import sys

import ida_auto
import ida_funcs
import ida_hexrays
import ida_name
import idaapi
import idautils
import idc

ida_auto.auto_wait()
argv = idc.ARGV
mode, out = argv[1], argv[2]


def dname(ea):
    n = ida_funcs.get_func_name(ea) or idc.get_name(ea)
    d = ida_name.demangle_name(n, idc.get_inf_attr(idc.INF_SHORT_DEMNAMES))
    return d or n


def matching(regex):
    rx = re.compile(regex, re.I)
    for ea in idautils.Functions():
        if rx.search(dname(ea)) or rx.search(ida_funcs.get_func_name(ea)):
            yield ea


def func_of(ea):
    f = ida_funcs.get_func(ea)
    return f.start_ea if f else None


if mode == "strings":
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["ea", "string", "refs"])
        for s in idautils.Strings():
            refs = set()
            for x in idautils.XrefsTo(s.ea):
                fe = func_of(x.frm)
                if fe is not None:
                    refs.add(dname(fe))
            w.writerow([f"{s.ea:#x}", str(s), " | ".join(sorted(refs))])
elif mode in ("decomp", "decompaddr"):
    ok = ida_hexrays.init_hexrays_plugin()
    eas = list(matching(argv[3])) if mode == "decomp" else [int(x, 16) for x in argv[3].split(",")]
    with open(out, "w", encoding="utf-8") as f:
        f.write(f"hexrays: {ok}\n")
        for ea in eas:
            f.write(f"\n// ===== {ea:#x} {dname(ea)} =====\n")
            if not ok:
                continue
            try:
                cf = ida_hexrays.decompile(ea)
                f.write(str(cf) + "\n")
            except Exception as e:  # noqa
                f.write(f"// decompile failed: {e}\n")
                for item in idautils.FuncItems(ea):
                    f.write(f"{item:#x}  {idc.GetDisasm(item)}\n")
elif mode == "callees":
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        for ea in matching(argv[3]):
            callees = set()
            for item in idautils.FuncItems(ea):
                for x in idautils.XrefsFrom(item, idaapi.XREF_FAR):
                    if x.type in (idaapi.fl_CN, idaapi.fl_CF):
                        callees.add(dname(x.to))
            w.writerow([f"{ea:#x}", dname(ea), " | ".join(sorted(callees))])
elif mode == "xrefs":
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        for ea in matching(argv[3]):
            callers = set()
            for x in idautils.XrefsTo(ea):
                fe = func_of(x.frm)
                if fe is not None:
                    callers.add(dname(fe))
            w.writerow([f"{ea:#x}", dname(ea), " | ".join(sorted(callers))])
print("done", mode, out)
idc.qexit(0)
