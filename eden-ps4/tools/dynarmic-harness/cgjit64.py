# cgjit64.py <dir>: (04.23) cgjit.py for the A64 JIT (A64JitState's layout): what the translated code executed, from callgrind --dump-instr=yes (<dir>/cg.out),
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
    # (cgjit64: A64JitState) x0-x30 0-240, sp 248, pc 256, nzcv 264, fpcr 276, vectors 288-799,
    # halt 808, exclusive 812, rsb_ptr 816, rsb locations 824-887, rsb code pointers 888-951
    m=re.search(r'\[r15(?: \+ (\d+))?(?: \+ 8\*r[a-z0-9]+)?(?: \+ (\d+))?\]',t)
    if m:
        off=int(m.group(1) or m.group(2) or 0)
        if '8*r' in t or off in (816,): return 'RSB push/pop'
        if off==808: return 'halt check'
        if off in (264,276,280): return 'flags/fpcr'
        if off==256: return 'PC'
        if off==248: return 'guest SP'
        if off<248: return 'guest GPR load/store'
        if 288<=off<800: return 'guest vector reg'
        return 'jit state other'
    if re.match(r'(cmc|lahf|sahf|seto|setb|setc|setne|sete|setl|setg)',t): return 'flags'
    if '[r13 +' in t: return 'fastmem access'
    if '[r14 +' in t or t.startswith('bt'): return 'page walk'
    if 'rip' in t and t.startswith('cmp'): return 'fastmem range check'
    if re.match(r'j|call|ret',t): return 'branch/jump'
    if t.startswith('lea'): return 'lea'
    if t.startswith('nop'): return 'nop'
    if t.startswith('mov'): return 'reg moves'
    if re.match(r'(add|sub|and|or|xor|shl|shr|sar|imul|cmp|test|neg|not|inc|dec|ror|rol)',t): return 'ALU'
    if 'xmm' in t: return 'SIMD/FP'
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
