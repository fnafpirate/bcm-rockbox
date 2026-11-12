import sys,subprocess,re,bisect
f=sys.argv[1]
rd=lambda *a: subprocess.run(['readelf']+list(a)+[f],capture_output=True,text=True).stdout
syms=[]
for l in rd('--dyn-syms','-W').splitlines():
    p=l.split()
    if len(p)>=8 and p[3]=='FUNC' and p[1]!='00000000':
        syms.append((int(p[1],16),int(p[2]),p[7]))
syms.sort()
starts=[s[0] for s in syms]
def fn(a):
    i=bisect.bisect_right(starts,a)-1
    return syms[i][2] if i>=0 and a<syms[i][0]+max(syms[i][1],2) else '?'
rel=[]
for l in rd('-r','-W').splitlines():
    p=l.split()
    if len(p)>=6 and re.match(r'^[0-9a-f]{8}$',p[0]) and p[5] not in ('+',):
        rel.append((int(p[0],16),'',p[5],''))
cur={}
for off,t,s,v in sorted(rel):
    cur.setdefault(fn(off),[]).append((off,t,s,v))
for k,v in sorted(cur.items(), key=lambda kv: min(x[0] for x in kv[1])):
    print(k, ' '.join(f"{x[2]}@{x[0]:x}" for x in v if x[2]))
