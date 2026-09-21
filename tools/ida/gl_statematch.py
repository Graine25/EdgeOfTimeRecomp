import csv
import re
import sys
from collections import defaultdict

s360, s3ds, z360, z3ds, out = sys.argv[1:6]
rep_path = sys.argv[6] if len(sys.argv) > 6 else None
PLACEHOLDER = re.compile(r"^(sub|helper|nullsub|j_sub|loc)_")


def strip_sig(n):
    return n.split("(")[0].strip()


def class_of(name):
    n = strip_sig(name)
    m = re.match(r"^GLInstanciate(\w+)$", n)
    if m:
        return m.group(1)
    if "::" in n:
        return n.split("::")[0]
    return None


def load_states(path):
    rows = defaultdict(dict)
    for r in csv.reader(open(path, encoding="utf-8")):
        if len(r) < 9 or r[2] == "":
            continue
        cls = class_of(r[1])
        if not cls:
            continue
        key = (cls, int(r[2]))
        for role, i in (("update", 3), ("enter", 5), ("exit", 7)):
            if r[i]:
                rows[key][role] = (int(r[i], 16), r[i + 1])
    return rows


def load_sizes(path):
    d = {}
    for r in csv.reader(open(path, encoding="utf-8")):
        try:
            d[int(r[1], 16)] = int(r[2])
        except (ValueError, IndexError):
            pass
    return d


st360, st3 = load_states(s360), load_states(s3ds)
sz360, sz3 = load_sizes(z360), load_sizes(z3ds)

def compatible(a, b):
    if a <= 40 and b <= 40:
        return True
    if a <= 40 or b <= 40:
        return False
    return 0.7 <= a / b <= 2.4


ROLE_PREFIX = {"update": "Update", "enter": "Enter", "exit": "Exit"}

classes = sorted({k[0] for k in st360} & {k[0] for k in st3})
aligned = {}
for cls in classes:
    keys = [k for k in st360 if k[0] == cls]
    both = [k for k in keys if k in st3]
    if not both:
        continue
    slots = good = 0
    pres = 0
    for k in both:
        if set(st360[k]) == set(st3[k]):
            pres += 1
        for role, (ea, cur) in st360[k].items():
            if role in st3[k]:
                slots += 1
                if compatible(sz360.get(ea, 0), sz3.get(st3[k][role][0], 0)):
                    good += 1
    ok = slots >= 1 and (good >= 0.75 * slots if slots >= 4 else good == slots) and pres >= 0.7 * len(both)
    aligned[cls] = (ok, len(both), len(keys), slots, good, pres)

def dp_align(cls):
    a = sorted(k[1] for k in st360 if k[0] == cls)
    b = sorted(k[1] for k in st3 if k[0] == cls)
    if len(a) < 3 or len(b) < 3:
        return {}

    def score(i, j):
        ra, rb = st360[(cls, a[i])], st3[(cls, b[j])]
        if set(ra) != set(rb):
            return -3
        sc = 2
        for role, (ea, cur) in ra.items():
            sc += 1 if compatible(sz360.get(ea, 0), sz3.get(rb[role][0], 0)) else -2
        return sc

    n, m = len(a), len(b)
    best = [[0] * (m + 1) for _ in range(n + 1)]
    back = [[None] * (m + 1) for _ in range(n + 1)]
    for i in range(1, n + 1):
        best[i][0] = -i
        back[i][0] = "u"
    for j in range(1, m + 1):
        best[0][j] = -j
        back[0][j] = "l"
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            d = best[i - 1][j - 1] + score(i - 1, j - 1)
            u = best[i - 1][j] - 1
            l = best[i][j - 1] - 1
            best[i][j] = max(d, u, l)
            back[i][j] = "d" if best[i][j] == d else ("u" if best[i][j] == u else "l")
    pairs = {}
    i, j = n, m
    while i > 0 and j > 0:
        if back[i][j] == "d":
            if score(i - 1, j - 1) >= 2 + len(st360[(cls, a[i - 1])]):
                pairs[(cls, a[i - 1])] = (cls, b[j - 1])
            i, j = i - 1, j - 1
        elif back[i][j] == "u":
            i -= 1
        else:
            j -= 1
    return pairs


