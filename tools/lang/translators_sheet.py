import re
import sys

MARKER = re.compile(r"\\\\[A-Z0-9_]+\\\\|%[0-9.]*[dsfux]")
COLUMNS = ["id", "package", "table", "string", "line", "start", "end", "crc", "English", "Translation"]
NOTES = [
    "# Edge of Time -- the game's text for translation. Fill the Translation column; leave a row empty to keep English.",
    "# Keep every \\\\NAME\\\\ marker exactly as it is (a button glyph or a value the game fills in) and every %d / %s; \\n is a line break.",
    "# A subtitle is several rows with start/end seconds: each row is one timed line, translate it to fit its slot.",
    "# Do not change the other columns: id ties a row back to the game. Encoding UTF-8, tab-separated.",
]


def read_template(path):
    rows = []
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        cols = line.split("\t", 8)
        if len(cols) == 9:
            rows.append(cols)
    return rows


def export(template, out):
    rows = read_template(template)
    with open(out, "w", encoding="utf-8", newline="") as f:
        for note in NOTES:
            f.write(note + "\n")
        f.write("\t".join(COLUMNS) + "\n")
        for i, (package, table, name, string, sub, crc, start, end, text) in enumerate(rows, 1):
            timing = (start, end) if sub != "0" or end != "-1" else ("", "")
            f.write("\t".join([str(i), package, name, string, sub, timing[0], timing[1], crc, text, ""]) + "\n")
    print(f"{out}: {len(rows)} rows")


def import_(sheet, template, out):
    rows = read_template(template)
    translations = {}
    with open(sheet, encoding="utf-8", newline="") as f:
        header = None
        for line in f:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            cols = line.split("\t")
            if header is None:
                header = {name: k for k, name in enumerate(cols)}
                if "id" not in header or "Translation" not in header:
                    sys.exit("the sheet needs an id and a Translation column")
                continue
            if len(cols) <= header["Translation"]:
                continue
            t = cols[header["Translation"]].strip()
            if t:
                translations[int(cols[header["id"]])] = t
    by_english = {}
    for i, row in enumerate(rows, 1):
        if i in translations:
            by_english.setdefault(row[8], translations[i])
    done = same = 0
    problems = []
    lines = ["# reeot text: from the translators' sheet by tools/lang/translators_sheet.py"]
    for i, row in enumerate(rows, 1):
        text = row[8]
        t = translations.get(i)
        if t is None and text in by_english:
            t = by_english[text]
            same += 1
        if t is not None:
            if sorted(MARKER.findall(t)) != sorted(MARKER.findall(text)):
                problems.append(f"row {i}: markers differ: {text!r} -> {t!r}")
            row = row[:8] + [t]
            done += 1
        lines.append("\t".join(row))
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    for p in problems[:50]:
        print(p)
    print(f"{out}: {len(rows)} rows, {done} translated ({same} by matching English), {len(rows) - done} English, "
          f"{len(problems)} marker problem(s)")


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "export":
        export(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 5 and sys.argv[1] == "import":
        import_(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
