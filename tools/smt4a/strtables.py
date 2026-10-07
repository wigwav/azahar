import struct, unicodedata
ro=open('/home/claude/smt4a/seg_00526000.bin','rb').read(); da=open('/home/claude/smt4a/seg_00568000.bin','rb').read()
def s(va):
    if 0x526000<=va<0x568000: b=ro; o=va-0x526000
    elif 0x568000<=va<0x64d000: b=da; o=va-0x568000
    else: return None
    try:
        e=b.index(b'\0',o)
        return unicodedata.normalize('NFKC',b[o:e].decode('shift_jis'))
    except Exception: return None
def tables(minlen=8):
    out=[]; i=0; n=len(da)//4
    while i<n:
        j=i
        while j<n:
            v=struct.unpack_from('<I',da,4*j)[0]
            if not (0x526000<=v<0x568000) or s(v) is None: break
            j+=1
        if j-i>=minlen:
            out.append((0x568000+4*i,[s(struct.unpack_from('<I',da,4*k)[0]) for k in range(i,j)]))
        i=j+1
    return out
if __name__=='__main__':
    for va,t in tables():
        print(hex(va),len(t),t[:6])
