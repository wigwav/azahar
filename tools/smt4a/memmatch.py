"""Match textures loaded in game memory against RomFS texture files (exact payload match)."""
import sys, os, struct, hashlib, glob
sys.path.insert(0, '/home/claude/smt4a/tools')
from rec import Raw
import stex, re
def mem_textures(dump):
    r = Raw(dump); out = {}
    for s in r.segs:
        va, data = s[0], s[2]
        for m in re.finditer(b'STEX', data):
            p = m.start()
            try: w, h, typ, fmt, size, off = struct.unpack_from('<IIIIII', data, p + 0x0C)
            except Exception: continue
            if w == h == 256 or (w == 512 and h == 512):
                pay = data[p + off:p + off + size]
                out[hashlib.md5(pay[:4096]).hexdigest()] = hex(va + p)
    return out
def file_hash(path):
    d = open(path, 'rb').read()
    if d[0] == 0x11 and d[4:8] != b'STEX': d = stex.lz11_decompress(d)
    w, h, typ, fmt, size, off = struct.unpack_from('<IIIIII', d, 0x0C)
    return hashlib.md5(d[off:off + 4096]).hexdigest()
if __name__ == '__main__':
    mem = mem_textures(sys.argv[1])
    for f in sys.argv[2:]:
        for p in sorted(glob.glob(f)):
            try: h = file_hash(p)
            except Exception: continue
            if h in mem: print(mem[h], p)
