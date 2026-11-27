#!/usr/bin/env python3
"""Experimental CPU reference for exact 3265 guest compositor logical pixels.
No compression decode, fixed mip0, clamp bilinear, float32 arithmetic. Not P8.
"""
import numpy as np, json, struct, sys
from pathlib import Path
import p8_paired_replay as p

def sample(im,u,v):
    h,w=im.shape[:2]; x=u*w-.5;y=v*h-.5
    ix=np.floor(x).astype(np.int32);iy=np.floor(y).astype(np.int32)
    fx=(x-ix).astype(np.float32)[...,None];fy=(y-iy).astype(np.float32)[...,None]
    x0=np.clip(ix,0,w-1);x1=np.clip(ix+1,0,w-1);y0=np.clip(iy,0,h-1);y1=np.clip(iy+1,0,h-1)
    a=im[y0,x0,:3];b=im[y0,x1,:3];c=im[y1,x0,:3];d=im[y1,x1,:3]
    return (a+(b-a)*fx)*(1-fy)+(c+(d-c)*fx)*fy

def weights(t):
    t2=t*t;t3=t2*t
    w0=-.9*t3+1.8*t2-.9*t
    w1=1+1.1*t3-2.1*t2
    w2=-1.1*t3+1.2*t2+.9*t
    w3=.9*t3-.9*t2
    return w0,w1+w2,w2,w3

def predict(scene,overlay,u,v,scale,offset):
    h,w=scene.shape[:2];x=(u*scale+offset)*w-.5;y=v*h-.5
    ix=np.floor(x);iy=np.floor(y);a,b,c,d=weights(x-ix);e,f,g,k=weights(y-iy)
    mx=(ix+.5+c/b)/w;my=(iy+.5+g/f)/h
    s0=sample(scene,mx,(iy-.5)/h);s1=sample(scene,(ix-.5)/w,my);s2=sample(scene,mx,my);s3=sample(scene,(ix+2.5)/w,my);s4=sample(scene,mx,(iy+2.5)/h)
    ws=[e*b,a*f,b*f,d*f,k*b];total=sum(ws)
    rgb=sum(s*q[...,None] for s,q in zip((s0,s1,s2,s3,s4),ws))/total[...,None]
    l1=s2.sum(axis=-1)*np.float32(.333);l2=rgb.sum(axis=-1)*np.float32(.333)
    q=np.clip(np.minimum(l1,l2)/np.maximum(np.float32(1e-5),np.maximum(l1,l2)),0,1)
    rgb=np.maximum(0,s2+(rgb-s2)*q[...,None]);rgb=rgb*(1-overlay[...,3:4])+overlay[...,:3]
    gamma=np.where(rgb<=np.float32(.0031308),rgb*np.float32(12.92),np.float32(1.055)*np.power(rgb,np.float32(1/2.4))-np.float32(.055))
    # Shader uses packed round-towards-zero FP16; correct nearest rounding when it overshoots.
    half=gamma.astype(np.float16);half=np.where(half.astype(np.float32)>gamma,np.nextafter(half,np.float16(0)),half)
    q=np.rint(np.clip(half.astype(np.float32),0,1)*1023).astype(np.uint32)
    return q[...,0]|q[...,1]<<10|q[...,2]<<20|np.uint32(3<<30)

