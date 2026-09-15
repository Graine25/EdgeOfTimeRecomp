import re
import sys

TEXT = "сука блять"
MARKER = re.compile(r"\\[A-Za-z0-9_]+\\")
TRAILING = re.compile(r"\\[A-Za-z0-9_]+\\\s*$")
ESCAPES = {"n": "\n", "t": "\t", "r": "\r"}


def unescape(text):
    out = []
    i = 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text):
            out.append(ESCAPES.get(text[i + 1], text[i + 1]))
            i += 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def escape(text):
    return text.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")


def replace(text):
    if not re.search(r"[A-Za-z]", MARKER.sub("", text)):
        return text
    lead = ""
    while True:
        m = MARKER.match(text)
        if not m:
            break
        lead += m.group()
        text = text[m.end():]
        if text.startswith(" "):
            lead += " "
            text = text[1:]
    trail = ""
    while True:
        m = TRAILING.search(text)
        if not m:
            break
        trail = text[m.start():] + trail
        text = text[:m.start()]
    return lead + TEXT + (" " + trail.lstrip() if trail else "")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    out = ["# reeot text: ru -- every line \"" + TEXT + "\", the markers kept"]
    n = 0
    for row in open(src, encoding="utf-8"):
        row = row.rstrip("\n")
        if not row or row.startswith("#"):
            continue
        cols = row.split("\t", 8)
        if len(cols) != 9:
            continue
        cols[8] = escape(replace(unescape(cols[8])))
        out.append("\t".join(cols))
        n += 1
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print(f"{dst}: {n} lines")


if __name__ == "__main__":
    main()
