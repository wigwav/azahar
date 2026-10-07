"""Render the battle HUD over the user's recorded game states (SRREC1 snapshots)."""
import sys, glob, subprocess, os, numpy as np
sys.path.insert(0, '/home/claude/smt4a/tools')
from PIL import Image
from rec import Raw
TOOL = '/home/claude/build-lin/bin/Release/sr_tool'
INI = os.environ.get('INI', '/home/claude/azahar/dist/screen_regions/000400000019A200.ini')
ASSETS = '/home/claude/smt4a/hud3'
SP = '/tmp/claude-0/-home-claude-azahar/759ff5b5-2ca7-5d7b-86b8-817c3fc82596/scratchpad'
fs = sorted(glob.glob(os.environ.get('RECGLOB', '/mnt/user-data/uploads/dump/screen_regions/*.zst')))
idx = [int(a) for a in sys.argv[1:]]
lines = ['load 1', 'run 3', 'dump /home/claude/smt4a/emu/d/base.bin']
for i in idx:
    raw = f'/tmp/claude-0/rec{i}.bin'
    subprocess.run(['/home/claude/smt4a/tools/unzstd', fs[i], raw, '0'], check=True)
    lines += [f'wrec {raw}', f'hud {INI} {ASSETS} {os.environ.get("PROFILE","battle")} /tmp/claude-0/rec{i}.rgba']
lines.append('quit')
env = dict(os.environ, CITRA_ALLOW_STATE_BUILD_MISMATCH='1')
subprocess.run([TOOL, '/home/claude/smt4a/emu/user', '/home/claude/smt4a/emu/smt4a.cxi'], input='\n'.join(lines) + '\n',
               text=True, capture_output=True, env=env, cwd='/home/claude/smt4a/emu')
top = Raw('/home/claude/smt4a/emu/d/base.bin').top.convert('RGB').resize((1800, 1080))
for i in idx:
    canvas = Image.new('RGBA', (1920, 1080), (0, 0, 0, 255)); canvas.paste(top, (60, 0))
    hud = np.fromfile(f'/tmp/claude-0/rec{i}.rgba', np.uint8)
    canvas.alpha_composite(Image.fromarray(hud.reshape(1080, 1920, 4)))
    # bottom screen as the user saw it (small, top-left) for reference
    from srrec import SRRec
    canvas.paste(SRRec(fs[i]).img.resize((320, 240)), (1590, 830))
    canvas.convert('RGB').save(f'{SP}/rec{i}.png')
    print(f'{SP}/rec{i}.png')
