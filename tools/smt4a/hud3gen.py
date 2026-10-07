"""Generates the native single-screen battle HUD ([hud battle]) for SMT IV: Apocalypse.

Everything is drawn from game memory with original art (hud3/); nothing is cropped from the
bottom screen: portraits are the game's own bust-up art (tex/stex/stbustup), chosen through the
bust-up index in the demon table (battle/NKMBaseTable.tbb +0x9C), see portraits.py.
Canvas is 1920x1080. Layout follows the HD-remaster concept:
  hints top-left, portrait + skill/item list + description bottom-left, command row centred,
  party cards bottom-centre.
"""
import re, sys

L = []          # output lines
def let(name, expr): L.append(f'let {name} = {expr}')
def c(expr):
    """compact an expression for use inside an element token (no spaces allowed there)"""
    assert "' '" not in expr
    return '=' + re.sub(r'\s+', '', expr)
def el(line): L.append(line)
def comment(s): L.append(''); L.append('# ' + s)

WHITE = '#F2F4F8'; DIM = '#8A93A3'; GREY = '#5A6170'; CYAN = '#5FD3FF'; GREEN = '#5BE37A'
HPC = '#E8455A'; MPC = '#4C8DFF'; BARBG = '#1A1E26E0'; GOLD = '#FFD25A'

# ---------------------------------------------------------------- state
# Battle UI mode (+0x18A0): 11 command menu (lists, talk picker, popups), 16 ally target or the
# demon list, 13 Effects, 14 Analyze, 0 anything else (enemy target, turns, negotiation).
let('O', 'smt4a_obj()')
let('A', '$O ? u32($O + 0x554) : -1')
let('BAT', '$O != 0 && smt4a_save() != 0 && smt4a_unit(0) != 0')     # inside a battle
let('M', 'u32($O + 0x18A0)')
let('ACT', '$BAT && $A >= 0 && $A <= 3')                              # a member is choosing
let('CUR', '$ACT ? u32($O + 0x5C8 + $A * 0x64) : 0')
let('NC', 'smt4a_cmdcount($A)')
let('CMD', 'smt4a_cmd($A, $CUR)')
let('LON', 'u32($O + 0x113FC) == 1 && u32($O + 0x14D4) != 0xFFFFFFFF')   # skill/item list open
let('PICK', 'u32($O + 0x898) == 0')                     # a command's sub-picker is open
let('DL', 'u32(0x57F268)')                              # demon list (swap/summon/status) UI
let('DLON', '$BAT && $DL != 0')
let('POP', '$ACT && u32($O + 0xE64C) != 0 && !$LON')   # Status popup
let('CD', 'smt4a_cmddesc(0)')
# The command row's on-screen transform: nonzero exactly while the row is drawn. The mode word
# above is not reliable (it reads 0 during demon turns and the talk picker in some battles).
# +0x14D4: the game's input-state stack; it is -1 the moment a command is confirmed (the row
# and list flags below stay set while the action plays). Some command screens (demon turns,
# the talk picker) run with it empty, so the command row's resting transform is also accepted.
let('INP', 'u32($O + 0x14D4) != 0xFFFFFFFF')
# Whether the game is drawing its command icons, read from the (hidden) bottom screen: three
# pixels on the icon line that are cream on an icon tile in both the 7- and 4-icon rows and
# never cream on the portraits shown while actions play. Memory alone cannot tell a demon's own
# command turn from an action playing (Auto-Battle) - both have an empty input stack.
for k, (px, py) in enumerate(((148, 176), (168, 176), (186, 178))):
    let(f'QP{k}', f'pix({px}, {py})')
    let(f'QC{k}', f'((($QP{k} >> 16) & 255) > 190) && ((($QP{k} >> 8) & 255) > 175) && (($QP{k} & 255) > 130) && (($QP{k} & 255) < 235) && ((($QP{k} >> 16) & 255) - ($QP{k} & 255) > 15)')
