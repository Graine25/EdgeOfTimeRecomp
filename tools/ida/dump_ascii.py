import csv
import ida_auto
import ida_bytes
import ida_segment
import idc

ida_auto.auto_wait()
argv = idc.ARGV
out = argv[1]
min_len = int(argv[2]) if len(argv) > 2 else 6

with open(out, 'w', newline='', encoding='utf-8') as f:
    w = csv.writer(f)
    w.writerow(['ea', 'string'])
    n = ida_segment.get_segm_qty()
    for i in range(n):
        seg = ida_segment.getnseg(i)
        start, end = seg.start_ea, seg.end_ea
        data = ida_bytes.get_bytes(start, end - start)
        if not data:
            continue
        run_start = None
        for k, b in enumerate(data):
            printable = 32 <= b < 127
            if printable and run_start is None:
                run_start = k
            elif not printable and run_start is not None:
                if k - run_start >= min_len and b == 0:
                    w.writerow([f'{start + run_start:#x}', data[run_start:k].decode('ascii')])
                run_start = None
print('done')
