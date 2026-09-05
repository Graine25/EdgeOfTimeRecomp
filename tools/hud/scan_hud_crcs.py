#!/usr/bin/env python3
import argparse
import os
import re
import struct
import sys

TAG = struct.pack(">I", 0x138E)
NAME_OFFSET = 36
CRC_OFFSET = 12
TYPE_OFFSET = 16
UI_PAKS = ("legal.pak", "main.pak", "frontscreen.pak", "common.pak")

TYPE_NAMES = {0x19: "label"}

NAME_RE = re.compile(rb"^[A-Za-z0-9_][A-Za-z0-9_.\- ]*$")


def records(data):
    pos = 0
    while True:
        pos = data.find(TAG, pos)
        if pos < 0:
            return
        start = pos
        pos += 4
        if start + NAME_OFFSET + 4 > len(data):
            return
        crc, type_id = struct.unpack_from(">II", data, start + CRC_OFFSET)
        end = data.find(b"\0", start + NAME_OFFSET, start + NAME_OFFSET + 96)
        if end <= start + NAME_OFFSET:
            continue
        name = data[start + NAME_OFFSET:end]
        if not NAME_RE.match(name):
            continue
        yield crc, type_id, name.decode("ascii")


def pak_files(paths, want_all):
    for path in paths:
        if os.path.isdir(path):
            for entry in sorted(os.listdir(path)):
                if not entry.lower().endswith(".pak"):
                    continue
                if want_all or entry.lower() in UI_PAKS:
                    yield os.path.join(path, entry)
        else:
            yield path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--all", action="store_true", help="every pak, not just the UI ones")
    ap.add_argument("--toml", action="store_true", help="emit policy-table entries")
    args = ap.parse_args()

    found = {}
    clashes = []
    for path in pak_files(args.paths, args.all):
        with open(path, "rb") as fh:
            data = fh.read()
        count = 0
        for crc, type_id, name in records(data):
            count += 1
            prev = found.get(crc)
            if prev is None:
                found[crc] = (name, type_id, {os.path.basename(path)})
            else:
                prev[2].add(os.path.basename(path))
                if prev[0] != name:
                    clashes.append((crc, prev[0], name))
        print("%-44s %6d records" % (os.path.basename(path), count), file=sys.stderr)

    print("%d distinct CRCs" % len(found), file=sys.stderr)
    if clashes:
        print("%d CRC collisions (same CRC, different name)" % len(clashes), file=sys.stderr)
        for crc, a, b in clashes[:10]:
            print("  %#010x %s != %s" % (crc, a, b), file=sys.stderr)

    for crc, (name, type_id, paks) in sorted(found.items(), key=lambda kv: kv[1][0].lower()):
        if args.toml:
            kind = TYPE_NAMES.get(type_id, "sprite")
            print('0x%08X = { policy = "auto", name = "%s", type = "%s" }' % (crc, name, kind))
        else:
            print("%#010x  type=%#06x  %-56s %s" % (crc, type_id, name, ",".join(sorted(paks))))


if __name__ == "__main__":
    main()