let('CROW', '$QC0 || $QC1 || $QC2')
let('ROW', '$ACT && ($INP || $CROW)')
let('TALK', '$ROW && $M != 16 && $CMD == 2 && $PICK && !$LON')   # Scout / Fundraise / Trade
let('NEG', '$ACT && !$ROW && $M == 0 && $CMD == 2 && $PICK')     # negotiation in progress
let('ON', 'recent(($ROW || ($ACT && $M == 16)) && !$DLON && !$POP && !$TALK, 120)')
let('TGT', '$ON && $M == 16')
let('BLINK', '(time() / 280) % 2')
let('SKL', '$ON && $LON && $CMD == 0')
let('ITM', '$ON && $LON && $CMD == 1')
let('LIST', '$SKL || $ITM')
# Negotiation answers: lines of the shared message buffer; the game centres the rows (27 px
# apart) on y=104 of the bottom screen and marks the chosen one with a cyan band, read back from
# the rendered bottom screen.
for k in range(4):
    let(f'NL{k}', f"$NEG ? smt4a_skilldesc({k}) : ''")
let('NN', '(len($NL0) > 0) + (len($NL1) > 0) + (len($NL2) > 0) + (len($NL3) > 0)')
for k in range(4):
    let(f'NP{k}', f'pix(48, (208 + {54 * k} - 27 * ($NN - 1)) / 2)')
    let(f'NS{k}', f'{k} < $NN && (($NP{k} & 255) > 90) && ((($NP{k} >> 16) & 255) < 70)')
let('NCH', '$NEG && $NN > 0 && ($NS0 || $NS1 || $NS2 || $NS3)')
# the game's own top-screen views that must stay uncovered: Effects (X), Analyze (Y), talk
let('CLEAR', '$DLON || $M == 13 || $M == 14 || $TALK || $NEG')
# skill list: absolute cursor, count, own 6-row scroll
let('SC', 'u32($O + 0x764)')
let('SN', 'smt4a_entries($A)')
let('ST', 'clamp($SC - u32($O + 0x730), 0, max($SN - 6, 0))')    # game's own scroll
let('MP', 'smt4a_mp($A)')
# item list
let('IC', 'u32($O + 0x82C)')
let('IN', ' : '.join(f'smt4a_item({i}) == 0 ? {i}' for i in range(96)) + ' : 96')
let('IT', 'clamp($IC - u32($O + 0x7F8), 0, max($IN - 6, 0))')
# portrait: the game's bust-up art for the acting member
let('BU', "$A == 0 ? 'nanashi' : lookup('bu_index.txt', smt4a_demon($A))")
let('PNAME', "$A == 0 ? sjis(smt4a_save(), 0) : lookup('names.txt', smt4a_demon($A))")
for k in range(4):
    let(f'N{k}', "sjis(smt4a_save(), 0)" if k == 0 else f"lookup('names.txt', smt4a_demon({k}))")
    let(f'P{k}', f'smt4a_save() != 0 && ({k} == 0 || smt4a_demon({k}) > 0)')
    let(f'SMK{k}', f'(u8(smt4a_unit({k}) + 0x10E) & 0x10) != 0')

# choosing an ally for the skill/item picked from the list (the list hides, its selection stays)
let('TSEL', '$TGT && u32($O + 0x113FC) == 1')
let('TD', "($LIST || $TSEL) ? smt4a_skilldesc(1) : ''")
let('TALL', "has($TD, 'allies') || has($TD, 'party') || has($TD, 'Party')")
let('TSELF', "has($TD, 'Self') || has($TD, 'self')")
let('TONE', "has($TD, '1 ally')")
let('TCUR', 'u32($O + 0x8E8)')
ON = c('$ON')

