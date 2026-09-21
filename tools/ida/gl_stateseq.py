import csv

import ida_auto
import ida_funcs
import ida_name
import ida_ua
import idaapi
import idautils
import idc

ida_auto.auto_wait()
out = idc.ARGV[1]
procname = idc.get_inf_attr(idc.INF_PROCNAME).lower()
PPC = procname.startswith("ppc")


def dname(ea):
    n = ida_funcs.get_func_name(ea) or idc.get_name(ea)
    d = ida_name.demangle_name(n, idc.get_inf_attr(idc.INF_SHORT_DEMNAMES))
    return d or n


def is_func_start(ea):
    f = ida_funcs.get_func(ea)
    return bool(f) and f.start_ea == ea


targets = set()
for ea in idautils.Functions():
    if dname(ea).startswith("BaseObject::SetStateFn"):
        targets.add(ea)
print("SetStateFn at", [hex(x) for x in targets])

callers = set()
for t in targets:
    for x in idautils.XrefsTo(t):
        f = ida_funcs.get_func(x.frm)
        if f:
            callers.add(f.start_ea)
print("callers", len(callers))


def name_pair(ea):
    if ea and is_func_start(ea):
        return "%#x" % ea, dname(ea)
    return "", ""


rows = []
for cea in sorted(callers):
    if PPC:
        regs = {}
        his = {}
        stack = {}
        for item in idautils.FuncItems(cea):
            insn = ida_ua.insn_t()
            if not ida_ua.decode_insn(insn, item):
                continue
            mn = insn.get_canon_mnem()
            if mn == "lis":
                his[insn.ops[0].reg] = (insn.ops[1].value & 0xFFFF) << 16
                regs.pop(insn.ops[0].reg, None)
            elif mn == "addi" and insn.ops[1].reg in his:
                lo = insn.ops[2].value & 0xFFFF
                lo = lo - 0x10000 if lo >= 0x8000 else lo
                regs[insn.ops[0].reg] = (his[insn.ops[1].reg] + lo) & 0xFFFFFFFF
            elif mn == "li":
                regs[insn.ops[0].reg] = insn.ops[1].value & 0xFFFFFFFF
            elif mn == "mr":
                regs[insn.ops[0].reg] = regs.get(insn.ops[1].reg)
            elif mn == "stw" and insn.ops[1].type == ida_ua.o_displ and insn.ops[1].reg == 1:
                v = regs.get(insn.ops[0].reg)
                if v is not None and is_func_start(v):
                    stack[insn.ops[1].addr] = v
                else:
                    stack.pop(insn.ops[1].addr, None)
            elif mn == "ld" and insn.ops[1].type == ida_ua.o_displ and insn.ops[1].reg == 1:
                regs[insn.ops[0].reg] = stack.get(insn.ops[1].addr)
            elif mn in ("b", "bl") and insn.ops[0].type == ida_ua.o_near:
                tgt = insn.ops[0].addr
                if not ida_funcs.get_func(tgt) or ida_funcs.get_func(tgt).start_ea == cea:
                    continue
                if tgt in targets:
                    st = regs.get(4)
                    upd, ent, ext = regs.get(5), regs.get(7), regs.get(9)
                    rows.append((cea, st, upd, ent, ext))
                for r in list(regs):
                    if r <= 12:
                        regs.pop(r, None)
                his.clear()
    else:
        regs = {}
        sp = {}
        for item in idautils.FuncItems(cea):
            insn = ida_ua.insn_t()
            if not ida_ua.decode_insn(insn, item):
                continue
            mn = insn.get_canon_mnem()
            if mn == "LDR" and insn.ops[1].type == ida_ua.o_mem:
                regs[insn.ops[0].reg] = idc.get_wide_dword(insn.ops[1].addr)
            elif mn == "LDR" and insn.ops[1].type == ida_ua.o_displ:
                base = regs.get(insn.ops[1].reg)
                if base is not None and insn.ops[1].reg != 13:
                    d = insn.ops[1].addr
                    d = d - (1 << 32) if d >= (1 << 31) else d
                    v = idc.get_wide_dword((base + d) & 0xFFFFFFFF)
                    regs[insn.ops[0].reg] = v if is_func_start(v) else None
                else:
                    regs[insn.ops[0].reg] = None
            elif mn == "MOV" and insn.ops[1].type == ida_ua.o_imm:
                regs[insn.ops[0].reg] = insn.ops[1].value & 0xFFFFFFFF
            elif mn == "MOV" and insn.ops[1].type == ida_ua.o_reg:
                regs[insn.ops[0].reg] = regs.get(insn.ops[1].reg)
            elif mn == "LDRD":
                mem = insn.ops[2]
                base = regs.get(mem.reg) if mem.type == ida_ua.o_displ else None
                if base is not None:
                    d = mem.addr
                    d = d - (1 << 32) if d >= (1 << 31) else d
                    v = idc.get_wide_dword((base + d) & 0xFFFFFFFF)
                    regs[insn.ops[0].reg] = v if is_func_start(v) else None
                else:
                    regs[insn.ops[0].reg] = None
            elif mn == "STRD" and insn.ops[2].type == ida_ua.o_displ and insn.ops[2].reg == 13:
                v = regs.get(insn.ops[0].reg)
                if v is not None and is_func_start(v):
                    sp[insn.ops[2].addr] = v
                else:
                    sp.pop(insn.ops[2].addr, None)
            elif mn == "STR" and insn.ops[1].type == ida_ua.o_displ and insn.ops[1].reg == 13:
                v = regs.get(insn.ops[0].reg)
                if v is not None and is_func_start(v):
                    sp[insn.ops[1].addr] = v
                else:
                    sp.pop(insn.ops[1].addr, None)
            elif mn == "BL":
                tgt = insn.ops[0].addr
                if tgt in targets:
                    st = regs.get(1)
                    upd = regs.get(2)
                    upd = upd if upd and is_func_start(upd) else None
                    rows.append((cea, st, upd, sp.get(0), sp.get(8)))
                for r in list(regs):
                    if r <= 3 or r == 12 or r == 14:
                        regs.pop(r, None)

with open(out, "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    for cea, st, upd, ent, ext in rows:
        w.writerow(["%#x" % cea, dname(cea), "" if st is None else st,
                    *name_pair(upd), *name_pair(ent), *name_pair(ext)])
print("done", len(rows), "registrations")
