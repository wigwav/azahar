"""Build a decrypted CXI for SMT4A from the RAM snapshot (code) and the RAM-cached RomFS metadata.
File data is filled in later (sparse) by fillromfs.py from staged dump files."""
import struct, json, os, sys
sys.path.insert(0, '/home/claude/smt4a/tools')
import romfsmeta as rm

OUT = sys.argv[1] if len(sys.argv) > 1 else '/home/claude/smt4a/emu/smt4a.cxi'
PID = 0x000400000019A200
seg = lambda va: open(f'/home/claude/smt4a/seg_{va:08x}.bin', 'rb').read()
text, ro, data = seg(0x100000), seg(0x526000), seg(0x568000)
assert len(text) == 0x426000 and len(ro) == 0x42000 and len(data) == 0xE5000, (len(text), len(ro), len(data))
code = text + ro + data

def al(x, a): return (x + a - 1) // a * a

# --- exheader (0x800) ---
exh = bytearray(0x800)
struct.pack_into('<8s', exh, 0, b'SDDS4G')
exh[0x0D] = 0  # flag: not compressed
struct.pack_into('<III', exh, 0x10, 0x100000, 0x426, 0x426000)
struct.pack_into('<I', exh, 0x1C, 0x40000)  # stack
struct.pack_into('<III', exh, 0x20, 0x526000, 0x42, 0x42000)
struct.pack_into('<III', exh, 0x30, 0x568000, 0xE5, 0xE5000)
struct.pack_into('<I', exh, 0x3C, 0)  # bss
struct.pack_into('<QQ', exh, 0x1C0, 0x80000, PID)  # system info: save size, jump id
caps = 0x200
struct.pack_into('<QI', exh, caps, PID, 2)
exh[caps + 0xE] = 0x30 & 0  # flags0
exh[caps + 0xF] = 0x30      # priority
struct.pack_into('<Q', exh, caps + 0x30, 0x19A2)  # ext save data id
exh[caps + 0x15F] = 0       # resource limit category: application
kd = [0xFC00022C, 0xFE000200, 0xFF000100] + [0xFFFFFFFF] * 25
struct.pack_into('<28I', exh, 0x370, *kd)
exh[0x400 + 0x200:0x400 + 0x400] = exh[0x200:0x400]

# --- exefs ---
exefs = bytearray(0x200)
struct.pack_into('<8sII', exefs, 0, b'.code', 0, len(code))
exefs += code
exefs += b'\0' * (al(len(exefs), 0x200) - len(exefs))

# --- romfs level 3 ---
dh, dm, fh, fmt = rm.tables()
o_dh = 0x28; o_dm = o_dh + len(dh); o_fh = o_dm + len(dm); o_fm = o_fh + len(fh)
assert al(o_fm + len(fmt), 0x10) == rm.DATA, hex(o_fm + len(fmt))
hdr = struct.pack('<10I', 0x28, o_dh, len(dh), o_dm, len(dm), o_fh, len(fh), o_fm, len(fmt), rm.DATA)
lvl3 = hdr + dh + dm + fh + fmt
files = rm.files()
lvl3_size = rm.DATA + max(f['off'] + f['size'] for f in files)

# --- layout ---
MU = 0x200
exh_off = 0x200
exefs_off = al(exh_off + 0x800, MU)
romfs_off = al(exefs_off + len(exefs), 0x1000)
romfs_size = al(0x1000 + lvl3_size, MU)
total = romfs_off + romfs_size

h = bytearray(0x200)
h[0x100:0x104] = b'NCCH'
struct.pack_into('<I', h, 0x104, total // MU)
struct.pack_into('<Q', h, 0x108, PID)
struct.pack_into('<HH', h, 0x110, 0x3030, 2)
struct.pack_into('<Q', h, 0x118, PID)
h[0x150:0x15A] = b'CTR-P-BFWE'
struct.pack_into('<I', h, 0x180, 0x400)
h[0x18C] = 2         # platform
h[0x18D] = 0x3       # data + executable, application
h[0x18E] = 0         # content unit size
h[0x18F] = 0x4       # no crypto
struct.pack_into('<II', h, 0x1A0, exefs_off // MU, len(exefs) // MU)
struct.pack_into('<II', h, 0x1A8, 1, 0)
struct.pack_into('<III', h, 0x1B0, romfs_off // MU, romfs_size // MU, 1)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, 'wb') as f:
    f.write(h); f.seek(exh_off); f.write(exh)
    f.seek(exefs_off); f.write(exefs)
    ivfc = bytearray(0x1000); ivfc[0:4] = b'IVFC'
    f.seek(romfs_off); f.write(ivfc); f.write(lvl3)
    f.truncate(total)
json.dump(dict(level3=romfs_off + 0x1000, data=rm.DATA), open(OUT + '.json', 'w'))
print('cxi', OUT, hex(total), 'level3 at', hex(romfs_off + 0x1000))
