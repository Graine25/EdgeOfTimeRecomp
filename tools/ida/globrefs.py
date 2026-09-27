import idc, idautils, ida_funcs, ida_name, ida_segment, ida_bytes, ida_auto

ida_auto.auto_wait()
argv = idc.ARGV
out = argv[1]
targets = {}
disps = []
for tok in argv[2].split(","):
    if tok.startswith("disp:"):
        disps.append(int(tok[5:], 16))
        continue
    ea = int(tok, 16) if tok.startswith("0x") else idc.get_name_ea_simple(tok)
    targets[ea] = tok

def dn(ea):
    f = ida_funcs.get_func(ea)
    if not f:
        return "-"
    n = ida_funcs.get_func_name(f.start_ea)
    return ida_name.demangle_name(n, idc.get_inf_attr(idc.INF_SHORT_DEMNAMES)) or n

OPS = {14: "addi", 32: "lwz", 33: "lwzu", 34: "lbz", 36: "stw", 38: "stb", 40: "lhz", 44: "sth", 48: "lfs", 52: "stfs", 50: "lfd", 54: "stfd"}
hits = {ea: [] for ea in targets}
dh = {d: [] for d in disps}
for i in range(ida_segment.get_segm_qty()):
    seg = ida_segment.getnseg(i)
    if not (seg.perm & 1) and ida_segment.get_segm_name(seg) not in (".text",):
        continue
    data = ida_bytes.get_bytes(seg.start_ea, seg.end_ea - seg.start_ea) or b""
    lis = {}
    for off in range(0, len(data) - 3, 4):
        w = int.from_bytes(data[off:off + 4], "big")
        op = w >> 26
        rd = (w >> 21) & 31
        ra = (w >> 16) & 31
        imm = w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        ea = seg.start_ea + off
        if op == 15 and ra == 0:
            lis[rd] = ((imm << 16) & 0xFFFFFFFF, ea, False)
            continue
        if op in OPS:
            if ra in lis and ea - lis[ra][1] < 0x100:
                addr = (lis[ra][0] + simm) & 0xFFFFFFFF
                for t in targets:
                    if t <= addr < t + 4:
                        hits[t].append((ea, OPS[op] if not lis[ra][2] else OPS[op] + "+"))
            if op == 14 and ra in lis and not lis[ra][2]:
                lis[rd] = ((lis[ra][0] + simm) & 0xFFFFFFFF, ea, True)
                continue
            if simm in dh and op in (32, 36):
                dh[simm].append((ea, OPS[op]))
            if op in (14, 32, 33, 34, 40):
                lis.pop(rd, None)
        elif op in (24, 25, 28, 29):
            lis.pop(ra, None)
        if op == 18 or (op == 19):
            if op == 18 and (w & 1):
                lis = {}
with open(out, "w", encoding="utf-8") as f:
    for t, name in targets.items():
        f.write("\n// ===== %s %#x : %d refs =====\n" % (name, t, len(hits[t])))
        for ea, o in hits[t]:
            f.write("%#x %-5s %s\n" % (ea, o, dn(ea)))
    for d in disps:
        byfn = {}
        for ea, o in dh[d]:
            byfn.setdefault(dn(ea), []).append(o)
        f.write("\n// ===== disp %#x : %d =====\n" % (d, len(dh[d])))
        for k, v in sorted(byfn.items()):
            f.write("%s  %s\n" % (k, " ".join(v)))
print("done globrefs", out)
idc.qexit(0)
