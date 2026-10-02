import mmap
import re
import struct
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SCHEMA = REPO_ROOT / "thirdparty" / "PKZLib" / "schemas" / "EOT-360.xml"
CHUNK_HEADER = struct.Struct(">IHHI")
HAS_CHILDREN = 0x0001

_SCALARS = {
    "u8": "B", "i8": "b", "u16": "H", "i16": "h", "u32": "I", "i32": "i", "u64": "Q",
    "i64": "q", "f16": "e", "f32": "f", "f64": "d", "char": "c",
}
_ARRAY_RE = re.compile(r"^(\w+)\[(\d+)\]$")
_CROSS_RE = re.compile(r"@?(0x[0-9A-Fa-f]+)\.(\w+)")
_FIELD_RE = re.compile(r"@(\w+)")


@dataclass
class Chunk:
    id: int
    version: int
    flags: int
    offset: int
    size: int
    depth: int
    parent: "Chunk | None"

    @property
    def end(self):
        return self.offset + self.size


def walk(data, start=0, end=None, depth=0, parent=None):
    if end is None:
        end = len(data)
    o = start
    while o + CHUNK_HEADER.size <= end:
        cid, ver, flags, size = CHUNK_HEADER.unpack_from(data, o)
        if o + CHUNK_HEADER.size + size > end:
            return
        c = Chunk(cid, ver, flags, o + CHUNK_HEADER.size, size, depth, parent)
        yield c
        if flags & HAS_CHILDREN:
            yield from walk(data, c.offset, c.end, depth + 1, c)
        o += CHUNK_HEADER.size + size


def children(data, chunk):
    if not chunk.flags & HAS_CHILDREN:
        return
    o = chunk.offset
    while o + CHUNK_HEADER.size <= chunk.end:
        cid, ver, flags, size = CHUNK_HEADER.unpack_from(data, o)
        if o + CHUNK_HEADER.size + size > chunk.end:
            return
        yield Chunk(cid, ver, flags, o + CHUNK_HEADER.size, size, chunk.depth + 1, chunk)
        o += CHUNK_HEADER.size + size


def open_pak(path):
    f = open(path, "rb")
    return mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)


class _Field:
    __slots__ = ("name", "offset", "type", "struct", "count", "size", "cond", "elem", "n")

    def __init__(self, el):
        self.name = el.get("name")
        self.offset = el.get("offset", "next")
        self.type = el.get("type")
        self.struct = el.get("struct")
        self.count = el.get("count")
        self.size = el.get("size")
        self.cond = el.get("if")
        self.elem, self.n = None, 1
        if self.type:
            m = _ARRAY_RE.match(self.type)
            if m:
                self.elem, self.n = m.group(1), int(m.group(2))
            elif self.type in _SCALARS:
                self.elem = self.type


class _Struct:
    def __init__(self, name, size, fields):
        self.name, self.size, self.fields = name, size, fields
        self.compiled = None


