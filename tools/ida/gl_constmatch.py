import csv
import math
import re
import sys
from collections import defaultdict

src3, src360, out = sys.argv[1], sys.argv[2], sys.argv[3]
rep_path = sys.argv[4] if len(sys.argv) > 4 else None
MIN_SHARED = int(sys.argv[5]) if len(sys.argv) > 5 else 2
PLACEHOLDER = re.compile(r"^(sub_|helper_|nullsub_|j_sub_|loc_|GameLogic_\d+|unk_|off_|dword_|byte_|word_|flt_)" + (("|" + sys.argv[6]) if len(sys.argv) > 6 else ""))
SKIP3 = re.compile(r"^(_|__|j_|nn::|std::|operator|\?|sub_|nullsub|unk_|loc_|\.|\$)")


def key(name):
    return name.split("(")[0].strip()


def load(path):
    rows = {}
    for r in csv.reader(open(path, encoding="utf-8")):
        if len(r) < 4:
            continue
        consts = {int(x, 16) for x in r[3].split(";") if x}
        rows[int(r[0], 16)] = (key(r[2]), int(r[1]), consts)
    return rows


d3, d360 = load(src3), load(src360)
df = defaultdict(int)
for _, (_, _, cs) in d3.items():
    for c in cs:
        df[c] += 1
n3 = len(d3)


def weight(c):
    return math.log(1 + n3 / (1 + df.get(c, 0)))


index = defaultdict(list)
for ea, (name, size, cs) in d3.items():
    if SKIP3.match(name):
        continue
    for c in cs:
        index[c].append(ea)
names360 = {v[0] for v in d360.values()}


def compatible(a, b):
    if a <= 40 and b <= 40:
        return True
    if a <= 40 or b <= 40:
        return False
    return 0.6 <= a / b <= 2.6


proposals = {}
claims = defaultdict(list)
for ea, (name, size, cs) in d360.items():
    if not PLACEHOLDER.match(name) or len(cs) < MIN_SHARED:
        continue
    m = re.match(r"^helper_[0-9A-F]+_in_(\w+)$", name)
    tu = m.group(1) if m else None
    cands = set()
    for c in cs:
        if df.get(c, 0) <= 64:
            cands.update(index.get(c, []))
    scored = []
    wa = sum(weight(c) for c in cs)
    for ea3 in cands:
        n3_, s3, c3 = d3[ea3]
        if n3_ in names360 or not compatible(size, s3):
            continue
        shared = cs & c3
        if len(shared) < MIN_SHARED:
            continue
        if len(shared) == 1 and df.get(next(iter(shared)), 0) > 2:
            continue
        ws = sum(weight(c) for c in shared)
        wb = sum(weight(c) for c in c3)
        score = ws / (wa + wb - ws)
        tie = 1 if (tu and n3_.startswith(tu + "::")) else 0
        scored.append((score, tie, ea3, n3_, len(shared)))
    scored.sort(reverse=True)
    if scored and scored[0][0] >= 0.5 and (len(scored) == 1 or scored[0][0] - scored[1][0] >= 0.2 or (scored[0][1] and scored[0][0] > scored[1][0])):
        proposals[ea] = scored[0]
        claims[scored[0][3]].append(ea)

final = {ea: v for ea, v in proposals.items() if len(claims[v[3]]) == 1}
with open(out, "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    for ea in sorted(final):
        w.writerow(["%#x" % ea, final[ea][3]])
print("candidates", len(proposals), "accepted", len(final))
if rep_path:
    with open(rep_path, "w", encoding="utf-8") as f:
        for ea in sorted(final):
            sc, tie, ea3, n, sh = final[ea]
            f.write("%#x %s -> %s  (%.2f, %d shared, sizes %d/%d)\n" % (ea, d360[ea][0], n, sc, sh, d360[ea][1], d3[ea3][1]))
        f.write("\n# contested\n")
        for n, eas in claims.items():
            if len(eas) > 1:
                f.write("%s <- %s\n" % (n, ", ".join("%#x" % e for e in eas)))
