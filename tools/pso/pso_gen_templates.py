#!/usr/bin/env python3
import argparse
import csv
import io
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pso_common import (BIAS_COLUMNS, COLUMNS, DEFAULT_SHADER_CACHE, DEFAULT_TEMPLATES,  # noqa: E402
                        SCHEMA_VERSION, canonicalize, collect_inputs, is_capture, is_pairs,
                        is_template_usage, load_shader_masks, read_rows, row_line, write_inc)

SPEC_LAYOUT_BITS = 0x5
PAIRS_HEADER = "vsHash,psHash,technique,pass"
DEPTH_TECHNIQUE = 5
CLEAR = {"vsHash": "0" * 16, "psHash": "0" * 16, "layoutKey": "0" * 16, "declRaw": "",
         "frame": "0", "session": "template", "package": "",
         "depthBias": "0", "slopeScaledDepthBias": "0", "targetScale": "1"}
TEMPLATE_PREFIX = ["technique", "pass", "class", "biasKind"]


def template_identity(t):
    return tuple(t[c] for c in COLUMNS if c not in ("frame", "session", "package", "layoutKey"))


def read_raw(path):
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    body = [l for l in text.splitlines() if l.strip() and not l.startswith("#")]
    if not body:
        return None, []
    reader = csv.DictReader(io.StringIO("\n".join(body)))
    return list(reader.fieldnames), list(reader)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", help="capture CSVs / merged table / pso_pairs / pso_templates files, or folders")
    ap.add_argument("-o", "--output", default=str(DEFAULT_TEMPLATES))
    ap.add_argument("--shader-cache", default=str(DEFAULT_SHADER_CACHE))
    ap.add_argument("--keep-unused", action="store_true", help="keep templates the usage files say were never used")
    ap.add_argument("--prune-min", type=int, default=100,
                    help="predicted pipelines a never-used template must have produced to be dropped")
    args = ap.parse_args()

    masks = load_shader_masks(args.shader_cache) if args.shader_cache else None
    pair_slots = defaultdict(set)
    vs_slots = defaultdict(set)
    captures = []
    usage = defaultdict(lambda: [0, 0])
    for f in collect_inputs(args.inputs):
        if is_pairs(f):
            cols, rows = read_raw(f)
            if not cols or cols[:4] != PAIRS_HEADER.split(","):
                sys.stderr.write(f"warning: {f}: not a pairs file; skipped\n")
                continue
            for r in rows:
                slot = (int(r["technique"]), int(r["pass"]), int(r.get("class", "0") or "0", 16))
                pair_slots[(r["vsHash"].lower(), r["psHash"].lower())].add(slot)
                vs_slots[r["vsHash"].lower()].add(slot)
        elif is_template_usage(f):
            cols, rows = read_raw(f)
            if not cols or cols[:4] != TEMPLATE_PREFIX:
                sys.stderr.write(f"warning: {f}: not a template usage file; skipped\n")
                continue
            for r in rows:
                key = (int(r["technique"]), int(r["pass"]), int(r["class"], 16), int(r["biasKind"]),
                       template_identity(r))
                usage[key][0] += int(r.get("predicted", "0") or "0")
                usage[key][1] += int(r.get("used", "0") or "0")
        elif f.name.startswith("pso_") and not is_capture(f):
            continue
        else:
            cols, rows = read_rows(f)
            if rows:
                captures.extend(rows)

    if not pair_slots:
        sys.exit("error: no pso_pairs_*.csv rows found (run the game once: the predictor writes them)")
    if not captures:
        sys.exit("error: no capture rows found")

    templates = {}
    matched = unmatched = 0
    for r in captures:
        canonicalize(r, masks)
        vs, ps = r["vsHash"].lower(), r["psHash"].lower()
        slots = pair_slots.get((vs, ps))
        if not slots and int(ps, 16) == 0:
            slots = vs_slots.get(vs)
        if not slots:
            unmatched += 1
            continue
        matched += 1
        biased = int(r["depthBias"] or "0") != 0 or float(r["slopeScaledDepthBias"] or "0") != 0.0
        t = dict(r)
        t.update(CLEAR)
        strides = t["strides"].split("|")
        strides[0] = "0"
        t["strides"] = "|".join(strides)
        t["spec"] = format(int(t["spec"], 16) & ~SPEC_LAYOUT_BITS, "x")
        ident = template_identity(t)
        for tech, pas, cls in slots:
            kind = 0 if not biased else (2 if tech == DEPTH_TECHNIQUE else 1)
            templates.setdefault((tech, pas, cls, kind, ident), t)

    pruned = 0
    if usage and not args.keep_unused:
        for key in list(templates):
            predicted, used = usage.get(key, (0, 0))
            if used == 0 and predicted >= args.prune_min:
                del templates[key]
                pruned += 1

    rows = sorted(templates.items())
    header = ",".join(TEMPLATE_PREFIX + COLUMNS)
    lines = []
    per_tech = defaultdict(int)
    per_kind = defaultdict(int)
    for (tech, pas, cls, kind, _), t in rows:
        lines.append(",".join([str(tech), str(pas), format(cls, "x"), str(kind)]) + "," + row_line(t))
        per_tech[tech] += 1
        per_kind[kind] += 1
    write_inc(args.output,
              f"// eot-pso-templates v{SCHEMA_VERSION}: {len(rows)} template(s). Generated by tools/pso/pso_gen_templates.py; do not edit.",
              header, lines)
    print(f"{args.output}: {len(rows)} templates from {matched} matched capture rows "
          f"({unmatched} rows with a shader pair no model slot has reported; {pruned} pruned as never used); "
          f"{len(pair_slots)} pairs known")
    print("templates per technique: " + ", ".join(f"{k}:{v}" for k, v in sorted(per_tech.items())))
    print("bias kinds: " + ", ".join(f"{k}:{v}" for k, v in sorted(per_kind.items())))


if __name__ == "__main__":
    main()
