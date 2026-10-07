import numpy as np
def arrays(recs, dtype='<u1'):
    """yields (va_base, [array per rec]) for each segment"""
    for si in range(len(recs[0].segs)):
        va = recs[0].segs[si][0]
        yield va, [np.frombuffer(r.segs[si][2], dtype=dtype) for r in recs]
def find(recs, values, dtype='<u1', limit=60):
    """addresses whose value in recs[i] == values[i] for all i"""
    size = np.dtype(dtype).itemsize
    out = []
    for va, arrs in arrays(recs, dtype):
        m = np.ones(len(arrs[0]), bool)
        for a, v in zip(arrs, values):
            m &= (a == v)
        for i in np.nonzero(m)[0][:limit]:
            out.append(va + int(i) * size)
    return out
