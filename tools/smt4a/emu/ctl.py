"""Drive a battle headlessly: pass every party member's turn (Next), film enemy turns.
ctl.py SLOT MAXSTEPS [pre-commands...]"""
import subprocess, sys, struct
sys.path.insert(0, '/tmp/claude-0/emu')
from fr import load

D = '/tmp/claude-0/emu'
F = D + '/film'


def cmd(*c):
    subprocess.run([D + '/drv.sh', 'cmd', *c], stdout=subprocess.DEVNULL, timeout=900)


n = 0


def snap(tag, count=1, step=1):
    global n
    p = f'{F}/{tag}{n:04d}_'
    cmd(f'film {p} {count} {step}')
    n += 1
    return p


def next_index(f, A):
    cnt = f['u32'](0x5B8 + A * 0x64)
    items = struct.unpack('<H', f['obj'][0x96C:0x96E])[0] != 0
    if A == 0:
        cm = [0, 1, 2, 3, 4, 5, 6, 7]
        if not items:
            cm.remove(1)
        for drop in (7, 5, 2):
            if len(cm) <= cnt:
                break
            cm.remove(drop)
    else:
        cm = [0, 1, 3, 4, 6]
        if not items or len(cm) > cnt:
            cm.remove(1)
    return cm.index(6) if 6 in cm else len(cm) - 1


slot = sys.argv[1]
maxsteps = int(sys.argv[2])
pre = sys.argv[3:]
cmd(f'load {slot}', 'run 10', 'hold b 6', 'run 20', 'hold b 6', 'run 20', *pre)
enemy_frames = 0
for it in range(maxsteps):
    p = snap('q')
    f = load(p + '0000.bin')
    A = f['u32'](0x554)
    INP = f['u32'](0x14D4)
    if A <= 3 and INP != 0xFFFFFFFF:
        cur = f['u32'](0x5C8 + A * 0x64)
        nxt = next_index(f, A)
        print(it, 'party', A, 'cur', cur, 'next', nxt, flush=True)
        if cur != nxt:
            cmd('tap left 12', 'run 6')
        else:
            cmd('hold a 6', 'run 30')
    else:
        print(it, 'other A', A, 'INP', hex(INP), 'hp', f['hp'], flush=True)
        if 4 <= A < 16:
            snap('E', 120, 2)
            enemy_frames += 120
            if enemy_frames >= 600:
                break
        else:
            cmd('run 20')
print('ALLDONE')
