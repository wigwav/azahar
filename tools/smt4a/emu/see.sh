#!/bin/bash
# see.sh name "cmd"... : run cmds, then dump + render HUD; writes scratchpad/<name>.png (what the
# user sees, left) next to the game's real bottom screen (right)
D=/tmp/claude-0/emu; n=$1; shift
INI=${INI:-/tmp/claude-0/full.ini}
timeout 1200 $D/drv.sh cmd "$@" "hud $INI /home/claude/smt4a/hud3 battle $D/$n.rgba" "dump $D/$n.bin" >/dev/null
python3 - "$D/$n" <<'PY'
import sys, numpy as np
sys.path.insert(0,'/home/claude/smt4a/tools')
from rec import Raw
from PIL import Image
p=sys.argv[1]; r=Raw(p+'.bin')
win=Image.new('RGBA',(1920,1080),(0,0,0,255))
win.paste(r.top.convert('RGBA').resize((1800,1080),Image.BILINEAR),(60,0))
h=Image.fromarray(np.fromfile(p+'.rgba',np.uint8).reshape(1080,1920,4))
win=Image.alpha_composite(win,h).convert('RGB').resize((1280,720))
out=Image.new('RGB',(1280+480,720),(20,20,20)); out.paste(win,(0,0)); out.paste(r.img.convert('RGB').resize((480,360)),(1280,0))
o=0x8650000
out.save('/tmp/claude-0/-home-claude-azahar/759ff5b5-2ca7-5d7b-86b8-817c3fc82596/scratchpad/'+p.split('/')[-1]+'.png')
print('A',r.u32(o+0x554),'M',r.u32(o+0x18A0),'inp',hex(r.u32(o+0x14d4)),'L',r.u32(o+0x113FC),'cur',r.u32(o+0x5C8+min(r.u32(o+0x554),3)*0x64) if r.u32(o+0x554)<4 else '-')
PY
