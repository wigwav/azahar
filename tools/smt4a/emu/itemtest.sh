#!/bin/bash
# itemtest.sh SLOT INDEX TAG : open Item list, move to INDEX, press A, report mode
D=/tmp/claude-0/emu
$D/fresh.sh
C=("load $1" "run 5" "tap right 25" "run 30")
for j in $(seq $2); do C+=("tap down 12" "run 4"); done
C+=("run 40" "hold a 10" "run 40" "film $D/film/it$3_ 1 1")
timeout 500 $D/drv.sh cmd "${C[@]}" | grep DIED
python3 -c "
import sys; sys.path.insert(0,'$D'); from fr import load
f=load('$D/film/it$3_0000.bin'); f['bottom'].save('$D/film/it$3.png')
print('$3','idx',$2,'M',f['u32'](0x18A0),'list',f['u32'](0x113FC),'cur',f['u32'](0x82C),'inp',hex(f['u32'](0x14D4)))"
