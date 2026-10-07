"""Reconstruct the game's RomFS (level 3) layout from the metadata the game caches in RAM."""
from mem3 import *
import struct, json
DH, DHS = 0x8103000, 0xe5c
DM, DMS = 0x8103e5c, 0x89c8
FH, FHS = 0x810c824, 0x1588c
FM, FMS = 0x81220b0, 0x143740
DATA = 0x162820

def tables():
    return raw(DH, DHS), raw(DM, DMS), raw(FH, FHS), raw(FM, FMS)

def dir_path(dm, off):
    parts = []
    while off != 0:
        p, s, cd, cf, h, n = struct.unpack_from('<6I', dm, off)
        parts.append(dm[off+24:off+24+n].decode('utf-16-le'))
        off = p
    return '/'.join(reversed(parts))

def files():
    dh, dm, fh, fmt = tables()
    out = []
    o = 0
    while o < len(fmt):
        p, s, doff, size, h, n = struct.unpack_from('<IIQQII', fmt, o)
        name = fmt[o+0x20:o+0x20+n].decode('utf-16-le')
        d = dir_path(dm, p)
        out.append(dict(path=(d + '/' if d else '') + name, off=doff, size=size))
        o += 0x20 + ((n + 3) & ~3)
    return out

if __name__ == '__main__':
    fs = files()
    json.dump(fs, open('/home/claude/smt4a/romfs_layout.json', 'w'))
    print(len(fs), 'files; image size', hex(DATA + max(f['off'] + f['size'] for f in fs)))
    print(sum(f['size'] for f in fs) / 1e6, 'MB')
