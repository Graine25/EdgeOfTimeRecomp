import csv
import ida_auto, ida_bytes, ida_funcs, ida_name, ida_ua, idaapi, idautils, idc

ida_auto.auto_wait()
out = idc.ARGV[1]

def dname(ea):
    n = ida_funcs.get_func_name(ea) or idc.get_name(ea)
    d = ida_name.demangle_name(n, idc.get_inf_attr(idc.INF_SHORT_DEMNAMES))
    return d or n

segs = [(idc.get_segm_start(s), idc.get_segm_end(s)) for s in idautils.Segments()]
def is_addr(v):
    for a, b in segs:
        if a <= v < b:
            return True
    return False

with open(out, "w", newline="", encoding="utf-8") as fh:
    w = csv.writer(fh)
    w.writerow(["ea", "size", "name", "imms", "strs"])
    for f in idautils.Functions():
        fn = ida_funcs.get_func(f)
        imms = []
        strs = []
        for ea in idautils.FuncItems(f):
            insn = ida_ua.insn_t()
            if ida_ua.decode_insn(insn, ea) == 0:
                continue
            for op in insn.ops:
                if op.type == ida_ua.o_void:
                    break
                if op.type == ida_ua.o_imm:
                    v = op.value
                    if v >= 0x80000000:
                        v -= 0x100000000
                    if abs(v) >= 32 and not is_addr(op.value):
                        imms.append(v)
            for x in idautils.DataRefsFrom(ea):
                s = idc.get_strlit_contents(x)
                if s:
                    strs.append(s.decode(errors="replace")[:40].replace(";", ","))
        w.writerow(["%#x" % f, fn.end_ea - fn.start_ea, dname(f), ";".join(str(v) for v in sorted(imms)), ";".join(strs)])
print("done")
idc.qexit(0)
