"""Native camp (main menu) HUD for SMT IV: Apocalypse: party + stock grid and the command row,
drawn from game memory. Sub-menus not covered yet still use the bottom-screen panel."""
import re
L = []
def let(n, e): L.append(f'let {n} = {e}')
def c(e): return '=' + re.sub(r'\s+', '', e)
def el(s): L.append(s)
WHITE = '#F2F4F8'; DIM = '#8A93A3'; CYAN = '#5FD3FF'; GOLD = '#FFD25A'

CMDS = ['Skill', 'Item', 'Party', 'Partner', 'Quests', 'Database', 'Mido', 'System']
DESC = ['Open the skill menu.', 'Open the item menu.', 'Open the summon/return menu.',
        'Confirm partner status. Set main partner.', 'Open the quest menu.', 'Open the database.',
        'Open the Cathedral of Shadows menu.', 'Open the system menu.']
# The camp's list widget ([0x57F108]): +0x78 entry count, +0x88 cursor. The main menu is the
# 8-entry list shown together with the demon grid ([0x57F268]); sub-menus reuse the widget
# with other counts.
let('DL', 'u32(0x57F268)')
let('SV', 'smt4a_save()')
let('LW', 'u32(0x57F108)')
let('CC', '$LW ? u32($LW + 0x88) : -1')
let('MAIN', '$DL != 0 && $SV != 0 && $LW != 0 && u32($LW + 0x78) == 8 && $CC >= 0 && $CC < 8')
for k in range(1, 4):
    let(f'PR{k}', f'smt4a_prec({k})')
    let(f'PI{k}', f'$PR{k} ? u16($PR{k} + 0x62) : 0')
M = c('$MAIN')

# hints
x = 28
for b, t in [('Y', 'Zoom'), ('X', 'Sort'), ('B', 'Back'), ('A', 'OK')]:
    el(f'image {x} 18 {36 + 16 + len(t) * 13 + 24} 44 chip.png if={M}')
    el(f'image {x + 5} 22 36 36 btn_{b}.png if={M}')
    el(f'text {x + 48} 26 26 {WHITE} left "{t}" if={M}')
    x += 36 + 16 + len(t) * 13 + 24 + 10

# command row, bottom centre
CW = 196
X0 = 960 - 4 * CW
el(f'rect {X0 - 12} 960 {8 * CW + 24} 84 #0A0D14D8 if={M}')
el(f'rect {X0 - 12} 960 {8 * CW + 24} 3 #5FD3FFFF if={M}')
for k, name in enumerate(CMDS):
    xx = X0 + k * CW
    el(f'image {xx + 4} 974 {CW - 8} 56 row_sel.png if={M} and={c(f"$CC=={k}")}')
    el(f'text {xx + CW // 2} 984 30 {WHITE} center "{name}" if={M} and={c(f"$CC=={k}")}')
    el(f'text {xx + CW // 2} 984 30 {DIM} center "{name}" if={M} and={c(f"$CC!={k}")}')

el(f'rect {X0 - 12} 906 {8 * CW + 24} 52 #0A0D14C0 if={M}')
for k, d in enumerate(DESC):
    el(f'text 960 916 26 {WHITE} center "{d}" if={M} and={c(f"$CC=={k}")}')

# party + stock grid, right side
GX, GY, CWD, CH, PXP, PYP = 1004, 270, 214, 88, 222, 94
el(f'rect {GX - 16} 122 {4 * PXP + 24} {GY + 6 * PYP - 122 + 16} #0A0D14D0 if={M}')
el(f'rect {GX - 16} 122 {4 * PXP + 24} 4 #5FD3FFFF if={M}')
def cell(x, y, face, name, lvl, hp, mp, cond, dim=None):
    el(f'rect {x} {y} {CWD} {CH} #161B26E8 if={cond}')
    el(f'rect {x + 6} {y + 11} 100 70 #0C0F16FF if={cond}')
    el(f'image {x + 6} {y + 11} 100 70 bu/bu_{{0}}.png v={face} crop=0,0,1,0.607 if={cond}')
    el(f'text {x + 114} {y + 8} 22 {WHITE} left "{{0}}" v={name} fit={CWD - 120} if={cond}')
    el(f'text {x + 114} {y + 36} 20 {CYAN} left "Lv {{0}}" v={lvl} if={cond}')
    nd = f' and={c("!(" + dim + ")")}' if dim else ''
    el(f'text {x + 114} {y + 62} 17 {DIM} left "HP {{0}}  MP {{1}}" v={hp} v={mp} fit={CWD - 120} if={cond}{nd}')
    if dim:
        el(f'rect {x} {y} {CWD} {CH} #0A0D14B0 if={cond} and={c(dim)}')
        el(f'text {x + CWD - 8} {y + 64} 18 {GOLD} right "PARTY" if={cond} and={c(dim)}')
cell(GX, 136, c("'nanashi'"), c('sjis($SV,0)'), c('u16($SV+0x64)'), c('min(u16($SV+0x24),9999)'),
     c('min(u16($SV+0x26),9999)'), M)
for k in range(1, 4):
    cell(GX + k * PXP, 136, c(f"lookup('bu_index.txt',$PI{k})"), c(f"lookup('names.txt',$PI{k})"),
         c(f'u16($PR{k}+0x64)'), c(f'min(u16($PR{k}+0x2A),9999)'), c(f'min(u16($PR{k}+0x2C),9999)'),
         c(f'$MAIN&&$PI{k}>0'))
el(f'rect {GX} {GY - 32} {4 * PXP - 8} 2 #5FD3FF60 if={M}')
el(f'text {GX} {GY - 30} 20 {DIM} left "STOCK" if={M}')
let('DN', 'u16($DL + 0x28)')
for r in range(6):
    for col in range(4):
        i = r * 4 + col
        x, y = GX + col * PXP, GY + r * PYP
        let(f'SR{i}', f'$SV + 0xF4 + u8($DL + 0xA80 + {i * 8}) * 0x12C')
        let(f'SI{i}', f'$MAIN && {i} < $DN ? u16($SR{i} + 0x62) : 0')
        cell(x, y, c(f"lookup('bu_index.txt',$SI{i})"), c(f"lookup('names.txt',$SI{i})"),
             c(f'u16($SR{i}+0x64)'), c(f'min(u16($SR{i}+0x2A),9999)'), c(f'min(u16($SR{i}+0x2C),9999)'),
             c(f'$SI{i}>0'), dim=f'$SI{i}==$PI1||$SI{i}==$PI2||$SI{i}==$PI3')

MAIN_EXPR = ('u32(0x57F268)!=0&&smt4a_save()!=0&&u32(0x57F108)!=0&&u32(u32(0x57F108)+0x78)==8'
             '&&u32(u32(0x57F108)+0x88)<8')

def section():
    return f"""# --- Menus: camp --------------------------------------------------------------------
#  Main menu: native party/stock grid and command row from game memory. Other camp screens:
#  the game's bottom screen as a small translucent panel (top-right).
[profile menu]
hide_bottom = 1
region = 0 0 320 240 -> 1452 600 448 336 opacity=0.6 space=window if==!({MAIN_EXPR})

[hud menu]
""" + '\n'.join(L) + '\n\n'
