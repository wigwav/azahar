"""Print JSON list of device paths for RomFS files read in the last run that are missing."""
import json,bisect,sys
m=json.load(open('/home/claude/smt4a/emu/smt4a.cxi.json')); L=json.load(open('/home/claude/smt4a/romfs_layout.json'))
have=set(json.load(open('/home/claude/smt4a/emu/have.json')))
st=sorted((m['data']+e['off'],e) for e in L); keys=[s for s,_ in st]
need=[]
for l in open(sys.argv[1] if len(sys.argv)>1 else '/home/claude/smt4a/emu/d/last_err.log'):
  if not l.startswith('ROMFS'): continue
  off,ln=[int(x,16) for x in l.split()[1:3]]
  i=bisect.bisect_right(keys,off)-1
  while i<len(st) and st[i][0]<off+ln:
    e=st[i][1]
    if e['path'] not in have and e['path'] not in need and not e['path'].startswith('snd/'): need.append(e['path'])
    i+=1
B='C:\\Users\\super\\AppData\\Roaming\\Azahar\\dump\\romfs\\000400000019A200\\'
print(len(need), file=sys.stderr)
for k in range(0,len(need),50):
  print(json.dumps([B+p.replace('/','\\') for p in need[k:k+50]]))
