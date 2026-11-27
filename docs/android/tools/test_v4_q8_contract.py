#!/usr/bin/env python3
"""Local independent contract fixtures: equality, wrong operands, reductions,
missing capture, and deliberate overflow. No game correctness claim."""
import numpy as np
import v4_q8_contract as q

def fail(fn,typ,part):
    try:fn()
    except typ as e:
        assert part in str(getattr(e,'row',e)),(part,e)
    else:raise AssertionError('mutation escaped: '+part)

def test():
    a=np.zeros(q.WORDS,'<u4');a[:3]=[1,0,15]
    f,o,t,s=q.parse_trace(bytearray(a.tobytes()),0,15);assert f.shape==(64,96) and not s
    fail(lambda:q.parse_trace(a[:-1].tobytes(),0,15),ValueError,'truncated')
    fail(lambda:q.parse_trace(a.tobytes(),0,16),ValueError,'stale')
    a[3]=1;fail(lambda:q.parse_trace(a.tobytes(),0,15),ValueError,'overflow');a[3]=0
    active=[0,15,63];raw={'buffer_descriptors':[[0]*8 for _ in range(4)]};d=[4096,4<<16,96,81924];raw['buffer_descriptors'][3]=d+[0]*4
    b={'guest':4096,'bytes':384};buf=np.zeros(96,'<u4')
    for l,idx,word in zip(active,[0,33,66],[1,2,0]):
        buf[idx]=word;f[l,27]=0xcf9;f[l,28]=idx;f[l,29:33]=d;f[l,34]=0xcf9;f[l,35]=word;f[l,36]=0xd4d;f[l,37]=3
    for k,values in enumerate([[2,0,8],[0,4,0]]):
        for l,val in zip(active,values):
            idx=int(f[l,28])+k+1;buf[idx]=val;r=o[k,l];r[:6]=[0xd69,idx,0xd69,val,0xdbd,10 if k==0 else 4]
    c=q.Checks();union,lights=q.verify_mask(c,f,o,active,buf,raw,b);assert (union,lights)==(3,[1,3,34])
    f[15,37]=1;fail(lambda:q.verify_mask(q.Checks(),f,o,active,buf,raw,b),q.Divergence,'outer_wave_or');f[15,37]=3
    o[1,63,5]=0;fail(lambda:q.verify_mask(q.Checks(),f,o,active,buf,raw,b),q.Divergence,'inner_wave_or');o[1,63,5]=4
    f[15,34]=0;fail(lambda:q.verify_mask(q.Checks(),f,o,active,buf,raw,b),ValueError,'missing trace');f[15,34]=0xcf9
    f[15,29]=0;fail(lambda:q.verify_mask(q.Checks(),f,o,active,buf,raw,b),ValueError,'descriptor mismatch');f[15,29]=4096
    calc=lambda pc,j,a,b,d:q.bits32(np.float32(q.float32(a)+q.float32(b)*(q.float32(d) if d is not None else 1)))
    for i in lights:
        r=t[1+q.TERMS+i];r[0]=0x1d5d;r[1:7]=[q.bits32(.25),q.bits32(.5)]*3;r[10]=0x1d65;r[11:14]=[q.bits32(.75)]*3
    result=q.verify_terms(q.Checks(),t,lights,15,calc);assert len(result['secondary_contributions'])==3
    t[1+q.TERMS+3,11]=q.bits32(.5);fail(lambda:q.verify_terms(q.Checks(),t,lights,15,calc),q.Divergence,'secondary_add')
    t[1+q.TERMS+3,11]=q.bits32(.75);t[1+q.TERMS+3,:]=0;fail(lambda:q.verify_terms(q.Checks(),t,lights,15,calc),ValueError,'missing or extra')
    # Do not confuse signed zero with a numerical error, or certify nonfinite.
    q.checked_float(q.Checks(),0,'zero',0,0x80000000,0)
    fail(lambda:q.checked_float(q.Checks(),0,'invalid',0x7fc00000,0x7fc00000,0),ValueError,'nonfinite')
    print('V4 q8 contract fixtures: PASS (admission layout, OR/load, arithmetic, malformed capture)')
if __name__=='__main__':test()