# ---------------------------------------------------------------- hints, top-left
comment('button hints')
hints = [('L', 'Pinpoint'), ('R', 'Auto'), ('Y', 'Analyze'), ('X', 'Effects'), ('B', 'Back'), ('A', 'OK')]
x = 28
for b, t in hints:
    cond = c('$ON&&!$TGT')
    el(f'image {x} 18 {36 + 16 + len(t) * 13 + 24} 44 chip.png if={cond}')
    el(f'image {x + 5} 22 36 36 btn_{b}.png if={cond}')
    el(f'text {x + 48} 26 26 {WHITE} left "{t}" if={cond}')
    x += 36 + 16 + len(t) * 13 + 24 + 10

# ---------------------------------------------------------------- portrait, left
comment('active member portrait (game bust-up art)')
PX, PY, PW, PH = 24, 196, 300, 314
el(f'rect {PX + 4} {PY + 4} {PW - 8} {PH - 8} #0C0F1640 if={ON}')
el(f'image {PX} {PY} {PW} {PH} bu/bu_{{0}}.png v={c("$BU")} opacity=0.92 if={ON}')
el(f'image {PX} {PY} {PW} {PH} portrait_frame.png opacity=0.6 if={ON}')
let('SMKA', '$A == 0 ? $SMK0 : $A == 1 ? $SMK1 : $A == 2 ? $SMK2 : $SMK3')
el(f'rect {PX + 3} {PY + 3} {PW - 6} {PH - 6} #FFD25A48 if={c("$ON&&$SMKA&&$BLINK")}')
el(f'image {PX - 4} {PY - 4} {PW + 8} {PH + 8} portrait_frame.png if={c("$ON&&$SMKA&&$BLINK")}')
el(f'image {PX + PW - 104} {PY + 8} 96 34 smirk.png if={c("$ON&&$SMKA")}')
el(f'rect {PX} {PY + PH + 2} {PW} 50 #0C0F1690 if={ON}')
el(f'rect {PX} {PY + PH + 2} 6 50 #5FD3FFFF if={ON}')
el(f'text {PX + 18} {PY + PH + 8} 30 #F2F4F8 left "{{0}}" v={c("$PNAME")} fit=200 if={ON}')
el(f'text {PX + PW - 16} {PY + PH + 12} 24 #5FD3FF right "Lv {{0}}" v={c("smt4a_level($A)")} if={ON}')

# Items the game refuses right now (it ignores A on them) are greyed: heals with everyone at full
# HP/MP, revives with nobody down (itemuse.txt, from item/ItemTable.tbb, see itemuse.py).
let('NEEDHP', ' || '.join(f'($P{k} && smt4a_hp({k}) > 0 && smt4a_hp({k}) < smt4a_maxhp({k}))' for k in range(4)))
let('NEEDMP', ' || '.join(f'($P{k} && smt4a_hp({k}) > 0 && smt4a_mp({k}) < smt4a_maxmp({k}))' for k in range(4)))
let('ANYKO', ' || '.join(f'($P{k} && smt4a_hp({k}) == 0)' for k in range(4)))
def item_usable(it):
    u = f"lookup('itemuse.txt',{it})"
    return (f"({u}=='h'?$NEEDHP:{u}=='m'?$NEEDMP:{u}=='b'?($NEEDHP||$NEEDMP):"
            f"{u}=='r'?$ANYKO:{u}=='f'?($NEEDHP||$NEEDMP||$ANYKO):1)")
