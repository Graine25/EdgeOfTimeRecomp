import collections
import os
import re

import idautils
import idc

RANGES = [(0x82080000, 0x82240000), (0x82300000, 0x82312000)]
GLOBALS = (0x82390000, 0x824F0000)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                   "tools", "ida", "engine_symbols.toml")
SKIP = ("sub_", "nullsub", "j_", "?", "__", "_")


def family(name):
    m = re.match(r"([A-Za-z0-9]+?)(::|_)", name)
    return m.group(1) if m else "misc"


def main():
    fams = collections.OrderedDict()
    count = 0
    for lo, hi in RANGES:
        for ea in idautils.Functions(lo, hi):
            name = idc.get_name(ea)
            if name.startswith(SKIP):
                continue
            fams.setdefault(family(name), []).append((ea, name))
            count += 1
    auto = re.compile(r"^(g|vt)_[A-Za-z0-9]+_[0-9A-F]{4}$")
    globs = [(ea, nm) for ea, nm in idautils.Names()
             if GLOBALS[0] <= ea < GLOBALS[1] and (nm.startswith("g_") or nm.startswith("vtbl_")) and not auto.match(nm)]
    with open(os.path.normpath(OUT), "w", encoding="utf-8") as fp:
        fp.write("# engine symbol index for Default.xex (EdgeOfTime.xex.i64), generated from the IDB.\n")
        fp.write("# Every named engine function in 0x82080000-0x82240000 and the BU/util slice 0x82300000-0x82312000,\n")
        fp.write("# grouped by family prefix (the text before the first :: or _). SDK libraries (D3D, D3DX, XAudio2, CRI,\n")
        fp.write("# DemonWare, CRT) above 0x82240000 are not indexed. Regenerate with tools/ida/dump_engine_symbols.py.\n")
        fp.write("# Names are the reverser's reading of the code; a name ending in SlotN / CSlotN / _A.._H is a vtable slot or\n")
        fp.write("# a sibling whose exact role was not read.\n")
        fp.write("# Globals named g_<Family>_<addr16> / vt_<Family>_<addr16> carry only the owner family of every function\n")
        fp.write("# that touches them (no meaning read yet) and are left out of [globals]; StaticInit_*/StaticDtor_* are the\n")
        fp.write("# CRT initialiser pairs (StaticInit_Pool_* build the BUPool globals g_Pool_*).\n\n")
        fp.write("[summary]\nfunctions = %d\nfamilies = %d\nglobals = %d\n\n" % (count, len(fams), len(globs)))
        for fam in sorted(fams, key=lambda k: (-len(fams[k]), k)):
            fp.write("[functions.%s]\n" % re.sub(r"[^A-Za-z0-9]", "_", fam))
            for ea, name in sorted(fams[fam]):
                fp.write('"0x%08X" = "%s"\n' % (ea, name.replace('"', '\\"')))
            fp.write("\n")
        fp.write("[globals]\n")
        for ea, name in sorted(globs):
            fp.write('"0x%08X" = "%s"\n' % (ea, name))
    print("wrote", OUT, count, "functions", len(fams), "families", len(globs), "globals")


main()
