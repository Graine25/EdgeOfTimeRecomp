import struct, mmap, sys, collections, re, os
root = r'D:\EOT_Extract\extracted'
def children(m, start, end):
    o = start
    while o + 12 <= end:
        t, a, s = struct.unpack_from('>IIi', m, o)
        if s < 0 or o + 12 + s > end: return
        yield t, a, o + 12, o + 12 + s
        o += 12 + s
for pack in sys.argv[1:]:
    p = os.path.join(root, pack + '.ext')
    f = open(p, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    t, a, s = struct.unpack_from('>IIi', m, 0)
    per = collections.defaultdict(collections.Counter); total = collections.Counter(); models = 0; envs = 0
    for tt, ta, ts, te in children(m, 12, 12 + s):
        if tt == 4:
            for kt, ka, ks, ke in children(m, ts, te):
                if kt != 5005: continue
                for st, sa, ss, se in children(m, ks, ke):
                    if st == 5006:
                        rtype = struct.unpack_from('>I', m, ss + 4)[0]
                        name = m[ss + 24:ss + 88].split(b'\0')[0].decode('ascii', 'replace')
                        total[rtype] += 1
                        stem = re.sub(r'[_ ]?\d+$', '', name)
                        stem = re.split(r'[_#]', stem)[0] if rtype in (0x102,0x103) else re.sub(r'_\d+.*$','',name)
                        per[rtype][stem] += 1
        elif tt == 7:
            models += sum(1 for k in children(m, ts, te) if k[0] == 5005)
        elif tt == 48:
            envs += sum(1 for k in children(m, ts, te) if k[0] == 5005)
    print('=====', pack, 'models', models, 'envs', envs, {hex(k): v for k, v in sorted(total.items())})
    for rt in (0x101, 0x102, 0x103):
        print('  rtype', hex(rt), per[rt].most_common(28))
