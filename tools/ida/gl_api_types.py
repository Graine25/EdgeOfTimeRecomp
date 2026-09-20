import json
import re

import ida_auto
import ida_nalt
import ida_typeinf
import idc

GENERIC = re.compile(r"^(v\d+|result|nullsub_\d+|Stub_|GLAPIStub_|j_|sub_)")


def field_name(slot, fn, used):
    m = re.match(r"GLAPI\w+::(\w+)", fn or "")
    name = m.group(1) if m else None
    if not name or name == "InitAPI" or GENERIC.match(fn or ""):
        name = "slot%d" % slot
    name = re.sub(r"[^A-Za-z0-9_]", "_", name)
    while name in used:
        name += "_"
    used.add(name)
    return name


PRIM = {
    "void": "void", "bool": "bool", "char": "char", "schar": "signed char", "uchar": "unsigned char",
    "short": "short", "ushort": "unsigned short", "int": "int", "uint": "unsigned int",
    "long": "int", "ulong": "unsigned int", "long long": "__int64", "unsigned long long": "unsigned __int64",
    "__int64": "__int64", "float": "float", "double": "double", "wchar_t": "wchar_t", "size_t": "unsigned int",
    "unsigned int": "unsigned int", "unsigned short": "unsigned short", "unsigned char": "unsigned char",
    "signed char": "signed char", "unsigned long": "unsigned int",
}


class TypeMap:
    def __init__(self):
        self.structs, self.typedefs, self.unknown = set(), {}, {}

    @staticmethod
    def split_args(text):
        out, depth, cur = [], 0, ""
        for ch in text:
            if ch in "<(":
                depth += 1
            elif ch in ">)":
                depth -= 1
            if ch == "," and depth == 0:
                out.append(cur.strip())
                cur = ""
            else:
                cur += ch
        if cur.strip():
            out.append(cur.strip())
        return out

    def ctype(self, arg, where=""):
        a = arg.strip()
        if a == "...":
            return "..."
        if "(" in a:
            return "void *"
        ptr = a.count("*") + (1 if "&" in a else 0)
        base = re.sub(r"\b(const|volatile|struct|class|enum)\b", "", a).replace("*", "").replace("&", "").strip()
        base = re.sub(r"\s+", " ", base)
        if base in PRIM:
            c = PRIM[base]
        else:
            ns = base.split("::")
            short = re.sub(r"[^A-Za-z0-9_]", "_", ns[-1])
            if short.endswith("_e") or "Enums" in ns:
                self.typedefs[short] = "int"
                c = short
            elif ptr == 0 and (short.endswith("Handle") or short.endswith("Id") or short.endswith("ID") or short.endswith("Crc")):
                self.typedefs[short] = "unsigned int"
                c = short
            elif ptr:
                self.structs.add(short)
                c = short
            else:
                self.unknown.setdefault(short, where)
                self.typedefs[short] = "int"
                c = short
        return c + " *" * ptr

    def ret(self, text):
        t = (text or "int").strip()
        if "*" in t:
            return "void *"
        t = t.replace("__fastcall", "").replace("__cdecl", "").strip()
        return t if t in ("void", "bool", "char", "float", "double", "int", "unsigned int", "__int64", "unsigned __int64",
                          "short", "unsigned short", "unsigned __int8", "__int8", "__int16", "unsigned __int16") else "int"

    def decls(self):
        out = ["struct %s;" % n for n in sorted(self.structs)]
        out += ["typedef %s %s;" % (t, n) for n, t in sorted(self.typedefs.items())]
        return "\n".join(out)


def load_signatures(names_csv, proto_csv):
    import csv
    sigs = {}
    if not names_csv:
        return sigs
    for r in csv.reader(open(names_csv, encoding="utf-8")):
        if len(r) < 5 or r[0] != "func":
            continue
        m = re.match(r"(GLAPI\w+::\w+)\((.*)\)$", r[4])
        if m:
            sigs[m.group(1)] = ["int", m.group(2)]
    if proto_csv:
        for r in csv.DictReader(open(proto_csv, encoding="utf-8")):
            k = r["name"].split("(")[0]
            if k in sigs and r["ret"] != "?":
                sigs[k][0] = r["ret"]
    return sigs


def apply(tables_path, map_path, report_path, names_csv=None, proto_csv=None):
    ida_auto.auto_wait()
    tables = json.load(open(tables_path))
    apimap = json.load(open(map_path))
    report = open(report_path, "w", encoding="utf-8")
    sigs = load_signatures(names_csv, proto_csv)
    tm = TypeMap()
    decls, sizes, typed = [], {}, 0
    for api, slots in tables.items():
        short = api[5:]
        slots = {int(k): v for k, v in slots.items()}
        if not slots:
            continue
        n = max(slots)
        used = set()
        lines = ["struct API%s {" % short]
        for i in range(1, n + 1):
            fn = slots.get(i, "")
            name = field_name(i, fn, used)
            sig = sigs.get(fn) if fn.startswith("GLAPI") else None
            if sig:
                ret, args = sig
                cargs = [tm.ctype(a, fn) for a in tm.split_args(args)] or ["void"]
                lines.append("  %s (*%s)(%s);" % (tm.ret(ret), name, ", ".join(cargs)))
                typed += 1
            else:
                lines.append("  int (*%s)();" % name)
        lines.append("};")
        decls.append("\n".join(lines))
        sizes[short] = n
    text = tm.decls() + "\n" + "\n".join(decls)
    errs = ida_typeinf.idc_parse_types(text, ida_typeinf.PT_REPLACE)
    report.write("parse errors: %s (%d tables, %d prototyped fields, %d structs, %d typedefs)\n" % (errs, len(decls), typed, len(tm.structs), len(tm.typedefs)))
    if tm.unknown:
        report.write("by-value classes passed as int: %s\n" % ", ".join("%s (%s)" % kv for kv in sorted(tm.unknown.items())))
    if errs:
        open(report_path + ".decls.h", "w", encoding="utf-8").write(text)
    pairs = [("", s) for s in apimap] if isinstance(apimap, list) else list(apimap.items())
    for key, short in pairs:
        gname = "gpAPI" + short
        ea = int(key, 16) if key else idc.get_name_ea_simple(gname)
        if ea == idc.BADADDR:
            report.write("%s: no such global\n" % gname)
            continue
        ok = idc.set_name(ea, gname, idc.SN_NOWARN | 0x800)
        if short not in sizes:
            report.write("%#x %s (untyped)\n" % (ea, gname))
            continue
        idc.create_dword(ea)
        tif = ida_typeinf.tinfo_t()
        ida_typeinf.parse_decl(tif, None, "API%s *;" % short, ida_typeinf.PT_SIL)
        ida_typeinf.apply_tinfo(ea, tif, ida_typeinf.TINFO_DEFINITE)
        got = ida_typeinf.tinfo_t()
        ida_nalt.get_tinfo(got, ea)
        report.write("%#x %s slots=%d name=%s type=%s\n" % (ea, gname, sizes[short], ok, str(got)))
    report.close()


if "ARGS" in globals():
    apply(*ARGS)  # noqa: F821  (GUI exec)
else:
    apply(*idc.ARGV[1:6])
    print("done gl_api_types")
    idc.qexit(0)
