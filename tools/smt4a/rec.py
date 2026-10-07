"""Reader for Screen Regions recorder files (rec_*.zst)."""
import struct, subprocess, numpy as np
from PIL import Image
UNZ = '/home/claude/smt4a/tools/unzstd'

class Rec:
    def __init__(self, path):
        raw = subprocess.run([UNZ, path, '/dev/stdout', '0'], capture_output=True).stdout
        assert raw[:6] == b'SRREC1', path
        self.frame = struct.unpack_from('<I', raw, 8)[0]
        self.profile = raw[12:28].split(b'\0')[0].decode()
        n = struct.unpack_from('<I', raw, 28)[0]; o = 32
        self.segs = []
        for _ in range(n):
            va, size = struct.unpack_from('<II', raw, o); o += 8
            self.segs.append((va, size, raw[o:o+size])); o += size
        w, h = struct.unpack_from('<II', raw, o); o += 8
        self.img = Image.frombytes('RGB', (w, h), raw[o:o+w*h*3])
    def mem(self, va, n):
        for v, s, b in self.segs:
            if v <= va and va + n <= v + s: return b[va-v:va-v+n]
        return None
    def u8(self, va): return self.mem(va, 1)[0]
    def u16(self, va): return struct.unpack('<H', self.mem(va, 2))[0]
    def u32(self, va): return struct.unpack('<I', self.mem(va, 4))[0]

class Raw(Rec):
    """Uncompressed sr_tool dump (SRRAW1): segments + bottom + top screens."""
    def __init__(self, path):
        raw = open(path, 'rb').read()
        assert raw[:6] == b'SRRAW1', path
        self.frame = struct.unpack_from('<I', raw, 8)[0]
        n = struct.unpack_from('<I', raw, 28)[0]; o = 32
        self.segs = []
        for _ in range(n):
            va, size = struct.unpack_from('<II', raw, o); o += 8
            self.segs.append((va, size, raw[o:o+size])); o += size
        imgs = []
        for _ in range(2):
            w, h = struct.unpack_from('<II', raw, o); o += 8
            imgs.append(Image.frombytes('RGB', (w, h), raw[o:o+w*h*3])); o += w*h*3
        self.img, self.top = imgs
    def both(self):
        c = Image.new('RGB', (400, 480)); c.paste(self.top, (0, 0)); c.paste(self.img, (40, 240)); return c
