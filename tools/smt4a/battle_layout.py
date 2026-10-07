"""SMT4A single-screen battle layout: the game's own bottom-screen UI pieces (rendered at
HD with their background removed) re-arranged on native HD panels, like the reference.
Generates panel art, the ini sections and a preview from a headless-emulator dump."""
import sys
sys.path.insert(0, '/home/claude/smt4a/tools')
from PIL import Image, ImageDraw, ImageFilter
import numpy as np

HUD = '/home/claude/smt4a/hud2/'
OBJ = '[[[0x5b3ce4]+0x384]+0x2f8]'            # battle UI object
LIST_OPEN = f'u32:{OBJ}+0x14e8!0xffffffff'
LIST_CLOSED = f'u32:{OBJ}+0x14e8=0xffffffff'
ACTIVE = lambda k: f'u32:{OBJ}+0x554={k}'

def panel(w, h, name, accent=True, radius=14, alpha=200):
    """Dark glass panel with a thin light border (supersampled)."""
    S = 4
    im = Image.new('RGBA', (w * S, h * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, w * S - 1, h * S - 1], radius * S, fill=(6, 9, 14, alpha))
    d.rounded_rectangle([S, S, w * S - 1 - S, h * S - 1 - S], radius * S - S, outline=(120, 190, 220, 90), width=S * 2)
    if accent:
        d.rounded_rectangle([S * 3, S * 3, S * 9, h * S - S * 3], S * 2, fill=(70, 200, 235, 200))
    im = im.resize((w, h), Image.LANCZOS)
    im.save(HUD + name)
    return name

# (src rect on the 320x240 bottom screen) -> (dst rect on the 1920x1080 canvas), condition
R = []   # regions: (src, dst, cond, blend)
H = []   # hud lines (drawn under the regions)

def region(src, dst, cond=None, blend='screen'):
    R.append((src, dst, cond, blend))

def hud(line):
    H.append(line)

# --- hints, top-left ------------------------------------------------------------
hud(f'image 26 12 828 58 {panel(828, 58, "p_hints.png", accent=False, radius=12, alpha=170)}')
region((0, 222, 320, 18), (40, 18, 800, 45))

# --- left column: active member portrait (selecting a command) or the skill/item list -----
hud(f'image 26 286 262 300 {panel(262, 300, "p_portrait.png")} if={LIST_CLOSED}')
for k in range(4):
    region((80 * k + 1, 40, 78, 90), (44, 296, 234, 270), (ACTIVE(k), LIST_CLOSED), 'normal')
hud(f'image 26 318 560 300 {panel(560, 300, "p_list.png")} if={LIST_OPEN}')
region((64, 40, 190, 98), (44, 330, 532, 274), LIST_OPEN)

# --- description under the left column ---------------------------------------------
hud(f'image 26 628 640 84 {panel(640, 84, "p_desc.png")}')
region((12, 191, 300, 28), (44, 638, 600, 56))

# --- command row, centred above the party ------------------------------------------
hud(f'image 556 752 808 122 {panel(808, 122, "p_cmd.png", accent=False, radius=16, alpha=150)}')
region((0, 146, 320, 46), (560, 756, 800, 115))

# --- party cards, bottom centre ------------------------------------------------------
hud(f'image 388 910 1144 162 {panel(1144, 162, "p_party.png", accent=False)}')
region((0, 0, 320, 40), (400, 920, 1120, 140))
# smirk badges from the game's own smirk marker (kept while the skill list covers it)
badge = Image.open('/home/claude/smt4a/emu/smirk_badge.png') if False else None
for k in range(4):
    x = 400 + k * 280 + 280 - 92
    hud(f'image {x} 870 80 46 smirk.png if=pixg:{65 + 80 * k},49>40 hold={LIST_OPEN}')

# --- partner, bottom-right -------------------------------------------------------------
hud(f'image 1540 1010 360 52 {panel(360, 52, "p_partner.png", accent=False, radius=10, alpha=170)} if={LIST_CLOSED}')
region((0, 129, 320, 19), (1548, 1016, 344, 40), LIST_CLOSED)

