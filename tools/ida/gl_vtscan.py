import re

import ida_auto
import ida_funcs
import ida_segment
import idaapi
import idautils
import idc

ida_auto.auto_wait()
vtraw_path, report_path = idc.ARGV[1], idc.ARGV[2]
SN = idc.SN_NOWARN | 0x800
report = open(report_path, "w", encoding="utf-8")


def log(*a):
    report.write(" ".join(str(x) for x in a) + "\n")


PLACEHOLDER = ("sub_", "helper_", "nullsub_", "off_", "unk_", "dword_", "byte_", "word_", "GameLogic_", "loc_")


def rename(ea, name):
    cur = idc.get_name(ea) or ""
    fn = ida_funcs.get_func(ea)
    if fn and (cur.startswith("nullsub_") or fn.end_ea - fn.start_ea <= 8):
        return False
    if cur and not cur.startswith(PLACEHOLDER):
        return False
    ok = idc.set_name(ea, name, SN) or idc.set_name(ea, "%s_%X" % (name, ea & 0xFFFF), SN)
    log("%s %s -> %s %s" % (hex(ea), cur, name, "OK" if ok else "FAIL"))
    return bool(ok)


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
    idx, name, label = int(m.group(1)), m.group(3).strip(), (m.group(4) or "").strip()
    if idx >= 2 and label.startswith("`vtable for'"):
        cls = None
        continue
    if idx >= 2:
        vt3ds[cls].append(name)


def method_of(entry):
    if entry in ("-", "__cxa_pure_virtual") or entry.startswith("`non-virtual thunk"):
        return None
    m = re.match(r"([A-Za-z_][\w:<>,]*?)::(~?[A-Za-z_]\w*)\(", entry)
    return (m.group(1), m.group(2)) if m else None


def slot_name3(cls_name, k):
    t = vt3ds.get(cls_name)
    if not t:
        return None
    j = 0 if k == 0 else k + 1
    if j >= len(t):
        return None
    mo = method_of(t[j])
    if mo and k == 0:
        return (mo[0], "DeletingDtor")
    return mo


exec_segs = []
data_segs = []
for i in range(ida_segment.get_segm_qty()):
    s = ida_segment.getnseg(i)
    (exec_segs if s.perm & idaapi.SEGPERM_EXEC else data_segs).append((s.start_ea, s.end_ea))


def is_func_start(v):
    f = ida_funcs.get_func(v)
    return f is not None and f.start_ea == v


tables = []
for lo, hi in data_segs:
    ea = lo
    run = []
    while ea + 4 <= hi:
        v = idc.get_wide_dword(ea)
        lab = idc.get_name(ea) or ""
        boundary = lab and not lab.startswith(("off_", "unk_", "dword_"))
        if is_func_start(v) and not (boundary and run):
            run.append((ea, v))
        else:
            if len(run) >= 3:
                tables.append(run)
            run = [(ea, v)] if is_func_start(v) else []
        ea += 4
    if len(run) >= 3:
        tables.append(run)
split = []
for run in tables:
    start = 0
    for k, (p, v) in enumerate(run):
        if k > start and (idc.get_name(v) or "").endswith("::DeletingDtor") and idc.get_name(p).startswith("vtbl_"):
            split.append(run[start:k])
            start = k
    split.append(run[start:])
tables = [r for r in split if len(r) >= 3]
log("candidate tables:", len(tables))

