import os, re, sys
from texram import payload
from rec import Raw
R=Raw('/home/claude/smt4a/emu/d/'+sys.argv[1]+'.bin')
D='/mnt/user-data/uploads/dump/romfs/000400000019A200/tex/stex/'+sys.argv[2]+'/'
def key(p):
    best=None;bs=0
    for o in range(0,len(p)-256,4096):
        w=p[o:o+256]; s=len(set(w))
        if s>bs: bs=s; best=(o,w)
    return best
for f in sorted(os.listdir(D)):
    try: pay,info=payload(D+f)
    except Exception as e: continue
    o,k=key(pay)
    for va,s,b in R.segs:
        i=b.find(k)
        if i>=0: print(f, hex(va+i-o)); break
