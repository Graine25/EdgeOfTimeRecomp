import collections
import csv
import re
import sys

src3, src360, out = sys.argv[1], sys.argv[2], sys.argv[3]
iterations = int(sys.argv[4]) if len(sys.argv) > 4 else 6
BAND_HI = float(sys.argv[5]) if len(sys.argv) > 5 else 2.4

SKIP3 = re.compile(r"^(_|__|j_|nn::|std::|operator|\?|sub_|nullsub|unk_|loc_|\.|\$)")
PLACEHOLDER = re.compile(r"^(sub_|nullsub_|GameLogic_\d+|loc_)")
HELPER = re.compile(r"^(__save|__rest|__aeabi|_RtlCheckStack|__chkstk|_savegpr|_restgpr|__u64|__i64|__ll|__dtoi|__itod|__ftoi|__itof)")


def key(name):
    return name.split("(")[0].strip()


def load(path):
    rows = {}
    for r in csv.reader(open(path, encoding="utf-8")):
        if len(r) < 4:
            continue
        ea, name, size, seq = r[0], r[1], int(r[2]), r[3]
        calls = [key(s) for s in seq.split("|")] if seq else []
        calls = [c for c in calls if not HELPER.match(c)]
        refs = [key(s) for s in r[4].split("|")] if len(r) > 4 and r[4] else []
        refs = [x for i, x in enumerate(refs) if i == 0 or refs[i - 1] != x]
        rows[ea] = (name, size, calls, refs)
    return rows


d3 = load(src3)
d360 = load(src360)
by_name3 = {}
for ea, (name, size, seq, refs) in d3.items():
    by_name3.setdefault(key(name), []).append(ea)

name360 = {ea: v[0] for ea, v in d360.items()}


ea_by_name360 = {v[0]: ea for ea, v in d360.items()}
size360 = {ea: v[1] for ea, v in d360.items()}
size3 = {key(v[0]): v[1] for ea, v in d3.items()}

RATIO = 1.35


def align_by_size(ga, gb):
    import math
    def cost(a, b):
        sa = size360.get(ea_by_name360.get(a, ""), 0)
        sb = size3.get(b, 0)
        if sa <= 16 or sb <= 16:
            return 1.0
        return abs(math.log(sa / (RATIO * sb)))
    la, lb = len(ga), len(gb)
    GAP = 0.6
    dp = [[0.0] * (lb + 1) for _ in range(la + 1)]
    for i in range(1, la + 1):
        dp[i][0] = i * GAP
    for j in range(1, lb + 1):
        dp[0][j] = j * GAP
    for i in range(1, la + 1):
        for j in range(1, lb + 1):
            dp[i][j] = min(dp[i - 1][j - 1] + cost(ga[i - 1], gb[j - 1]), dp[i - 1][j] + GAP, dp[i][j - 1] + GAP)
    out = []
    i, j = la, lb
    while i > 0 and j > 0:
        c = cost(ga[i - 1], gb[j - 1])
        if abs(dp[i][j] - (dp[i - 1][j - 1] + c)) < 1e-9:
            if c < 0.4:
                out.append((ga[i - 1], gb[j - 1]))
            i -= 1
            j -= 1
        elif abs(dp[i][j] - (dp[i - 1][j] + GAP)) < 1e-9:
            i -= 1
        else:
            j -= 1
    return out


