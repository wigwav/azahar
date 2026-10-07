import struct, numpy as np
from PIL import Image
def load(p):
    d=open(p,'rb').read(); o=0; scr=[]
    for _ in range(2):
        w,h=struct.unpack('<II',d[o:o+8]); o+=8
        a=np.frombuffer(d[o:o+w*h*3],np.uint8).reshape(h,w,3); o+=w*h*3
        scr.append(a)
    obj=d[o:o+0x12000]; o+=0x12000
    m=struct.unpack('<I',d[o:o+4])[0]; o+=4
    units=d[o:o+4*0x408]
    # screens are stored transposed (w=height) -> rotate to normal
    def fix(a):
        im=Image.fromarray(a)
        if a.shape[1]<a.shape[0]: im=im.transpose(Image.ROTATE_90)
        return im
    return dict(bottom=fix(scr[0]),top=fix(scr[1]),obj=obj,units=units,
                u32=lambda off: struct.unpack('<I',obj[off:off+4])[0],
                hp=[struct.unpack('<H',units[k*0x408+0xF2:k*0x408+0xF4])[0] for k in range(4)])
