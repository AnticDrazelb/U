# cgjit.py <dir>: what the translated code executed, from callgrind --dump-instr=yes (<dir>/cg.out),
# the harness's DUMP_JIT (<dir>/dump) and perf map (<dir>/perf): instructions by kind, hottest blocks
# cgjit.py <dir>: callgrind per-instruction counts inside JIT blocks, categorised
import sys,os,re,subprocess,collections,bisect
d=sys.argv[1]
mp=open(os.path.join(d,'perf',os.listdir(os.path.join(d,'perf'))[0])).read().split('\n')
blocks=[]
for l in mp:
    p=l.split()
    if len(p)>=3: blocks.append((int(p[0],16),int(p[1],16),p[2]))
blocks.sort(); starts=[b[0] for b in blocks]
cost=collections.Counter()
pos=0; skip=False
for l in open(os.path.join(d,'cg.out')):
    if l.startswith('calls='): skip=True; continue
    c=l[0]
    if c in '0123456789+-*':
        parts=l.split()
        a=parts[0]
        if a.startswith('0x'): pos=int(a,16)
        elif a.startswith('+'): pos+=int(a[1:],16) if a[1:].startswith('0x') else int(a[1:])
        elif a.startswith('-'): pos-=int(a[1:],16) if a[1:].startswith('0x') else int(a[1:])
        if skip: skip=False; continue
        if len(parts)>2: cost[pos]+=int(parts[2])
    else:
        skip=False
jit=collections.Counter(); perblock=collections.Counter(); tot=sum(cost.values())
for a,v in cost.items():
    i=bisect.bisect_right(starts,a)-1
    if i>=0 and a<blocks[i][0]+blocks[i][1]:
        jit[a]+=v; perblock[blocks[i][2]]+=v
print('total Ir %d, in JIT %d (%.1f%%)'%(tot,sum(jit.values()),100*sum(jit.values())/tot))
# disassemble each block with addresses
cat=collections.Counter(); catn={}
ins_at={}
for s,sz,name in blocks:
    f=os.path.join(d,'dump',name+'.bin')
    if not os.path.exists(f) or perblock[name]==0: continue
    data=open(f,'rb').read()
    bs=' '.join('0x%02x'%c for c in data)
    out=subprocess.run(['llvm-mc','--disassemble','-triple=x86_64','--output-asm-variant=1','-show-encoding'],input=bs,capture_output=True,text=True).stdout
    off=0
    for l in out.split('\n'):
        m=re.match(r'\s+(.*?)\s+# encoding: \[(.*)\]',l)
        if not m: continue
        n=len(m.group(2).split(','))
        ins_at[s+off]=(m.group(1).strip(),name); off+=n
def classify(t):
    if re.search(r'\[r15 \+ 8\*r[a-z0-9]+ \+ (376|440)\]|\[r15 \+ 368\]',t) : return 'RSB push/pop'
    if '[r15 + 360]' in t: return 'halt check'
    if re.match(r'(cmc|lahf|sahf|seto)',t) or '[r15 + 76]' in t or '[r15 + 77]' in t: return 'flags'
    if re.search(r'\[r15 \+ (6[04])\]',t): return 'PC/upper descriptor'
    if re.search(r'\[r15( \+ \d+)?\]',t) and not 'xmm' in t: return 'guest GPR load/store'
    if re.search(r'\[r15',t): return 'guest ext/vector reg'
    if '[r13 +' in t: return 'fastmem access'
    if '[r14 +' in t or t.startswith('bt'): return 'page walk'
    if re.match(r'j|call|ret',t): return 'branch/jump'
    if t.startswith('lea'): return 'lea'
    if t.startswith('nop'): return 'nop'
    return 'other'
unknown=0
for a,v in jit.items():
    if a in ins_at:
        t,name=ins_at[a]; k=classify(t); cat[k]+=v
        catn.setdefault(k,collections.Counter())[t.split()[0]]+=v
    else: unknown+=v
T=sum(cat.values())
for k,v in cat.most_common(): print('  %-22s %5.1f%%   %s'%(k,100*v/T, ', '.join('%s %d%%'%(x,100*y/v) for x,y in catn[k].most_common(4))))
print('  (not matched: %d)'%unknown)
print('hottest blocks:', ', '.join('%s %.1f%%'%(n[5:14],100*v/sum(perblock.values())) for n,v in perblock.most_common(8)))
