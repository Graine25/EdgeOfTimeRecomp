import re
import sys
import ida_auto
import ida_funcs
import ida_hexrays
import ida_name
import idautils
import idc

ida_auto.auto_wait()
argv = idc.ARGV
out = argv[1]
offs = [int(x, 16) for x in argv[2].split(',')]
decomp = len(argv) > 3 and argv[3] == 'decomp'


def dname(ea):
    n = ida_funcs.get_func_name(ea) or idc.get_name(ea)
    d = ida_name.demangle_name(n, idc.get_inf_attr(idc.INF_SHORT_DEMNAMES))
    return d or n


hits = {}
for fea in idautils.Functions():
    items = list(idautils.FuncItems(fea))
    for i, ea in enumerate(items):
        if idc.print_insn_mnem(ea) != 'lwz':
            continue
        op1 = idc.print_operand(ea, 1)
        m = re.match(r'(?:0x)?([0-9A-Fa-f]+)\((r\d+)\)', op1)
        if not m:
            continue
        off = int(m.group(1), 16)
        if off not in offs:
            continue
        reg = idc.print_operand(ea, 0)
        ok = False
        for ea2 in items[i + 1:i + 6]:
            mn = idc.print_insn_mnem(ea2)
            if mn in ('mtctr', 'mtspr') and reg in idc.print_operand(ea2, 0) + idc.print_operand(ea2, 1):
                ok = True
                break
        if ok:
            hits.setdefault(fea, []).append((ea, off))

with open(out, 'w', encoding='utf-8') as f:
    f.write(f'{len(hits)} functions\n')
    for fea, lst in sorted(hits.items()):
        f.write(f'\n// ===== {fea:#x} {dname(fea)}: ' + ', '.join(f'{ea:#x} slot+{off:#x}' for ea, off in lst) + '\n')
        if decomp:
            try:
                ida_hexrays.init_hexrays_plugin()
                f.write(str(ida_hexrays.decompile(fea)) + '\n')
            except Exception as e:  # noqa
                f.write(f'// decompile failed: {e}\n')
    f.write('done\n')
