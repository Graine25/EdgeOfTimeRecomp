import csv, re, sys
import ida_auto, ida_funcs, ida_hexrays, ida_name, idc
ida_auto.auto_wait()
src, out = idc.ARGV[1], idc.ARGV[2]
eas = {}
for r in csv.reader(open(src, encoding="utf-8")):
    if len(r) >= 9 and r[3]:
        eas[int(r[3], 16)] = r[4]
INC = re.compile(r"\*\(_(?:DWORD|WORD|BYTE) \*\)\([a-z0-9]+ \+ \d+\)\s*(\+\+|--|[+-]= 1\b)|\+\+\*\(_(?:DWORD|WORD|BYTE) \*\)|--\*\(_(?:DWORD|WORD|BYTE) \*\)|\+\+[a-z0-9]+\[\d+\]|--[a-z0-9]+\[\d+\]|[a-z0-9]+\[\d+\](\+\+|--)")
FINC = re.compile(r"\*\(float \*\)\([a-z0-9]+ \+ \d+\)\s*[+-]= [0-9.]+;|\*\(float \*\)\([a-z0-9]+ \+ \d+\) = \(float\)\(\*\(float \*\)\([a-z0-9]+ \+ \d+\) [+-] [0-9.]+\)")
with open(out, "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    w.writerow(["ea", "name", "size", "dt_used", "int_counters", "float_consts", "GetDeltaTime", "GetFrameCount", "SetUpdateFrequency", "TimeScale", "SetNextState"])
    for ea in sorted(eas):
        fn = ida_funcs.get_func(ea)
        if not fn or fn.end_ea - fn.start_ea <= 8:
            continue
        try:
            cf = ida_hexrays.decompile(ea, None, ida_hexrays.DECOMP_NO_CACHE)
        except Exception:
            continue
        if not cf:
            continue
        txt = str(cf)
        head = txt.split("\n", 2)[0] if txt.startswith("//") else txt.split("{", 1)[0]
        proto = txt.split("{", 1)[0]
        dt = bool(re.search(r"(double|float) a\d", proto))
        body = txt.split("{", 1)[1] if "{" in txt else txt
        w.writerow(["%#x" % ea, ida_funcs.get_func_name(ea), fn.end_ea - fn.start_ea, int(dt),
                    len(INC.findall(body)), len(FINC.findall(body)),
                    body.count("GetDeltaTime"), body.count("GetFrameCount"), body.count("SetUpdateFrequency"),
                    body.count("TimeScale"), body.count("SetNextState")])
print("done")
idc.qexit(0)
