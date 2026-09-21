import csv

import ida_auto
import ida_funcs
import ida_name
import ida_ua
import idautils
import idc

ida_auto.auto_wait()
out = idc.ARGV[1]
PPC = idc.get_inf_attr(idc.INF_PROCNAME).lower().startswith("ppc")
segs = [(idc.get_segm_start(s), idc.get_segm_end(s)) for s in idautils.Segments()]


def dname(ea):
    n = ida_funcs.get_func_name(ea) or idc.get_name(ea)
    d = ida_name.demangle_name(n, idc.get_inf_attr(idc.INF_SHORT_DEMNAMES))
    return d or n


def is_addr(v):
    return any(a <= v < b for a, b in segs)


def keep(v):
    v &= 0xFFFFFFFF
    m = v if v < 0x80000000 else 0x100000000 - v
    return m >= 0x1000 and not is_addr(v)


with open(out, "w", newline="", encoding="utf-8") as fh:
    w = csv.writer(fh)
    for f in idautils.Functions():
        fn = ida_funcs.get_func(f)
        consts = set()
        his = {}
        idx = 0
        for ea in idautils.FuncItems(f):
            idx += 1
            insn = ida_ua.insn_t()
            if not ida_ua.decode_insn(insn, ea):
                continue
            mn = insn.get_canon_mnem()
            if PPC:
                if mn == "lis":
                    his[insn.ops[0].reg] = ((insn.ops[1].value & 0xFFFF) << 16, idx)
                elif mn in ("ori", "addi") and insn.ops[1].reg in his and his[insn.ops[1].reg][1] >= idx - 8:
                    hi = his[insn.ops[1].reg][0]
                    lo = insn.ops[2].value & 0xFFFF
                    if mn == "addi" and lo >= 0x8000:
                        lo -= 0x10000
                    v = (hi + lo) & 0xFFFFFFFF
                    if keep(v):
                        consts.add(v)
                    his.pop(insn.ops[1].reg, None)
                elif mn == "li":
                    v = insn.ops[1].value & 0xFFFFFFFF
                    if keep(v):
                        consts.add(v)
            else:
                if mn == "LDR" and insn.ops[1].type == ida_ua.o_mem:
                    v = idc.get_wide_dword(insn.ops[1].addr)
                    if keep(v):
                        consts.add(v)
                elif mn == "MOVW":
                    his[insn.ops[0].reg] = (insn.ops[1].value & 0xFFFF, idx)
                elif mn == "MOVT" and insn.ops[0].reg in his and his[insn.ops[0].reg][1] >= idx - 8:
                    v = ((insn.ops[1].value & 0xFFFF) << 16) | his[insn.ops[0].reg][0]
                    if keep(v):
                        consts.add(v)
                    his.pop(insn.ops[0].reg, None)
                elif mn in ("MOV", "MOVS") and insn.ops[1].type == ida_ua.o_imm:
                    v = insn.ops[1].value & 0xFFFFFFFF
                    if keep(v):
                        consts.add(v)
        w.writerow(["%#x" % f, fn.end_ea - fn.start_ea, dname(f), ";".join("%x" % v for v in sorted(consts))])
print("done constdump")
idc.qexit(0)
