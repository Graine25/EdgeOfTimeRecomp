import bisect
import csv
import math
import re
import sys

src3, src360, out = sys.argv[1], sys.argv[2], sys.argv[3]
PLACEHOLDER = re.compile(r"^(sub_|nullsub_|GameLogic_\d+|loc_)")
SKIP3 = re.compile(r"^(_|__|j_|nn::|std::|operator|\?|sub_|nullsub|unk_|loc_|\.|\$|\`)")
RATIO = 1.35


def key(name):
    return name.split("(")[0].strip()


def load(path):
    rows = []
    for r in csv.DictReader(open(path, encoding="utf-8")):
        if r["kind"] != "func":
            continue
        name = r["demangled"] or r["name"]
        rows.append((int(r["ea"], 16), int(r["size"]), key(name)))
    rows.sort()
    return rows


f3 = load(src3)
f360 = load(src360)
by3 = {}
for ea, sz, nm in f3:
    by3.setdefault(nm, []).append((ea, sz))

pairs = []
for ea, sz, nm in f360:
    if PLACEHOLDER.match(nm) or nm not in by3 or len(by3[nm]) != 1:
        continue
    pairs.append((ea, by3[nm][0][0], nm))
pairs.sort()


def lis(seq):
    tails, prev = [], [None] * len(seq)
    for i, (ea, ea3, nm) in enumerate(seq):
        pos = bisect.bisect_left([seq[t][1] for t in tails], ea3)
        if pos == len(tails):
            tails.append(i)
        else:
            tails[pos] = i
        prev[i] = tails[pos - 1] if pos > 0 else None
    out = []
    i = tails[-1] if tails else None
    while i is not None:
        out.append(seq[i])
        i = prev[i]
    out.reverse()
    return out


by_class = {}
for p in pairs:
    cls = p[2].split("::")[0] if "::" in p[2] else None
    if cls:
        by_class.setdefault(cls, []).append(p)
chains = [lis(v) for v in by_class.values() if len(v) >= 2]
print("pairs", len(pairs), "classes with chains", len(chains), "chain links", sum(len(c) - 1 for c in chains))

used = set(nm for _, _, nm in f360 if not PLACEHOLDER.match(nm))
idx360 = {ea: i for i, (ea, sz, nm) in enumerate(f360)}
idx3 = {ea: i for i, (ea, sz, nm) in enumerate(f3)}


def cost(sa, sb):
    if sa <= 16 or sb <= 16:
        return 1.0
    return abs(math.log(sa / (RATIO * sb)))


def align(ga, gb):
    la, lb = len(ga), len(gb)
    GAP = 0.6
    dp = [[0.0] * (lb + 1) for _ in range(la + 1)]
    for i in range(1, la + 1):
        dp[i][0] = i * GAP
    for j in range(1, lb + 1):
        dp[0][j] = j * GAP
    for i in range(1, la + 1):
        for j in range(1, lb + 1):
            dp[i][j] = min(dp[i - 1][j - 1] + cost(ga[i - 1][1], gb[j - 1][1]), dp[i - 1][j] + GAP, dp[i][j - 1] + GAP)
    res = []
    i, j = la, lb
    while i > 0 and j > 0:
        c = cost(ga[i - 1][1], gb[j - 1][1])
        if abs(dp[i][j] - (dp[i - 1][j - 1] + c)) < 1e-9:
            if c < 0.35:
                res.append((ga[i - 1], gb[j - 1]))
            i -= 1
            j -= 1
        elif abs(dp[i][j] - (dp[i - 1][j] + GAP)) < 1e-9:
            i -= 1
        else:
            j -= 1
    return res


proposals = {}
for chain in chains:
    cls = chain[0][2].split("::")[0]
    for (a360, a3, _), (b360, b3, _) in zip(chain, chain[1:]):
        ga = [(ea, sz, nm) for ea, sz, nm in f360[idx360[a360] + 1:idx360[b360]] if PLACEHOLDER.match(nm)]
        gb = [(ea, sz, nm) for ea, sz, nm in f3[idx3[a3] + 1:idx3[b3]] if not SKIP3.match(nm) and nm not in used and nm.startswith(cls + "::")]
        if not ga or not gb:
            continue
        if len(ga) == len(gb):
            cand = [(x, y) for x, y in zip(ga, gb) if x[1] <= 40 or y[1] <= 40 or 0.7 <= x[1] / y[1] <= 2.6]
        elif len(ga) <= 40 and len(gb) <= 40:
            cand = align(ga, gb)
        else:
            continue
        for (ea, sz, nm), (ea3, sz3, nm3) in cand:
            if nm3 in used or ea in proposals:
                continue
            proposals[ea] = nm3
            used.add(nm3)

with open(out, "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    for ea, nm in sorted(proposals.items()):
        w.writerow(["%#x" % ea, nm])
print("proposed", len(proposals))