def ini():
    out = ['[profile battle]', 'hide_bottom = 1', 'hud_under = 1']
    for (sx, sy, sw, sh), (dx, dy, dw, dh), cond, blend in R:
        conds = cond if isinstance(cond, tuple) else ((cond,) if cond else ())
        opts = f'space=window blend={blend}' + ''.join(f' {k}={c}' for k, c in zip(('if', 'and'), conds))
        out.append(f'region = {sx} {sy} {sw} {sh} -> {dx} {dy} {dw} {dh} {opts}')
    out += ['', '[hud battle]'] + H
    return '\n'.join(out) + '\n'

def make_smirk():
    """'SMIRK' tag in the game's colours."""
    from PIL import ImageFont
    w, h = 80, 46
    im = Image.new('RGBA', (w * 4, h * 4), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([8, 8, w * 4 - 8, h * 4 - 8], 40, fill=(30, 190, 70, 235), outline=(220, 255, 200, 255), width=8)
    f = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf', 84)
    t = 'SMIRK'
    tw = d.textlength(t, font=f)
    d.text(((w * 4 - tw) / 2, 46), t, font=f, fill=(255, 255, 255, 255))
    im.resize((w, h), Image.LANCZOS).save(HUD + 'smirk.png')

def eval_cond(cond, rec):
    if not cond:
        return True
    if isinstance(cond, tuple):
        return all(eval_cond(c, rec) for c in cond)
    if cond.startswith('pixg:'):
        xy, thr = cond[5:].split('>')
        x, y = map(int, xy.split(','))
        r, g, b = rec.img.getpixel((x, y))
        return g - max(r, b) > int(thr)
    obj = rec.u32(rec.u32(rec.u32(0x5b3ce4) + 0x384) + 0x2f8)
    if '+0x14e8' in cond:
        v = rec.u32(obj + 0x14e8)
        return (v != 0xffffffff) if '!' in cond else (v == 0xffffffff)
    if '+0x554=' in cond:
        return rec.u32(obj + 0x554) == int(cond.split('=')[-1])
    raise ValueError(cond)

def preview(rec, out, scale_bottom=5):
    W, Hh = 1920, 1080
    canvas = Image.new('RGBA', (W, Hh), (0, 0, 0, 255))
    top = rec.top.resize((1800, 1080), Image.BICUBIC).convert('RGBA')
    canvas.alpha_composite(top, (60, 0))
    bot = rec.img.convert('RGB').resize((320 * scale_bottom, 240 * scale_bottom), Image.BICUBIC)
    # HUD (under)
    for line in H:
        t = line.split()
        x, y, w, h, f = int(t[1]), int(t[2]), int(t[3]), int(t[4]), t[5]
        conds = [o[3:] for o in t[6:] if o.startswith('if=')]
        if conds and not eval_cond(conds[0], rec):
            continue
        im = Image.open(HUD + f).convert('RGBA').resize((w, h), Image.LANCZOS)
        canvas.alpha_composite(im, (x, y))
    # regions
    base = np.array(canvas.convert('RGB')).astype(np.float32) / 255
    for (sx, sy, sw, sh), (dx, dy, dw, dh), cond, blend in R:
        if not eval_cond(cond, rec):
            continue
        s = scale_bottom
        crop = bot.crop((sx * s, sy * s, (sx + sw) * s, (sy + sh) * s)).resize((dw, dh), Image.LANCZOS)
        c = np.array(crop).astype(np.float32) / 255
        d = base[dy:dy + dh, dx:dx + dw]
        base[dy:dy + dh, dx:dx + dw] = c + d * (1 - c) if blend == 'screen' else c
    Image.fromarray((base * 255).astype(np.uint8)).save(out)

if __name__ == '__main__':
    make_smirk()
    open('/home/claude/smt4a/hud2/battle_profile.ini', 'w').write(ini())
    from rec import Raw
    D = '/home/claude/smt4a/emu/d/'
    for n in sys.argv[1:]:
        preview(Raw(D + n + '.bin'), f'/home/claude/smt4a/emu/prev_{n}.png')
    print(ini())
