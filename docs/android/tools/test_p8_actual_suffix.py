#!/usr/bin/env python3
"""Bilateral fixtures with the real #756 compositor and observed flip pixel.

The upstream operations and altered roots are synthetic controls, not a
Demon's Souls P8 decision. Only the final propagation edge is real.
"""
from dataclasses import replace
from pathlib import Path
import struct,sys
import numpy as np
from prelight_vertex_oracle import LLVM
from p8_bilateral import Version,Operation,decide,operation_reference
from p8_compositor_isa import Replay
from p8_paired_replay import Archive

def run(path,runner):
    a=Archive(Path(path));d=LLVM()
    try:
        r=Replay(a);x,y=960,540;params=r.contract(x,y);quad=r.quad(x,y)
        pred,reads,_=r.execute(d,runner,quad);baseline=struct.pack('<I',int(r.actual[y,x]));assert baseline==struct.pack('<I',pred[0])
        overlay,scene=r.payloads;changed=np.frombuffer(scene,'<f2').copy().reshape(r.images[r.slots[1]].shape)
        coords=set(t for row in reads if row['descriptor']==r.slots[1] for t in row['texels_by_lane'][0]);assert coords
        for i,j in coords:changed[j,i,:3]=.25
        bright=changed.tobytes()
        # Artificial upstream roots/operations; final actual flip uses exact
        # original owner, allocation, version, write order, and pixel offset.
        root=Version(0x100000000000,1,1,1,0,len(scene));cut=Version(0x200000000000,2,1,2,0,len(scene))
        v=r.identities[r.slots[1]];sc=Version(v['owner'],v['native'],v['version'],v['write_order'],0,len(scene),v['mip'],v['layer'],v['aspect'],v['object'],v['format'])
        v=r.identities[r.slots[0]];ov=Version(v['owner'],v['native'],v['version'],v['write_order'],0,len(overlay),v['mip'],v['layer'],v['aspect'],v['object'],v['format'])
        v=r.output;fin=Version(v['owner'],v['native'],v['version'],v['write_order'],4*(y*r.w+x),4,v['mip'],v['layer'],v['aspect'],v['object'],v['format'])
        ops=[Operation(1,7,'copy',(root,),cut,{}),Operation(2,13,'copy',(cut,),sc,{}),Operation(3,16,'compositor_pixel_isa',(ov,sc),fin,params)]
        roots={root:bright,ov:overlay};census=[1,2,3]
        def verdict(observed,roots_=roots,census_=census,boundary=(cut,)):
            return decide(ops,roots_,observed,census_,boundary,fin,0,runner)
        fail=verdict({cut:scene,sc:scene,fin:baseline});assert fail.status=='FAIL' and fail.interval==(0,8) and fail.counterexample['path']==[1,2,3],fail
        passed=verdict({cut:bright,sc:scene,fin:baseline});assert passed.status=='PASS' and passed.interval==(8,16) and passed.counterexample['path']==[2,3],passed
        assert verdict({cut:bright,sc:scene,fin:baseline},census_=[1,3]).status=='BLOCKED'
        assert verdict({cut:bright,sc:scene,fin:baseline},boundary=(sc,)).status=='BLOCKED'
        assert verdict({cut:bright,fin:baseline}).status=='BLOCKED'
        outside=np.frombuffer(scene,'<f2').copy().reshape(changed.shape);outside[0,0,:3]=.25
        assert verdict({cut:scene,sc:scene,fin:baseline},roots_={root:outside.tobytes(),ov:overlay}).status=='BLOCKED'
        print('actual compositor propagation fixture: FAIL',fail.counterexample['path'],fail.counterexample['replayed_luminance'])
        print('actual compositor propagation fixture: PASS',passed.counterexample['path'],passed.counterexample['replayed_luminance'])
        print('four real-suffix negative controls passed; upstream synthetic, no real P8 decision')
    finally:d.close();a.close()
if __name__=='__main__':run(sys.argv[1],sys.argv[2])
