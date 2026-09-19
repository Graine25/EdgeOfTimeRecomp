import sys, os, struct, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, r'C:\Users\rieng\Documents\GitHub\reeot-dni\reference\eot_tools\SPIDER-MAN EDGE OF TIME TOOLS\tools')
import x360_tex as X
from tex_pack import *
from pakwriter import Pak, kids, chunk, build_header

bg = Pak('D:/EOT_Extract/extracted/BaseGameplay.ext')

def retail_levels(name):
    ep, es, h = bg.entry(0x09, name=name)
    post = bg.postload(h['dataoff'])
    q = next(k for k in kids(post, 12, len(post)) if k[1] == 0x195)
    (df, W, H, n, levels), err = X.parse_texture(post, q[0], q[4])
    desc = struct.unpack_from('>16I', post, q[0] + 12)
    return df, [(w, hh, np.asarray(X.decode_level(df, w, hh, lin))) for (w, hh, lin) in levels], desc, post[q[0] + 12:q[0] + 12 + q[4]]

for name, fmt, nm in (('SMA_CEO_Civilian_D[Hi]', FMT_DXT1_GAMMA, False), ('SMA_CEO_Civilian_N[Hi]', FMT_DXT5_NORMAL, True), ('SMA_CEO_Civilian_S[Hi]', FMT_DXT1_LINEAR, False)):
    df, levels, desc, raw = retail_levels(name)
    src = levels[0][2].astype(np.uint8)
    t = time.time()
    chain = mip_chain(src, len(levels))
    payload = texture_data(chain, fmt, normal_map=nm)
    dt = time.time() - t
    ours = struct.unpack_from('>16I', payload)
    print('%s: retail desc %s' % (name, ' '.join('%08x' % x for x in desc[:16])))
    print('%s  ours   desc %s  (size retail %d ours %d, %.1fs)' % (' ' * len(name), ' '.join('%08x' % x for x in ours[:16]), len(raw), len(payload), dt))
    fake = chunk(0x195, 7, payload)
    (df2, W2, H2, n2, lv2), err = X.parse_texture(fake, 0, len(fake) - 12)
    for k, (w, hh, lin) in enumerate(lv2):
        dec = np.asarray(X.decode_level(df2, w, hh, lin)).astype(np.float32)
        ref = chain[k].astype(np.float32)
        if ref.shape[-1] == 3: dec = dec[..., :3]
        mse = ((dec - ref) ** 2).mean()
        print('   level %d %dx%d: PSNR vs source %.1f dB' % (k, w, hh, 10 * np.log10(255 ** 2 / max(mse, 1e-6))))
    if not nm:
        pass
