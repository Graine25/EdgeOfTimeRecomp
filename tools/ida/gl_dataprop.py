import collections
import csv
import re
import sys

csv.field_size_limit(1 << 30)
src3, src360, out = sys.argv[1], sys.argv[2], sys.argv[3]
PLACE360 = re.compile(r"^@(dword_|unk_|byte_|word_|off_|flt_|qword_|dbl_|stru_|xmmword_|asc_)")
GOOD3 = re.compile(r"^@([A-Z][A-Za-z0-9_]*(::[A-Za-z0-9_~]+)+|g_[A-Za-z0-9_]+|s_[A-Za-z0-9_]+|sm[A-Z][A-Za-z0-9_]*|g[a-z]{0,3}[A-Z][A-Za-z0-9_]*)$")
STRING = re.compile(r"^@a[A-Z0-9]")


def key(name):
    return name.split("(")[0].strip()


def load(path):
    rows = {}
    for r in csv.reader(open(path, encoding="utf-8")):
        if len(r) < 4:
            continue
        toks = [key(t) if not t.startswith("@") else t for t in r[3].split("|")] if r[3] else []
        rows[key(r[1])] = toks
    return rows


d3 = load(src3)
d360 = load(src360)
votes = collections.defaultdict(collections.Counter)


def vote(p360, n3, w=1):
    votes[p360][n3] += w


def lcs_anchors(A, B):
    la, lb = len(A), len(B)
    dp = [[0] * (lb + 1) for _ in range(la + 1)]
    for x in range(la - 1, -1, -1):
        for y in range(lb - 1, -1, -1):
            if A[x] == B[y] and not PLACE360.match(A[x]):
                dp[x][y] = dp[x + 1][y + 1] + 1
            else:
                dp[x][y] = max(dp[x + 1][y], dp[x][y + 1])
    anchors = [(-1, -1)]
    x = y = 0
    while x < la and y < lb:
        if A[x] == B[y] and not PLACE360.match(A[x]):
            anchors.append((x, y))
            x += 1
            y += 1
        elif dp[x + 1][y] >= dp[x][y + 1]:
            x += 1
        else:
            y += 1
    anchors.append((la, lb))
    return anchors


pairs = 0
for name, t360 in d360.items():
    t3 = d3.get(name)
    if not t3:
        continue
    u360 = [t for t in t360 if PLACE360.match(t)]
    n3 = [t for t in t3 if GOOD3.match(t)]
    if not u360 or not n3:
        continue
    pairs += 1
    c360, c3 = collections.Counter(u360), collections.Counter(n3)
    if len(c360) == 1 and len(c3) == 1:
        vote(next(iter(c360)), next(iter(c3)), 2)
    elif len(c360) == len(c3) and len(set(c360.values())) == len(c360) and len(set(c3.values())) == len(c3):
        for (a, _), (b, _) in zip(c360.most_common(), c3.most_common()):
            vote(a, b)
    anchors = lcs_anchors(t360, t3)
    for (x0, y0), (x1, y1) in zip(anchors, anchors[1:]):
        ga = [t for t in t360[x0 + 1:x1] if PLACE360.match(t)]
        gb = [t for t in t3[y0 + 1:y1] if t.startswith("@")]
        if ga and len(ga) == len(gb) and len(ga) <= 6:
            for a, b in zip(ga, gb):
                if GOOD3.match(b):
                    vote(a, b)

accepted = {}
by_name = collections.defaultdict(list)
for p360, cnt in votes.items():
    top, n = cnt.most_common(1)[0]
    if len(cnt) == 1 or (n >= 4 and n >= 0.75 * sum(cnt.values())):
        accepted[p360] = top
        by_name[top].append((n, p360))
dropped = set()
for nm, cands in by_name.items():
    if len(cands) > 1:
        cands.sort(reverse=True)
        if cands[0][0] >= 3 and cands[0][0] >= 3 * cands[1][0]:
            dropped.update(p for _, p in cands[1:])
        else:
            dropped.update(p for _, p in cands)
with open(out, "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    for p360, nm in sorted(accepted.items()):
        if p360 in dropped:
            continue
        w.writerow(["0x" + p360.rsplit("_", 1)[-1], nm[1:]])
print("pairs", pairs, "voted", len(votes), "accepted", len(accepted) - len(dropped), "ambiguous names dropped", len(dropped))
