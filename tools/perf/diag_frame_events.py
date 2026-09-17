import re, sys
path, tag = sys.argv[1], sys.argv[2]
lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
start = next(i for i,l in enumerate(lines) if "[diag] scene reached at " + tag in l)
end = next((i for i,l in enumerate(lines[start+1:], start+1) if "[diag] scene reached" in l), len(lines))
block = lines[start:end]
gpu = {}
for l in block:
    m = re.search(r"\[diag-gpu\] (.*?) ([\d.]+) us$", l)
    if m: gpu[m.group(1)] = float(m.group(2))
names = {}
for l in block:
    m = re.search(r"\[surfaces\] (0x[0-9a-f]+): (color|depth) (\d+)x(\d+) fmt=(\d+) msaa=\d+ tile=(\d+)", l)
    if m: names[m.group(1)] = "%s %sx%s f%s t%s" % (m.group(2), m.group(3), m.group(4), m.group(5), m.group(6))
ev = []
for l in block:
    m = re.search(r"\[diag\] draw (\d+) prim (\d+) (\w+)( rect)? smp=(\d+)( add)?( dtwin)?.*? rt0=(0x[0-9a-f]+) (\d+)x(\d+) fmt(\d+) .*? ds=(0x[0-9a-f]+) vs=([0-9a-f]+) ps=([0-9a-f]+) .*?z=(\w+) func(\d+) blend=(\w+) mask=(0x[0-9a-f]+) spec=(0x[0-9a-f]+)", l)
    if m:
        k = m.group(1)
        key = "rt0=%s %sx%s f%s smp%s%s%s ds=%s ps=%s z=%s f%s blend=%s mask=%s spec=%s" % (m.group(8), m.group(9), m.group(10), m.group(11), m.group(5), m.group(6) or "", m.group(7) or "", m.group(12), m.group(14)[:8], m.group(15), m.group(16), m.group(17), m.group(18), m.group(19))
        ev.append(("draw", key, gpu.get("draw " + k, 0.0), k)); continue
    m = re.search(r"\[diag\]   rect verts \(next draw\)(.*)", l)
    if m: ev.append(("verts", m.group(1).strip()[:200], 0.0, "")); continue
    m = re.search(r"\[diag\] ((?:derive|propagate|broadcast|resolve host|hand|take back|redirect|clear depth twin)[^\(]*)\(", l)
    if m:
        t = m.group(1).strip(); ev.append(("xfer", t, gpu.get(t, 0.0), "")); continue
    m = re.search(r"\[diag\] clear flags (0x[0-9a-f]+) (\w+) rt0=(0x[0-9a-f]+) ds=(0x[0-9a-f]+)", l)
    if m: ev.append(("clear", "clear %s %s rt0=%s ds=%s" % m.groups(), 0.0, "")); continue
    m = re.search(r"\[resolve\] (color|depth) (0x[0-9a-f]+) -> tex (0x[0-9a-f]+) mip (\d+) rect (\S+) at (\S+)", l)
    if m: ev.append(("resolve", "resolve %s %s -> tex %s mip %s rect %s at %s" % m.groups(), 0.0, "")); continue
    m = re.search(r"\[diag-gpu\] (resolve (?:copy|blit) .*?|present|.*twin.*?|take back.*?) ([\d.]+) us$", l)
    if m and not any(e[1] == m.group(1) for e in ev[-3:]): ev.append(("gpu", m.group(1), float(m.group(2)), ""))
print("frame block lines", len(block), "gpu items", len(gpu))
for va, n in sorted(names.items()): print("  ", va, n)
run = None
def flush():
    global run
    if run: print("  %4d draws %7.0f us  #%s  %s" % (run[1], run[2], run[3], run[0]))
    run = None
for kind, key, us, idx in ev:
    if kind == "draw":
        if run and run[0] == key: run[1] += 1; run[2] += us
        else: flush(); run = [key, 1, us, idx]
    else:
        flush(); print("  ---- %-70s %7.0f us" % (key, us))
flush()