# ---------------------------------------------------------------- skill / item list
comment('skill list (Skill highlighted) / item list (Item highlighted)')
LX, LY = 24, 600
el(f'image {LX} {LY} 420 {6 * 40 + 22} list_panel.png if={c("$LIST")}')
el(f'text {LX + 16} {LY - 34} 26 {CYAN} left "SKILLS" if={c("$SKL")}')
el(f'text {LX + 16} {LY - 34} 26 {CYAN} left "ITEMS" if={c("$ITM")}')
el(f'text {LX + 404} {LY - 32} 22 {DIM} right "{{0}}/{{1}}" v={c("$SC+1")} v={c("$SN")} if={c("$SKL")}')
el(f'text {LX + 404} {LY - 32} 22 {DIM} right "{{0}}/{{1}}" v={c("$IC+1")} v={c("$IN")} if={c("$ITM")}')
for i in range(6):
    y = LY + 11 + i * 40
    sel_s = c(f'$SKL&&$ST+{i}==$SC')
    sel_i = c(f'$ITM&&$IT+{i}==$IC')
    e = f'smt4a_entry($A,$ST+{i})'
    has = c(f'$SKL&&$ST+{i}<$SN')
    el(f'image {LX + 14} {y} 392 36 row_sel.png if={sel_s}')
    el(f'image {LX + 14} {y} 392 36 row_sel.png if={sel_i}')
    # skill row
    el(f'image {LX + 22} {y + 3} 30 30 el_{{0}}.png v={c(f"smt4a_skillicon({e})")} if={has}')
    ok = f'smt4a_cost($A,{e})<=$MP'
    el(f'text {LX + 62} {y + 2} 27 {WHITE} left "{{0}}" v={c(f"smt4a_skillname({e})")} fit=250 if={has} and={c(ok)}')
    el(f'text {LX + 62} {y + 2} 27 {GREY} left "{{0}}" v={c(f"smt4a_skillname({e})")} fit=250 if={has} and={c("!(" + ok + ")")}')
    el(f'text {LX + 398} {y + 4} 24 {CYAN} right "{{0}} MP" v={c(f"smt4a_cost($A,{e})")} if={has} and={c(f"smt4a_cost($A,{e})>0")}')
    # item row
    it = f'smt4a_item($IT+{i})'
    hasi = c(f'$ITM&&$IT+{i}<$IN')
    let(f'IU{i}', item_usable(it))
    for col, cnt, cond in ((WHITE, GOLD, f'$IU{i}'), (GREY, GREY, f'!$IU{i}')):
        el(f'text {LX + 26} {y + 2} 27 {col} left "{{0}}" v={c(f"smt4a_itemname({it})")} fit=290 if={hasi} and={c(cond)}')
        el(f'text {LX + 398} {y + 4} 24 {cnt} right "x{{0}}" v={c(f"smt4a_itemcount({it})")} if={hasi} and={c(cond)}')
# target prompt: what is being used, in the list's place
let('TNAME', "$CMD == 0 ? smt4a_skillname(smt4a_entry($A, $SC)) : smt4a_itemname(smt4a_item($IC))")
TP = c('$TSEL')
el(f'image {LX} {LY} 420 132 list_panel.png if={TP}')
el(f'text {LX + 20} {LY + 14} 22 {GOLD} left "CHOOSE A TARGET" if={TP}')
el(f'text {LX + 20} {LY + 50} 34 {WHITE} left "{{0}}" v={c("$TNAME")} fit=380 if={TP}')
el(f'text {LX + 20} {LY + 96} 20 {DIM} left "Left / Right to move   A to use" if={TP}')
# scroll hints
el(f'text {LX + 210} {LY - 4} 20 {DIM} center "^" if={c("$SKL&&$ST>0")}')
el(f'text {LX + 210} {LY + 6 * 40 + 6} 20 {DIM} center "v" if={c("$SKL&&$ST+6<$SN")}')
el(f'text {LX + 210} {LY - 4} 20 {DIM} center "^" if={c("$ITM&&$IT>0")}')
el(f'text {LX + 210} {LY + 6 * 40 + 6} 20 {DIM} center "v" if={c("$ITM&&$IT+6<$IN")}')

# ---------------------------------------------------------------- description
comment('description')
DX, DY = 24, 880
el(f'image {DX} {DY} 430 92 desc_panel.png if={ON}')
for ln in range(2):
    el(f'text {DX + 18} {DY + 12 + ln * 36} 25 {WHITE} left "{{0}}" v={c(f"smt4a_skilldesc({ln})")} fit=396 if={c("$LIST||$TSEL")}')
    el(f'text {DX + 18} {DY + 12 + ln * 36} 25 {WHITE} left "{{0}}" v={c(f"smt4a_cmddesc({ln})")} fit=396 if={c("$ON&&!$LIST&&!$TSEL")}')

