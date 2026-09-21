import re
import sys

import ida_auto
import ida_funcs
import ida_hexrays
import idaapi
import idautils
import idc

ida_auto.auto_wait()
argv = idc.ARGV
vtraw_path, report_path = argv[1], argv[2]
factory_ea = int(argv[3], 16) if len(argv) > 3 else 0x880A0040
SN = idc.SN_NOWARN | 0x800

report = open(report_path, "w", encoding="utf-8")


def log(*a):
    report.write(" ".join(str(x) for x in a) + "\n")


def decomp(ea):
    try:
        return str(ida_hexrays.decompile(ea))
    except Exception as e:  # noqa: BLE001
        log("decompile failed", hex(ea), e)
        return ""


def rename(ea, name, force=False):
    cur = idc.get_name(ea) or ""
    fn = ida_funcs.get_func(ea)
    if fn and not force and (cur.startswith("nullsub_") or fn.end_ea - fn.start_ea <= 8):
        return False
    if not force and cur and not cur.startswith(("sub_", "helper_", "nullsub_", "off_", "unk_", "dword_", "byte_", "word_", "GameLogic_", "loc_")):
        return False
    ok = idc.set_name(ea, name, SN)
    if not ok:
        ok = idc.set_name(ea, "%s_%X" % (name, ea & 0xFFFF), SN)
    log("%s %s -> %s %s" % (hex(ea), cur, name, "OK" if ok else "FAIL"))
    return ok


vt3ds = {}
cls = None
for line in open(vtraw_path, encoding="utf-8"):
    m = re.match(r"// ===== 0x[0-9a-f]+ `vtable for'(.+?) =====", line)
    if m:
        cls = m.group(1)
        vt3ds[cls] = []
        continue
    if cls is None or not line.strip():
        continue
    m = re.match(r"\s*(\d+) 0x[0-9a-f]+ (0x[0-9a-f]+) (.*?)(?: <- (.*))?$", line.rstrip("\n"))
    if not m:
        continue
    idx, val, name, label = int(m.group(1)), int(m.group(2), 16), m.group(3).strip(), (m.group(4) or "").strip()
    if idx >= 2 and label.startswith("`vtable for'"):
        cls = None
        continue
    if idx < 2:
        continue
    vt3ds[cls].append((val, name))
log("3DS vtables:", len(vt3ds))


def method_of(entry):
    if entry in ("-", "__cxa_pure_virtual") or entry.startswith("`non-virtual thunk"):
        return None
    m = re.match(r"([A-Za-z_][\w:<>,]*?)::(~?[A-Za-z_]\w*)\(", entry)
    if not m:
        return None
    return m.group(1), m.group(2)


def name_vtable(vt_ea, cls_name):
    slots = vt3ds.get(cls_name)
    if not slots:
        log("no 3DS vtable for", cls_name)
        return 0
    named = 0
    k = 0
    while True:
        p = vt_ea + 4 * k
        lab = idc.get_name(p) if k > 0 else ""
        v = idc.get_wide_dword(p)
        f = ida_funcs.get_func(v)
        if k > 0 and lab and not lab.startswith(("off_", "unk_", "dword_")):
            log("  stop at", hex(p), "label", lab)
            break
        if not f:
            seg = idaapi.getseg(v)
            if seg and (seg.perm & idaapi.SEGPERM_EXEC) and ida_funcs.add_func(v):
                f = ida_funcs.get_func(v)
                log("  made function at", hex(v))
            else:
                log("  stop at", hex(p), "value", hex(v), "not code")
                break
        if f.start_ea != v:
            k += 1
            continue
        j = 0 if k == 0 else k + 1
        if j < len(slots):
            mo = method_of(slots[j][1])
            if mo:
                c, mth = mo
                if k == 0:
                    mth = "DeletingDtor"
                nrefs = sum(1 for _ in idautils.XrefsTo(v))
                if nrefs <= 1 and c != cls_name:
                    c = cls_name
                if rename(v, "%s::%s" % (c, mth)):
                    named += 1
        k += 1
        if k > 300:
            break
    return named


def label_ea(lab):
    m = re.match(r"off_([0-9A-F]+)$", lab)
    if m:
        return int(m.group(1), 16)
    return idc.get_name_ea_simple(lab)


fac = decomp(factory_ea)
pairs = re.findall(r'\("([A-Za-z0-9_]+)"\);[^;]*;\s*\}\s*if \( v\d+ == v\d+ \)\s*return (sub_[0-9A-F]+|\w+);', fac)
log("factory pairs:", len(pairs))
rename(factory_ea, "GLGetLogicFn", force=True)

total_named = 0
for ent, creator in pairs:
    cea = idc.get_name_ea_simple(creator)
    if cea == idaapi.BADADDR:
        continue
    rename(cea, "GLInstanciate" + ent)
    c = decomp(cea)
    PRIMARY = r"\*(?:\(_DWORD \*\))?(?:v\d+|a1|result|this) = (?:\([^)]*\))?&?(off_[0-9A-F]+|vtbl_\w+);"
    THISSTORE = r"\*(?:\(_DWORD \*\))?(?:a1|this) = (?:\([^)]*\))?&?(off_[0-9A-F]+|vtbl_\w+);"
    SECONDARY = r"\*\(_DWORD \*\)\((?:v\d+|a1|result|this) \+ (\d+)\) = (?:\([^)]*\))?&?(off_[0-9A-F]+|vtbl_\w+);"
    stores = re.findall(PRIMARY, c)
    secs = re.findall(SECONDARY, c)
    calls = [mm.group(1) for mm in re.finditer(r"(sub_[0-9A-F]+)\((?:\([^)]*\))?(?:v\d+|result)", c)]
    vt = idaapi.BADADDR
    if stores:
        vt = label_ea(stores[-1])
    elif calls:
        ctor = idc.get_name_ea_simple(calls[0])
        if ctor != idaapi.BADADDR:
            cc = decomp(ctor)
            cstores = re.findall(THISSTORE, cc) or re.findall(PRIMARY, cc)
            secs = re.findall(SECONDARY, cc)
            if cstores:
                rename(ctor, "%s::%s" % (ent, ent))
                vt = label_ea(cstores[-1])
            else:
                log("no vtable store in ctor of", ent, hex(ctor))
    for off, lab in secs:
        sea = label_ea(lab)
        if sea != idaapi.BADADDR:
            rename(sea, "vtbl_%s_plus%s" % (ent, off))
    if vt == idaapi.BADADDR:
        log("no vtable for", ent, "calls", calls[:4], "stores", stores[:3], "text", c[:200].replace("\n", " "))
        continue
    rename(vt, "vtbl_" + ent)
    total_named += name_vtable(vt, ent)

log("total slot names:", total_named)
report.close()
print("done gl_crossname", report_path)
idc.qexit(0)
