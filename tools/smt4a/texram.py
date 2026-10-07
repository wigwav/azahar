import struct, re, sys
sys.path.insert(0,'/home/claude/smt4a/tools')
from stex import lz11_decompress
def payload(path):
    d=open(path,'rb').read()
    if d[0]==0x11 and d[4:8]!=b'STEX': d=lz11_decompress(d)
    w,h,typ,fmt,size,off=struct.unpack_from('<IIIIII',d,0x0C)
    return d[off:off+size], (w,h,fmt,typ)
def find(rec, pay):
    out=[]
    for va,s,b in rec.segs:
        for m in re.finditer(re.escape(pay[:512]),b): out.append(va+m.start())
    return out
