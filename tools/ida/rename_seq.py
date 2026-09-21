import csv
import re
import sys

csv.field_size_limit(1 << 30)
src, out = sys.argv[1], sys.argv[-1]
names = {}
for path in sys.argv[2:-1]:
    for r in csv.reader(open(path, encoding="utf-8")):
        if r and not r[0].startswith("#"):
            names[int(r[0], 16)] = r[1].strip()

rows = list(csv.reader(open(src, encoding="utf-8")))
old_to_new = {}
for r in rows:
    ea = int(r[0], 16)
    if ea in names and r[1] != names[ea]:
        old_to_new[r[1]] = names[ea]
        r[1] = names[ea]
PLACE = re.compile(r"^(@?)(?:sub_|helper_|nullsub_|dword_|unk_|byte_|word_|off_|flt_|qword_|dbl_|stru_|asc_)([0-9A-Fa-f]{6,8})$")


def map_token(t):
    if t in old_to_new:
        return old_to_new[t]
    m = PLACE.match(t)
    if m and int(m.group(2), 16) in names:
        return m.group(1) + names[int(m.group(2), 16)]
    return t


changed = 0
for r in rows:
    for col in (3, 4):
        if len(r) > col and r[col]:
            toks = r[col].split("|")
            new = [map_token(t) for t in toks]
            if new != toks:
                changed += 1
                r[col] = "|".join(new)
with open(out, "w", newline="", encoding="utf-8") as f:
    csv.writer(f).writerows(rows)
print("renamed", len(old_to_new), "functions, rewrote", changed, "sequences ->", out)
