# cgtop.py <dir> <n>: the n most executed instruction forms (registers and numbers folded)
import sys,re,collections
exec(open(__import__('os').path.join(__import__('os').path.dirname(__file__),'cgjit.py')).read().split("def classify")[0].replace("print('total","pass #print('total"))
c=collections.Counter()
for a,v in jit.items():
    if a in ins_at:
        t=ins_at[a][0]
        t=re.sub(r'\b(e|r)?[abcd]x\b|\b(e|r)?(si|di)\b|\br\d+d?\b|\bxmm\d+\b','R',t)
        t=re.sub(r'\b-?\d+\b','N',t); t=re.sub(r'0x[0-9a-f]+','N',t)
        c[t]+=v
T=sum(c.values())
for t,v in c.most_common(int(sys.argv[2])): print('  %5.1f%%  %s'%(100*v/T,t))