# ---------------------------------------------------------------- command row
comment('command row, centred')
CY = 690
for i in range(8):
    x0 = f'960-$NC*62+{i}*124'
    cond = c(f'$ON&&!$TGT&&{i}<$NC')
    el(f'image 0 {CY} 112 112 cmd_{{0}}.png v={c(f"smt4a_cmd($A,{i})")} ox={c(x0 + "+6")} if={cond} and={c(f"$CUR!={i}")}')
    el(f'image -6 {CY - 6} 124 124 cmdsel_{{0}}.png v={c(f"smt4a_cmd($A,{i})")} ox={c(x0 + "+6")} if={cond} and={c(f"$CUR=={i}")}')
    el(f'image -8 {CY + 112} 140 30 lbl_{{0}}.png v={c(f"smt4a_cmd($A,{i})")} ox={c(x0)} if={cond} and={c(f"$CUR!={i}")} opacity=0.85')
    el(f'image -8 {CY + 112} 140 30 lbl_{{0}}.png v={c(f"smt4a_cmd($A,{i})")} ox={c(x0)} if={cond} and={c(f"$CUR=={i}")}')

# ---------------------------------------------------------------- party cards
comment('party cards')
# Small cards on the bottom edge, always in the same place (below the game's skill-name banner).
# A hit shakes the card, flashes it, plays a strike mark over it and raises the number above it.
let('FRESH', 'recent(!$BAT, 2000)')     # no damage popups for HP that changed between battles
KW, KH, KG = 236, 104, 12
KX = 960 - 2 * KW - 3 * KG // 2
KY = 1080 - KH - 10
RED, GREEN = '#FF5A5A', '#6CFF8E'
for k in range(4):
    x = KX + k * (KW + KG)
    show = c(f'$BAT&&$P{k}&&!$CLEAR')
    act = f'($ON&&$A=={k})'
    # damage / healing: summed over a burst of hits, shown for 1.8 s
    let(f'SW{k}', f'track(\'id{k}\', smt4a_demon({k}), 2000)')
    let(f'D{k}', f'track(\'hp{k}\', smt4a_hp({k}), 1800)')
    let(f'AG{k}', f'trackage(\'hp{k}\')')
    let(f'DV{k}', f'$BAT && $P{k} && $D{k} != 0 && $SW{k} == 0 && !$FRESH')
    let(f'HIT{k}', f'$DV{k} && $D{k} < 0 && $AG{k} < 450')
    tk = f'$TGT&&($TALL||($TSELF&&$A=={k})||($TONE&&$TCUR=={k}))'
    # shake while hit (decaying), lift while targeted / acting
    ox = c(f'$HIT{k}?(((time()/45)%2)*2-1)*max(0,9-$AG{k}/50):0')
    oy = c(f'({tk})?-14:({act}?-6:0)')
    pos = f'ox={ox} oy={oy}'
    el(f'rect {x - 6} {KY - 6} {KW + 12} {KH + 12} #FFD25AFF {pos} if={show} and={c(tk)}')
    el(f'rect {x - 6} {KY - 6} {KW + 12} {KH + 12} #FFFFFF60 {pos} if={show} and={c(tk + "&&$BLINK")}')
    el(f'image {x} {KY} {KW} {KH} card.png {pos} if={show} and={c("!" + act)}')
    el(f'image {x} {KY} {KW} {KH} card_active.png {pos} if={show} and={c(act)}')
    el(f'rect {x + KW // 2 - 56} {KY - 44} 112 30 #FFD25AFF {pos} if={show} and={c(tk)}')
    el(f'text {x + KW // 2} {KY - 42} 22 #0A0D14 center "TARGET" {pos} if={show} and={c(tk)}')
    el(f'rect {x + 3} {KY + 3} {KW - 6} {KH - 6} #FFD25A40 {pos} if={show} and={c(f"$SMK{k}&&$BLINK")}')
    el(f'text {x + 14} {KY + 8} 26 {WHITE} left "{{0}}" v={c(f"$N{k}")} fit=140 {pos} if={show}')
    el(f'text {x + KW - 12} {KY + 12} 18 {CYAN} right "Lv {{0}}" v={c(f"smt4a_level({k})")} {pos} if={show} and={c(f"!$SMK{k}")}')
    el(f'image {x + KW - 86} {KY + 6} 76 26 smirk.png {pos} if={show} and={c(f"$SMK{k}")}')
    for j, (lab, cur, mx, col) in enumerate([('HP', 'smt4a_hp', 'smt4a_maxhp', HPC), ('MP', 'smt4a_mp', 'smt4a_maxmp', MPC)]):
        by = KY + 52 + j * 26
        el(f'text {x + 14} {by - 7} 17 {DIM} left "{lab}" {pos} if={show}')
        el(f'bar {x + 46} {by} 96 9 {col}FF {BARBG} v={c(f"{cur}({k})")} max={c(f"max({mx}({k}),1)")} {pos} if={show}')
        el(f'text {x + KW - 12} {by - 9} 23 {WHITE} right "{{0}}" v={c(f"{cur}({k})")} {pos} if={show}')
    # hit: red flash, then the strike mark (three frames), heal: green flash
    el(f'rect {x} {KY} {KW} {KH} #FF303090 fade={c(f"max(0,100-$AG{k}/4)")} {pos} if={show} and={c(f"$DV{k}&&$D{k}<0")}')
    el(f'rect {x} {KY} {KW} {KH} #40FF7070 fade={c(f"max(0,100-$AG{k}/4)")} {pos} if={show} and={c(f"$DV{k}&&$D{k}>0")}')
    for fr, (t0, t1) in enumerate(((0, 70), (70, 150), (150, 320))):
        el(f'image {x + KW // 2 - 90} {KY + KH // 2 - 90} 180 180 hit_{fr}.png {pos} '
           f'fade={c(f"max(0,100-($AG{k}-{t0})*60/{t1 - t0})" if fr == 2 else "100")} '
           f'if={show} and={c(f"$DV{k}&&$D{k}<0&&$AG{k}>={t0}&&$AG{k}<{t1}")}')
    for col, sign, cond in ((RED, '-', '<0'), (GREEN, '+', '>0')):
        for dx, dy, cc in ((3, 3, '#000000'), (0, 0, col)):
            el(f'text {x + KW // 2 + dx} {dy} 50 {cc} center "{sign}{{0}}" v={c(f"abs($D{k})")} '
               f'ox=0 oy={c(f"{KY + 20}-min($AG{k},600)/12")} '
               f'fade={c(f"$AG{k}<1100?100:max(0,100-($AG{k}-1100)/7)")} if={c(f"$DV{k}&&$D{k}{cond}")}')

