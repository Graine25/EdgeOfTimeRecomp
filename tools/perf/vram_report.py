import re
import sys

TIME = re.compile(r"^\[(\d{4}-\d\d-\d\d )?(\d\d:\d\d:\d\d)")
VRAM = re.compile(
    r"\[vram\] process (\d+) MB of a (\d+) MB budget in video memory, (\d+) MB in shared memory \| "
    r"allocator: (\d+) MB in (\d+) allocations in video memory \((\d+) MB of blocks\), (\d+) MB in all heaps"
    r"(?: \| peak (\d+) MB \| the driver's own (\d+) MB)?")
CENSUS_KEYS = [
    ("images", r"images (\d+) MB"),
    ("surfaces", r"surfaces (\d+) MB ="),
    ("frame MS", r"frame \d+x (\d+) MB"),
    ("MS never drawn", r"of which (\d+) MB \(\d+\) never drawn"),
    ("MS not made", r"not made yet \(about (\d+) MB"),
    ("twins", r"their twins (\d+) MB"),
    ("frame 1x", r"frame single-sample (\d+) MB"),
    ("shadow tile", r"shadow tile (\d+) MB"),
    ("PiP surf", r"surfaces \d+ MB = .*?picture-in-picture (\d+) MB"),
    ("mirrors", r"resolve mirrors (\d+) MB"),
    ("atlas", r"shadow atlas (\d+) MB"),
    ("PiP mirr", r"resolve mirrors .*?picture-in-picture (\d+) MB"),
    ("textures", r"\| textures (\d+) MB"),
    ("recycle", r"recycle list (\d+) MB"),
    ("TAA", r"temporal history (\d+) MB"),
    ("vectors", r"motion vectors (\d+) MB"),
    ("PiP heaps", r"picture-in-picture heaps (\d+) MB"),
]
BUFFER_KEYS = [
    ("idx vram", r"index cache (\d+) MB VRAM"),
    ("idx up", r"index cache \d+ MB VRAM \+ (\d+) MB"),
    ("vtx vram", r"vertex mirrors (\d+) MB VRAM"),
    ("vtx up", r"vertex mirrors \d+ MB VRAM \+ (\d+) MB"),
    ("ring", r"upload ring (\d+) MB"),
    ("mv hist", r"motion-vector history (\d+) MB"),
    ("pipelines", r"\| (\d+) pipelines"),
]
BY_TAG = re.compile(r"\[vram-by-tag\] (.*)$")


def grab(keys, text):
    out = {}
    for name, pattern in keys:
        m = re.search(pattern, text)
        out[name] = int(m.group(1)) if m else None
    return out


def cell(v, width=7):
    return f"{'-' if v is None else v:>{width}}"


SHORT = [("tag ", ""), ("MS not made", "MS unmade"), ("surface-", "s-"), ("resolve-mirror", "mirror"), ("guest-texture", "textures"),
         ("index-cache-vram", "idx-vram"), ("vertex-mirror-vram", "vtx-vram"), ("overlay-texture", "overlay"),
         ("velocity-resolved", "vel-1x"), ("taa-history", "taa"), ("(FREE)", "free")]


def header(c):
    for a, b in SHORT:
        c = c.replace(a, b)
    return c[:9]


def main(paths):
    lines = []
    for p in paths:
        with open(p, encoding="utf-8", errors="replace") as f:
            lines.extend(f)
    settings, models, allocs, warnings = [], [], [], []
    rows = []
    last_surfaces = last_census = last_churn = last_by_tag = None
    pending = None
    for l in lines:
        t = TIME.match(l)
        stamp = t.group(2) if t else "?"
        if "[gpu] D3D12" in l or "[gpu] internal render" in l or "[quality] preset" in l or "  profile:" in l:
            settings.append(l.split("] ", 4)[-1].strip())
        elif "[vram-model]" in l:
            models.append(l.split("[vram-model] ", 1)[1].strip())
        elif "[vram-alloc]" in l:
            allocs.append(f"{stamp} {l.split('[vram-alloc] ', 1)[1].strip()}")
        elif "[vram] over budget" in l or ("[vram]" in l and "images " in l and "[warning]" in l):
            warnings.append(f"{stamp} {l.split('[vram] ', 1)[1].strip()}")
        m = VRAM.search(l)
        if m:
            g = [int(x) if x else None for x in m.groups()]
            pending = {"time": stamp, "process": g[0], "budget": g[1], "shared": g[2], "alloc": g[3],
                       "blocks": g[5], "peak": g[7], "driver": g[8] if g[8] is not None else g[0] - g[5]}
            rows.append(pending)
            continue
        if "[vram-census] images" in l:
            last_census = l.split("[vram-census] ", 1)[1].strip()
            if pending is not None:
                pending.update(grab(CENSUS_KEYS, last_census))
        elif "[vram-census] buffers" in l:
            if pending is not None:
                pending.update(grab(BUFFER_KEYS, l))
        elif "[vram-surfaces]" in l:
            last_surfaces = l.split("[vram-surfaces] ", 1)[1].strip()
        elif "[vram-churn]" in l:
            last_churn = l.split("[vram-churn] ", 1)[1].strip()
        else:
            b = BY_TAG.search(l)
            if b:
                last_by_tag = b.group(1).strip()
                if pending is not None and "images" not in pending:
                    for tag, mb in re.findall(r"DEFAULT:([a-z0-9-]+|\(FREE\)) (\d+) MB", last_by_tag):
                        pending[f"tag {tag}"] = int(mb)

    print("== session")
    for s in dict.fromkeys(settings):
        print("  " + s)
    if models:
        print("\n== the model (what these settings cost on that GPU)")
        for m in models:
            print("  " + m)
    if rows:
        print("\n== timeline (MB)")
        census_cols = [k for k, _ in CENSUS_KEYS + BUFFER_KEYS if any(r.get(k) is not None for r in rows)]
        tag_cols = sorted({k for r in rows for k in r if k.startswith("tag ")},
                          key=lambda k: -max(r.get(k) or 0 for r in rows))[:10]
        cols = ["process", "budget", "peak", "shared", "alloc", "driver"] + census_cols + tag_cols
        print(f"{'time':>8} " + " ".join(f"{header(c):>9}" for c in cols))
        for r in rows:
            print(f"{r['time']:>8} " + " ".join(cell(r.get(c), 9) for c in cols))
        worst = max(rows, key=lambda r: r["process"])
        print(f"\n  highest report: {worst['process']} MB at {worst['time']} of a {worst['budget']} MB budget"
              + (f"; session peak {max(r['peak'] or 0 for r in rows)} MB" if any(r['peak'] for r in rows) else ""))
    if warnings:
        print(f"\n== over budget ({len(warnings)} lines)")
        for w in warnings[:40]:
            print("  " + w)
    if allocs:
        print(f"\n== first allocation of each shape of 16 MB or more ({len(allocs)})")
        for a in allocs:
            print("  " + a)
    if last_census:
        print("\n== last census\n  " + last_census.replace(" | ", "\n  "))
    if last_surfaces:
        head, _, rest = last_surfaces.partition(": ")
        print(f"\n== last surface list ({head})")
        for s in rest.split("; "):
            print("  " + s)
    if last_churn:
        print("\n== last churn\n  " + last_churn)
    if last_by_tag:
        print("\n== last allocator view by tag\n  " + last_by_tag.replace(", ", "\n  "))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1:])
