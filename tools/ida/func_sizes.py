import ida_auto
import ida_funcs
import idc

ida_auto.auto_wait()
argv = idc.ARGV
out = argv[1]
with open(out, 'w', encoding='utf-8') as f:
    for x in argv[2].split(','):
        ea = int(x, 16)
        fn = ida_funcs.get_func(ea)
        if fn:
            f.write(f'{fn.start_ea:#x} {ida_funcs.get_func_name(fn.start_ea)} size {fn.end_ea - fn.start_ea:#x}\n')
        else:
            f.write(f'{ea:#x} no function\n')
    f.write('done\n')
