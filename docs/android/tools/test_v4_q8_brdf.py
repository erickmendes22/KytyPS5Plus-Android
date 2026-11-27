#!/usr/bin/env python3
import numpy as np
import v4_q8_contract as q
import v4_q8_brdf as b
import v4_q8_pair_replay as r

def rejects(fn):
    try:fn()
    except ValueError:return
    raise AssertionError('incomplete/unbound trace accepted')

def main():
    assert b.STRIDE==198 and b.EXTRA_WORDS==88704
    a=np.zeros(q.PAIR_WORDS+b.EXTRA_WORDS,'<u4')
    a[q.PAIR_WORDS]=3;a[q.PAIR_WORDS+b.LIGHTS*b.STRIDE]=7
    assert b.extract(a.tobytes(),q.PAIR_WORDS,'dark')[0,0]==3
    assert b.extract(a.tobytes(),q.PAIR_WORDS,'control')[0,0]==7
    rejects(lambda:b.extract(a[:-1].tobytes(),q.PAIR_WORDS,'dark'))
    row=np.zeros(b.STRIDE,'<u4');record=np.zeros(48,'<u4');term=np.zeros(24,'<u4')
    assert not b.validate(row,record,term)
    row[0]=record[30]=0x1bb5;row[b.SCALAR:b.SCALAR+2]=[0x1bb9,0x3faaaaab]
    for j,(pc,dst) in enumerate(b.OPS):row[b.RESULTS+2*j]=pc+1
    assert b.validate(row,record,term)
    k=next(j for j,(pc,dst) in enumerate(b.OPS) if pc==0x1c94)
    bad=row.copy();bad[b.RESULTS+2*k]=0;rejects(lambda:b.validate(bad,record,term))
    bad=row.copy();bad[-1]=1;rejects(lambda:b.validate(bad,record,term))
    bad=row.copy();bad[1+b.LIVE.index(49)]=1;rejects(lambda:b.validate(bad,record,term))
    # Deterministic comparator plumbing control. The original-ISA runner
    # itself was exercised with the actual fingerprinted code offline.
    orig=r.run_wave
    try:
        r.run_wave=lambda *args:{(32+j,0):(15,0,(0,0,0,0)) for j in range(20)}
        sizes={pc:4 for pc,dst in b.OPS}
        code=bytes(0x1d54)
        assert b.same_input(code,None,0,0,sizes,row)['status']=='MATCH_LOCAL'
        bad=row.copy();bad[b.RESULTS+2*k+1]=0x3f800000
        result=b.same_input(code,None,0,0,sizes,bad)
        assert result['first']['pc']=='0x1c94'
        bad=row.copy();bad[b.SCALAR+1]=0
        assert b.same_input(code,None,0,0,sizes,bad)['first']['pc']=='0x1bb8'
        r.run_wave=lambda *args:{}
        rejects(lambda:b.same_input(code,None,0,0,sizes,row))
    finally:r.run_wave=orig
    print('V4 BRDF schema 4: layout, disjoint tails, equality, first divergence, missing marker/export, term binding PASS')
if __name__=='__main__':main()