dp_pairs = {}
for cls, v in aligned.items():
    if not v[0]:
        dp_pairs.update(dp_align(cls))

names = {}
forced = set()
conflicts = []
skipped = []
for key, roles in st360.items():
    key3 = key if (key in st3 and aligned.get(key[0], (False,))[0]) else dp_pairs.get(key)
    if key3 is None:
        continue
    via_dp = key3 != key or not aligned.get(key[0], (False,))[0]
    for role, (ea, cur) in roles.items():
        if role not in st3[key3]:
            continue
        ea3, n3 = st3[key3][role]
        n3 = strip_sig(n3)
        if not n3 or PLACEHOLDER.match(n3) or n3.startswith(("sub_", "nullsub_")):
            continue
        a, b = sz360.get(ea, 0), sz3.get(ea3, 0)
        if not PLACEHOLDER.match(cur):
            if cur == n3 or cur.startswith("j_"):
                continue
            leaf = cur.split("::")[-1]
            wrong_role = any(leaf.startswith(p) for p in ROLE_PREFIX.values()) and not leaf.startswith(ROLE_PREFIX[role])
            if wrong_role and compatible(a, b) and a > 8 and not via_dp:
                names[ea] = n3
                forced.add(ea)
            else:
                conflicts.append((key, role, ea, cur, n3))
            continue
        if a <= 8 or b <= 8:
            skipped.append((key, role, ea, n3, "tiny", a, b))
            continue
        if not compatible(a, b):
            skipped.append((key, role, ea, n3, "size", a, b))
            continue
        if ea in names and names[ea] != n3:
            skipped.append((key, role, ea, n3, "already " + names[ea], a, b))
            continue
        names[ea] = n3

by_name = defaultdict(list)
for ea, n in names.items():
    by_name[n].append(ea)
existing = {}
for r in csv.reader(open(z360, encoding="utf-8")):
    if len(r) > 3:
        existing[strip_sig(r[4] or r[3])] = int(r[1], 16)
vacated = {existing_name for existing_name, ea in existing.items() if ea in forced}
final = {}
for n, eas in by_name.items():
    if len(set(eas)) > 1:
        skipped.append((None, None, eas, n, "dup", 0, 0))
        continue
    if n in existing and existing[n] != eas[0] and n not in vacated:
        skipped.append((None, None, eas, n, "exists", 0, 0))
        continue
    final[eas[0]] = n

with open(out, "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    for ea in sorted(final):
        w.writerow(["%#x" % ea, final[ea]] + (["force"] if ea in forced else []))
print("aligned classes", sum(1 for v in aligned.values() if v[0]), "of", len(aligned), "dp pairs", len(dp_pairs),
      "named", len(final), "forced", len(forced & set(final)), "conflicts", len(conflicts), "skipped", len(skipped))
if rep_path:
    with open(rep_path, "w", encoding="utf-8") as f:
        f.write("# classes: ok, states in both, states on 360, slots, compatible, same-presence\n")
        for cls, v in sorted(aligned.items()):
            f.write("%s %s\n" % (cls, v))
        f.write("\n# conflicts (360 already named, 3DS says otherwise)\n")
        for key, role, ea, cur, n3 in conflicts:
            f.write("%s state %d %s: %#x %s  <- 3DS %s\n" % (key[0], key[1], role, ea, cur, n3))
        f.write("\n# skipped\n")
        for key, role, ea, n3, why, a, b in skipped:
            f.write("%s %s %s %s: %s (%s/%s)\n" % (key, role, ea if isinstance(ea, int) is False else "%#x" % ea, n3, why, a, b))
