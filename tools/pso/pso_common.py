import csv
import io
import re
import sys
from pathlib import Path

SCHEMA_VERSION = 4
COLUMNS = [
    "vsHash", "psHash", "spec", "layoutKey", "declRaw", "strides", "topology", "rtFormats",
    "rtCount", "dsFormat", "sampleCount", "cull", "frontFace", "depthBias",
    "slopeScaledDepthBias", "targetScale", "depthClip", "depthEnable", "depthWrite", "depthFunc",
    "stencilEnable", "stencilReadMask", "stencilWriteMask", "stencilRef", "stencilFront",
    "stencilBack", "blend0", "blend1", "blend2", "blend3", "alphaToCoverage", "velocity", "frame",
    "session", "package",
]
DIAGNOSTIC_COLUMNS = {"layoutKey", "frame", "session", "package"}
BIAS_COLUMNS = ("depthBias", "slopeScaledDepthBias", "targetScale")
VERSION_RE = re.compile(r"^#\s*eot-pso\s+v(\d+)\s*$")
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SHADER_CACHE = REPO_ROOT / "generated" / "shader_cache.cpp"
DEFAULT_TABLE = REPO_ROOT / "config" / "pso" / "eot_pipelines.csv"
DEFAULT_INC = REPO_ROOT / "src" / "gpu" / "pipeline" / "cache" / "eot_pipelines.inc"
DEFAULT_TEMPLATES = REPO_ROOT / "src" / "gpu" / "pipeline" / "cache" / "eot_pso_templates.inc"

BLEND_ONE, BLEND_ZERO, BLEND_OP_ADD = 2, 1, 1
COMPARE_ALWAYS = 8
DISABLED_BLEND_PREFIX = f"0|{BLEND_ONE}|{BLEND_ZERO}|{BLEND_OP_ADD}|{BLEND_ONE}|{BLEND_ZERO}|{BLEND_OP_ADD}"
DISABLED_STENCIL_FACE = f"0|0|0|{COMPARE_ALWAYS}"

CAPTURE_PREFIX = "pso_misses_"
SIDE_FILE_PREFIXES = ("pso_pairs_", "pso_predicted_", "pso_used_", "pso_templates_", "shader_canon")


def is_capture(path):
    return path.name.startswith(CAPTURE_PREFIX)


def is_pairs(path):
    return path.name.startswith("pso_pairs_")


def is_template_usage(path):
    return path.name.startswith("pso_templates_")


def upgrade_v1(columns, rows):
    if "targetScale" in columns:
        return columns, rows
    i = columns.index("slopeScaledDepthBias") + 1
    columns = columns[:i] + ["targetScale"] + columns[i:]
    for r in rows:
        b = int(r.get("depthBias", "0") or "0")
        if b:
            layers = -(-(abs(b) + 0.5) // 4)
            b = int(layers) * 8 * (1 if b > 0 else -1)
            r["depthBias"] = str(b)
        slope = float(r.get("slopeScaledDepthBias", "0") or "0")
        r["targetScale"] = "0" if (b or slope != 0.0) else "1"
    return columns, rows


def upgrade_v2(columns, rows):
    if "package" in columns:
        return columns, rows
    columns = columns + ["package"]
    for r in rows:
        r["package"] = ""
    return columns, rows


def read_rows(path):
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    body = []
    for line in text.splitlines():
        if VERSION_RE.match(line) or line.startswith("#") or not line.strip():
            continue
        body.append(line)
    if not body or not body[0].startswith("vsHash"):
        return None, []
    reader = csv.DictReader(io.StringIO("\n".join(body)))
    rows = [r for r in reader if r.get("vsHash")]
    columns = list(reader.fieldnames)
    if "depthBias" in columns:
        columns, rows = upgrade_v1(columns, rows)
        columns, rows = upgrade_v2(columns, rows)
    return columns, rows


def canon_decl(hexstr):
    out = []
    for i in range(0, len(hexstr), 24):
        e = hexstr[i:i + 24]
        out.append(e[:22] + "00" if len(e) == 24 else e)
    return "".join(out)


def load_shader_masks(path):
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return None
    masks = {}
    for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]+)\s*,\s*\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*(\d+)", text):
        masks[int(m.group(1), 16)] = int(m.group(2))
    return masks or None


def canonicalize(row, masks=None):
    row["declRaw"] = canon_decl(row.get("declRaw", ""))
    row["sampleCount"] = "1"
    biased = int(row["depthBias"] or "0") != 0 or float(row["slopeScaledDepthBias"] or "0") != 0.0
    if row["depthEnable"] == "0":
        row["depthWrite"] = "0"
        row["depthFunc"] = str(COMPARE_ALWAYS)
        row["depthBias"] = "0"
        row["slopeScaledDepthBias"] = "0"
        biased = False
    if not biased:
        row["slopeScaledDepthBias"] = "0"
        row["targetScale"] = "1"
    elif int(row["rtCount"] or "0") == 0 and row["dsFormat"] not in ("", "0"):
        row["targetScale"] = "0"
    if row["stencilEnable"] == "0":
        row["stencilReadMask"] = row["stencilWriteMask"] = row["stencilRef"] = "0"
        row["stencilFront"] = row["stencilBack"] = DISABLED_STENCIL_FACE
    rt_count = int(row["rtCount"] or "0")
    formats = row["rtFormats"].split("|")
    for i in range(4):
        b = row[f"blend{i}"].split("|")
        if len(b) != 8:
            continue
        if i >= rt_count:
            row[f"blend{i}"] = "0|0|0|0|0|0|0|0"
            if i < len(formats):
                formats[i] = "0"
            continue
        if b[7] == "0":
            b[0] = "0"
        if b[0] == "0":
            b = DISABLED_BLEND_PREFIX.split("|") + [b[7]]
        row[f"blend{i}"] = "|".join(b)
    row["rtFormats"] = "|".join(formats)
    if rt_count == 0:
        row["alphaToCoverage"] = "0"
    if masks:
        vs, ps = int(row["vsHash"], 16), int(row["psHash"], 16)
        mask = masks.get(vs, 0) | (masks.get(ps, 0) if ps else 0)
        row["spec"] = format(int(row["spec"] or "0", 16) & mask, "x")
    return row


def identity(row, columns=None):
    return tuple(row.get(c, "") for c in (columns or COLUMNS) if c not in DIAGNOSTIC_COLUMNS)


def row_line(row, columns=None):
    out = io.StringIO()
    w = csv.writer(out, lineterminator="")
    w.writerow([row.get(c, "") for c in (columns or COLUMNS)])
    return out.getvalue()


def merge_packages(a, b):
    ids = set(x for x in (a or "").split("|") if x) | set(x for x in (b or "").split("|") if x)
    return "|".join(sorted(ids, key=lambda x: int(x, 16)))


def collect_inputs(inputs):
    files = []
    for raw in inputs:
        p = Path(raw)
        if p.is_dir():
            found = sorted(p.rglob("*.csv"))
            if not found:
                sys.stderr.write(f"warning: no .csv files under {p}\n")
            files.extend(found)
        elif p.is_file():
            files.append(p)
        else:
            sys.stderr.write(f"warning: skipping missing path {p}\n")
    return files


def write_inc(path, first_line, header, rows):
    lines = [first_line, f'"{header}",']
    for line in rows:
        line = line.replace("\\", "\\\\").replace('"', '\\"')
        lines.append(f'"{line}",')
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
