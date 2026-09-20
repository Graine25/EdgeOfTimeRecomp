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
                cf = ida_hexrays.decompile(ea, None, ida_hexrays.DECOMP_NO_CACHE)
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
elif mode == "strrefs":
    import ida_ua
    strs = {}
    for s in idautils.Strings():
        strs[s.ea] = str(s)
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["string_ea", "string", "func_ea", "func"])
        for fea in idautils.Functions():
            his = {}
            for item in idautils.FuncItems(fea):
                insn = ida_ua.insn_t()
                if ida_ua.decode_insn(insn, item) == 0:
                    continue
                mn = insn.get_canon_mnem()
                if mn == "lis":
                    his[insn.ops[0].reg] = (insn.ops[1].value & 0xFFFF) << 16
                elif mn in ("addi", "lwz", "lbz", "lhz", "stw", "stb", "sth", "lfs", "lfd") and len(his):
                    base = insn.ops[1].reg if mn == "addi" else insn.ops[1].phrase
                    if base in his:
                        lo = insn.ops[2].value if mn == "addi" else insn.ops[1].addr
                        lo = lo - 0x10000 if lo >= 0x8000 else lo
                        target = (his[base] + lo) & 0xFFFFFFFF
                        if target in strs:
                            w.writerow([f"{target:#x}", strs[target], f"{fea:#x}", dname(fea)])
                        if mn != "addi":
                            pass
elif mode == "callseq":
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        for ea in matching(argv[3]):
            fn = ida_funcs.get_func(ea)
            seq = []
            refs = []
            import ida_ua
            ppc = idaapi.get_inf_structure().procname.lower().startswith("ppc") if hasattr(idaapi, "get_inf_structure") else idc.get_inf_attr(idc.INF_PROCNAME).lower().startswith("ppc")
            his = {}
            for item in idautils.FuncItems(ea):
                for x in idautils.XrefsFrom(item, idaapi.XREF_FAR):
                    if x.type in (idaapi.fl_CN, idaapi.fl_CF):
                        seq.append(dname(x.to))
                    elif x.type in (idaapi.dr_O, idaapi.dr_R):
                        f2 = ida_funcs.get_func(x.to)
                        if f2 and f2.start_ea == x.to:
                            refs.append(dname(x.to))
                        elif x.type == idaapi.dr_R and not ida_funcs.get_func(x.to):
                            v = idc.get_wide_dword(x.to)
                            f3 = ida_funcs.get_func(v)
                            if f3 and f3.start_ea == v:
                                refs.append(dname(v))
                if ppc:
                    insn = ida_ua.insn_t()
                    if ida_ua.decode_insn(insn, item):
                        mn = insn.get_canon_mnem()
                        if mn == "lis":
                            his[insn.ops[0].reg] = (insn.ops[1].value & 0xFFFF) << 16
                        elif mn == "addi" and insn.ops[1].reg in his:
                            lo = insn.ops[2].value & 0xFFFF
                            lo = lo - 0x10000 if lo >= 0x8000 else lo
                            t = (his[insn.ops[1].reg] + lo) & 0xFFFFFFFF
                            f4 = ida_funcs.get_func(t)
                            if f4 and f4.start_ea == t:
                                refs.append(dname(t))
            w.writerow([f"{ea:#x}", dname(ea), fn.end_ea - fn.start_ea, "|".join(seq), "|".join(refs)])
