import sys, os, struct, zlib, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pakwriter import Pak, assemble, chunk, whole, build_header, kids, check_alignment

DLC1 = 'C:/Users/rieng/Documents/GitHub/reeot-dni/reference/model_import/dlc/_DLC001.ext'
BG = 'D:/EOT_Extract/extracted/BaseGameplay.ext'
GAL = 'D:/EOT_Extract/extracted/Gallery.ext'

def crc_name(name):
    return zlib.crc32(re.sub(r'\[.*?\]', '', name).upper().encode()) & 0xFFFFFFFF

def material_texture_crcs(chunk_bytes):
    out = []
    for m in re.finditer(rb'[ -~]{4,}\.(?:dds|tga)', chunk_bytes):
        o = m.start()
        if o >= 12:
            c = struct.unpack_from('>I', chunk_bytes, o - 12)[0]
            if c not in out:
                out.append(c)
    return out

def string_table(strings, tname='ReeotDLC002'):
    crcs = [crc_name(n) for n, _ in strings]
    data = b''; offsets = []
    for _, text in strings:
        offsets.append(len(data) // 2); data += text.encode('utf-16-be') + b'\0\0'
    header = struct.pack('>I', len(strings)) + struct.pack('>%dI' % len(crcs), *crcs) + struct.pack('>I', len(strings))
    for off in offsets:
        header += struct.pack('>I', 1) + struct.pack('>Iff', off, 0.0, -1.0)
    table = chunk(0x5DC, 4, children=[chunk(0x5DD, 4, header), chunk(0x5E1, 2, data)])
    res = chunk(0x138D, 1, children=[build_header(crc_name(tname), 0x82, 0x1F, 0, 0, 0xFFFFFFFF, tname), table])
    return chunk(0x12, 4, children=[chunk(0x139A, 1, struct.pack('>I', 1)), res])

def main():
    variant, out = sys.argv[1], sys.argv[2]
    dlc = Pak(DLC1)
    libs = {}
    for lib in (0x09, 0x07, 0x05, 0x04):
        libs[lib] = [dlc.item(lib, crc=h['crc'], ) for (ep, es, h) in dlc.entries(lib)]
    fixed = {}
    if variant == 'id':
        for (p, t, a, b, size) in dlc.top:
            if t == 0x11:
                level = bytearray(whole(dlc.d, p)); struct.pack_into('>I', level, 12, 0xBBA); fixed[0x11] = bytes(level)
    if variant == 'cards':
        gal = Pak(GAL)
        for src_name, new_name in (('Gallery_AltSuit_SelectionAmazingPeterParkerCivilian_DA[Hi]', 'Reeot_AltSuit_SelectionCasualMask_DA[Hi]'),
                                   ('Gallery_AltSuit_SelectedAmazingPeterParkerCivilian_DA[Hi]', 'Reeot_AltSuit_SelectedCasualMaskSmall_DA[Hi]')):
            libs[0x09].append(gal.item(0x09, name=src_name, rename=(crc_name(new_name), new_name)))
    if variant == 'master':
        import build_suit_pak as B
        bg = Pak(BG)
        libs[0x07].append(bg.item(0x07, name='SpiderManAmazing_Casual[Hi]', rename=(crc_name('ReeotCasual[Hi]'), 'ReeotCasual[Hi]')))
        ep, es, h = dlc.entry(0x04, name='DLC001Master')
        go_type = None
        for (p, t, a, b, size) in kids(dlc.d, ep + 12, ep + 12 + es):
            if t == 0x1F4:
                for (gp, gt, ga, gb, gs) in kids(dlc.d, p + 12, p + 12 + size):
                    if gt == 0x1FF: go_type = whole(dlc.d, gp)
        ghosts = [h2['crc'] for (_, _, h2) in dlc.entries(0x07) if 'Ghost' in h2['name']]
        hiers = [h2['crc'] for (_, _, h2) in dlc.entries(0x05)]
        cards = {h2['name']: h2['crc'] for (_, _, h2) in dlc.entries(0x09)}
        large = next(c for n, c in cards.items() if 'Selection' in n and 'Prodigy' in n)
        small = next(c for n, c in cards.items() if 'Selected' in n and 'Prodigy' in n)
        fixed[0x04] = B.build_master(go_type, crc_name('ReeotCasual[Hi]'), large, small, ghosts, hiers)
        fixed[0x12] = string_table([(B.STRING_NAME, B.SUIT_TITLE), (B.STRING_DESC, B.SUIT_DESC)])
    if variant in ('bggeo', 'bggeotex'):
        bg = Pak(BG)
        src = 'SpiderManAmazing_Casual[Hi]'
        libs[0x07].append(bg.item(0x07, name=src, rename=(crc_name('ReeotCasual[Hi]'), 'ReeotCasual[Hi]')))
        if variant == 'bggeotex':
            ep, es, h = bg.entry(0x07, name=src)
            for tc in material_texture_crcs(whole(bg.d, ep)):
                tep, tes, th = bg.entry(0x09, crc=tc)
                new = 'Reeot_' + th['name']
                libs[0x09].append(bg.item(0x09, crc=tc, rename=(crc_name(new), new)))
                print('texture %s -> %s' % (th['name'], new))
    if variant == 'strings':
        fixed[0x12] = string_table([('REEOT_TEST_NAME', 'TEST SUIT'), ('REEOT_TEST_DESC', 'A test description.')])
    counts = {0x09: 58} if variant == 'dlc001' else None
    data = assemble(dlc, libs, fixed, counts)
    open(out, 'wb').write(data)
    bad = check_alignment(data)
    print('%s: %d bytes (DLC001 is %d), %d unaligned chunks %s' % (out, len(data), len(dlc.d), len(bad), bad[:5]))
    if variant == 'dlc001':
        same = data == dlc.d
        print('byte-identical to DLC001:', same)
        if not same:
            n = min(len(data), len(dlc.d))
            i = next((k for k in range(n) if data[k] != dlc.d[k]), n)
            print('first difference at %#x: ours %s theirs %s' % (i, data[i:i+16].hex(), dlc.d[i:i+16].hex()))

if __name__ == '__main__':
    main()
