import os
import subprocess
import sys

HEADER = "# reeot text template v1: package\ttable\ttableName\tstring\tsub\tcrc\tstart\tend\ttext\n"


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    data, out = sys.argv[1], sys.argv[2]
    pkztool = sys.argv[3] if len(sys.argv) == 4 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "out", "pkzlib", "pkztool.exe")
    if os.path.exists(out):
        os.remove(out)
    packages = sorted(f for f in os.listdir(data) if f.lower().endswith(".pkz"))
    rows = 0
    with_text = 0
    for name in packages:
        before = os.path.getsize(out) if os.path.exists(out) else 0
        result = subprocess.run([pkztool, "export-strings", os.path.join(data, name), out, "0x1"],
                                capture_output=True, text=True)
        if result.returncode != 0:
            print(f"  {name}: {result.stderr.strip() or result.stdout.strip()}")
            continue
        after = os.path.getsize(out) if os.path.exists(out) else 0
        if after > before:
            with_text += 1
            print("  " + result.stdout.strip())
    if not os.path.exists(out):
        sys.exit(f"no text found in {data}")
    body = open(out, encoding="utf-8").read()
    rows = body.count("\n")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER)
        f.write(body)
    print(f"{out}: {rows} rows from {with_text} of {len(packages)} packages")


if __name__ == "__main__":
    main()
