#!/usr/bin/env python3
"""Replay the flip-linked fullscreen PS using original ISA plus modeled sampling.

Only the exact #756 shader/descriptor contract is supported. Native images are
operands, never assumed guest reference roots. This is a local executable
propagation edge, not a P8 verdict. Physical decode and HW filter precision
remain outside the reference. No mutation of the captured archive.
"""
import argparse
import json
from pathlib import Path
import struct
import numpy as np
from prelight_vertex_oracle import LLVM
from p8_guest_graphics import pack
from p8_image_trap import execute, LogicalImages
import p8_paired_replay as p

# Captured original words: PS5 sampler word2 [21:20]/[23:22] XY mag/min,
# word0 clamp XYZ and anisotropy. Overlay point/wrap; scene explicit LZ,
# aniso-linear/edge. LZ uses mip0. Model bilinear (not a hardware guarantee).
OVERLAY=(0x8007000,0xfff000,0x5000000,0x40000000)
SCENE=(0x8007492,0xfff000,0xaf00000,0x40000000)
POLICY={OVERLAY:('nearest','wrap','wrap'),SCENE:('linear','clamp','clamp')}

class Replay:
    def __init__(self,archive):
        self.a=archive
        es=[(i,e) for i,e in enumerate(archive.events) if e.get('kind')=='composite_after' and e.get('frame')==archive.frame and e.get('shader')==p.COMPOSITE]
        p.need(len(es)==1,'missing/ambiguous frame compositor')
        i,e=es[0];ui,u=archive.use[e['use']];self.marker=e['use']
        p.need(ui<i and u['frame']==e['frame'] and u['submit']==e['submit'],'execution identity mismatch')
        states=[x for j,x in enumerate(archive.events) if ui<j<i and x.get('kind')=='composite_state' and x.get('use')==self.marker]
        p.need(len(states)==1,'missing raster state');self.state=states[0]
        p.need(len(u['stages'])==2 and u['stages'][0]['shader']==p.FULLSCREEN and u['stages'][1]['shader']==p.COMPOSITE,'pipeline mismatch')
        archive.module('vs',p.FULLSCREEN);self.code=archive.module('ps',p.COMPOSITE)
        st=u['stages'][1];p.need(st['valid'] and not st['buffers'] and not st['bda_writes'] and not st['uses_dma'] and not st['uses_gds'] and len(st['images'])==2,'unsupported bindings')
        raw=st['guest_operands'];self.raw=raw;self.parts=p.scalar_segments(raw);self.ud=raw['user_data'];p.need(len(self.ud)==2,'unexpected user data')
        root=p.ptr(pack(self.ud));r,s=struct.unpack('<QQ',p.read(self.parts,root,16));dw=struct.unpack('<16I',p.read(self.parts,r+32,64))
        p.need({struct.unpack('<4I',p.read(self.parts,s+x,16)) for x in (0xa0,0xc0)}==set(POLICY),'unknown sampler state')
        self.images={};self.identities={};self.slots=[];self.payloads=[]
        for n,entry in enumerate(st['images']):
            p.need(entry['slot']==n and entry['read'] and entry['access_known'] and not entry['written'] and not entry['placeholder'] and len(entry['snapshots'])==1,'invalid image access')
            snap=entry['snapshots'][0];p.need(p.key(snap)==p.key(entry['image']),'image version mismatch')
            desc=tuple(dw[n*8:n*8+8]);d=p.bound_descriptor(desc,entry,snap,71)
            p.need(d['select']==0xfac and d['max_mip']==0 and snap['format']==97 and snap['aspect']==1 and snap['layer']==snap['mip']==0,'unsupported logical format')
            data=archive.snapshot(snap,('compositor_overlay_input','compositor_scene_input')[n],later=ui);w,h=snap['extent'];im=np.frombuffer(data,'<f2').reshape(h,w,4).astype(np.float32)
            p.need(np.isfinite(im).all(),'nonfinite image input');self.images[desc]=im;self.identities[desc]=dict(zip(p.KEYS+('mip','layer','aspect'),p.subkey(snap)));self.slots.append(desc);self.payloads.append(data)
        p.need(len(e['outputs'])==1 and len(u['targets'])==1,'ambiguous target')
        out=e['outputs'][0]['full'];data=archive.snapshot(out,'compositor',earlier=ui,later=i)
        p.need(out['format']==64 and out['aspect']==1 and out['texel_bytes']==4 and p.key(out)[:3]==p.key(u['targets'][0]['image'])[:3],'output mismatch')
        p.need(any(x.get('kind')=='flip' and x.get('frame') in (archive.frame,archive.frame+1) and p.key(x['image'])==p.key(out) for x in archive.events[i+1:]),'output not linked to actual flip')
        self.w,self.h=out['extent'];self.coords=p.sample_coordinates(self.state,self.w,self.h);self.actual=np.frombuffer(data,'<u4').reshape(self.h,self.w);self.output=dict(zip(p.KEYS+('mip','layer','aspect'),p.subkey(out)))
        # Simulator LDS parameter-bank layout: attr0.x P10 at dword1,
        # attr0.y P20 at dword9. Calibrated on both channels and four lanes.
        self.lds=pack([0,p.bits(1.),0,0,0,0,0,0,0,p.bits(1.),0,0])

    def quad(self,x,y):
        p.need(x%2==y%2==0 and 0<=x<self.w-1 and 0<=y<self.h-1,'unaligned/outside quad')
        vx,vy,w,h,_,_=self.state['viewport']
        return [(x+dx,y+dy,(x+dx+.5-vx)/w,1-(y+dy+.5-vy)/h) for dy in (0,1) for dx in (0,1)]

    def execute(self,decoder,runner,coords,images=None):
        p.need(0<len(coords)<=64 and len(coords)%4==0,'complete quads required')
        if images is None:images=self.images
        p.need(set(images)==set(self.images) and all(images[k].shape==v.shape for k,v in self.images.items()),'image replacement identity/extent')
        outcomes=[]
        for poison in (0,0x7fa54321):
            sg=[poison]*106;sg[:3]=[*self.ud,0];vg=[poison]*(256*64)
            for lane,(_,_,u,v) in enumerate(coords):vg[lane]=p.bits(u);vg[64+lane]=p.bits(v)
            sampler=LogicalImages(images,POLICY)
            exp,trace=execute(decoder,runner,self.code,sg,vg,self.parts,self.state['mode'],(1<<len(coords))-1,self.lds,sampler)
            p.need(set(exp)=={(0,k) for k in range(len(coords))} and all(v[:2]==(15,1) for v in exp.values()),'missing or unsupported original MRT export')
            pred=[p.pack10(exp[0,k][2]) for k in range(len(coords))]
            outcomes.append((pred,sampler.reads,trace))
        p.need(outcomes[0]==outcomes[1],'unbound launch state affects sampling or color')
        return outcomes[0]

    def compare(self,decoder,runner,coords):
        pred,reads,trace=self.execute(decoder,runner,coords);obs=[int(self.actual[y,x]) for x,y,_,_ in coords]
        rows=[]
        for lane,((x,y,_,_),v,o) in enumerate(zip(coords,pred,obs)):
            support={}
            for read in reads:
                slot=self.slots.index(read['descriptor']);support.setdefault(slot,set()).update(read['texels_by_lane'].get(lane,[]))
            rows.append(dict(x=x,y=y,expected=f'{v:08x}',observed=f'{o:08x}',rgb_lsb_delta=max(abs(((v>>k)&1023)-((o>>k)&1023)) for k in (0,10,20)),support=[{'version':self.identities[self.slots[s]],'texels':sorted(t)} for s,t in sorted(support.items())]))
        return rows


    def contract(self,x,y):
        return dict(code=self.code,raw=self.raw,state=self.state,extent=[self.w,self.h],
                    images=[dict(descriptor=list(k),extent=[self.images[k].shape[1],self.images[k].shape[0]]) for k in self.slots],pixel=[x,y])


