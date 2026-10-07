"""Helpers for the battle snapshot (state3.bin)."""
import struct, numpy as np
X = 0x9964482 - 0x77DA000          # file offset of FCRAM byte 0
HEAP_FC = 0x6C00000                # heap VA 0x08000000 -> FCRAM offset
SEGS = [(0x100000, 0x426000, 0x77DA000), (0x526000, 0x42000, 0x7798000), (0x568000, 0xE5000, 0x76B3000)]
d = open('/home/claude/smt4a/state3.bin', 'rb').read()
fc = np.frombuffer(d[X:X + 0x8000000], dtype='<u4')

def fco(va):
    for v, s, f in SEGS:
        if v <= va < v + s: return f + va - v
    if 0x08000000 <= va < 0x10000000: return HEAP_FC + va - 0x08000000
    if 0x30000000 <= va < 0x38000000: return va - 0x30000000
    if 0x14000000 <= va < 0x1C000000: return va - 0x14000000
    raise ValueError(hex(va))

def va_of(o):
    for v, s, f in SEGS:
        if f <= o < f + s: return v + o - f
    if HEAP_FC <= o < 0x76B3000: return 0x08000000 + o - HEAP_FC
    return 0x30000000 + o

def rd(va, fmt): return struct.unpack_from('<' + fmt, d, X + fco(va))
def u32(va): return rd(va, 'I')[0]
def u16(va): return rd(va, 'H')[0]
def raw(va, n): o = X + fco(va); return d[o:o + n]

def refs(lo, hi, limit=200):
    idx = np.nonzero((fc >= lo) & (fc < hi))[0]
    return [(va_of(int(i) * 4), int(fc[i])) for i in idx[:limit]]

def dump(va, n, w=16):
    for a in range(va, va + n, w * 2):
        print(hex(a), ' '.join('%04x' % x for x in rd(a, '%dH' % w)))