class Schema:
    def __init__(self, path=DEFAULT_SCHEMA):
        root = ET.parse(path).getroot()
        self.flag_sets, self.enums, self.structs, self.chunks, self.chunk_names = {}, {}, {}, {}, {}
        for fs in root.iter("flagSet"):
            self.flag_sets[fs.get("name")] = [(int(b.get("mask"), 0), b.get("name"))
                                              for b in fs.iter("bit")]
        for en in root.iter("enum"):
            self.enums[en.get("name")] = {int(v.get("value"), 0): v.get("name")
                                          for v in en.iter("value")}
        for st in root.iter("struct"):
            size = st.get("size")
            s = _Struct(st.get("name"), int(size, 0) if size else None,
                        [_Field(f) for f in st.findall("field")])
            self.structs[s.name] = s
        for ch in root.iter("chunk"):
            cid = int(ch.get("id"), 0)
            self.chunk_names[cid] = ch.get("name")
            self.chunks[cid] = [_Field(f) for f in ch.findall("field")]
        for s in self.structs.values():
            self._compile(s)

    def chunk_id(self, name):
        for cid, n in self.chunk_names.items():
            if n == name:
                return cid
        raise KeyError(name)

    def flag_names(self, flag_set, value):
        return [n for mask, n in self.flag_sets.get(flag_set, []) if value & mask == mask and mask]

    def _compile(self, s):
        if s.size is None:
            return
        fmt, names, pos = [">"], [], 0
        for f in s.fields:
            if f.cond or f.struct or f.count or not f.elem or not f.offset.isdigit():
                return
            off = int(f.offset)
            if off < pos:
                return
            if off > pos:
                fmt.append(f"{off - pos}x")
            if f.elem == "char":
                fmt.append(f"{f.n}s")
                names.append((f.name, "s", 1))
                pos = off + f.n
            else:
                code = _SCALARS[f.elem]
                fmt.append(f"{f.n}{code}")
                names.append((f.name, "a" if f.type != f.elem else "v", f.n))
                pos = off + f.n * struct.calcsize(">" + code)
        if pos > s.size:
            return
        if pos < s.size:
            fmt.append(f"{s.size - pos}x")
        s.compiled = (struct.Struct("".join(fmt)), names)

    @staticmethod
    def _eval(expr, rec, sib):
        def cross(m):
            return repr(sib.get(int(m.group(1), 16), {}).get(m.group(2), 0))
        e = _CROSS_RE.sub(cross, expr)
        e = _FIELD_RE.sub(lambda m: repr(rec.get(m.group(1), 0)), e)
        chunk = sib.get("chunk", {})
        e = re.sub(r"(?<![0-9xA-Fa-f'\"])\b([A-Za-z_]\w*)\b",
                   lambda m: repr(rec.get(m.group(1), chunk.get(m.group(1), 0))), e)
        return eval(e, {"__builtins__": {}}, {})

    def decode(self, chunk, data, siblings=None):
        fields = self.chunks.get(chunk.id)
        if fields is None:
            return None
        sib = dict(siblings or {})
        sib["chunk"] = {"version": chunk.version, "flags": chunk.flags}
        rec, _ = self._fields(fields, data, chunk.offset, chunk.end, sib)
        return rec

    def decode_struct(self, name, data, off, end, siblings=None):
        return self._struct(self.structs[name], data, off, end, siblings or {})

    def _struct(self, s, data, off, end, sib):
        if s.compiled:
            st, names = s.compiled
            if off + st.size > end:
                raise ValueError(f"{s.name} at {off:#x} runs past {end:#x}")
            vals = st.unpack_from(data, off)
            rec, i = {}, 0
            for name, kind, n in names:
                if kind == "v":
                    rec[name] = vals[i]
                elif kind == "s":
                    rec[name] = vals[i].split(b"\0", 1)[0].decode("latin-1")
                else:
                    rec[name] = vals[i:i + n]
                i += n
            return rec, st.size
        rec, used = self._fields(s.fields, data, off, end, sib)
        return rec, (s.size if s.size is not None else used)

    def _count(self, f, rec, sib, data, off, end):
        c = f.count
        if c is None:
            return None
        if c == "rest":
            return "rest"
        if c.startswith("sum:"):
            lst, fld = c[4:].split(".")
            return sum(x.get(fld, 0) for x in rec.get(lst, []))
        if c.startswith("until:"):
            return c
        return int(self._eval(c, rec, sib))

    def _fields(self, fields, data, base, end, sib):
        rec, cur = {}, base
        for f in fields:
            if f.cond and not self._eval(f.cond, rec, sib):
                continue
            if f.offset == "next":
                off = cur
            elif f.offset.startswith("@"):
                off = base + int(self._eval(f.offset, rec, sib))
            else:
                off = base + int(f.offset, 0)
            count = self._count(f, rec, sib, data, off, end)
            if f.struct:
                s = self.structs[f.struct]
                items, o = [], off
                if count is None:
                    item, used = self._struct(s, data, o, end, sib)
                    rec[f.name] = item
                    cur = o + used
                    continue
                if isinstance(count, str) and count.startswith("until:"):
                    key, val = count[6:].split("=")
                    val = int(val, 0)
                    while o < end:
                        item, used = self._struct(s, data, o, end, sib)
                        items.append(item)
                        o += used
                        if item.get(key) == val:
                            break
                else:
                    while (o < end) if count == "rest" else (len(items) < count):
                        item, used = self._struct(s, data, o, end, sib)
                        items.append(item)
                        o += used
                rec[f.name] = items
                cur = o
                continue
            if f.type == "bytes":
                n = int(self._eval(f.size, rec, sib)) if f.size else end - off
                rec[f.name] = bytes(data[off:off + n])
                cur = off + n
                continue
            if f.type == "cstring":
                z = data.find(b"\0", off, end)
                z = end if z < 0 else z
                rec[f.name] = bytes(data[off:z]).decode("latin-1")
                cur = z + 1
                continue
            code = _SCALARS[f.elem]
            width = struct.calcsize(">" + code)
            if f.elem == "char" and f.type != "char":
                raw = bytes(data[off:off + f.n])
                rec[f.name] = raw.split(b"\0", 1)[0].decode("latin-1")
                cur = off + f.n
                continue
            n = f.n
            if count is not None:
                if count == "rest":
                    count = (end - off) // (width * n)
                n *= count
            if off + n * width > end:
                raise ValueError(f"{f.name} at {off:#x} runs past {end:#x}")
            vals = struct.unpack_from(f">{n}{code}", data, off)
            if f.elem == "char":
                vals = tuple(v.decode("latin-1") for v in vals)
            rec[f.name] = vals[0] if (n == 1 and count is None) else vals
            cur = off + n * width
        return rec, cur - base
