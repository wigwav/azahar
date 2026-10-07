from emu import *
import numpy as np, sys
from PIL import Image
N=int(sys.argv[1]); slot=int(sys.argv[2]); tag=sys.argv[3]
sc=[]
for i in range(N):
    sc+=['tap b 30','dump %s%d'%(tag,i),'tap a 20','tap a 20','tap a 20','run 320']
r=run(sc,slot=slot)
c=Image.new('RGB',(320*N,240))
for i in range(N):
    R=r['%s%d'%(tag,i)]; im=np.array(R.img).astype(int); c.paste(R.img,(i*320,0))
    obj=R.u32(R.u32(R.u32(0x5b3ce4)+0x384)+0x2f8)
    sm=[int(im[49,65+80*k,1]-max(im[49,65+80*k,0],im[49,65+80*k,2])>40) for k in range(4)]
    print(i,'active',R.u32(obj+0x554),'smirk',sm, flush=True)
c.save('/home/claude/smt4a/emu/%s.png'%tag)
import fillromfs as F; print(F.missing_from_log())
