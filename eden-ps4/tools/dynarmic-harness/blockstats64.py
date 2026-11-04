# blockstats64.py <dir>: (04.23) how often translated blocks were entered (the count on each block's
# first instruction), the JIT instructions per entry, and the guest instructions per block entered
# (weighted), from cgjit's inputs (callgrind --dump-instr=yes, DUMP_JIT, the perf map)
import sys,os,re,collections,bisect
d=sys.argv[1]
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
entries=0; jit=0
for i,(s,sz,name) in enumerate(blocks):
    entries+=cost.get(s,0)
for a,v in cost.items():
    i=bisect.bisect_right(starts,a)-1
    if i>=0 and a<blocks[i][0]+blocks[i][1]: jit+=v
print('block entries %d, JIT instructions %d, %.1f a block entered'%(entries,jit,jit/max(entries,1)))
