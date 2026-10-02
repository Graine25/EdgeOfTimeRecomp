import csv
import math
import os
import pickle
import struct
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

import xxhash

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pak_schema import Schema, children, open_pak, walk  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PAKS = REPO_ROOT / "shader_notes" / "EOT_pak"
DEFAULT_DIRECTORY = REPO_ROOT / "tools" / "pso" / "package_directory.csv"
DEFAULT_CACHE = REPO_ROOT / "out" / "pso" / "pak_index.pkl"
INDEX_VERSION = 5

TECHNIQUES = ["0G_0SM", "2G_1SM", "4G_1SM", "6G_4SM", "1G_1I_1SM", "Depth", "ConstantColor",
              "ShadowReceiver", "ZPassNormal", "Prelighted", "SSEdgeExtrude"]

VF_SKIN, VF_MORPH_STREAM, VF_INSTANCE_STREAM = 0x1000, 0x4000, 0x8000

LIST_MODELS, LIST_ENVIRONMENTS, LIST_OBJECTS = 0x7, 0x30, 0x4
OBJ_RECORD, OBJ_HEADER, OBJ_BODY, OBJ_PARAMS = 0x138D, 0x138E, 0x1F4, 0x1391
SHADOW_INIT_CLASS = (0x70, 0xB6FD4077, 0x70)
GEO_MODEL, GEO_MATERIAL, GEO_MATERIAL_INFO = 0x321, 0x324, 0x0331
GEO_LODS, GEO_MESH = 0x325, 0x0327
ENV_OCTREE, ENV_MATERIAL_LIST = 0xBBB, 0xCA
ENV_PRIMS, ENV_OBJS = 0x00CC, 0x00CE

DECL_COUNT, DECL_ELEMENTS, DECL_ELEMENT = 0x18, 0x34, 12


def vertex_set(fmt):
    skin, morph = bool(fmt & VF_SKIN), bool(fmt & VF_MORPH_STREAM)
    if fmt & VF_INSTANCE_STREAM:
        return 4
    return (1 if skin else 0) + (2 if morph else 0)


def decl_raw(image):
    if len(image) < DECL_ELEMENTS:
        return ""
    count = int.from_bytes(image[DECL_COUNT:DECL_COUNT + 4], "big")
    out = bytearray(image[DECL_ELEMENTS:DECL_ELEMENTS + count * DECL_ELEMENT])
    for i in range(count):
        out[i * DECL_ELEMENT + 11] = 0
    return out.hex()


def shader_key(record):
    return record.get("id32") or record.get("id64", 0)


def shader_hash(record, header):
    if not record.get("size0"):
        return 0
    return xxhash.xxh3_64_intdigest(record["data0"][header:] + record["data1"])


@dataclass
class Slot:
    technique: int
    set: int
    vs: int
    ps: int
    decl: str
    vertex_format: int
    generic_flags: int
    lighting_flags: int
    stage_flags: tuple
    vs_key: int = 0
    ps_key: int = 0


@dataclass
class Material:
    pak: str
    kind: str
    owner: int
    index: int
    name: str
    header: dict
    stage_blend: tuple
    slots: list = field(default_factory=list)
    uses: set = field(default_factory=set)


def _material(schema, data, chunk_324, pak, kind, owner, index):
    info = next((c for c in children(data, chunk_324) if c.id == GEO_MATERIAL_INFO), None)
    if info is None:
        return None
    m = schema.decode(info, data)
    header = {k: v for k, v in m.items()
              if k not in ("samplers", "stageParams", "passes", "text3dLayout", "name")
              and not isinstance(v, (list, tuple, bytes))}
    mat = Material(pak, kind, owner, index, m["name"], header,
                   tuple(s["blendMode"] for s in m["stageParams"]))
    for p in m["passes"]:
        d, vs, ps = p["description"], p["vertexShader"], p["pixelShader"]
        mat.slots.append(Slot(p["slot"], p["set"], shader_hash(vs, 0x368), shader_hash(ps, 0x28),
                              decl_raw(vs["decl"]), d["vertexFormat"], d["genericFlags"],
                              d["lightingFlags"], tuple(s["flags"] for s in d["stages"]),
                              shader_key(vs), shader_key(ps) if ps.get("size0") else 0))
    return mat


def polygon_offset_units(offset):
    units = min(math.ceil(abs(offset) * (1 << 21)), 100000000) << 3
    return -units if offset < 0 else units


def shadow_initializers(data, lst):
    out = []
    for rec in walk(data, lst.offset, lst.end, lst.depth + 1, lst):
        if rec.id != OBJ_RECORD:
            continue
        name, block = "", None
        for sub in children(data, rec):
            if sub.id == OBJ_HEADER:
                name = bytes(data[sub.offset + 24:sub.offset + 88]).split(b"\0")[0].decode("latin-1")
            elif sub.id == OBJ_BODY:
                for b in walk(data, sub.offset, sub.end, sub.depth + 1, sub):
                    if (b.id == OBJ_PARAMS and b.size >= 48 and
                            struct.unpack_from(">4I", data, b.offset)[1:4] == SHADOW_INIT_CLASS):
                        block = b
        if block is None or not struct.unpack_from(">I", data, block.offset + 32)[0]:
            continue
        slope = struct.unpack_from(">f", data, block.offset + 28)[0]
        bias = struct.unpack_from(">f", data, block.offset + 36)[0]
        out.append((name.split("_", 1)[-1], polygon_offset_units(bias), f"{slope:.9g}"))
    return out


