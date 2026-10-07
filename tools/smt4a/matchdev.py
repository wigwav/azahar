import cv2, numpy as np, os, sys, struct
sys.path.insert(0,'/home/claude/smt4a/tools')
from rec import Raw
from stex import load
D='/mnt/user-data/uploads/dump/romfs/000400000019A200/tex/stex/devbu/'
R=Raw('/home/claude/smt4a/emu/d/'+(sys.argv[1] if len(sys.argv)>1 else 'P2')+'.bin')
img=cv2.cvtColor(np.array(R.img),cv2.COLOR_RGB2GRAY)
cells=[img[58:112, 80*k+8:80*k+60] for k in range(4)]   # art region, avoiding tab/smirk/level overlays
cache='/home/claude/smt4a/emu/devbu_gray'
os.makedirs(cache,exist_ok=True)
best=[[] for _ in range(4)]
for f in sorted(os.listdir(D)):
    cp=os.path.join(cache,f+'.npy')
    if os.path.exists(cp): tex=np.load(cp)
    else:
        try: _,w,h,fmt,typ,im=load(D+f)
        except Exception: continue
        a=np.array(im); g=cv2.cvtColor(a[...,:3],cv2.COLOR_RGB2GRAY).astype(np.float32)
        g[a[...,3]<128]=0; tex=g; np.save(cp,tex)
    for s in (0.3,0.38,0.46,0.55):
        t=cv2.resize(tex,None,fx=s,fy=s,interpolation=cv2.INTER_AREA)
        for k in range(4):
            c=cells[k].astype(np.float32)
            if t.shape[0]<c.shape[0] or t.shape[1]<c.shape[1]: continue
            r=cv2.matchTemplate(t,c,cv2.TM_CCOEFF_NORMED)
            # local std of the texture windows: ignore flat (transparent) areas
            m1=cv2.boxFilter(t,-1,(c.shape[1],c.shape[0]),normalize=True,borderType=cv2.BORDER_CONSTANT)
            m2=cv2.boxFilter(t*t,-1,(c.shape[1],c.shape[0]),normalize=True,borderType=cv2.BORDER_CONSTANT)
            sd=np.sqrt(np.maximum(m2-m1*m1,0))
            oy,ox=c.shape[0]//2,c.shape[1]//2
            sd=sd[oy:oy+r.shape[0],ox:ox+r.shape[1]]
            r=np.where((sd[:r.shape[0],:r.shape[1]]>12)&np.isfinite(r),r,-1)
            best[k].append((float(r.max()),f,s))
for k in range(4):
    best[k].sort(reverse=True); print(k,best[k][:3])
