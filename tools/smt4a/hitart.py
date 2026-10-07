"""Strike mark played over a party card when it takes damage (hit_0..2.png, 180x180)."""
import math, random, sys
from PIL import Image, ImageDraw, ImageFilter
S = 180
def layer():
    return Image.new('RGBA', (S * 4, S * 4), (0, 0, 0, 0))
def glow(im, r):
    return Image.alpha_composite(im.filter(ImageFilter.GaussianBlur(r)), im)
def slash(d, a, length, width, col):
    c = S * 2
    dx, dy = math.cos(a) * length, math.sin(a) * length
    nx, ny = -math.sin(a) * width, math.cos(a) * width
    d.polygon([(c - dx, c - dy), (c + nx, c + ny), (c + dx, c + dy), (c - nx, c - ny)], fill=col)
def star(d, n, r0, r1, col, rot=0):
    c = S * 2
    pts = []
    for i in range(n * 2):
        r = r1 if i % 2 == 0 else r0
        a = rot + i * math.pi / n
        pts.append((c + math.cos(a) * r, c + math.sin(a) * r))
    d.polygon(pts, fill=col)
out = sys.argv[1]
# frame 0: white-hot burst
im = layer(); d = ImageDraw.Draw(im)
star(d, 8, 40, 260, (255, 240, 200, 255), 0.2)
d.ellipse((S * 2 - 90, S * 2 - 90, S * 2 + 90, S * 2 + 90), fill=(255, 255, 255, 255))
im = glow(im, 30).resize((S, S), Image.LANCZOS); im.save(out + '/hit_0.png')
# frame 1: crossed slash with hot core
im = layer(); d = ImageDraw.Draw(im)
slash(d, -0.6, 330, 34, (255, 120, 80, 255)); slash(d, 0.75, 300, 26, (255, 170, 110, 255))
slash(d, -0.6, 300, 12, (255, 255, 240, 255)); slash(d, 0.75, 270, 9, (255, 255, 240, 255))
im = glow(im, 22).resize((S, S), Image.LANCZOS); im.save(out + '/hit_1.png')
# frame 2: sparks flying out, fading
im = layer(); d = ImageDraw.Draw(im)
random.seed(4)
for i in range(18):
    a = random.uniform(0, 2 * math.pi); r = random.uniform(170, 320); l = random.uniform(30, 70)
    c = S * 2
    x0, y0 = c + math.cos(a) * r, c + math.sin(a) * r
    x1, y1 = c + math.cos(a) * (r + l), c + math.sin(a) * (r + l)
    d.line((x0, y0, x1, y1), fill=(255, 200, 140, 230), width=10)
slash(d, -0.6, 300, 8, (255, 150, 110, 150)); slash(d, 0.75, 270, 6, (255, 170, 120, 130))
im = glow(im, 14).resize((S, S), Image.LANCZOS); im.save(out + '/hit_2.png')
