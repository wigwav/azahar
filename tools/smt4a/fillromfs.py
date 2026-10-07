"""Copy staged RomFS dump files into the sparse CXI at their original offsets.
Also prints which RomFS reads of the last run hit files that are still missing."""
import json, os, sys
CXI = '/home/claude/smt4a/emu/smt4a.cxi'
SRC = '/mnt/user-data/uploads/dump/romfs/000400000019A200/'
meta = json.load(open(CXI + '.json'))
L = json.load(open('/home/claude/smt4a/romfs_layout.json'))
have_path = '/home/claude/smt4a/emu/have.json'
have = set(json.load(open(have_path))) if os.path.exists(have_path) else set()
base = meta['level3'] + meta['data']
n = 0
with open(CXI, 'r+b') as f:
    for e in L:
        p = SRC + e['path']
        if e['path'] in have or not os.path.exists(p):
            continue
        b = open(p, 'rb').read()
        if len(b) != e['size']:
            print('size mismatch', e['path'], len(b), e['size']); continue
        f.seek(base + e['off']); f.write(b); have.add(e['path']); n += 1
json.dump(sorted(have), open(have_path, 'w'))
print('filled', n, 'total present', len(have))

def missing_from_log(log='/home/claude/smt4a/emu/d/last_err.log'):
    reads = [tuple(int(x, 16) for x in l.split()[1:3]) for l in open(log) if l.startswith('ROMFS ')]
    starts = sorted((meta['data'] + e['off'], e) for e in L)
    import bisect
    keys = [s for s, _ in starts]
    miss = set()
    for off, ln in reads:
        i = bisect.bisect_right(keys, off) - 1
        while i < len(starts) and i >= 0 and starts[i][0] < off + ln:
            s, e = starts[i]
            if s + e['size'] > off and e['path'] not in have:
                miss.add(e['path'])
            i += 1
    return sorted(miss)
if __name__ == '__main__':
    m = missing_from_log()
    print('missing from last run:', len(m)); print('\n'.join(m[:80]))

def stage_list(paths):
    b = 'C:\\Users\\super\\AppData\\Roaming\\Azahar\\dump\\romfs\\000400000019A200\\'
    return json.dumps([b + p.replace('/', '\\') for p in paths])