def execute_contract(runner,params,inputs):
    """Compute one flip pixel from replacement bytes, not asserted colors.

    The real adapter must create params from verified capture and link inputs to
    producer versions. This contract alone cannot confer root authority.
    """
    p.need(set(params)=={'code','raw','state','extent','images','pixel'} and len(inputs)==2,'bad compositor contract')
    r=Replay.__new__(Replay);r.code=params['code'];p.need(p.digest(r.code)==p.HASHES[p.COMPOSITE],'wrong compositor module')
    r.state=params['state'];r.w,r.h=params['extent'];p.sample_coordinates(r.state,r.w,r.h)
    raw=params['raw'];r.ud=raw['user_data'];r.parts=p.scalar_segments(raw);p.need(len(r.ud)==2,'unexpected user data')
    root=p.ptr(pack(r.ud));a,b=struct.unpack('<QQ',p.read(r.parts,root,16));dw=struct.unpack('<16I',p.read(r.parts,a+32,64))
    p.need({struct.unpack('<4I',p.read(r.parts,b+x,16)) for x in (0xa0,0xc0)}==set(POLICY),'unknown sampler state')
    p.need(len(params['images'])==2,'wrong image count');r.slots=[];r.images={}
    for n,(m,data) in enumerate(zip(params['images'],inputs)):
        desc=tuple(m['descriptor']);p.need(desc==dw[n*8:n*8+8],'descriptor changed');d=p.descriptor(desc);w,h=m['extent']
        p.need(d['type']==9 and d['format']==71 and d['select']==0xfac and d['max_mip']==d['base_mip']==d['last_mip']==0 and [d['width'],d['height']]==[w,h] and len(data)==w*h*8,'invalid logical input bytes or descriptor')
        r.slots.append(desc);r.images[desc]=np.frombuffer(data,'<f2').reshape(h,w,4).astype(np.float32);p.need(np.isfinite(r.images[desc]).all(),'nonfinite input')
    r.lds=pack([0,p.bits(1.),0,0,0,0,0,0,0,p.bits(1.),0,0]);x,y=params['pixel'];quad=r.quad(x&~1,y&~1);lane=(y&1)*2+(x&1)
    d=LLVM()
    try:return struct.pack('<I',r.execute(d,runner,quad)[0][lane])
    finally:d.close()


