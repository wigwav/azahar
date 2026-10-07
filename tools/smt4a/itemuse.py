"""itemuse.txt: one line per item id - what the item needs to be usable in battle.
h: someone alive below max HP, m: below max MP, b: either, r: someone knocked out,
f: any of those (full restoration), c: cures an ailment, empty: always usable.
From item/ItemTable.tbb: consumables (table 1, id = index + 1) and the later additions
(table 6, id = 1951 + index); tag at +0x20, HP +0x34, MP +0x36, HP% +0x38,
bit 0x10 of +0x42 set on items usable on allies in battle."""
import struct, sys
ROM = '/mnt/user-data/uploads/dump/romfs/000400000019A200/'
d = open(ROM + 'item/ItemTable.tbb', 'rb').read()
codes = {}
def scan(table_off, first_id, count):
    base = table_off + 0x10
    for i in range(count):
        e = d[base + i * 0x6c: base + (i + 1) * 0x6c]
        tag = e[0x20:0x24]
        hp, mp, pct = struct.unpack_from('<HHH', e, 0x34)
        code = ''
        if e[0x42] & 0x10:   # usable on allies in battle (incense, cards, kits are not)
            if tag in (b'hpup', b'mpup'):
                h, m = bool(hp or pct), bool(mp)
                code = 'b' if h and m else 'h' if h else 'm' if m else ''
            elif tag == b'rest':
                code = 'r'
            elif tag == b'recv':
                code = 'c'
            elif tag == b'mris':
                code = 'f'
        codes[first_id + i] = code
scan(0x1300, 1, 60)
scan(0x272b0, 1951, 64)
out = '\n'.join(codes.get(i, '') for i in range(2100)) + '\n'
for p in sys.argv[1:]:
    open(p, 'w').write(out)
print({k: v for k, v in codes.items() if v})