named_total = 0
owned = 0
for run in tables:
    vt_ea = run[0][0]
    cur_lab = idc.get_name(vt_ea) or ""
    cands = {}
    for k, (p, v) in enumerate(run):
        n = idc.get_name(v) or ""
        if "::" in n and not n.startswith(PLACEHOLDER):
            cands[n.split("::")[0]] = 1
    if cur_lab.startswith("vtbl_"):
        cands[cur_lab[5:].split("_plus")[0]] = 1
    best, best_score = None, 0
    slot0 = idc.get_name(run[0][1]) or ""
    dtor_cls = slot0[:-len("::DeletingDtor")] if slot0.endswith("::DeletingDtor") else None
    label_cls = cur_lab[5:] if cur_lab.startswith("vtbl_") and "_plus" not in cur_lab else None
    for c in cands:
        if c not in vt3ds:
            continue
        agree = disagree = 0
        for k, (p, v) in enumerate(run):
            n = idc.get_name(v) or ""
            if not ("::" in n and not n.startswith(PLACEHOLDER)):
                continue
            s3 = slot_name3(c, k)
            if s3 is None:
                continue
            if n.split("::", 1)[1].split("_")[0] == s3[1] or n == "%s::%s" % s3:
                agree += 1
            else:
                disagree += 1
        if c == label_cls or (c == dtor_cls and disagree == 0):
            if disagree:
                log("%s %s: %d named slots disagree with %s" % (hex(vt_ea), cur_lab, disagree, c))
            best, best_score = c, 1000
            break
        if agree >= 2 and disagree == 0 and agree > best_score:
            best, best_score = c, agree
    if not best and label_cls and label_cls not in vt3ds:
        base, base_len = None, 0
        for c in cands:
            if c not in vt3ds or c == label_cls:
                continue
            agree = disagree = 0
            for k, (p, v) in enumerate(run):
                n = idc.get_name(v) or ""
                if not ("::" in n and not n.startswith(PLACEHOLDER)):
                    continue
                s3 = slot_name3(c, k)
                if s3 is None:
                    continue
                if n.split("::", 1)[1].split("_")[0] == s3[1] or n == "%s::%s" % s3:
                    agree += 1
                else:
                    disagree += 1
            if agree >= 1 and disagree == 0 and len(vt3ds[c]) > base_len:
                base, base_len = c, len(vt3ds[c])
        if base:
            owned += 1
            log("%s %s: 360-only class, slots from base %s" % (hex(vt_ea), cur_lab, base))
            for k, (p, v) in enumerate(run):
                n = idc.get_name(v) or ""
                if "::" in n and not n.startswith(PLACEHOLDER):
                    continue
                s3 = slot_name3(base, k)
                if s3 and rename(v, "%s::%s" % (label_cls, s3[1])):
                    named_total += 1
        continue
    if not best:
        continue
    owned += 1
    if not cur_lab.startswith("vtbl_"):
        rename(vt_ea, "vtbl_" + best)
    for k, (p, v) in enumerate(run):
        n = idc.get_name(v) or ""
        if "::" in n and not n.startswith(PLACEHOLDER):
            continue
        s3 = slot_name3(best, k)
        if not s3:
            continue
        c, mth = s3
        nrefs = sum(1 for _ in idautils.XrefsTo(v))
        if nrefs <= 1 and c != best:
            c = best
        if rename(v, "%s::%s" % (c, mth)):
            named_total += 1
holders = {}
held = {}
for run in tables:
    vt_ea = run[0][0]
    lab = idc.get_name(vt_ea) or ""
    cls = lab[5:] if lab.startswith("vtbl_") and "_plus" not in lab else None
    s0 = idc.get_name(run[0][1]) or ""
    if not cls and s0.endswith("::DeletingDtor"):
        cls = s0[:-len("::DeletingDtor")]
    if not cls or cls not in vt3ds:
        continue
    for k, (p, v) in enumerate(run):
        s3 = slot_name3(cls, k)
        if s3:
            holders.setdefault(v, {})[cls] = s3[1]
            held[(cls, s3[1])] = v
rehomed = 0
for v, byc in holders.items():
    cur = idc.get_name(v) or ""
    if "::" not in cur or cur.startswith(PLACEHOLDER) or cur.startswith("nullsub"):
        continue
    ccls, cm = cur.split("::", 1)
    cm = re.sub(r"_\d+$", "", cm)
    methods = set(byc.values())
    if len(methods) != 1 or cm not in methods or ccls in byc or len(byc) != 1:
        continue
    fn = ida_funcs.get_func(v)
    if fn is None or fn.end_ea - fn.start_ea <= 8:
        continue
    x = next(iter(byc))
    if not re.search(r"_\d+$", cur.split("::", 1)[1]):
        other = held.get((ccls, cm))
        if other is None or other == v:
            continue
    new = "%s::%s" % (x, cm)
    ok = idc.set_name(v, new, SN) or idc.set_name(v, "%s_%X" % (new, v & 0xFFFF), SN)
    log("%s %s -> %s (override, tables: %s) %s" % (hex(v), cur, new, ",".join(sorted(byc)), "OK" if ok else "FAIL"))
    rehomed += ok
log("tables owned:", owned, "slots named:", named_total, "overrides re-homed:", rehomed)
report.close()
print("done gl_vtscan", owned, named_total)
idc.qexit(0)
