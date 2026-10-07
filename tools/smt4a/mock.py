import sys
from PIL import Image
U='/root/.claude/uploads/759ff5b5-2ca7-5d7b-86b8-817c3fc82596/'
def render(shot, regions, out, top_rect=(60,0,1800,1080)):
    im=Image.open(U+shot).convert('RGB'); top=im.crop((0,0,400,240)); bot=im.crop((40,240,360,480)).convert('RGBA')
    tx,ty,tw,th=top_rect; c=Image.new('RGBA',(1920,1080),(0,0,0,255)); c.paste(top.resize((tw,th),Image.BICUBIC),(tx,ty)); s=tw/400
    for (sx,sy,sw,sh),(dx,dy,dw,dh),op in regions:
        X,Y,W,H=int(tx+dx*s),int(ty+dy*s),int(dw*s),int(dh*s)
        r=bot.crop((sx,sy,sx+sw,sy+sh)).resize((W,H),Image.BICUBIC); r.putalpha(int(255*op)); c.alpha_composite(r,(X,Y))
    c.convert('RGB').save(out)
