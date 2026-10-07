"""Portraits for the battle HUD straight from the game's bust-up art (tex/stex/stbustup).

NKMBaseTable.tbb (battle/) holds one 0xB4-byte record per demon from 0x30; the u16 at +0x9C is the
bust-up index the game itself uses (st_bu_enNNN). Writes bu/bu_NNN.png framed to the HUD portrait
box and bu_index.txt (line = demon id -> bust-up index).
"""
import struct, os, sys
import numpy as np
from PIL import Image
sys.path.insert(0, '/home/claude/smt4a/tools')
import stex
ROM = '/mnt/user-data/uploads/dump/romfs/000400000019A200/'
OUT = '/home/claude/smt4a/hud3/'
W, H = 300, 346
tbl = open(ROM + 'battle/NKMBaseTable.tbb', 'rb').read()
n = 1201
index = [struct.unpack_from('<H', tbl, 0x30 + i * 0xB4 + 0x9C)[0] for i in range(n)]
ok = set()
for idx in sorted(set(index)):
    p = ROM + f'tex/stex/stbustup/st_bu_en{idx:03d}.cmp'
    if not os.path.exists(p) or os.path.getsize(p) < 1200:
        continue
    try:
        im = stex.load(p)[-1]
    except Exception as e:
        print('skip', idx, e); continue
    a = np.asarray(im)[..., 3]
    ys, xs = np.nonzero(a > 24)
    if len(xs) < 200:
        continue
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    bw, bh = x1 - x0, y1 - y0
    # frame the upper body: crop height covers the head and torso of tall figures, whole art of wide ones
    ch = min(bh, max(bw * H / W, bh * 0.62))
    cw = ch * W / H
    cx = (x0 + x1) / 2
    box = (cx - cw / 2, y0 - ch * 0.04, cx + cw / 2, y0 - ch * 0.04 + ch)
    crop = im.crop(tuple(int(round(v)) for v in box)).resize((W, H), Image.LANCZOS)
    crop.quantize(256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE).save(OUT + f'bu/bu_{idx}.png', optimize=True)
    ok.add(idx)
open(OUT + 'bu_index.txt', 'w').write('\n'.join(str(i if i in ok else -1) for i in index) + '\n')
print(len(ok), 'portraits')
