"""Render the native battle HUD over the game's top screen for a list of input scenarios."""
import sys, os, numpy as np
sys.path.insert(0, '/home/claude/smt4a/tools')
from emu import run, OUT
from PIL import Image
INI = '/home/claude/azahar/dist/screen_regions/000400000019A200.ini'
ASSETS = '/home/claude/smt4a/hud3'
SP = '/tmp/claude-0/-home-claude-azahar/759ff5b5-2ca7-5d7b-86b8-817c3fc82596/scratchpad'
def shot(name, slot, steps):
    # a quick pass with the portrait uncovered first so the capture cache has it
    steps = [x.replace('HUD', f'hud {INI} {ASSETS} battle {OUT}/tmp.rgba') for x in steps]
    script = steps + [f'dump {name}', f'hud {INI} {ASSETS} battle {OUT}/{name}.rgba']
    r = run(script, slot=slot)
    top = r[name].top.convert('RGB').resize((1800, 1080), Image.LANCZOS)
    canvas = Image.new('RGB', (1920, 1080), (0, 0, 0)); canvas.paste(top, (60, 0))
    hud = np.fromfile(f'{OUT}/{name}.rgba', np.uint8)
    if hud.size == 1920 * 1080 * 4:
        canvas = canvas.convert('RGBA'); canvas.alpha_composite(Image.fromarray(hud.reshape(1080, 1920, 4)))
    canvas.convert('RGB').save(f'{SP}/{name}.png')
    err = open(f'{OUT}/last_err.log').read()
    for l in err.splitlines():
        if 'error' in l.lower() and ('let' in l or 'element' in l or 'HUD' in l): print(l)
    return f'{SP}/{name}.png'
if __name__ == '__main__':
    which = sys.argv[1:] or ['n_skill']
    S = {
      'n_skill': (1, ['hold right 2', 'run 20', 'hold right 2', 'run 30', 'HUD', 'hold right 2', 'run 20', 'hold right 2', 'run 20', 'hold left 2', 'run 20', 'hold left 2', 'run 20', 'hold down 2', 'run 20', 'hold down 2', 'run 20']),
      'n_list': (1, ['hold down 2', 'run 20', 'hold down 2', 'run 20']),
      'n_item': (1, ['hold right 2', 'run 20', 'hold down 2', 'run 20']),
      'n_talk': (1, ['hold right 2', 'run 20', 'hold right 2', 'run 20']),
      'd_skill': (2, ['hold right 2', 'run 20', 'hold right 2', 'run 30', 'HUD', 'hold left 2', 'run 20', 'hold left 2', 'run 20', 'hold down 2', 'run 20']),
    }
    for w in which:
        print(shot(w, *S[w]))