# ---------------------------------------------------------------- context hints for sub-screens
def hintrow(items, cond):
    x = 28
    for b, t in items:
        el(f'image {x} 18 {36 + 16 + len(t) * 13 + 24} 44 chip.png if={cond}')
        el(f'image {x + 5} 22 36 36 btn_{b}.png if={cond}')
        el(f'text {x + 48} 26 26 {WHITE} left "{t}" if={cond}')
        x += 36 + 16 + len(t) * 13 + 24 + 10
comment('sub-screen hints')
hintrow([('L', 'Prev'), ('R', 'Next'), ('B', 'Back')], c('$BAT&&$M==14'))
hintrow([('B', 'Back')], c('$BAT&&$M==13'))
hintrow([('L', 'Profile'), ('R', 'Skill'), ('Y', 'Zoom'), ('X', 'Sort'), ('B', 'Back'), ('A', 'OK')], c('$DLON'))
hintrow([('B', 'Back'), ('A', 'OK')], c('$POP||$TALK||$TGT'))
hintrow([('A', 'OK')], c('$NCH'))

# ---------------------------------------------------------------- popup menus (Status, ...)
def menu(title, rows, sel_exprs, cond, x0=960 - 230, y0=430, bg='#0C0F16E6'):
    w = 460
    el(f'rect {x0} {y0} {w} {64 + len(rows) * 54 + 70} {bg} if={cond}')
    el(f'rect {x0} {y0} {w} 4 #5FD3FFFF if={cond}')
    el(f'text {x0 + 24} {y0 + 16} 28 {CYAN} left "{title}" if={cond}')
    for i, (t, se) in enumerate(zip(rows, sel_exprs)):
        y = y0 + 64 + i * 54
        el(f'image {x0 + 16} {y} {w - 32} 46 row_sel.png if={cond} and={c(se)}')
        el(f'text {x0 + 40} {y + 6} 30 {WHITE} left "{t}" if={cond}')
    yd = y0 + 64 + len(rows) * 54 + 14
    el(f'text {x0 + 24} {yd} 24 {DIM} left "{{0}}" v={c("$CD")} fit={w - 48} if={cond}')