def main():
    cli=argparse.ArgumentParser(description=__doc__);cli.add_argument('archive',type=Path);cli.add_argument('runner',type=Path);args=cli.parse_args();a=p.Archive(args.archive);d=LLVM()
    try:
        r=Replay(a);points=r.compare(d,args.runner,r.coords);quad=r.quad((r.w//2)&~1,(r.h//2)&~1);witness=r.compare(d,args.runner,quad)
        # Negative controls: a substitution outside the sampled footprint must
        # not affect the witness; inside the footprint must be observable.
        pred,reads,_=r.execute(d,args.runner,quad)
        changed={k:v.copy() for k,v in r.images.items()};scene=r.slots[1]
        supports=set(t for s in reads if s['descriptor']==scene for ts in s['texels_by_lane'].values() for t in ts)
        p.need(supports and (0,0) not in supports,'control overlaps actual sampled texels');changed[scene][0,0,:3]=.25
        outside=r.execute(d,args.runner,quad,changed)[0];p.need(outside==pred,'outside support affects output')
        for x,y in supports:changed[scene][y,x,:3]=.25
        inside=r.execute(d,args.runner,quad,changed)[0];p.need(inside!=pred,'inside support substitution did not propagate')
        result=dict(status='LOCAL_PROPAGATION_ONLY',p8='BLOCKED',frame=a.frame,use=r.marker,output=r.output,grid=points,witness=witness,exact=sum(x['expected']==x['observed'] for x in points),samples=len(points),max_lsb=max(x['rgb_lsb_delta'] for x in points),scope='original scalar/vector ISA, logical image filtering model; no physical decode or independent prefix authority',controls=dict(outside_unchanged=outside==pred,inside_changed=inside!=pred,synthetic_inside=[f'{v:08x}' for v in inside]))
        print(json.dumps(result,indent=2))
    finally:d.close();a.close()
if __name__=='__main__':main()
