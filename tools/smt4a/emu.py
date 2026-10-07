"""Drive the headless sr_tool and collect dumps."""
import subprocess, os, sys
sys.path.insert(0, '/home/claude/smt4a/tools')
from rec import Raw
TOOL = '/home/claude/build-lin/bin/Release/sr_tool'
USER = '/home/claude/smt4a/emu/user'
CXI = '/home/claude/smt4a/emu/smt4a.cxi'
OUT = '/home/claude/smt4a/emu/d'

def run(script, slot=2, settle=5):
    """script: list of commands; 'dump NAME' entries become files; returns {name: Raw}"""
    os.makedirs(OUT, exist_ok=True)
    lines = [f'load {slot}', f'run {settle}']
    names = []
    for c in script:
        if c.startswith('dump '):
            n = c.split()[1]; names.append(n); c = f'dump {OUT}/{n}.bin'
        lines.append(c)
    lines.append('quit')
    env = dict(os.environ, CITRA_ALLOW_STATE_BUILD_MISMATCH='1', CITRA_LOG_ROMFS='1')
    p = subprocess.run([TOOL, USER, CXI], input='\n'.join(lines) + '\n', capture_output=True, text=True, env=env, cwd='/home/claude/smt4a/emu')
    open(f'{OUT}/last_err.log', 'w').write(p.stderr)
    res = {}
    for n in names:
        res[n] = Raw(f'{OUT}/{n}.bin')
    return res

def romfs_reads():
    out = []
    for l in open(f'{OUT}/last_err.log'):
        if l.startswith('ROMFS '):
            a, b = l.split()[1:3]; out.append((int(a, 16), int(b, 16)))
    return out
