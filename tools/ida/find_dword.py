import ida_auto
import ida_bytes
import ida_funcs
import ida_name
import ida_segment
import idc

ida_auto.auto_wait()
argv = idc.ARGV
out = argv[1]
values = [int(x, 16) & 0xFFFFFFFF for x in argv[2].split(',')]


def dname(ea):
    f = ida_funcs.get_func(ea)
    if not f:
        return '-'
    n = ida_funcs.get_func_name(f.start_ea)
    d = ida_name.demangle_name(n, idc.get_inf_attr(idc.INF_SHORT_DEMNAMES))
    return d or n


segs = []
for i in range(ida_segment.get_segm_qty()):
    seg = ida_segment.getnseg(i)
    data = ida_bytes.get_bytes(seg.start_ea, seg.end_ea - seg.start_ea)
    if data:
        segs.append((seg, data))

with open(out, 'w', encoding='utf-8') as f:
    for v in values:
        pat = v.to_bytes(4, 'big')
        f.write(f'\n// ===== {v:#010x} =====\n')
        for seg, data in segs:
            k = data.find(pat)
            while k >= 0:
                ea = seg.start_ea + k
                base = ea - (k % 4)
                around = ' '.join('%08x' % int.from_bytes(data[j:j + 4], 'big') for j in range(max(0, k - 16), min(len(data) - 3, k + 32), 4))
                f.write(f'{ea:#x} seg {ida_segment.get_segm_name(seg)} align {k % 4} fn {dname(ea)}  [{around}]\n')
                k = data.find(pat, k + 1)
    f.write('done\n')
