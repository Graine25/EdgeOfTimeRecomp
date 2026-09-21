import csv
import re
import sys
from collections import defaultdict

src3, src360, out = sys.argv[1], sys.argv[2], sys.argv[3]
rep_path = sys.argv[4] if len(sys.argv) > 4 else None
PLACEHOLDER = re.compile(r"^(sub_|helper_|nullsub_|j_sub_|loc_|GameLogic_\d+|unk_|off_|dword_|byte_|word_|flt_)")
HELPER = re.compile(r"^(__save|__rest|__aeabi|_RtlCheckStack|__chkstk|_savegpr|_restgpr|memcpy|memset)")


def key(name):
    return name.split("(")[0].strip()


def load(path):
    rows = {}
    for r in csv.reader(open(path, encoding="utf-8")):
        if len(r) < 4:
            continue
        ea = int(r[0], 16)
        name = key(r[1])
        calls = [key(c) for c in r[3].split("|") if c] if r[3] else []
        refs = [key(c) for c in r[4].split("|") if c] if len(r) > 4 and r[4] else []
        bag = {c for c in calls + refs if not PLACEHOLDER.match(c) and not HELPER.match(c)}
        rows[ea] = (name, int(r[2]), bag)
    return rows


d3, d360 = load(src3), load(src360)
names360 = {v[0] for v in d360.values()}
by_class3 = defaultdict(list)
for ea, (name, size, bag) in d3.items():
    if "::" in name and not PLACEHOLDER.match(name):
        by_class3[name.split("::")[0]].append(ea)


def compatible(a, b):
    if a <= 40 and b <= 40:
        return True
    if a <= 40 or b <= 40:
        return False
    return 0.7 <= a / b <= 2.4


proposals = {}
claims = defaultdict(list)
for ea, (name, size, bag) in d360.items():
    m = re.match(r"^helper_[0-9A-F]+_in_(\w+)$", name)
    if not m or len(bag) < 2:
        continue
    cls = m.group(1)
    scored = []
    for ea3 in by_class3.get(cls, []):
        n3, s3, b3 = d3[ea3]
        if n3 in names360 or len(b3) < 2 or not compatible(size, s3):
            continue
        j = len(bag & b3) / len(bag | b3)
        scored.append((j, ea3, n3))
    scored.sort(reverse=True)
    if scored and scored[0][0] >= 0.6 and (len(scored) == 1 or scored[0][0] - scored[1][0] >= 0.25):
        proposals[ea] = (scored[0][2], scored[0][0], scored[1][0] if len(scored) > 1 else 0.0)
        claims[scored[0][2]].append(ea)

final = {ea: v for ea, v in proposals.items() if len(claims[v[0]]) == 1}
with open(out, "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    for ea in sorted(final):
        w.writerow(["%#x" % ea, final[ea][0]])
print("candidates", len(proposals), "accepted", len(final))
if rep_path:
    with open(rep_path, "w", encoding="utf-8") as f:
        for ea in sorted(final):
            n3, sc, ru = final[ea]
            f.write("%#x %s -> %s  (%.2f / %.2f)\n" % (ea, d360[ea][0], n3, sc, ru))
        f.write("\n# contested\n")
        for n3, eas in claims.items():
            if len(eas) > 1:
                f.write("%s <- %s\n" % (n3, ", ".join("%#x" % e for e in eas)))
