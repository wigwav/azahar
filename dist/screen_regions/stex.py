"""SMT IV Apocalypse STEX / CMP texture decoder (PICA200 formats).

STEX header (0x80 bytes, little endian):
  0x00 'STEX'   0x08 u32 ?(0x0DE1)  0x0C u32 width  0x10 u32 height
  0x14 u32 GL type (0x1401 ubyte, 0x8033 ushort_4444, 0x8034 5551, 0x8363 565)
  0x18 u32 GL format (0x6752 RGBA, 0x6754 RGB, 0x6756 A, 0x6757 L, 0x6758 LA,
                      0x675A ETC1, 0x675B ETC1A4)
  0x1C u32 data size   0x20 u32 data offset (0x80)   0x28 name[0x58]
.cmp = LZ11-compressed STEX.
"""
import struct, sys, os
import numpy as np
from PIL import Image


def lz11_decompress(d):
    assert d[0] == 0x11, "not LZ11"
    size = d[1] | d[2] << 8 | d[3] << 16
    p = 4
    if size == 0:
        size = struct.unpack_from('<I', d, 4)[0]; p = 8
    out = bytearray()
    while len(out) < size:
        flags = d[p]; p += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                b0 = d[p]; ind = b0 >> 4
                if ind == 0:
                    length = (((b0 & 0xF) << 4) | (d[p+1] >> 4)) + 0x11
                    disp = (((d[p+1] & 0xF) << 8) | d[p+2]) + 1; p += 3
                elif ind == 1:
                    length = (((b0 & 0xF) << 12) | (d[p+1] << 4) | (d[p+2] >> 4)) + 0x111
                    disp = (((d[p+2] & 0xF) << 8) | d[p+3]) + 1; p += 4
                else:
                    length = ind + 1
                    disp = (((b0 & 0xF) << 8) | d[p+1]) + 1; p += 2
                for _ in range(length):
                    out.append(out[-disp])
            else:
                out.append(d[p]); p += 1
    return bytes(out)


ETC_MOD = [[2, 8, -2, -8], [5, 17, -5, -17], [9, 29, -9, -29], [13, 42, -13, -42],
           [18, 60, -18, -60], [24, 80, -24, -80], [33, 106, -33, -106], [47, 183, -47, -183]]


def _etc1_block(v):
    """v: u64 ETC1 block -> 4x4x3 uint8 (pixel [y][x])."""
    flip = (v >> 32) & 1
    diff = (v >> 33) & 1
    t1 = (v >> 37) & 7
    t2 = (v >> 34) & 7
    if diff:
        r = (v >> 59) & 31; g = (v >> 51) & 31; b = (v >> 43) & 31
        dr = ((v >> 56) & 7); dg = ((v >> 48) & 7); db = ((v >> 40) & 7)
        dr = dr - 8 if dr > 3 else dr; dg = dg - 8 if dg > 3 else dg; db = db - 8 if db > 3 else db
        c1 = [r, g, b]; c2 = [(r + dr) & 31, (g + dg) & 31, (b + db) & 31]
        c1 = [(c << 3) | (c >> 2) for c in c1]; c2 = [(c << 3) | (c >> 2) for c in c2]
    else:
        c1 = [((v >> 60) & 15) * 17, ((v >> 52) & 15) * 17, ((v >> 44) & 15) * 17]
        c2 = [((v >> 56) & 15) * 17, ((v >> 48) & 15) * 17, ((v >> 40) & 15) * 17]
    out = np.zeros((4, 4, 3), np.uint8)
    for x in range(4):
        for y in range(4):
            i = x * 4 + y
            lsb = (v >> i) & 1; msb = (v >> (16 + i)) & 1
            idx = (msb << 1) | lsb
            sub2 = (y >= 2) if flip else (x >= 2)
            base, tab = (c2, t2) if sub2 else (c1, t1)
            m = ETC_MOD[tab][idx]
            out[y, x] = [min(255, max(0, c + m)) for c in base]
    return out


