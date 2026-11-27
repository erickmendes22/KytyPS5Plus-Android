#!/usr/bin/env python3
"""Scoped q8 original ISA replay: conditional on captured sampled values.
Uses previously checked wave reductions, and verifies every visited image PC,
index, descriptor and coordinate before injecting the observed result.
This is not a texture sampling or guest-root oracle.
"""
from pathlib import Path
import re
import numpy as np
from p8_paired_replay import need,digest
from p8_guest_graphics import run_wave,pack,merge,decode_all
from prelight_vertex_oracle import LLVM
HASH='c99fdd9616b9975702f3fec398d27d29ccde9a76b75483d1248228e1c03fcaa6'

def u(x):return pack([x])
def mov(d,s):return u(0x7e000200|(d<<17)|s)
def lit(d,x):return mov(d,255)+u(x)
def jump(pc,dst,op=0xbf820000):
    delta=dst-pc-4;need(delta%4==0 and -32768<=delta//4<=32767,'branch range');return u(op|(delta//4&65535))
def exp(t,vs,done=False):return pack([0xf800000f|(t<<4)|(0x800 if done else 0),sum(x<<(8*i) for i,x in enumerate(vs))])
NOP=u(0xbf800000);END=u(0xbf810000)
def smov(r,x):return pack([0xbe8003ff|(r<<16),x])
def scmp(r,x):return pack([0xbf060000|(255<<8)|r,x])
def fl(x):return float(np.asarray(x,dtype='<u4').view('<f4'))
def dist(a,b):
    if a==b or (a&0x7fffffff==b&0x7fffffff==0):return 0
    if not np.isfinite(fl(a)) or not np.isfinite(fl(b)) or a>>31!=b>>31:return 2**32
    return abs(a-b)
def regs(s,k):
    v=[]
    for m in re.finditer(k+r'(?:\[(\d+):(\d+)\]|(\d+))',s):v.extend([int(m[3])] if m[3] else range(int(m[1]),int(m[2])+1))
    return v

def replay(code,runner,mode,wg,lane,raw,buffers,f,o,samples,union,lights,terms=None):
    need(digest(code)==HASH,'unrecognized q8 ISA')
    need(len(raw['user_data'])==2 and len(buffers)==4,'launch operands')
    dec=LLVM()
    try:rows=decode_all(dec,code)
    finally:dec.close()
    sizes={pc:(rows[i+1][0]-pc if i+1<len(rows) else 4) for i,(pc,t) in enumerate(rows)}
    original=code[:rows[-1][0]+4]
    # All v100..v123 and s100..101 are unused by the fingerprinted program.
    need(not any(set(regs(t,'v'))&set(range(100,124)) or set(regs(t,'s'))&{100,101,102,103} for _,t in rows),'reserved registers used')
    parts=[(r['address'],pack(r['words'])) for r in raw['scalar_regions']]+buffers
    seg=merge(parts);sg=[0]*106;sg[:2]=raw['user_data'];sg[2]=wg;sg[3]=0;sg[102]=(1<<lane)&0xffffffff;sg[103]=(1<<lane)>>32
    vg=[0]*(256*64);vg[lane]=lane%8;vg[64+lane]=lane//8
    sites={}
    for pc,t in rows:
        if not t.startswith('image_') or pc==0x1d9c:continue
        vs=regs(t,'v');ss=regs(t,'s');m=re.match(r'image_\w+\s+(v(?:\[\d+:\d+\]|\d+))',t);need(m is not None,'image opcode')
        dst=regs(m[1],'v');coords=vs[len(dst):];need(len(ss) in (8,12) and 2<=len(coords)<=7,'MIMG shape')
        loop=1 if pc==0xa70 else 2 if 0x1000<=pc<=0x1b9c else 0
        sites[pc]=(dst,coords,ss[:8],ss[8:],loop,t.startswith('image_load'))
    need(set(samples)<=set((pc,k) for pc in sites for k in range(1024 if sites[pc][4] else 1)),'unknown sample site')
    # All fixed sample loads, including actual HDR input, must have records.
    expected=dict(samples);expected[(0x1d84,0)]={'coords':list(map(int,f[lane,39:41])),'descriptor':list(map(int,f[lane,68:76])),'sampler':[0]*4,'result':list(map(int,f[lane,41:44]))}
    done={};coord_deltas=[]
    def patched(check_light=None):
        p=bytearray(original)
        def replace(pc,body):
            start=len(p);p[pc:pc+sizes[pc]]=jump(pc,start)+NOP*(sizes[pc]//4-1);p.extend(body(start))
        replace(0xf8,lambda start:code[0xf8:0xfc]+u(0x87fe7e66)+jump(start+8,0xfc))
        # Wave reductions were independently checked across the actual wave.
        p[0xd00:0xd50]=NOP*((0xd50-0xd00)//4);p[0xd00:0xd0c]=code[0xd04:0xd08]+smov(86,union)
        p[0xd70:0xdc0]=NOP*((0xdc0-0xd70)//4)
        def inner(start):
            b=bytearray(code[0xd74:0xd78]);cases=[]
            for k in range(32):
                if union>>k&1:b+=scmp(87,k);cases.append((len(b),int(o[k,lane,5])));b+=NOP
            b+=lit(100,0xd70)+exp(60,[100]*4,True)+END
            for loc,val in cases:
                b[loc:loc+4]=jump(start+loc,start+len(b),0xbf850000);b+=smov(89,val);b+=jump(start+len(b),0xdc0)
            return b
        replace(0xd70,inner)
        for pc,(dst,coords,desc,sampler,loop,is_load) in sites.items():
            def body(start,pc=pc,dst=dst,coords=coords,desc=desc,sampler=sampler,loop=loop):
                b=bytearray(jump(start,pc+sizes[pc],0xbf880000));cases=[]
                # Only substitute a dynamic read after its operands were checked.
                if loop:
                    b+=u(0x85000000|(101<<16)|(128<<8)|129)
                    for (site,index),vals in done.items():
                        if site==pc:b+=scmp(20 if loop==1 else 91,index);cases.append((len(b),vals));b+=NOP
                    b+=u(0xbf070000|(128<<8)|101)
                elif (pc,0) in done:
                    b+=b''.join(lit(d,v) for d,v in zip(dst,done[(pc,0)]));b+=jump(start+len(b),pc+sizes[pc]);return b
                # Export original address registers, descriptors, index and PC.
                b+=lit(100,pc)+(mov(101,20 if loop==1 else 91) if loop else lit(101,0))
                for j in range(8):b+=mov(104+j,256+coords[j]) if j<len(coords) else lit(104+j,0)
                b+=exp(60,[100,101,104,105])+exp(59,[106,107,108,109])+exp(58,[110,111,110,111])
                for j,r in enumerate(desc):b+=mov(112+j,r)
                b+=exp(61,[112,113,114,115])+exp(62,[116,117,118,119])
                for j in range(4):b+=mov(120+j,sampler[j]) if sampler else lit(120+j,0)
                b+=exp(63,[120,121,122,123],True)+END
                for loc,vals in cases:
                    b[loc:loc+4]=jump(start+loc,start+len(b),0xbf850000);b+=u(0xbf070000|(128<<8)|101)
                    b+=b''.join(lit(d,v) for d,v in zip(dst,vals));b+=jump(start+len(b),pc+sizes[pc])
                return b
            replace(pc,body)
        replace(0x1d9c,lambda start:exp(1,[29,30,31,3])+exp(0,[0,1,2,3],True)+END)
        if check_light is not None:
            def checkpoint(start):
                b=u(0x85000000|(101<<16)|(128<<8)|129)+scmp(91,check_light)
                b+=jump(start+len(b),start+len(b)+4+len(exp(2,[0,50,1,3],True))+4,0xbf840000)
                b+=exp(2,[0,50,1,3],True)+END+u(0xbf070000|(128<<8)|101)+code[0x1d5c:0x1d60]
                return b+jump(start+len(b),0x1d60)
            replace(0x1d5c,checkpoint)
        return bytes(p)
    for _ in range(min(len(expected)+1,128)):
        out=run_wave(runner,patched(),sg,vg,seg,mode,1<<lane)
        if (0,lane) in out:
            need(set(done)==set(expected),'observed samples not visited by replay')
            words=out[0,lane][2];actual=list(map(int,f[lane,48:52]));delta=[dist(a,b) for a,b in zip(words,actual)]
            acc=list(out[1,lane][2][:3]);obs_acc=list(map(int,f[lane,44:47]));ad=[dist(a,b) for a,b in zip(acc,obs_acc)]
            light_checks=[]
            if terms is not None:
                for light in lights:
                    z=run_wave(runner,patched(light),sg,vg,seg,mode,1<<lane)
                    need((2,lane) in z,'missing replay light checkpoint')
                    predicted=list(z[2,lane][2][:3]);observed=list(map(int,terms[1025+light,[2,4,6]]));ld=[dist(a,b) for a,b in zip(predicted,observed)]
                    light_checks.append(dict(light=light,predicted=predicted,observed=observed,ulp=ld))
            worst=max(delta+ad+[d for r in light_checks for d in r['ulp']])
            return dict(status='MATCH_CONDITIONAL' if worst<=8 else 'DIVERGENCE_CANDIDATE',words=list(words),observed=actual,ulp=delta,accumulation_ulp=ad,light_checks=light_checks,coordinate_ulp=coord_deltas,visits=len(done))
        need((60,lane) in out,'missing trap export');pc,k,*co=out[60,lane][2];need(pc in sites,'unsupported reduction path')
        key=(pc,k);need(key not in done,'repeated dynamic site/index')
        if key not in expected:return dict(status='INCOMPLETE',pc=hex(pc),light=k,reason='native sample missing',visits=len(done))
        r=expected[key];dst,coords,desc,samp,loop,is_load=sites[pc]
        actual_desc=list(out[61,lane][2]+out[62,lane][2]);actual_sampler=list(out[63,lane][2]);actual_co=(co+list(out[59,lane][2])+list(out[58,lane][2]))[:len(coords)]
        if actual_desc!=r['descriptor'] or actual_sampler!=r['sampler']:
            return dict(status='DIVERGENCE_CANDIDATE',pc=hex(pc),light=k,reason='descriptor/sampler differs',reference_descriptor=actual_desc,observed_descriptor=r['descriptor'],reference_sampler=actual_sampler,observed_sampler=r['sampler'])
        dif=[(0 if a==b else 2**32) if is_load else dist(a,b) for a,b in zip(actual_co,r['coords'])];need(len(actual_co)==len(r['coords']) and len(dst)==len(r['result']),'sample shape mismatch')
        if max(dif)>8:return dict(status='DIVERGENCE_CANDIDATE',pc=hex(pc),light=k,reason='coordinates differ',reference=actual_co,observed=r['coords'],ulp=dif)
        coord_deltas.append([hex(pc),k,max(dif)]);done[key]=r['result']
    return dict(status='INCOMPLETE',reason='dynamic sample replay limit',visits=len(done))
