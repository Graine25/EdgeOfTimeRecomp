import struct

import ida_auto
import ida_bytes
import ida_funcs
import ida_segment
import idc

ida_auto.auto_wait()
argv = idc.ARGV
out = argv[1]
disps = [int(x, 16) for x in argv[2].split(",")]
stores_only = len(argv) < 4 or argv[3] != "all"

OPS = {32: "lwz", 34: "lbz", 40: "lhz", 48: "lfs", 50: "lfd",
       36: "stw", 37: "stwu", 38: "stb", 44: "sth", 52: "stfs", 54: "stfd"}
STORES = {36, 37, 38, 44, 52, 54}

with open(out, "w", encoding="utf-8") as f:
    seg = ida_segment.get_first_seg()
    while seg:
        if seg.perm & ida_segment.SEGPERM_EXEC or idc.get_segm_name(seg.start_ea) in (".text", "text"):
            data = ida_bytes.get_bytes(seg.start_ea, seg.end_ea - seg.start_ea) or b""
            for off in range(0, len(data) - 3, 4):
                w = struct.unpack_from(">I", data, off)[0]
                op = w >> 26
                if op not in OPS or (stores_only and op not in STORES):
                    continue
                d = w & 0xFFFF
                if d not in disps:
                    continue
                ea = seg.start_ea + off
                fn = ida_funcs.get_func(ea)
                name = idc.get_func_name(fn.start_ea) if fn else "?"
                rt = (w >> 21) & 31
                ra = (w >> 16) & 31
                f.write(f"{ea:#x} {OPS[op]} r{rt}/f{rt}, {d:#x}(r{ra})  in {name}\n")
        seg = ida_segment.get_next_seg(seg.start_ea)
    f.write("done\n")
print("done")
