"""Build an Azahar custom-texture pack (HD UI) for SMT IV Apocalypse straight from RomFS STEX files.

For each .stex/.cmp: decode, compute Azahar's texture hash (CityHash64 of the raw PICA payload),
upscale 4x with Real-ESRGAN (NumPy), and write
  <out>/textures/000400000019A200/<group>/tex1_<W>x<H>_<HASH>_<FMT>.png
"""
import os, sys, struct, subprocess, json, time
sys.path.insert(0, os.path.dirname(__file__))
import stex, esrgan_np

TITLE = '000400000019A200'
HERE = os.path.dirname(os.path.abspath(__file__))
CITY = os.path.join(HERE, 'cityhash_tool')
# (GL format, GL type) -> Azahar VideoCore::PixelFormat
PIXFMT = {(0x6752, 0x1401): 0, (0x6754, 0x1401): 1, (0x6752, 0x8034): 2, (0x6754, 0x8363): 3,
          (0x6752, 0x8033): 4, (0x6758, 0x1401): 5, (0x6757, 0x1401): 7, (0x6756, 0x1401): 8,
          (0x675A, 0x1401): 12, (0x675B, 0x1401): 13}


def payload(path):
    d = open(path, 'rb').read()
    if d[0] == 0x11 and d[4:8] != b'STEX':
        d = stex.lz11_decompress(d)
    w, h, typ, fmt, size, off = struct.unpack_from('<IIIIII', d, 0x0C)
    return d, w, h, typ, fmt, size, off


def city(data):
    tmp = '/tmp/claude-0/_hash.bin'
    open(tmp, 'wb').write(data)
    return subprocess.check_output([CITY, tmp, '0', str(len(data))]).decode().strip()


def main(src_root, out_root, files, scale_limit=1024):
    net = esrgan_np.Net(esrgan_np.load_pth(os.path.join(HERE, '..', 'models', 'anime6b.pth')))
    manifest = []
    for f in files:
        group = os.path.basename(os.path.dirname(f))
        d, w, h, typ, fmt, size, off = payload(f)
        h64 = city(d[off:off + size])
        pf = PIXFMT.get((fmt, typ))
        if pf is None:
            print('skip (format)', f, hex(fmt), hex(typ), flush=True)
            continue
        name = f'tex1_{w}x{h}_{h64}_{pf}.png'
        dst_dir = os.path.join(out_root, 'textures', TITLE, group)
        os.makedirs(dst_dir, exist_ok=True)
        dst = os.path.join(dst_dir, name)
        manifest.append({'source': os.path.relpath(f, src_root), 'file': f'{group}/{name}',
                         'size': [w, h], 'format': pf})
        if os.path.exists(dst):
            continue
        t = time.time()
        _, _, _, _, _, im = stex.load(f)
        if max(w, h) > scale_limit:
            out = im.resize((w * 4, h * 4), 3)  # bicubic fallback for very large sheets
        else:
            out = esrgan_np.upscale_image(net, im)
        out.save(dst, optimize=True)
        print(f'{group}/{os.path.basename(f)} {w}x{h} -> {name} ({time.time() - t:.0f}s)', flush=True)
    json.dump(manifest, open(os.path.join(out_root, 'manifest.json'), 'w'), indent=1)
    pack = {'author': 'SMT4A single-screen mod', 'version': '0.1.0',
            'description': 'HD (4x Real-ESRGAN) UI textures for Shin Megami Tensei IV: Apocalypse',
            'options': {'skip_mipmap': False, 'flip_png_files': True, 'use_new_hash': True}}
    json.dump(pack, open(os.path.join(out_root, 'textures', TITLE, 'pack.json'), 'w'), indent=1)


if __name__ == '__main__':
    src_root = sys.argv[1]
    files = [os.path.join(dp, fn) for dp, _, fns in os.walk(src_root) for fn in sorted(fns)
             if fn.endswith(('.stex', '.cmp'))]
    files.sort(key=lambda p: os.path.getsize(p))  # small/quick first
    main(src_root, sys.argv[2], files)
