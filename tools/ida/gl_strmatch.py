import collections
import csv
import re
import sys

csv.field_size_limit(1 << 30)
src3, src360, out = sys.argv[1], sys.argv[2], sys.argv[3]
PLACEHOLDER = re.compile(r"^(sub_|helper_|nullsub_|GameLogic_\d+|loc_)")
SKIP3 = re.compile(r"^(_|__|j_|nn::|std::|operator|\?|sub_|nullsub|unk_|loc_|\.|\$|\`)")
STRING = re.compile(r"^@a[A-Z0-9_]")


def load(path):
    owners = collections.defaultdict(set)
    names = {}
    for r in csv.reader(open(path, encoding="utf-8")):
        if len(r) < 4:
            continue
        name = r[1].split("(")[0].strip()
        names[r[0]] = name
        for t in set(r[3].split("|")):
            if STRING.match(t):
                owners[t].add(r[0])
    return owners, names


o3, n3 = load(src3)
o360, n360 = load(src360)
votes = collections.defaultdict(collections.Counter)
for s, eas360 in o360.items():
    eas3 = o3.get(s)
    if not eas3 or len(eas360) != 1 or len(eas3) != 1:
        continue
    ea360, ea3 = next(iter(eas360)), next(iter(eas3))
    if PLACEHOLDER.match(n360[ea360]) and not SKIP3.match(n3[ea3]) and ("::" in n3[ea3] or re.match(r"^[A-Z]", n3[ea3])):
        votes[ea360][n3[ea3]] += 1
taken = set(n360.values())
with open(out, "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    n = 0
    for ea, cnt in sorted(votes.items()):
        if len(cnt) == 1:
            nm = next(iter(cnt))
            if nm not in taken:
                w.writerow([ea, nm])
                taken.add(nm)
                n += 1
print("strings shared", len(set(o3) & set(o360)), "singleton pairs voted", len(votes), "proposed", n)
