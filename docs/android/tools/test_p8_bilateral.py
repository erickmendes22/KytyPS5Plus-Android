#!/usr/bin/env python3
"""Execute both decisions through the real independent RDNA1 runner.
The deliberate faulty execution variants stand in for observed device bytes;
fixtures do not assert a verdict about Demon's Souls.
"""
import struct
import sys
from dataclasses import replace
from p8_bilateral import Version, Operation, operation_reference, decide, brightness


def main(executable):
    # v_mov_b32 v0, literal; buffer_store_format_x v0,v1,s[0:3],0 idxen; s_endpgm
    def code(pixel): return struct.pack('<5I', 0x7e0002ff,pixel,0xe0102000,0x80000001,0xbf810000)
    def wave(v):
        return dict(code=code(0xff808080),wave=64,exec=1,mode=0,sgprs=[v.owner,0x40000,0x40,0x14004],
                    vgprs=[0]*128,addresses=[v.owner],output_address=v.owner,output_size=4,
                    arch='rdna1',lds_bytes=0,waves_per_group=1)
    root=Version(0x4000,1,1,1,0,4)
    a=Version(0x4000,1,2,2,0,4)
    b=Version(0x5000,2,1,3,0,4)
    c=Version(0x6000,3,1,4,0,4)
    d=Version(0x7000,4,1,5,0,4)
    root2=Version(0x8000,5,1,1,0,4)
    e=Version(0x8000,5,2,6,0,4)
    final=Version(0x9000,6,1,7,0,4)
    dark=struct.pack('<I',0xff000000)
    bright=struct.pack('<I',0xff808080)
    roots={root:dark,root2:dark}
    prefix=Operation(10,3,'guest_wave',(root,),a,wave(root))
    pre_store=Operation(11,7,'copy',(a,),b,{})
    publish=Operation(12,8,'copy',(b,),c,{})
    # A distinct suffix guest wave may be a separate input to composition;
    # the test graph uses raw copy for propagation, not a forced light value.
    suffix=Operation(13,12,'copy',(c,),d,{})
    present=Operation(14,16,'copy',(d,),final,{})
    ref=operation_reference(prefix,[dark],executable)
    wrong=operation_reference(replace(prefix,parameters={**prefix.parameters,'code':code(0xff000000)}),[dark],executable)
    assert ref==bright and wrong==dark, (ref,wrong)
    ops=[prefix,pre_store,publish,suffix,present]
    obs={a:wrong,b:wrong,c:wrong,d:wrong,final:wrong}
    def run(ops,obs,boundary,census=None,roots_=None):
        return decide(ops,roots if roots_ is None else roots_,obs,
                      [x.command for x in ops] if census is None else census,boundary,final,0,executable)
    fail=run(ops,obs,[c]);assert fail.status=='FAIL' and fail.interval==(0,8) and fail.counterexample['path']==[10,11,12,13,14],fail
    # Same guest-side prefix, actual equal. Independent suffix contracts
    # require copying the complete boundary. A faulty native copy produces
    # dark; the final copy propagates the corrected reference.
    obs={a:ref,b:ref,c:ref,d:wrong,final:wrong}
    passed=run(ops,obs,[c]);assert passed.status=='PASS' and passed.interval==(8,16) and passed.counterexample['path']==[13,14],passed
    # Do not promote equal boundary bytes if an earlier error was compensated.
    compensated={a:wrong,b:ref,c:ref,d:wrong,final:wrong}
    assert run(ops,compensated,[c]).status=='BLOCKED'
    # A divergence that vanishes before the final pixel is not causal.
    assert run(ops,{a:wrong,b:wrong,c:wrong,d:ref,final:wrong},[c]).status=='BLOCKED'
    # Missing observed op, omitted command, corrupt byte length, wrong boundary.
    assert run(ops,obs,[c],[10,11,12,13,14,15]).status=='BLOCKED'
    assert run(ops,obs,[b]).status=='BLOCKED'
    assert run(ops,{k:v for k,v in obs.items() if k!=b},[c]).status=='BLOCKED'
    assert run(ops,{**obs,c:b'\x00'},[c]).status=='BLOCKED'
    # Unknown independent contract on a prefix ancestor cannot yield PASS.
    assert run([replace(prefix,kind='not_implemented'),*ops[1:]],obs,[c]).status=='BLOCKED'
    # No dark symptom from independent reference => no G0.
    assert run(ops,{k:ref for k in obs},[c]).status=='BLOCKED'
    assert run(ops,{k:dark for k in obs},[c],roots_={root:dark}).status=='FAIL'
    # Unrelated missing reference is not a veto on an established cone.
    outside=Operation(15,2,'unsupported',(root2,),e,{})
    assert run([*ops,outside],{**obs,e:dark},[c]).status=='PASS'
    assert run([*ops,outside],obs,[c]).status=='PASS'
    # The real independent runner rejects an architectural out-of-range access.
    bad=replace(prefix,parameters={**prefix.parameters,'sgprs':[0x5000,0x40000,0x40,0x14004]})
    assert run([bad,*ops[1:]],obs,[c]).status=='BLOCKED'
    # A copied stale version is not a valid independent operand.
    stale=replace(pre_store,inputs=(root,))
    assert run([prefix,stale,*ops[2:]],obs,[c]).status=='BLOCKED'
    print('P8 finite-cone FAIL:',fail)
    print('P8 finite-cone PASS:',passed)
    # Check actual raw fixed-function contracts and packed flip interpretation.
    mask=struct.pack('<I',0x0000ff00)
    ms=Operation(20,7,'masked_store_u32',(a,b,c),d,{})
    assert operation_reference(ms,[dark,bright,mask],executable)==struct.pack('<I',0xff008000)
    dt=replace(ms,kind='depth_test_d32')
    assert operation_reference(dt,[struct.pack('<f',.75),struct.pack('<f',.25),struct.pack('<I',4)],executable)==struct.pack('<I',1)
    assert brightness(struct.pack('<I',0xc0000000),0,64)==0
    assert abs(brightness(struct.pack('<I',0xffffffff),0,64)-1)<1e-12
    # Guest buffer overlap across native allocations is not independent data.
    alias=Version(root.owner+1,99,1,1,0,3)
    assert run(ops,obs,[c],roots_={**roots,alias:b'\x00'*3}).status=='BLOCKED'
    print('18 rejection/scope/format controls passed; real RDNA1 reference executed')

if __name__=='__main__':main(sys.argv[1])
