# cgblock.py <dir> [n]: (04.23) the n-th hottest translated block (from cgjit's inputs), each host
# instruction with what callgrind counted on it
import sys,os,re,subprocess,collections,bisect
d=sys.argv[1]; want=int(sys.argv[2]) if len(sys.argv)>2 else 0
mp=open(os.path.join(d,'perf',os.listdir(os.path.join(d,'perf'))[0])).read().split('\n')
blocks=sorted((int(p[0],16),int(p[1],16),p[2]) for p in (l.split() for l in mp) if len(p)>=3)
starts=[b[0] for b in blocks]
cost=collections.Counter(); pos=0; skip=False
for l in open(os.path.join(d,'cg.out')):
    if l.startswith('calls='): skip=True; continue
    if l[0] in '0123456789+-*':
        parts=l.split(); a=parts[0]
        if a.startswith('0x'): pos=int(a,16)
        elif a.startswith('+'): pos+=int(a[1:],16) if a[1:].startswith('0x') else int(a[1:])
        elif a.startswith('-'): pos-=int(a[1:],16) if a[1:].startswith('0x') else int(a[1:])
        if skip: skip=False; continue
        if len(parts)>2: cost[pos]+=int(parts[2])
    else: skip=False
per=collections.Counter()
for a,v in cost.items():
    i=bisect.bisect_right(starts,a)-1
    if i>=0 and a<blocks[i][0]+blocks[i][1]: per[i]+=v
i=per.most_common()[want][0]; s,sz,name=blocks[i]
data=open(os.path.join(d,'dump',name+'.bin'),'rb').read()
out=subprocess.run(['llvm-mc','--disassemble','-triple=x86_64','--output-asm-variant=1','-show-encoding'],input=' '.join('0x%02x'%c for c in data),capture_output=True,text=True).stdout
off=0; print(name, 'share %.1f%%'%(100*per[i]/sum(per.values())))
for l in out.split('\n'):
    m=re.match(r'\s+(.*?)\s+# encoding: \[(.*)\]',l)
    if not m: continue
    n=len(m.group(2).split(',')); c=cost.get(s+off,0)
    if c: print('%8d  %s'%(c, m.group(1).strip()))
    off+=n
