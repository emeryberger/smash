# Load the generated transpiler output and check the transitions behave like the
# Lean model (SmashCoreConc). We reconstruct Page as a 6-tuple over the enums and
# check every transition on every reachable-ish page state.
import itertools, importlib.util, sys

import types
m = types.ModuleType("m"); m.__file__="smashcoreconc_431d.py"; sys.modules["m"]=m
exec(compile(open("smashcoreconc_431d.py").read(),"smashcoreconc_431d.py","exec"), m.__dict__)

PS = [m.EMPTY, m.ACTIVE, m.ACTIVE_MONITORING, m.COMPRESSING, m.COMPRESSED, m.COMPRESSED_SHADOW]
PR = [m.PROT_RW, m.PROT_READ, m.PROT_NONE]
B  = [False, True]

def mkpage(s, pr, hb, hp, dq, fl):
    return m.Page_mk(s, pr, hb, hp, dq, fl)

# Reference semantics in plain Python, transcribed from the Lean defs.
def name(x): return type(x).__name__
def ref_faultRestore(p):
    s=name(p.field_0)
    if s=="COMPRESSED":        return mkpage(m.ACTIVE(), m.PROT_RW(), False, True, p.field_4, p.field_5)
    if s=="COMPRESSED_SHADOW": return mkpage(m.ACTIVE(), m.PROT_RW(), False, p.field_3, p.field_4, p.field_5)
    if s=="ACTIVE_MONITORING": return mkpage(m.ACTIVE(), m.PROT_RW(), p.field_2, p.field_3, p.field_4, p.field_5)
    if s=="ACTIVE":            return mkpage(p.field_0, m.PROT_RW(), p.field_2, p.field_3, p.field_4, p.field_5)
    return p
def ref_appFree(p):
    if name(p.field_0)!="EMPTY" and p.field_5==False and p.field_4==False:
        return mkpage(m.EMPTY(), p.field_1, False, False, True, p.field_5)
    return p
def ref_decProcess(buggy,p):
    if p.field_4==True:
        q=mkpage(p.field_0,p.field_1,p.field_2,p.field_3,False,True)
        return q if buggy else mkpage(q.field_0,m.PROT_RW(),q.field_2,q.field_3,q.field_4,q.field_5)
    return p
def ref_allocPop(p):
    if p.field_5==True and name(p.field_0)=="EMPTY":
        return mkpage(m.ACTIVE(),p.field_1,p.field_2,True,p.field_4,False)
    return p

def eq(a,b):
    return (name(a.field_0)==name(b.field_0) and name(a.field_1)==name(b.field_1)
            and a.field_2==b.field_2 and a.field_3==b.field_3
            and a.field_4==b.field_4 and a.field_5==b.field_5)

fails=0; total=0
for s,pr,hb,hp,dq,fl in itertools.product(PS,PR,B,B,B,B):
    p=mkpage(s(),pr(),hb,hp,dq,fl)
    checks=[("faultRestore",m.fault_restore(p),ref_faultRestore(p)),
            ("appFree",m.app_free(p),ref_appFree(p)),
            ("allocPop",m.alloc_pop(p),ref_allocPop(p)),
            ("decProcess/F",m.dec_process(False,p),ref_decProcess(False,p)),
            ("decProcess/T",m.dec_process(True,p),ref_decProcess(True,p))]
    for nm,got,exp in checks:
        total+=1
        if not eq(got,exp):
            fails+=1
            if fails<=5: print(f"MISMATCH {nm}: in=({name(s())},{name(pr())},{hb},{hp},{dq},{fl})")
print(f"\n{total-fails}/{total} transition checks passed"+(" — ALL MATCH" if fails==0 else f" — {fails} FAIL"))
sys.exit(1 if fails else 0)