elif mode == "apiseq":
    import ida_typeinf
    ok = ida_hexrays.init_hexrays_plugin()
    fields = {}
    for i in range(1, ida_typeinf.get_ordinal_limit()):
        t = ida_typeinf.tinfo_t()
        if t.get_numbered_type(None, i) and t.is_struct() and t.get_type_name() and t.get_type_name().startswith("API"):
            udt = ida_typeinf.udt_type_data_t()
            t.get_udt_details(udt)
            fields[t.get_type_name()] = {m.offset // 8: m.name for m in udt}

    class V(ida_hexrays.ctree_visitor_t):
        def __init__(self):
            super().__init__(ida_hexrays.CV_FAST)
            self.seq = []

        def visit_expr(self, e):
            if e.op != ida_hexrays.cot_call:
                return 0
            x = e.x
            while x.op == ida_hexrays.cot_cast:
                x = x.x
            if x.op == ida_hexrays.cot_obj:
                self.seq.append(dname(x.obj_ea))
            elif x.op in (ida_hexrays.cot_memptr, ida_hexrays.cot_memref):
                b = x.x
                while b.op == ida_hexrays.cot_cast:
                    b = b.x
                if b.op == ida_hexrays.cot_obj:
                    g = idc.get_name(b.obj_ea)
                    if g.startswith("gpAPI"):
                        st = "API" + g[5:]
                        self.seq.append("%s::%s" % (st, fields.get(st, {}).get(x.m, "off%d" % x.m)))
            return 0

    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        for ea in matching(argv[3]):
            fn = ida_funcs.get_func(ea)
            seq = []
            if ok:
                try:
                    cf = ida_hexrays.decompile(ea, None, ida_hexrays.DECOMP_NO_CACHE)
                    v = V()
                    v.apply_to(cf.body, None)
                    seq = v.seq
                except Exception:  # noqa
                    seq = []
            w.writerow([f"{ea:#x}", dname(ea), fn.end_ea - fn.start_ea, "|".join(seq), ""])
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
elif mode == "vtable":
    ok = ida_hexrays.init_hexrays_plugin()
    base = int(argv[3], 16)
    n = int(argv[4]) if len(argv) > 4 else 24
    with open(out, "w", encoding="utf-8") as f:
        f.write(f"hexrays: {ok}  vtable {base:#x}\n")
        for i in range(n):
            ea = idc.get_wide_dword(base + 4 * i)
            f.write(f"\n// ===== slot {i} @ {base + 4 * i:#x} -> {ea:#x} {dname(ea)} =====\n")
            if not ok or ida_funcs.get_func(ea) is None:
                continue
            try:
                f.write(str(ida_hexrays.decompile(ea, None, ida_hexrays.DECOMP_NO_CACHE)) + "\n")
            except Exception as e:  # noqa
                f.write(f"// decompile failed: {e}\n")
elif mode == "dword":
    import ida_bytes
    import ida_segment
    values = [int(x, 16) for x in argv[3].split(",")]
    with open(out, "w", encoding="utf-8") as f:
        for value in values:
            pat = value.to_bytes(4, "big")
            f.write("\n// ===== %#x =====\n" % value)
            for i in range(ida_segment.get_segm_qty()):
                seg = ida_segment.getnseg(i)
                ea = seg.start_ea
                while ea < seg.end_ea:
                    ea = ida_bytes.find_bytes(pat, ea, range_end=seg.end_ea)
                    if ea is None or ea == idaapi.BADADDR:
                        break
                    fe = func_of(ea)
                    around = " ".join("%08x" % idc.get_wide_dword(ea + 4 * k) for k in range(-4, 8))
                    f.write("%#x seg %s fn %s  [%s]\n" % (ea, ida_segment.get_segm_name(seg), dname(fe) if fe else "-", around))
                    ea += 1
elif mode == "vtraw":
    rx = re.compile(argv[3], re.I)
    n = int(argv[4]) if len(argv) > 4 else 64
    ptr = 8 if idaapi.inf_is_64bit() else 4
    rd = idc.get_qword if ptr == 8 else idc.get_wide_dword
    with open(out, "w", encoding="utf-8") as f:
        for ea, name in idautils.Names():
            d = ida_name.demangle_name(name, idc.get_inf_attr(idc.INF_SHORT_DEMNAMES)) or name
            if not d.startswith("`vtable for'") or not rx.search(d[len("`vtable for'"):]):
                continue
            f.write("\n// ===== %#x %s =====\n" % (ea, d))
            for i in range(n):
                p2 = ea + i * ptr
                v = rd(p2)
                fe = ida_funcs.get_func(v)
                lab = idc.get_name(p2)
                lab = (ida_name.demangle_name(lab, idc.get_inf_attr(idc.INF_SHORT_DEMNAMES)) or lab) if lab else ""
                f.write("%3d %#x %#x %s %s\n" % (i, p2, v, dname(fe.start_ea) if fe else "-", ("<- " + lab) if lab else ""))
elif mode == "drefs":
    with open(out, "w", encoding="utf-8") as f:
        for ea in [int(x, 16) for x in argv[3].split(",")]:
            f.write("\n// ===== refs to %#x %s =====\n" % (ea, idc.get_name(ea)))
            for x in idautils.XrefsTo(ea):
                fe = func_of(x.frm)
                f.write("%#x type %d in %s  %s\n" % (x.frm, x.type, dname(fe) if fe else "-", idc.GetDisasm(x.frm)))
print("done", mode, out)
idc.qexit(0)
