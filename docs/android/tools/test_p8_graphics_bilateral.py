#!/usr/bin/env python3
"""Real GFX10 EXP feeding the bilateral decision, including propagation."""
from dataclasses import replace
import struct
import sys
from p8_bilateral import Version, Operation, decide, operation_reference

def pack(values):return struct.pack('<%dI'%len(values),*values)

def main(exe):
    def ver(n,size,order=1,fmt=0):return Version(0x10000*n,n,1,order,0,size,format=fmt)
    code=pack([0xf80008cf,0x03020100,0xbf810000])
    v=[0]*256
    for i in range(3):v[i*64]=0x3f000000
    v[3*64]=0x3f800000
    roots_data=[code,struct.pack('<QI',1,0xc0),pack([0]),pack(v)]
    roots={ver(i+1,len(x)):x for i,x in enumerate(roots_data)}
    export=ver(10,16,2);material=ver(11,4,3,37);post=ver(12,4,4,37);final=ver(13,4,5,37)
    p=dict(wave=64,addresses=[],lds=False,target=12,enabled=15,compressed=0,lanes=1)
    a=Operation(10,5,'guest_export',tuple(roots),export,p)
    b=Operation(11,8,'pack_rgba8_unorm',(export,),material,{})
    c=Operation(12,12,'copy',(material,),post,{})
    d=Operation(13,16,'copy',(post,),final,{})
    ops=[a,b,c,d];census=[10,11,12,13]
    bright_exp=operation_reference(a,list(roots.values()),exe)
    bright=operation_reference(b,[bright_exp],exe)
    assert bright==b'\x80\x80\x80\xff'
    v[:]=[0]*256;v[3*64]=0x3f800000
    bad_inputs=[*roots_data[:3],pack(v)]
    dark_exp=operation_reference(a,bad_inputs,exe)
    dark=operation_reference(b,[dark_exp],exe);assert dark==b'\x00\x00\x00\xff'
    def run(obs,ops_=ops,census_=census,boundary=(material,)):
        return decide(ops_,roots,obs,census_,boundary,final,0,exe)
    failed=run({export:dark_exp,material:dark,post:dark,final:dark})
    assert failed.status=='FAIL' and failed.interval==(0,8) and failed.counterexample['path']==census,failed
    passed=run({export:bright_exp,material:bright,post:dark,final:dark})
    assert passed.status=='PASS' and passed.interval==(8,16) and passed.counterexample['path']==[12,13],passed
    assert run({export:dark_exp,material:bright,post:dark,final:dark}).status=='BLOCKED'
    assert run({export:bright_exp,material:bright,post:dark,final:dark},census_=[10,11,12,13,14]).status=='BLOCKED'
    assert run({material:bright,post:dark,final:dark}).status=='BLOCKED'
    assert run({export:bright_exp,material:bright,post:dark,final:dark},boundary=(export,)).status=='BLOCKED'
    wrong=replace(a,parameters={**p,'target':13})
    assert run({export:bright_exp,material:bright,post:dark,final:dark},ops_=[wrong,b,c,d]).status=='BLOCKED'
    print('GFX10 EXP -> pack -> post -> flip: P8 PASS, FAIL and 5 negative controls')
if __name__=='__main__':main(sys.argv[1])
