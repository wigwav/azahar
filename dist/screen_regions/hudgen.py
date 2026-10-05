"""Generates the SMT4A battle HUD definition and a preview using real snapshot values."""
import struct, sys
from PIL import Image, ImageDraw, ImageFont
FONT='/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf'
names=open('/home/claude/smt4a/hud/names.txt').read().split('\n')

# File offsets in the snapshot (converted to game addresses once the memory map is known)
NANA=dict(hp=0x2e84d59, mp=0x2e84d5b, hpmax=0x2e84d65, mpmax=0x2e84d67, lv=0x2e84d99)
W0=0x2e84df9
def stock(k):
    o=W0+k*300
    return dict(hp=o+48, mp=o+50, hpmax=o+90, mpmax=o+92, id=o+146, lv=o+148)

CW=300; CH=150; GAP=12; X0=(1920-(4*CW+3*GAP))//2; Y0=918
ORANGE='#F0A030FF'; BLUE='#4FA8FFFF'; WHITE='#FFFFFFFF'; GREY='#A8A8B8FF'; DARK='#000000B0'

def hud_lines(addr):
    """addr(fileoffset)->binding string"""
    L=[]
    slots=[('Nanashi',NANA,None)]+[(None,stock(k),stock(k)['id']) for k in range(3)]
    for i,(fixed,f,idaddr) in enumerate(slots):
        x=X0+i*(CW+GAP); y=Y0
        L.append(f'image {x} {y} {CW} {CH} card.png')
        if fixed: L.append(f'text {x+26} {y+10} 34 {WHITE} left "{fixed}"')
        else: L.append(f'text {x+26} {y+10} 34 {WHITE} left "{{0}}" v={addr(idaddr,"u16")} names=names.txt')
        L.append(f'text {x+CW-30} {y+16} 24 {GREY} right "Lv {{0}}" v={addr(f["lv"],"u16")}')
        L.append(f'text {x+26} {y+58} 22 {ORANGE} left "HP"')
        L.append(f'text {x+CW-34} {y+48} 36 {WHITE} right "{{0}}" v={addr(f["hp"],"u16")}')
        L.append(f'bar {x+26} {y+92} {CW-66} 7 {ORANGE} {DARK} value={addr(f["hp"],"u16")} max={addr(f["hpmax"],"u16")}')
        L.append(f'text {x+26} {y+106} 22 {BLUE} left "MP"')
        L.append(f'text {x+CW-34} {y+96} 36 {WHITE} right "{{0}}" v={addr(f["mp"],"u16")}')
        L.append(f'bar {x+26} {y+140} {CW-66} 6 {BLUE} {DARK} value={addr(f["mp"],"u16")} max={addr(f["mpmax"],"u16")}')
    return L

def preview(lines, snapshot, base_img, regions, out):
    b=open(snapshot,'rb').read()
    def rd(tok):
        t,a=tok.split(':'); a=int(a,0); return struct.unpack_from('<H',b,a)[0]
    canvas=Image.new('RGBA',(1920,1080),(0,0,0,255))
    shot=Image.open(base_img).convert('RGBA'); top=shot.crop((0,0,400,240)); bot=shot.crop((40,240,360,480))
    canvas.paste(top.resize((1800,1080),Image.BICUBIC),(60,0))
    for (sx,sy,sw,sh),(dx,dy,dw,dh) in regions:
        canvas.alpha_composite(bot.crop((sx,sy,sx+sw,sy+sh)).resize((dw,dh),Image.BICUBIC),(dx,dy))
    d=ImageDraw.Draw(canvas)
    col=lambda h:tuple(int(h[i:i+2],16) for i in (1,3,5,7))
    import shlex
    for line in lines:
        t=shlex.split(line); k=t[0]
        kv=dict(x.split('=',1) for x in t if '=' in x and not x.startswith('"'))
        if k=='image':
            x,y,w,h=map(int,t[1:5]); im=Image.open('/home/claude/smt4a/hud/'+t[5]).resize((w,h),Image.BICUBIC); canvas.alpha_composite(im,(x,y))
        elif k=='bar':
            x,y,w,h=map(int,t[1:5]); d.rectangle([x,y,x+w,y+h],fill=col(t[6]))
            v=rd(kv['value']); m=rd(kv['max']); f=max(0,min(1,v/m)) if m else 0
            d.rectangle([x,y,x+int(w*f),y+h],fill=col(t[5]))
        elif k=='text':
            x,y=int(t[1]),int(t[2]); size=int(t[3]); al=t[5]; s=t[6]
            if 'v' in kv:
                v=rd(kv['v']); s=s.replace('{0}', names[v] if 'names' in kv else str(v))
            f=ImageFont.truetype(FONT,size); w=f.getlength(s)
            if al=='right': x-=w
            elif al=='center': x-=w/2
            d.text((x,y),s,font=f,fill=col(t[4]))
    canvas.convert('RGB').save(out)

if __name__=='__main__':
    fo=lambda a,t: f'{t}:{a:#x}'
    lines=hud_lines(fo)
    regions=[((100,220,220,20),(66,10,440,40)), ((0,40,320,180),(60,640,480,270))]
    preview(lines,'/home/claude/smt4a/state1.bin','/root/.claude/uploads/759ff5b5-2ca7-5d7b-86b8-817c3fc82596/b1b50848-image.png',regions,'/home/claude/smt4a/out/hud_preview.png')
    print('\n'.join(lines[:10]))
