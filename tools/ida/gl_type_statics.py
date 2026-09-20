import re

import ida_auto
import ida_typeinf
import idautils
import idc

ida_auto.auto_wait()
report = open(idc.ARGV[1], "w", encoding="utf-8")
RULES = [
    (r"::s?m?b[A-Z]", "bool"),
    (r"::s?m?ui[A-Z]", "unsigned int"),
    (r"::s?m?i[A-Z]", "int"),
    (r"::s?m?f[A-Z]", "float"),
    (r"::s?m?h[A-Z]", "GameObjHandle"),
    (r"::s?m?pp[A-Z]", "void **"),
    (r"::s?m?p[A-Z]", "void *"),
]
n = 0
for ea, name in idautils.Names():
    if "::" not in name or idc.get_func_attr(ea, idc.FUNCATTR_START) != idc.BADADDR:
        continue
    for rx, ctype in RULES:
        if re.search(rx, name):
            tif = ida_typeinf.tinfo_t()
            if ida_typeinf.parse_decl(tif, None, ctype + ";", ida_typeinf.PT_SIL) is None:
                report.write("%#x %s: cannot parse %s\n" % (ea, name, ctype))
                break
            ok = ida_typeinf.apply_tinfo(ea, tif, ida_typeinf.TINFO_DEFINITE)
            report.write("%#x %s -> %s %s\n" % (ea, name, ctype, ok))
            n += ok
            break
report.write("typed %d\n" % n)
report.close()
print("done gl_type_statics", n)
idc.qexit(0)