comment('Status popup (Analyze/Status): Enemy / Demon')
let('STAT', "$POP && (has($CD, 'enemy demon') || has($CD, 'your demon'))")
menu('STATUS', ['Enemy', 'Demon'], ["has($CD,'enemy')", "!has($CD,'enemy')"], c('$STAT'))
comment('any other popup: its description')
menu('SELECT', [], [], c('$POP&&!$STAT'))
comment('Talk: Scout / Fundraise / Trade (the game draws the target on the top screen)')
menu('TALK', ['Scout', 'Fundraise', 'Trade'],
     ["has($CD,'join')", "has($CD,'donate')", "has($CD,'Bargain')"],
     c('$TALK'), x0=24, y0=160, bg='#0C0F1680')

comment('negotiation answers (the demon speaks in the top-screen message box)')
NX, NY, NW = 1240, 300, 620
el(f'rect {NX} {NY} {NW} {4 * 62 + 28} #0A0D14E0 if={c("$NCH")}')
el(f'rect {NX} {NY} {NW} 4 #5FD3FFFF if={c("$NCH")}')
for k in range(4):
    y = NY + 18 + k * 62
    show = c(f'$NCH&&len($NL{k})>0')
    el(f'image {NX + 14} {y} {NW - 28} 54 row_sel.png if={show} and={c(f"$NS{k}")}')
    el(f'text {NX + 40} {y + 9} 32 {WHITE} left "{{0}}" v={c(f"$NL{k}")} fit={NW - 70} if={show}')

