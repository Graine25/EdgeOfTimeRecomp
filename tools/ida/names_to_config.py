import csv
import re
import sys

src, cfg = sys.argv[1], sys.argv[2]
rep = sys.argv[3] if len(sys.argv) > 3 else None
SKIP = re.compile(r"^(sub_|helper_|nullsub_|j_|loc_|GameLogic_\d+|\?|__|_|Ke[A-Z]|Nt[A-Z]|Rtl[A-Z]|Xam[A-Z]|X[A-Z][a-z]|Ex[A-Z]|Mm[A-Z]|Ob[A-Z]|Io[A-Z]|Dbg|Hal[A-Z]|start$|Stub_)")
ENTRY = re.compile(r"^(0x[0-9A-Fa-f]{8})\s*=\s*\{(.*)\}\s*(#.*)?$")


def ident(name):
    n = name.split("(")[0].strip()
    n = re.sub(r"::~(\w+)", r"::\1_dtor", n)
    n = n.replace("operator new", "operator_new").replace("operator delete", "operator_delete").replace("operator()", "operator_call")
    n = n.replace("::", "_")
    n = re.sub(r"[^0-9A-Za-z_]+", "_", n)
    n = re.sub(r"_+", "_", n).strip("_")
    if not n or n[0].isdigit():
        n = "f_" + n
    return "eot_" + n


names = {}
for r in csv.reader(open(src, encoding="utf-8")):
    if len(r) > 3 and r[0] == "func":
        n = (r[4] or r[3]).strip()
        if n and not SKIP.match(n):
            names[int(r[1], 16)] = n

lines = open(cfg, encoding="utf-8").read().split("\n")
taken = set()
for ln in lines:
    m = re.search(r'name\s*=\s*"([^"]+)"', ln)
    if m:
        taken.add(m.group(1))
added = []
out = []
for ln in lines:
    m = ENTRY.match(ln.strip())
    if m and "name" not in m.group(2):
        ea = int(m.group(1), 16)
        if ea in names:
            nm = ident(names[ea])
            if nm in taken:
                nm = "%s_%04X" % (nm, ea & 0xFFFF)
            taken.add(nm)
            body = m.group(2).strip()
            body = ('name = "%s"' % nm) + (", " + body if body else "")
            ln = "%s = { %s }%s" % (m.group(1), body, (" " + m.group(3)) if m.group(3) else "")
            added.append((ea, names[ea], nm))
    out.append(ln)
open(cfg, "w", encoding="utf-8", newline="\n").write("\n".join(out))
print("named", len(added), "entries in", cfg)
if rep:
    with open(rep, "w", encoding="utf-8") as f:
        for ea, n, nm in added:
            f.write("%#x %s -> %s\n" % (ea, n, nm))
