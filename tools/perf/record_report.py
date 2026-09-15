import collections
import re
import sys

BLOCK = re.compile(r"\[record\] frame (\d+): ([0-9.]+) ms of items, (\d+) items")
ROW = re.compile(r"\[record\]   ([0-9.]+) us x(\d+) max ([0-9.]+): (.*?)\s*$")
PERF = re.compile(r"\[perf\] (\d+) frames.*?\((\d+) draws.*?gpu ([0-9.]+) ms/frame: (.*?)\s*$")
CAT = re.compile(r"([a-z0-9+@ -]+?) ([0-9.]+)(?: \((\d+) draws\))?(?= [a-z]|$)")
MARK = re.compile(r"\[record\] (on|off) at guest frame (\d+)")


def main(paths):
    lines = []
    for p in paths:
        with open(p, encoding="utf-8", errors="replace") as f:
            lines.extend(f)
    frames = []
    perfs = []
    recording = None
    cur = None
    for l in lines:
        m = MARK.search(l)
        if m:
            recording = m.group(1) == "on"
            continue
        m = BLOCK.search(l)
        if m:
            cur = dict(total=float(m.group(2)), items=int(m.group(3)), rows=[])
            if cur["items"] >= 200:
                frames.append(cur)
            continue
        m = ROW.search(l)
        if m and cur is not None:
            cur["rows"].append((float(m.group(1)), int(m.group(2)), float(m.group(3)), m.group(4)))
            continue
        m = PERF.search(l)
        if m and int(m.group(2)) >= 300 and (recording or recording is None):
            cats = {}
            for c in CAT.finditer(m.group(4)):
                cats[c.group(1).strip()] = float(c.group(2))
            perfs.append((int(m.group(2)), float(m.group(3)), cats))
    if not frames:
        print("no [record] frames found")
        return
    n = len(frames)
    by_key = collections.defaultdict(lambda: [0.0, 0, 0.0])
    for f in frames:
        for us, cnt, mx, key in f["rows"]:
            a = by_key[key]; a[0] += us; a[1] += cnt; a[2] = max(a[2], mx)
    draws = {k: v for k, v in by_key.items() if k.startswith("draw ")}
    other = {k: v for k, v in by_key.items() if not k.startswith("draw ")}
    print("%d recorded frames, %.2f ms of items per frame (%d items; each draw carries ~5 us of query)" % (
        n, sum(f["total"] for f in frames) / n, sum(f["items"] for f in frames) / n))
    if perfs:
        m = len(perfs)
        cats = collections.defaultdict(float)
        for _, _, c in perfs:
            for k, v in c.items(): cats[k] += v
        print("\nuninflated GPU categories from %d [perf] windows (ms/frame, avg %d draws, gpu %.2f ms):" % (
            m, sum(p[0] for p in perfs) / m, sum(p[1] for p in perfs) / m))
        for k, v in sorted(cats.items(), key=lambda kv: -kv[1])[:12]:
            print("  %6.2f  %s" % (v / m, k))
    def table(title, d, limit):
        print("\n%s (us/frame, draws/frame, max single us):" % title)
        for key, (us, cnt, mx) in sorted(d.items(), key=lambda kv: -kv[1][0])[:limit]:
            print("  %8.0f  %7.1f  %7.0f  %s" % (us / n, cnt / n, mx, key))
    by_target = collections.defaultdict(lambda: [0.0, 0, 0.0])
    by_ps = collections.defaultdict(lambda: [0.0, 0, 0.0])
    for key, (us, cnt, mx) in draws.items():
        t = key.split(" ps=")[0]
        a = by_target[t]; a[0] += us; a[1] += cnt; a[2] = max(a[2], mx)
        ps = key.split(" ps=")[1].split()[0] if " ps=" in key else "?"
        a = by_ps["ps=" + ps]; a[0] += us; a[1] += cnt; a[2] = max(a[2], mx)
    table("by target", by_target, 14)
    table("transfers and clears", other, 14)
    table("by pixel shader", by_ps, 12)
    table("by target and shader", draws, 16)


if __name__ == "__main__":
    main(sys.argv[1:])
