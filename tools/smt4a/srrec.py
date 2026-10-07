"""Reader for Screen Regions recordings (SRREC1, zstd): guest memory ranges + bottom screen."""
import struct, subprocess, sys
from PIL import Image
sys.path.insert(0, '/home/claude/smt4a/tools')
class SRRec:
    def __init__(self, path):
        tmp = '/tmp/claude-0/_srrec.bin'
        subprocess.run(['/home/claude/smt4a/tools/unzstd', path, tmp, '0'], check=True)
        raw = open(tmp, 'rb').read()
        assert raw[:6] == b'SRREC1', path
        self.frame, = struct.unpack_from('<I', raw, 8)
        self.profile = raw[12:28].split(b'\0')[0].decode()
        n, = struct.unpack_from('<I', raw, 28); p = 32
        self.segs = []
        for _ in range(n):
            va, size = struct.unpack_from('<II', raw, p); p += 8
            self.segs.append((va, size, raw[p:p + size])); p += size
        w, h = struct.unpack_from('<II', raw, p); p += 8
        self.img = Image.frombytes('RGB', (w, h), raw[p:p + w * h * 3])
    def mem(self, va, n):
        for base, size, data in self.segs:
            if base <= va and va + n <= base + size:
                return data[va - base:va - base + n]
        return None
    def u8(self, va): m = self.mem(va, 1); return m[0] if m else 0
    def u16(self, va): m = self.mem(va, 2); return struct.unpack('<H', m)[0] if m else 0
    def u32(self, va): m = self.mem(va, 4); return struct.unpack('<I', m)[0] if m else 0