def scan_pak(path, schema, shadows=None):
    data = open_pak(path)
    pak = Path(path).stem
    out = []
    root = next(walk(data), None)
    if root is None:
        return out
    for lst in children(data, root):
        if lst.id == LIST_OBJECTS and shadows is not None:
            shadows[pak] = shadow_initializers(data, lst)
        if lst.id == LIST_MODELS:
            owner = 0
            for c in walk(data, lst.offset, lst.end, lst.depth + 1, lst):
                if c.id != GEO_MODEL:
                    continue
                mats = []
                for sub in children(data, c):
                    if sub.id == GEO_MATERIAL:
                        mat = _material(schema, data, sub, pak, "model", owner, len(mats))
                        mats.append(mat)
                for sub in children(data, c):
                    if sub.id != GEO_LODS:
                        continue
                    for mesh in children(data, sub):
                        if mesh.id != GEO_MESH:
                            continue
                        r = schema.decode(mesh, data)
                        i = r["material"]
                        if i < len(mats) and mats[i]:
                            mats[i].uses.add((vertex_set(r["format"]), r["stride"], r["renderGroup"], 0))
                out.extend(m for m in mats if m)
                owner += 1
        elif lst.id == LIST_ENVIRONMENTS:
            owner = 0
            for c in walk(data, lst.offset, lst.end, lst.depth + 1, lst):
                if c.id != ENV_OCTREE:
                    continue
                mats = []
                for sub in children(data, c):
                    if sub.id == ENV_MATERIAL_LIST:
                        for mc in children(data, sub):
                            if mc.id == GEO_MATERIAL:
                                mats.append(_material(schema, data, mc, pak, "env", owner, len(mats)))
                for sub in children(data, c):
                    if sub.id == ENV_PRIMS:
                        for p in schema.decode(sub, data)["prims"]:
                            i = p["materialIndex"]
                            if i < len(mats) and mats[i]:
                                mats[i].uses.add((vertex_set(p["vertexFormat"]), p["vertexStride"], -1,
                                                  p["drawFlags"]))
                    elif sub.id == ENV_OBJS:
                        for o in schema.decode(sub, data)["objs"]:
                            for it in o["items"]:
                                i = it["materialIndex"]
                                if i < len(mats) and mats[i]:
                                    mats[i].uses.add((vertex_set(it["vertexFormat"]), it["vertexStride"],
                                                      -2, it["drawFlags"]))
                out.extend(m for m in mats if m)
                owner += 1
    return out


def load_directory(path=DEFAULT_DIRECTORY):
    out = {}
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            parents = [int(x) for x in r["parent_ids"].replace("|", " ").split()]
            out[int(r["id"])] = (r["name"], parents)
    return out


def ancestors(directory, pid):
    seen, stack = [], [pid]
    while stack:
        p = stack.pop()
        if p in seen or p not in directory:
            continue
        seen.append(p)
        stack.extend(directory[p][1])
    return seen


def load_index(paks=DEFAULT_PAKS, cache=DEFAULT_CACHE, verbose=True):
    return load_paks(paks, cache, verbose)[0]


def load_shadow_initializers(paks=DEFAULT_PAKS, cache=DEFAULT_CACHE, verbose=True):
    return load_paks(paks, cache, verbose)[1]


def load_paks(paks=DEFAULT_PAKS, cache=DEFAULT_CACHE, verbose=True):
    paks = Path(paks)
    files = sorted(paks.glob("*.pak"))
    stamp = (INDEX_VERSION, tuple((p.name, p.stat().st_size, int(p.stat().st_mtime)) for p in files))
    cache = Path(cache)
    if cache.exists():
        try:
            with open(cache, "rb") as f:
                saved = pickle.load(f)
            if saved.get("stamp") == stamp:
                return saved["index"], saved["shadows"]
        except Exception:
            pass
    schema = Schema()
    index, shadows = {}, {}
    for i, p in enumerate(files):
        index[p.stem] = scan_pak(p, schema, shadows)
        if verbose and (i + 1) % 50 == 0:
            print(f"[paks] {i + 1}/{len(files)}", file=sys.stderr)
    cache.parent.mkdir(parents=True, exist_ok=True)
    with open(cache, "wb") as f:
        pickle.dump({"stamp": stamp, "index": index, "shadows": shadows}, f,
                    protocol=pickle.HIGHEST_PROTOCOL)
    return index, shadows


if __name__ == "__main__":
    import pso_paks
    idx = pso_paks.load_index()
    mats = sum(len(v) for v in idx.values())
    slots = sum(len(m.slots) for v in idx.values() for m in v)
    unused = sum(1 for v in idx.values() for m in v if not m.uses)
    print(f"{len(idx)} paks, {mats} materials, {slots} shader records, {unused} materials no mesh uses")
    by_set = defaultdict(int)
    for v in idx.values():
        for m in v:
            for u in m.uses:
                by_set[u[0]] += 1
    print("uses by set:", dict(sorted(by_set.items())))