# ---------------------------------------------------------------- swap / summon: native stock grid
comment('demon stock grid (Swap / Summon)')
let('DCOL', 'u32($DL + 0xDFC)')
let('DROW', 'u32($DL + 0xE08)')
let('DN', 'u16($DL + 0x28)')
let('DSEL', 'u16($DL + 0x3A)')
GX, GY, CW, CH, PXP, PYP = 1004, 300, 214, 92, 222, 98
el(f'rect {GX - 16} 92 {4 * PXP + 24} {GY + 6 * PYP - 92 + 16} #0A0D14D0 if={c("$DLON")}')
el(f'rect {GX - 16} 92 {4 * PXP + 24} 4 #5FD3FFFF if={c("$DLON")}')
el(f'text {GX} 104 28 {CYAN} left "DEMONS" if={c("$DLON")}')
el(f'text {GX + 4 * PXP - 14} 108 24 {WHITE} right "{{0}}" v={c("lookup(\'names.txt\',$DSEL)")} fit=520 if={c("$DLON&&$DSEL>0")}')
def cell(x, y, face, name, lvl, sub, sel, cond, dim=None):
    el(f'rect {x} {y} {CW} {CH} #161B26E8 if={cond}')
    el(f'image {x - 4} {y - 4} {CW + 8} {CH + 8} card_target.png if={cond} and={sel}')
    el(f'rect {x + 6} {y + 11} 100 70 #0C0F16FF if={cond}')
    el(f'image {x + 6} {y + 11} 100 70 bu/bu_{{0}}.png v={face} crop=0,0,1,0.607 if={cond}')
    el(f'text {x + 114} {y + 8} 22 {WHITE} left "{{0}}" v={name} fit={CW - 120} if={cond}')
    el(f'text {x + 114} {y + 36} 20 {CYAN} left "Lv {{0}}" v={lvl} if={cond}')
    if sub:
        nd = f' and=={dim[1:].join(["!(", ")"])}' if dim else ''
        el(f'text {x + 114} {y + 62} 17 {DIM} left "{sub[0]}" v={sub[1]} fit={CW - 120} if={cond}{nd}')
    if dim:
        el(f'rect {x} {y} {CW} {CH} #0A0D14B0 if={cond} and={dim}')
        el(f'text {x + CW - 8} {y + 64} 18 {GOLD} right "PARTY" if={cond} and={dim}')
# party bar (row 0): Nanashi + the three party demons
for i in range(4):
    x, y = GX + i * PXP, 152
    face = c("'nanashi'" if i == 0 else f"lookup('bu_index.txt',smt4a_demon({i}))")
    cond = c(f'$DLON&&$P{i}')
    cell(x, y, face, c(f'$N{i}'), c(f'smt4a_level({i})'),
         ('HP {0}  MP {1}', f'{c(f"smt4a_hp({i})")} v={c(f"smt4a_mp({i})")}'),
         c(f'$DROW==0&&$DCOL=={i}'), cond)
el(f'rect {GX} {GY - 32} {4 * PXP - 8} 2 #5FD3FF60 if={c("$DLON")}')
el(f'text {GX} {GY - 30} 20 {DIM} left "STOCK" if={c("$DLON")}')
for r in range(6):
    for col in range(4):
        i = r * 4 + col
        x, y = GX + col * PXP, GY + r * PYP
        let(f'SR{i}', f'smt4a_save() + 0xF4 + u8($DL + 0xA80 + {i * 8}) * 0x12C')
        let(f'SI{i}', f'$DLON && {i} < $DN ? u16($SR{i} + 0x62) : 0')
        cond = c(f'$SI{i}>0')
        cell(x, y, c(f"lookup('bu_index.txt',$SI{i})"), c(f"lookup('names.txt',$SI{i})"), c(f'u16($SR{i}+0x64)'),
             ('HP {0}  MP {1}', f'{c(f"u16($SR{i}+0x2A)")} v={c(f"u16($SR{i}+0x2C)")}'),
             c(f'$DROW=={r + 1}&&$DCOL=={col}'), cond, dim=c(f'u32($DL+{0xA84 + i * 8})==0'))

# ---------------------------------------------------------------- write the ini
if __name__ == '__main__':
    src = open('/home/claude/azahar/dist/screen_regions/000400000019A200.ini').read()
    head, rest = src.split('# --- Battle ---', 1)
    import campgen
    tail = campgen.section() + rest[rest.index('# --- Toggleable'):]
    battle = '''# --- Battle -------------------------------------------------------------------
#  Native HUD drawn from the game's battle data with original art (no bottom-screen crops):
#  hints top-left, the acting member's portrait, skill/item list and description bottom-left,
#  command row centred, party cards bottom-centre. The game's top screen stays full-size.
[profile battle]
hide_bottom = 1
[hud battle]
''' + '\n'.join(L) + '\n\n'
    out = head + battle + tail
    for p in sys.argv[1:]:
        open(p, 'w').write(out)
    print(len(L), 'hud lines')
