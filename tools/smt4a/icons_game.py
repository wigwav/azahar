"""Command tiles, labels and element icons built from the game's own HD UI sprites
(battle_low001 glyphs/labels, element_icon sheet), styled like the in-game command bar."""
from PIL import Image, ImageDraw, ImageFilter
import numpy as np
PACK = '/home/claude/smt4a/pack/textures/000400000019A200/'
OUT = '/home/claude/smt4a/hud3/'
sheet = Image.open(PACK + 'battlelow/tex1_256x128_FD9DFDB47802F91E_13.png').convert('RGBA')
els = Image.open(PACK + 'icon/tex1_128x64_42661010597C4D4E_4.png').convert('RGBA')
GLYPH = {0: (527, 12, 596, 79), 1: (534, 114, 586, 174), 2: (616, 108, 695, 178), 3: (515, 202, 606, 274),
         4: (619, 202, 702, 283), 5: (609, 290, 702, 378), 6: (513, 292, 607, 375), 7: (514, 386, 602, 479)}
LABEL_Y = {0: (5, 57), 1: (116, 167), 2: (170, 224), 3: (224, 280), 4: (341, 394), 5: (284, 338), 6: (395, 448), 7: (448, 500)}
S = 4
T = 112

def tile(selected):
    im = Image.new('RGBA', (T * S, T * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    if selected:
        glow = Image.new('RGBA', im.size, (0, 0, 0, 0))
        ImageDraw.Draw(glow).rounded_rectangle([4 * S, 4 * S, (T - 4) * S, (T - 4) * S], 16 * S, fill=(90, 200, 255, 255))
        im.alpha_composite(glow.filter(ImageFilter.GaussianBlur(5 * S)))
        top, bot, edge = (70, 165, 255), (10, 60, 150), (190, 240, 255, 255)
    else:
        top, bot, edge = (240, 232, 214), (196, 182, 150), (110, 92, 66, 255)
    grad = Image.new('RGBA', im.size, (0, 0, 0, 0))
    g = np.zeros((T * S, T * S, 4), np.uint8)
    t = np.linspace(0, 1, T * S)[:, None]
    for c in range(3):
        g[..., c] = (top[c] * (1 - t) + bot[c] * t).astype(np.uint8)
    g[..., 3] = 255
    mask = Image.new('L', im.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([8 * S, 8 * S, (T - 8) * S, (T - 8) * S], 12 * S, fill=255)
    im.paste(Image.fromarray(g, 'RGBA'), (0, 0), mask)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([8 * S, 8 * S, (T - 8) * S, (T - 8) * S], 12 * S, outline=edge, width=3 * S)
    # inner highlight like the 3DS tiles
    hl = Image.new('RGBA', im.size, (0, 0, 0, 0))
    ImageDraw.Draw(hl).rounded_rectangle([12 * S, 12 * S, (T - 12) * S, (T - 62) * S], 9 * S, fill=(255, 255, 255, 46))
    im.alpha_composite(hl)
    return im

def sparkle(color):
    import math
    n = 72 * S
    im = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    cx = cy = n / 2
    sz = n * 0.5
    for (dx, dy, r) in ((0, 0, 1.0), (-0.55, -0.45, 0.45), (0.55, 0.5, 0.4)):
        x0, y0, rr = cx + dx * sz, cy + dy * sz, r * sz * 0.55
        pts = []
        for i in range(8):
            ang = i * math.pi / 4
            rad = rr if i % 2 == 0 else rr * 0.32
            pts.append((x0 + math.cos(ang) * rad, y0 + math.sin(ang) * rad))
        d.polygon(pts, fill=tuple(color) + (255,))
    return im

def glyph(k, color):
    if k == 0:  # Skill keeps the original sparkle glyph
        return sparkle(color)
    g = sheet.crop(GLYPH[k])
    a = np.asarray(g)[..., 3].astype(np.float32)
    col = np.zeros((g.height, g.width, 4), np.uint8)
    col[..., :3] = color
    col[..., 3] = a.astype(np.uint8)
    g = Image.fromarray(col, 'RGBA')
    box = 62 * S
    sc = box / max(g.width, g.height)
    return g.resize((int(g.width * sc), int(g.height * sc)), Image.LANCZOS)

for k in GLYPH:
    for sel in (False, True):
        im = tile(sel)
        gl = glyph(k, (255, 255, 255) if sel else (58, 46, 36))
        if sel:  # soft shadow under the white glyph
            sh = Image.new('RGBA', im.size, (0, 0, 0, 0))
            sh.alpha_composite(glyph(k, (0, 30, 80)), ((im.width - gl.width) // 2 + 2 * S, (im.height - gl.height) // 2 + 3 * S))
            im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(2 * S)))
        im.alpha_composite(gl, ((im.width - gl.width) // 2, (im.height - gl.height) // 2))
        im.resize((T, T), Image.LANCZOS).save(OUT + ('cmdsel_%d.png' if sel else 'cmd_%d.png') % k)
    y0, y1 = LABEL_Y[k]
    lab = sheet.crop((712, y0, 875, y1))
    bb = lab.getbbox()
    lab = lab.crop(bb)
    h = 30
    lab = lab.resize((max(1, int(lab.width * h / lab.height)), h), Image.LANCZOS)
    if k == 7:
        from PIL import ImageFont
        f = ImageFont.truetype('/usr/share/fonts/opentype/inter/Inter-Bold.otf', 26)
        lab = Image.new('RGBA', (120, 34), (0, 0, 0, 0))
        dd = ImageDraw.Draw(lab)
        dd.text((60, 17), 'Partner', font=f, fill=(255, 255, 255, 255), anchor='mm', stroke_width=3, stroke_fill=(0, 0, 0, 255))
        lab = lab.crop(lab.getbbox()); lab = lab.resize((int(lab.width * 24 / lab.height), 24), Image.LANCZOS)
    pad = Image.new('RGBA', (140, 30), (0, 0, 0, 0))
    pad.alpha_composite(lab.resize((min(lab.width, 140), lab.height)) if lab.width > 140 else lab, ((140 - min(lab.width, 140)) // 2, (30 - lab.height) // 2))
    pad.save(OUT + 'lbl_%d.png' % k)

# element icons: game element_icon sheet, 48px HD cells
CELL = {0: (3, 3), 1: (1, 2), 2: (1, 2), 3: (1, 4), 4: (0, 0), 5: (0, 1), 6: (0, 2), 7: (0, 3), 8: (1, 0), 9: (1, 1),
        10: (2, 3), 11: (0, 4), 12: (2, 4), 13: (3, 2), 14: (3, 0), 15: (3, 1), 16: (4, 1), 17: (3, 1), 18: (4, 2)}
for code, (r, c) in CELL.items():
    els.crop((c * 48, r * 48, c * 48 + 48, r * 48 + 48)).resize((32, 32), Image.LANCZOS).save(OUT + 'el_%d.png' % code)
print('ok')