def main(path):
    a=p.Archive(Path(path));i,u=a.use['P8_USE_418'];e=next(x for x in a.events if x.get('kind')=='composite_after' and x.get('use')=='P8_USE_418');state=next(x for x in a.events if x.get('kind')=='composite_state' and x.get('use')=='P8_USE_418');st=u['stages'][1];raw=st['guest_operands'];p.need(p.digest(a.module('ps',p.COMPOSITE))==p.HASHES[p.COMPOSITE],'shader');p.sample_coordinates(state,1920,1080)
    parts=p.scalar_segments(raw);root=p.ptr(p.pack(raw['user_data']));r,s=struct.unpack('<QQ',p.read(parts,root,16));dims=struct.unpack('<4f',p.read(parts,r,16));scale,offset=struct.unpack('<2f',p.read(parts,r+20,8));p.need(dims==(1600.,900.,np.float32(1/1600),np.float32(1/900)) and scale==1 and offset==0,'constants')
    dw=struct.unpack('<16I',p.read(parts,r+32,64));samplers=[struct.unpack('<4I',p.read(parts,s+x,16)) for x in (0xa0,0xc0)];p.need(samplers==[(0x8007000,0xfff000,0x5000000,0x40000000),(0x8007492,0xfff000,0xaf00000,0x40000000)],'sampler fingerprint')
    imgs=[]
    for n,ent in enumerate(st['images']):
        snap=ent['snapshots'][0];d=p.bound_descriptor(dw[n*8:n*8+8],ent,snap,71);p.need(d['select']==0xfac and d['max_mip']==0 and snap['format']==97 and snap['extent']==([1920,1080] if n==0 else [1600,900]),'descriptor');data=a.snapshot(snap,('compositor_overlay_input','compositor_scene_input')[n],later=i);w,h=snap['extent'];imgs.append(np.frombuffer(data,'<f2').reshape(h,w,4).astype(np.float32))
    o,scene=imgs;out=e['outputs'][0]['full'];j=a.events.index(e);data=a.snapshot(out,'compositor',earlier=i,later=j);p.need(any(x.get('kind')=='flip' and p.key(x['image'])==p.key(out) for x in a.events[j+1:]),'flip');actual=np.frombuffer(data,'<u4').reshape(1080,1920)
    p.need(np.isfinite(o).all() and np.isfinite(scene).all(),'nonfinite input');total=exact=near=0;worst=0;hist={};first=None
    for y in range(0,1080,48):
        yy,xx=np.mgrid[y:min(y+48,1080),:1920];u=(xx.astype(np.float32)+.5)/1920;v=(yy.astype(np.float32)+.5)/1080
        pred=predict(scene,o[y:y+len(yy)],u,v,scale,offset);obs=actual[y:y+len(yy)];delta=np.maximum.reduce([np.abs(((pred>>k)&1023).astype(np.int32)-((obs>>k)&1023).astype(np.int32)) for k in (0,10,20)])
        exact+=int((pred==obs).sum());near+=int((delta<=1).sum());total+=pred.size;worst=max(worst,int(delta.max()))
        if first is None and (delta>1).any():
            j,k=np.argwhere(delta>1)[0];first={'x':int(k),'y':int(y+j),'expected':hex(int(pred[j,k])),'observed':hex(int(obs[j,k])),'delta':int(delta[j,k])}
        for k,c in zip(*np.unique(delta,return_counts=True)):hist[int(k)]=hist.get(int(k),0)+int(c)
    # The same unoccluded final pixel is a fixed witness; an artificial scene
    # patch tests propagation only. It is not an expected game input or fix.
    y,x=540,960;u=np.array([[(x+.5)/1920]],np.float32);v=np.array([[(y+.5)/1080]],np.float32);ov=o[y:y+1,x:x+1]
    base=int(predict(scene,ov,u,v,scale,offset)[0,0]);native=int(actual[y,x]);changed=scene.copy();changed[447:454,797:804,:3]=.25;control=int(predict(changed,ov,u,v,scale,offset)[0,0]);p.need(base==native and control!=base,'fixed pixel propagation control failed')
    for val,want in ((0.,0xc0000000),(.25,0xe2589625),(1.,0xffffffff)):
        constant=np.full((8,8,4),val,np.float32);zero=np.zeros((1,1,4),np.float32);p.need(int(predict(constant,zero,np.array([[.5]],np.float32),np.array([[.5]],np.float32),1,0)[0,0])==want,'constant fixture')
    print(json.dumps(dict(status='EXPERIMENTAL_REFERENCE',total=total,exact=exact,within_1_rgb_lsb=near,worst_rgb_lsb=worst,first_over_1=first,delta_hist=hist,scope='analytic logical pixel reference, sampler precision not independently certified; no P8 promotion',witness=dict(x=x,y=y,baseline=hex(base),observed=hex(native),synthetic_scene_control=hex(control),scope='sensitivity only; not expected game input')),indent=2))
if __name__=='__main__':main(sys.argv[1])