accepted = {}
for it in range(iterations):
    votes = collections.defaultdict(collections.Counter)
    pairs = 0
    def vote_gaps(A, B):
        la, lb = len(A), len(B)
        dp = [[0] * (lb + 1) for _ in range(la + 1)]
        for x in range(la - 1, -1, -1):
            for y in range(lb - 1, -1, -1):
                if A[x] == B[y] and not PLACEHOLDER.match(A[x]):
                    dp[x][y] = dp[x + 1][y + 1] + 1
                else:
                    dp[x][y] = max(dp[x + 1][y], dp[x][y + 1])
        anchors = [(-1, -1)]
        x = y = 0
        while x < la and y < lb:
            if A[x] == B[y] and not PLACEHOLDER.match(A[x]):
                anchors.append((x, y))
                x += 1
                y += 1
            elif dp[x + 1][y] >= dp[x][y + 1]:
                x += 1
            else:
                y += 1
        anchors.append((la, lb))
        for (x0, y0), (x1, y1) in zip(anchors, anchors[1:]):
            ga, gb = A[x0 + 1:x1], B[y0 + 1:y1]
            if not ga or not gb:
                continue
            if len(ga) == len(gb):
                pairs_ = list(zip(ga, gb))
            elif len(ga) <= 10 and len(gb) <= 10:
                pairs_ = align_by_size(ga, gb)
            else:
                continue
            for c360, c3 in pairs_:
                if not PLACEHOLDER.match(c360) or SKIP3.match(c3) or "::" not in c3 and not re.match(r"^[A-Z]", c3):
                    continue
                cea = ea_by_name360.get(c360)
                if cea and PLACEHOLDER.match(name360[cea]):
                    votes[cea][c3] += 1

    for ea, (name, size, seq, refs) in d360.items():
        cur = name360[ea]
        if PLACEHOLDER.match(cur) or cur not in by_name3 or len(by_name3[cur]) != 1:
            continue
        ea3 = by_name3[cur][0]
        seq3, refs3 = d3[ea3][2], d3[ea3][3]
        s360 = [name360.get(ea_by_name360.get(c, ""), c) for c in seq]
        r360 = [name360.get(ea_by_name360.get(c, ""), c) for c in refs]
        if seq3 and s360:
            pairs += 1
            vote_gaps(s360, seq3)
        if refs3 and r360:
            vote_gaps(r360, refs3)
    new = 0
    for cea, cnt in votes.items():
        nm, n = cnt.most_common(1)[0]
        if len(cnt) == 1 or (n >= 4 and n >= 0.75 * sum(cnt.values())):
            sa, sb = size360.get(cea, 0), size3.get(nm, 0)
            if sa > 40 and sb > 40 and not (0.8 <= sa / sb <= BAND_HI):
                continue
            if nm not in name360.values():
                name360[cea] = nm
                accepted[cea] = nm
                new += 1
    print("iteration", it, "pairs", pairs, "new", new)
    if not new:
        break

callers3 = collections.defaultdict(set)
callers360 = collections.defaultdict(set)
for ea, (nm, sz, seq, refs) in d3.items():
    for c in seq:
        callers3[c].add(key(nm))
for ea, (nm, sz, seq, refs) in d360.items():
    for c in seq:
        cea = ea_by_name360.get(c)
        callers360[cea if cea else c].add(name360[ea])
idx3 = collections.defaultdict(list)
for fn3, cs in callers3.items():
    idx3[frozenset(cs)].append(fn3)
taken = set(name360.values())
for cea, cs in callers360.items():
    if not isinstance(cea, str) or not PLACEHOLDER.match(name360.get(cea, "sub_")):
        continue
    if len(cs) < 2 or any(PLACEHOLDER.match(c) for c in cs):
        continue
    cand = [c for c in idx3.get(frozenset(cs), []) if c not in taken and ("::" in c or re.match(r"^[A-Z]", c))]
    if len(cand) != 1:
        continue
    sa, sb = size360.get(cea, 0), size3.get(cand[0], 0)
    if sa > 40 and sb > 40 and not (0.8 <= sa / sb <= BAND_HI):
        continue
    name360[cea] = cand[0]
    accepted[cea] = cand[0]
    taken.add(cand[0])
print("with caller-set matches:", len(accepted))

callees3 = {key(v[0]): set(v[2]) for ea, v in d3.items()}
for ea, (nm, sz, seq, refs) in d360.items():
    cur = name360[ea]
    if PLACEHOLDER.match(cur) or cur not in by_name3 or len(by_name3[cur]) != 1:
        continue
    g3 = [c for c in callees3.get(cur, ()) if len(callers3.get(c, ())) == 1 and not SKIP3.match(c) and c not in taken and ("::" in c or re.match(r"^[A-Z]", c))]
    u = [ea_by_name360.get(c) for c in seq]
    u = [x for x in set(u) if x and PLACEHOLDER.match(name360[x]) and len(callers360.get(x, ())) == 1]
    if len(g3) == 1 and len(u) == 1:
        sa, sb = size360.get(u[0], 0), size3.get(g3[0], 0)
        if sa > 40 and sb > 40 and not (0.8 <= sa / sb <= BAND_HI):
            continue
        name360[u[0]] = g3[0]
        accepted[u[0]] = g3[0]
        taken.add(g3[0])
print("with private-callee matches:", len(accepted))

with open(out, "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    for cea, nm in sorted(accepted.items()):
        w.writerow([cea, nm])
print("proposed", len(accepted))
