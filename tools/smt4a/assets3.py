"""Original HD HUD art for the SMT4A single-screen battle UI (drawn from scratch with PIL)."""
from PIL import Image, ImageDraw, ImageFont, ImageFilter
import math, os
OUT = '/home/claude/smt4a/hud3/'
S = 4  # supersampling
INTER = '/usr/share/fonts/opentype/inter/Inter-SemiBold.otf'
if not os.path.exists(INTER):
    import glob
    INTER = [f for f in glob.glob('/usr/share/fonts/**/Inter-SemiBold.[ot]tf', recursive=True)][0]
INTER_B = INTER.replace('SemiBold', 'Bold') if os.path.exists(INTER.replace('SemiBold', 'Bold')) else INTER

def canvas(w, h):
    return Image.new('RGBA', (w * S, h * S), (0, 0, 0, 0))
def save(im, name, w, h):
    im.resize((w, h), Image.LANCZOS).save(OUT + name)

def vgrad(w, h, top, bot):
    g = Image.new('RGBA', (w, h))
    for y in range(h):
        t = y / max(h - 1, 1)
        g.paste(tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(4)), (0, y, w, y + 1))
    return g

def rounded(w, h, r, top, bot, outline=None, ow=0):
    im = canvas(w, h)
    mask = Image.new('L', im.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w * S - 1, h * S - 1], r * S, fill=255)
    g = vgrad(w * S, h * S, top, bot)
    im.paste(g, (0, 0), mask)
    if outline:
        ImageDraw.Draw(im).rounded_rectangle([ow * S // 2, ow * S // 2, w * S - 1 - ow * S // 2, h * S - 1 - ow * S // 2],
                                             r * S, outline=outline, width=ow * S)
    return im

# ---------------------------------------------------------------- font atlas (Inter)
def font_atlas(path, px=64):
    f = ImageFont.truetype(path, px)
    chars = [chr(c) for c in range(32, 127)]
    W = 1024
    x = y = 0
    rowh = int(px * 1.35)
    atlas = Image.new('RGBA', (W, 1024), (255, 255, 255, 0))
    d = ImageDraw.Draw(atlas)
    lines = [f'line {px}']
    asc, desc = f.getmetrics()
    for ch in chars:
        bbox = f.getbbox(ch)
        adv = int(round(f.getlength(ch)))
        gw = max(1, bbox[2] - bbox[0]) + 4
        if x + gw >= W:
            x = 0; y += rowh
        d.text((x + 2 - bbox[0], y), ch, font=f, fill=(255, 255, 255, 255))
        # glyph box: full line height so yoff = 0
        lines.append(f'{ord(ch)} {x} {y} {gw} {rowh} {bbox[0] - 2} 0 {adv}')
        x += gw + 2
    atlas = atlas.crop((0, 0, W, y + rowh + 2))
    atlas.save(OUT + 'font.png')
    open(OUT + 'font.txt', 'w').write('\n'.join(lines) + '\n')

# ---------------------------------------------------------------- command icons
def glyph(d, kind, cx, cy, s, col):
    """Simple vector glyphs, s = half size (supersampled px)."""
    w = max(2, int(s * 0.12))
    if kind == 0:  # Skill: sparkles
        for (dx, dy, r) in ((0, 0, 1.0), (-0.55, -0.45, 0.45), (0.55, 0.5, 0.4)):
            x0, y0, rr = cx + dx * s, cy + dy * s, r * s * 0.55
            pts = []
            for k in range(8):
                a = k * math.pi / 4
                rad = rr if k % 2 == 0 else rr * 0.32
                pts.append((x0 + math.cos(a) * rad, y0 + math.sin(a) * rad))
            d.polygon(pts, fill=col)
    elif kind == 1:  # Item: potion bottle
        d.rounded_rectangle([cx - s * 0.45, cy - s * 0.15, cx + s * 0.45, cy + s * 0.8], s * 0.2, fill=col)
        d.rectangle([cx - s * 0.18, cy - s * 0.6, cx + s * 0.18, cy - s * 0.1], fill=col)
        d.rectangle([cx - s * 0.28, cy - s * 0.78, cx + s * 0.28, cy - s * 0.58], fill=col)
    elif kind == 2:  # Talk: speech bubble
        d.rounded_rectangle([cx - s * 0.8, cy - s * 0.65, cx + s * 0.8, cy + s * 0.35], s * 0.3, fill=col)
        d.polygon([(cx - s * 0.35, cy + s * 0.3), (cx - s * 0.55, cy + s * 0.8), (cx, cy + s * 0.3)], fill=col)
    elif kind == 3:  # Swap: two arrows in a circle
        bb = [cx - s * 0.7, cy - s * 0.7, cx + s * 0.7, cy + s * 0.7]
        d.arc(bb, 200, 340, fill=col, width=w * 2)
        d.arc(bb, 20, 160, fill=col, width=w * 2)
        for a, sgn in ((340, 1), (160, -1)):
            r = math.radians(a)
            px, py = cx + math.cos(r) * s * 0.7, cy + math.sin(r) * s * 0.7
            d.polygon([(px - s * 0.25, py - sgn * s * 0.05), (px + s * 0.25, py - sgn * s * 0.05), (px, py + sgn * s * 0.3)], fill=col)
    elif kind == 4:  # Flee: arrow leaving a doorway
        d.rectangle([cx - s * 0.8, cy - s * 0.75, cx - s * 0.6, cy + s * 0.75], fill=col)
        d.rectangle([cx - s * 0.8, cy - s * 0.75, cx - s * 0.1, cy - s * 0.58], fill=col)
        d.rectangle([cx - s * 0.8, cy + s * 0.58, cx - s * 0.1, cy + s * 0.75], fill=col)
        d.rectangle([cx - s * 0.35, cy - s * 0.12, cx + s * 0.35, cy + s * 0.12], fill=col)
        d.polygon([(cx + s * 0.3, cy - s * 0.42), (cx + s * 0.85, cy), (cx + s * 0.3, cy + s * 0.42)], fill=col)
    elif kind == 5:  # Fusion: two circles merging into a star
        d.ellipse([cx - s * 0.85, cy - s * 0.35, cx - s * 0.15, cy + s * 0.35], outline=col, width=w * 2)
        d.ellipse([cx + s * 0.15, cy - s * 0.35, cx + s * 0.85, cy + s * 0.35], outline=col, width=w * 2)
        d.ellipse([cx - s * 0.2, cy - s * 0.2, cx + s * 0.2, cy + s * 0.2], fill=col)
    elif kind == 6:  # Next: chevron arrow
        d.rectangle([cx - s * 0.8, cy - s * 0.15, cx + s * 0.2, cy + s * 0.15], fill=col)
        d.polygon([(cx + s * 0.05, cy - s * 0.55), (cx + s * 0.8, cy), (cx + s * 0.05, cy + s * 0.55)], fill=col)
    elif kind == 7:  # Partner: two figures
        for dx, sc in ((-0.35, 0.85), (0.35, 1.0)):
            x0 = cx + dx * s
            d.ellipse([x0 - s * 0.2 * sc, cy - s * 0.7 * sc, x0 + s * 0.2 * sc, cy - s * 0.3 * sc], fill=col)
            d.rounded_rectangle([x0 - s * 0.3 * sc, cy - s * 0.22 * sc, x0 + s * 0.3 * sc, cy + s * 0.75], s * 0.15, fill=col)

CMD_NAMES = ['Skill', 'Item', 'Talk', 'Swap', 'Flee', 'Fusion', 'Next', 'Partner']

def command_icons():
    W = H = 112
    for k in range(8):
        for sel in (False, True):
            if sel:
                im = rounded(W, H, 16, (120, 240, 120, 255), (30, 150, 60, 255), (220, 255, 210, 255), 3)
                col = (255, 255, 255, 255)
            else:
                im = rounded(W, H, 16, (232, 232, 228, 240), (176, 178, 176, 240), (255, 255, 255, 200), 2)
                col = (52, 56, 62, 255)
            d = ImageDraw.Draw(im)
            glyph(d, k, W * S / 2, H * S / 2 - 8 * S, 30 * S, col)
            if sel:
                glow = im.filter(ImageFilter.GaussianBlur(10 * S))
            save(im, f'cmd{"sel" if sel else ""}_{k}.png', W, H)
    open(OUT + 'smt4a_cmds.txt', 'w').write('\n'.join(CMD_NAMES) + '\n')

# ---------------------------------------------------------------- element icons
ELEMENTS = {  # code: (fill, symbol)
    0: ((150, 150, 160), '*'), 1: ((235, 140, 60), 'P'), 2: ((235, 140, 60), 'P'), 3: ((150, 160, 170), 'G'),
    4: ((235, 70, 40), 'F'), 5: ((80, 200, 240), 'I'), 6: ((245, 215, 40), 'E'), 7: ((90, 210, 110), 'W'),
    8: ((250, 245, 200), 'L'), 9: ((160, 70, 200), 'D'), 10: ((235, 235, 235), 'A'), 11: ((255, 120, 200), 'A'),
    12: ((180, 120, 220), 'S'), 13: ((110, 200, 120), '+'), 14: ((110, 220, 130), '+'), 15: ((90, 150, 240), 'B'),
    16: ((90, 160, 240), 'B'), 17: ((190, 90, 220), 'X'), 18: ((140, 140, 140), '?'),
}
def element_icons():
    W = 30
    f = ImageFont.truetype(INTER_B, 19 * S)
    for code, (fill, sym) in ELEMENTS.items():
        im = canvas(W, W)
        d = ImageDraw.Draw(im)
        d.rounded_rectangle([S, S, W * S - S, W * S - S], 6 * S, fill=fill + (255,), outline=(255, 255, 255, 220), width=S * 2)
        tw = d.textlength(sym, font=f)
        d.text(((W * S - tw) / 2, 3 * S), sym, font=f, fill=(20, 20, 24, 255) if sum(fill) > 450 else (255, 255, 255, 255))
        save(im, f'el_{code}.png', W, W)

# ---------------------------------------------------------------- panels
def panels():
    save(rounded(400, 210, 10, (12, 14, 20, 225), (8, 9, 14, 215), (255, 255, 255, 40), 1), 'list_panel.png', 400, 210)
    # selected row: red accent like the reference, light bar
    im = rounded(392, 36, 6, (40, 120, 150, 235), (22, 70, 92, 235), (110, 220, 255, 255), 2)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([3 * S, 6 * S, 8 * S, 30 * S], 2 * S, fill=(120, 240, 150, 255))
    save(im, 'row_sel.png', 392, 36)
    save(rounded(392, 32, 4, (255, 255, 255, 16), (255, 255, 255, 6)), 'row.png', 392, 32)
    save(rounded(430, 92, 10, (12, 14, 20, 225), (8, 9, 14, 215), (255, 255, 255, 40), 1), 'desc_panel.png', 430, 92)
    # party cards
    for active in (False, True):
        top = (34, 40, 52, 235) if active else (20, 22, 28, 220)
        bot = (14, 16, 22, 235) if active else (10, 11, 14, 220)
        im = rounded(244, 140, 8, top, bot, (110, 220, 255, 255) if active else (255, 255, 255, 50), 3 if active else 1)
        d = ImageDraw.Draw(im)
        d.rectangle([12 * S, 44 * S, 232 * S, 46 * S], fill=(255, 255, 255, 40))
        save(im, 'card_active.png' if active else 'card.png', 244, 140)
    # portrait frame (border only; portrait drawn under it)
    W, H = 320, 270
    im = canvas(W, H)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([2 * S, 2 * S, W * S - 2 * S, H * S - 2 * S], 12 * S, outline=(110, 220, 255, 255), width=3 * S)
    save(im, 'portrait_frame.png', W, H)
    # name plate under the portrait
    im = rounded(320, 54, 8, (16, 18, 24, 235), (8, 9, 12, 235), (110, 220, 255, 255), 2)
    save(im, 'name_plate.png', 320, 54)
    save(rounded(440, 78, 10, (16, 18, 24, 225), (8, 9, 12, 225), (255, 255, 255, 50), 1), 'partner_panel.png', 440, 78)
    # bar backgrounds
    save(rounded(200, 12, 4, (0, 0, 0, 200), (0, 0, 0, 200)), 'bar_bg.png', 200, 12)

# ---------------------------------------------------------------- button chips
def chips():
    f = ImageFont.truetype(INTER_B, 22 * S)
    for b, col in (('L', (80, 80, 90)), ('R', (80, 80, 90)), ('A', (210, 60, 60)), ('B', (240, 190, 40)),
                   ('X', (60, 120, 220)), ('Y', (50, 170, 90))):
        W = 36
        im = canvas(W, W)
        d = ImageDraw.Draw(im)
        if b in 'LR':
            d.rounded_rectangle([S, 4 * S, W * S - S, W * S - 4 * S], 6 * S, fill=col + (255,), outline=(255, 255, 255, 230), width=2 * S)
        else:
            d.ellipse([S, S, W * S - S, W * S - S], fill=col + (255,), outline=(255, 255, 255, 230), width=2 * S)
        tw = d.textlength(b, font=f)
        d.text(((W * S - tw) / 2, 4 * S), b, font=f, fill=(255, 255, 255, 255))
        save(im, f'btn_{b}.png', W, W)
    save(rounded(150, 44, 22, (10, 12, 16, 200), (10, 12, 16, 200), (255, 255, 255, 60), 1), 'chip.png', 150, 44)

def smirk():
    f = ImageFont.truetype(INTER_B, 22 * S)
    W, H = 96, 34
    im = rounded(W, H, 17, (110, 245, 110, 255), (30, 160, 60, 255), (230, 255, 220, 255), 2)
    d = ImageDraw.Draw(im)
    tw = d.textlength('SMIRK', font=f)
    d.text(((W * S - tw) / 2, 4 * S), 'SMIRK', font=f, fill=(255, 255, 255, 255))
    save(im, 'smirk.png', W, H)

if __name__ == '__main__':
    font_atlas(INTER)
    command_icons(); element_icons(); panels(); chips(); smirk()
    print(sorted(os.listdir(OUT)))
