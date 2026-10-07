"""Single-screen layout for SMT IV: Apocalypse built around the game's real bottom screen.

The bottom screen is always docked (same place, translucent, mouse = touch) in battle and
menus, so every menu, cursor and prompt is the game's own and can never be out of sync.
Battle adds a compact column of native party cards (live HP/MP/level, smirk flash) on the
left, clear of the top screen's message box and of the dock."""
import re, sys
L = []
def let(n, e): L.append(f'let {n} = {e}')
def c(e): return '=' + re.sub(r'\s+', '', e)
def el(s): L.append(s)
WHITE = '#F2F4F8'; DIM = '#8A93A3'; CYAN = '#5FD3FF'; HPC = '#E8455A'; MPC = '#4C8DFF'
BARBG = '#1A1E26E0'

DOCK = '0 0 320 240 -> 1300 150 592 444 space=window touch=1'

let('BAT', 'smt4a_save() != 0 && smt4a_unit(0) != 0 && smt4a_obj() != 0')
let('BLINK', '(time() / 280) % 2')
for k in range(4):
    let(f'N{k}', "sjis(smt4a_save(), 0)" if k == 0 else f"lookup('names.txt', smt4a_demon({k}))")
    let(f'P{k}', f'{k} == 0 || smt4a_demon({k}) > 0')
    let(f'SMK{k}', f'(u8(smt4a_unit({k}) + 0x10E) & 0x10) != 0')
KX, KY, KW, KH, GAP = 24, 140, 300, 104, 10
for k in range(4):
    y = KY + k * (KH + GAP)
    show = c(f'$BAT&&$P{k}')
    el(f'rect {KX} {y} {KW} {KH} #0C0F16C8 if={show}')
    el(f'rect {KX} {y} 5 {KH} #5FD3FFFF if={show}')
    el(f'rect {KX} {y} {KW} {KH} #FFD25A50 if={show} and={c(f"$SMK{k}&&$BLINK")}')
    el(f'text {KX + 18} {y + 8} 30 {WHITE} left "{{0}}" v={c(f"$N{k}")} fit=130 if={show}')
    el(f'text {KX + KW - 14} {y + 12} 22 {CYAN} right "Lv {{0}}" v={c(f"smt4a_level({k})")} if={show}')
    el(f'image {KX + 150} {y + 10} 70 25 smirk.png if={show} and={c(f"$SMK{k}")}')
    for j, (lab, cur, mx, col) in enumerate([('HP', 'smt4a_hp', 'smt4a_maxhp', HPC), ('MP', 'smt4a_mp', 'smt4a_maxmp', MPC)]):
        by = y + 56 + j * 24
        el(f'text {KX + 18} {by - 8} 18 {DIM} left "{lab}" if={show}')
        el(f'bar {KX + 52} {by} 120 10 {col}FF {BARBG} v={c(f"{cur}({k})")} max={c(f"max({mx}({k}),1)")} if={show}')
        el(f'text {KX + 260} {by - 10} 24 {WHITE} right "{{0}}" v={c(f"min({cur}({k}),9999)")} if={show}')

def build(src):
    head, rest = src.split('# --- Battle ---', 1)
    tail = rest[rest.index('# --- Toggleable'):]
    mid = f'''# --- Battle -------------------------------------------------------------------
#  The game's bottom screen stays docked on the right (translucent; the mouse works as the
#  stylus), so menus, cursors and negotiation prompts are always the game's own. Native party
#  cards on the left read the live battle units.
[profile battle]
hide_bottom = 1
region = {DOCK} opacity=0.85

[hud battle]
''' + '\n'.join(L) + f'''

# --- Menus ------------------------------------------------------------------------
#  Same dock as battle, more transparent.
[profile menu]
hide_bottom = 1
region = {DOCK} opacity=0.7

'''
    return head + mid + tail

if __name__ == '__main__':
    p = '/home/claude/azahar/dist/screen_regions/000400000019A200.ini'
    out = build(open(p).read())
    for q in [p] + sys.argv[1:]:
        open(q, 'w').write(out)
    print(len(L), 'lines')
