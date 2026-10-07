"""Convert snapshot file offsets to guest virtual addresses using the memmap log lines.

Snapshot layout facts (state1.bin, decompressed save state):
  * game code VA 0x00100000 is at file offset CODE_FILE
  * FCRAM is one contiguous blob in the file, so file = FCRAM_FILE + fcram_offset
"""
import re, sys

CODE_FILE = 0x9965ab5


def load_map(log_text):
    vmas = []
    for m in re.finditer(r'memmap: va=([0-9A-F]{8}) size=([0-9A-F]{8}) fcram=([0-9A-F]{8})', log_text):
        vmas.append(tuple(int(x, 16) for x in m.groups()))
    # keep the last complete set (after the most recent save-state load)
    return vmas


def build(vmas):
    code = [v for v in vmas if v[0] <= 0x00100000 < v[0] + v[1]]
    if not code:
        raise SystemExit('no code VMA in map')
    va, size, fc = code[-1]
    fcram_file = CODE_FILE - (fc + (0x00100000 - va))

    def to_va(file_off):
        f = file_off - fcram_file
        for va, size, fc in vmas:
            if fc <= f < fc + size:
                return va + (f - fc)
        return None
    return fcram_file, to_va


if __name__ == '__main__':
    vmas = load_map(open(sys.argv[1], encoding='utf-8', errors='replace').read())
    base, to_va = build(vmas)
    print('fcram file base', hex(base), 'vmas', len(vmas))
    for off in sys.argv[2:]:
        v = to_va(int(off, 0))
        print(off, '->', hex(v) if v is not None else None)
