import sys
import ida_auto
import ida_funcs
import ida_name
import ida_ua
import idaapi
import idautils
import idc

ida_auto.auto_wait()
argv = idc.ARGV
out = argv[1]
values = [int(x, 16) & 0xFFFFFFFF for x in argv[2].split(',')]


def dname(ea):
    n = ida_funcs.get_func_name(ea) or idc.get_name(ea)
    d = ida_name.demangle_name(n, idc.get_inf_attr(idc.INF_SHORT_DEMNAMES))
    return d or n


def imm_of(ea, n):
    op = idc.get_operand_type(ea, n)
    if op == idc.o_imm:
        return idc.get_operand_value(ea, n) & 0xFFFFFFFF
    return None


with open(out, 'w', encoding='utf-8') as f:
    for v in values:
        hi = (v >> 16) & 0xFFFF
        lo = v & 0xFFFF
        f.write(f'\n// ===== {v:#010x} (lis {hi:#06x} / lo {lo:#06x}) =====\n')
        hits = 0
        for fea in idautils.Functions():
            items = list(idautils.FuncItems(fea))
            for i, ea in enumerate(items):
                mn = idc.print_insn_mnem(ea)
                if mn not in ('lis', 'addis'):
                    continue
                im = imm_of(ea, 1)
                if im is None:
                    continue
                im16 = im & 0xFFFF
                cand = [(hi, lo, 'ori'), (((hi + 1) & 0xFFFF) if lo & 0x8000 else hi, lo, 'addi')]
                for h, l, kind in cand:
                    if im16 != h:
                        continue
                    reg = idc.print_operand(ea, 0)
                    for ea2 in items[i + 1:i + 9]:
                        mn2 = idc.print_insn_mnem(ea2)
                        if mn2 not in ('ori', 'addi', 'subi'):
                            continue
                        if idc.print_operand(ea2, 1) != reg:
                            continue
                        im2 = imm_of(ea2, 2)
                        if im2 is None:
                            continue
                        if mn2 == 'subi':
                            im2 = (-im2) & 0xFFFF
                        if (im2 & 0xFFFF) == l:
                            f.write(f'{ea:#x} in {dname(fea)} ({fea:#x}): {idc.GetDisasm(ea)} ; {idc.GetDisasm(ea2)}\n')
                            hits += 1
                            break
        f.write(f'// {hits} hits\n')
    f.write('done\n')
