"""Find code that loads given literal values (ARM pc-relative ldr + Thumb16)."""
import numpy as np, sys
CODE = 0x100000
c = open('/home/claude/smt4a/seg_00100000.bin', 'rb').read()
w = np.frombuffer(c[:len(c)//4*4], dtype='<u4')
h = np.frombuffer(c[:len(c)//2*2], dtype='<u2')
def pools(lo, hi):
    return {CODE+int(i)*4: int(w[i]) for i in np.nonzero((w >= lo) & (w < hi))[0]}
arm_idx = np.nonzero((w & 0x0F7F0000) == 0x051F0000)[0]
arm_src = CODE + arm_idx*4
imm = (w[arm_idx] & 0xFFF).astype(np.int64)
up = (w[arm_idx] >> 23) & 1
arm_tgt = arm_src + 8 + np.where(up == 1, imm, -imm)
def loaders(addr_set):
    out = []
    m = np.isin(arm_tgt, list(addr_set))
    for s, t in zip(arm_src[m], arm_tgt[m]): out.append(('arm', int(s), int(t)))
    return out
if __name__ == '__main__':
    lo, hi = int(sys.argv[1], 0), int(sys.argv[2], 0)
    p = pools(lo, hi)
    for kind, s, t in loaders(p.keys()):
        print(kind, hex(s), 'loads', hex(p[t]), '(pool', hex(t) + ')')
