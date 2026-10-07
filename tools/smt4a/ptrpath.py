"""Find pointer chains from static .data/.bss to a target heap address."""
import numpy as np, struct
def u32arr(rec):
    return [(va, np.frombuffer(b[:len(b)//4*4], dtype='<u4')) for va, s, b in rec.segs]
def paths(rec, target, max_off=0x2000, depth=3):
    segs = u32arr(rec)
    results = []
    frontier = [(target, [])]
    for d in range(depth):
        nxt = []
        for tgt, chain in frontier:
            for va, arr in segs:
                m = (arr <= tgt) & (arr > tgt - max_off) & (arr >= 0x08000000)
                for i in np.nonzero(m)[0][:50]:
                    src = va + int(i) * 4
                    off = tgt - int(arr[i])
                    c = [(src, off)] + chain
                    if 0x568000 <= src < 0x64D000:
                        results.append(c)
                    else:
                        nxt.append((src, c))
        frontier = nxt[:400]
        if results: break
    return results
def fmt(c):
    s = f'[0x{c[0][0]:x}]+0x{c[0][1]:x}'
    for src, off in c[1:]:
        pass
    # chain: static -> ptr1 (+off1 is location of next pointer) ...
    expr = f'0x{c[0][0]:x}'
    for i, (src, off) in enumerate(c):
        expr = f'[{expr}]+0x{off:x}'
    return expr