def decode_etc(data, w, h, alpha):
    img = np.zeros((h, w, 4), np.uint8); img[..., 3] = 255
    p = 0
    for ty in range(0, h, 8):
        for tx in range(0, w, 8):
            for by, bx in ((0, 0), (0, 4), (4, 0), (4, 4)):
                if alpha:
                    a = struct.unpack_from('<Q', data, p)[0]; p += 8
                v = struct.unpack_from('<Q', data, p)[0]; p += 8
                blk = _etc1_block(v)
                y0, x0 = ty + by, tx + bx
                img[y0:y0+4, x0:x0+4, :3] = blk
                if alpha:
                    for x in range(4):
                        for y in range(4):
                            img[y0+y, x0+x, 3] = ((a >> ((x * 4 + y) * 4)) & 15) * 17
    return img


def _morton(n=8):
    tbl = []
    for i in range(64):
        x = y = 0
        for b in range(3):
            x |= ((i >> (2*b)) & 1) << b
            y |= ((i >> (2*b+1)) & 1) << b
        tbl.append((x, y))
    return tbl
MORTON = _morton()


def decode_linear(data, w, h, fmt, typ):
    bpp = {(0x6752, 0x1401): 4, (0x6754, 0x1401): 3, (0x6752, 0x8033): 2, (0x6752, 0x8034): 2,
           (0x6754, 0x8363): 2, (0x6758, 0x1401): 2, (0x6757, 0x1401): 1, (0x6756, 0x1401): 1}[(fmt, typ)]
    img = np.zeros((h, w, 4), np.uint8)
    p = 0
    for ty in range(0, h, 8):
        for tx in range(0, w, 8):
            for (x, y) in MORTON:
                px = data[p:p+bpp]; p += bpp
                if (fmt, typ) == (0x6752, 0x1401): c = (px[3], px[2], px[1], px[0])
                elif (fmt, typ) == (0x6754, 0x1401): c = (px[2], px[1], px[0], 255)
                elif typ == 0x8033:
                    v = px[0] | px[1] << 8
                    c = (((v >> 12) & 15)*17, ((v >> 8) & 15)*17, ((v >> 4) & 15)*17, (v & 15)*17)
                elif typ == 0x8034:
                    v = px[0] | px[1] << 8
                    c = (((v >> 11) & 31)*255//31, ((v >> 6) & 31)*255//31, ((v >> 1) & 31)*255//31, 255*(v & 1))
                elif typ == 0x8363:
                    v = px[0] | px[1] << 8
                    c = (((v >> 11) & 31)*255//31, ((v >> 5) & 63)*255//63, (v & 31)*255//31, 255)
                elif fmt == 0x6758: c = (px[1], px[1], px[1], px[0])
                elif fmt == 0x6757: c = (px[0], px[0], px[0], 255)
                else: c = (255, 255, 255, px[0])
                img[ty+y, tx+x] = c
    return img


def load(path):
    d = open(path, 'rb').read()
    if d[0] == 0x11 and d[4:8] != b'STEX':
        d = lz11_decompress(d)
    assert d[:4] == b'STEX', path
    w, h, typ, fmt, size, off = struct.unpack_from('<IIIIII', d, 0x0C)
    name = d[0x28:0x80].split(b'\0')[0].decode('ascii', 'replace')
    data = d[off:off+size]
    if fmt in (0x675A, 0x675B):
        img = decode_etc(data, w, h, fmt == 0x675B)
    else:
        img = decode_linear(data, w, h, fmt, typ)
    return name, w, h, fmt, typ, Image.fromarray(img, 'RGBA')


if __name__ == '__main__':
    outdir = sys.argv[1]
    for p in sys.argv[2:]:
        name, w, h, fmt, typ, im = load(p)
        o = os.path.join(outdir, os.path.splitext(os.path.basename(p))[0] + '.png')
        im.save(o)
        print(f'{os.path.basename(p)}: {name} {w}x{h} fmt={fmt:#x} type={typ:#x} -> {o}')
