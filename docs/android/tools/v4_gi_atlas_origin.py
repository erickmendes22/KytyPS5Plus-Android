#!/usr/bin/env python3
"""Reproduce the actual #764 q33 atlas/4957 projector evidence offline.
The original shaders are read from the user's retained diagnostic, never
redistributed. Descriptor/record reads and producer MODE are conditional;
this is not a certificate of unobserved producer operations.
"""
import argparse,json,hashlib
from pathlib import Path
import numpy as np
from p8_paired_replay import Archive,need
from p8_guest_graphics import run_wave,pack
from v4_q8_pair_replay import jump,NOP,END,exp,smov,lit,mov,xor_sign,cndmask,fl
from v4_relight_slice import half_rtz
ATLAS_HASH='f8a9a431d076920cfd4ede67a44c1f1f106d6b34ca99c26947afa2e010d7b76c'
PROJECTOR_HASH='5b49245ef42bedb60e05a9a345364298bb3ad72a93e4885daa71cec1d2902020'
ROOT='ds-lighting-capture/'
def replace(p,pc,n,body):
    start=len(p);p[pc:pc+n]=jump(pc,start)+NOP*(n//4-1)
    p.extend(body+jump(start+len(body),pc+n))
def wave(runner,code,sg,vg,segments,mode):
    return {k:list(v[2]) for k,v in run_wave(runner,code,sg,vg,segments,mode,(1<<64)-1).items()}
def xy_regs():
    vg=[0]*(256*64)
    for l in range(64):vg[l]=l%8;vg[64+l]=l//8
    return vg
def atlas_shader(code,record):
    p=bytearray(code[:0x30c]);p+=exp(0,[2,3,4,4])+exp(1,[9,14,12,12],True)+END
    for pc in (0x1c,0x28,0x3c,0x70,0x10c):
        body=b''.join(smov(24+i,v) for i,v in enumerate([4,8,15,15])) if pc==0x3c else smov(30,511) if pc==0x70 else b''
        replace(p,pc,8,body)
    for pc,d,r,t,other in ((0x88,6,5,True,7),(0xa8,7,4,True,7),(0xd8,8,7,True,7),(0xec,9,9,False,9)):
        replace(p,pc,8,xor_sign(100,r)+(cndmask(d,other,100) if t else cndmask(d,100,other)))
    replace(p,0x128,8,lit(6,record['packed_index']))
    replace(p,0x1b0,8,b''.join(smov(j,x) for j,x in enumerate(record['coefficient_words'])))
    return bytes(p)
def audit(a,runner):
    events=[e for e in a.events if e['kind']=='v4_gi_contract']
    need(len(events)==1,'q33 acquisition');e=events[0]
    atlas=a.z.read(ROOT+'guest-shaders/cs_c03afccb96a13c5d_probe.bin')
    need(hashlib.sha256(atlas).hexdigest()==ATLAS_HASH,'atlas guest fingerprint')
    unique={};samples=[]
    for fp in e['footprints']:
        if fp['slot']!=7:continue
        need(fp['ok'] and fp['side']==2 and len(fp['bytes'])==32 and len(fp['atlas_writer_candidates'])==4,'atlas footprint')
        x,y,_=fp['xyz'];layer=fp['layer'];v=np.frombuffer(bytes(fp['bytes']),'<u2').reshape(2,2,4)
        for j,w in enumerate(fp['atlas_writer_candidates']):
            need(w and not w['ambiguous'] and len(w['coefficient_words'])==14,'writer unavailable')
            xx,yy=x+j%2,y+j//2;tile=layer*256+(yy//16)*16+xx//16
            need(tile==w['tile']==w['packed_index']&0xffffff,'tile mapping')
            key=w['packed_index'];need(key not in unique or unique[key]==w,'ambiguous historical coefficient')
            unique[key]=w;samples.append((key,xx,yy,layer,list(map(int,v[j//2,j%2,:3]))))
    need(samples and 4957 in unique,'target historical producer missing')
    rec=unique[4957]
    reports=[]
    for n in a.z.namelist():
        if not n.startswith(ROOT+'queue-state/cs_c03afccb96a13c5d_atlas_inputs_') or not n.endswith('.json'):continue
        j=json.loads(a.z.read(n))
        if j.get('frame')==rec['frame'] and j.get('submit')==rec['submit'] and 4957 in j.get('index_words',[]):reports.append(j)
    need(len(reports)==1,'unique actual coefficient pair')
    j=reports[0];need(j['srt_22_to_27']==[4,8,15,15,511,0],'atlas layout')
    all_out={}
    for key,w in unique.items():
        for gx in range(2):
            for gy in range(2):
                sg=[0]*106;sg[2:5]=[gx,gy,w['z']]
                out=wave(runner,atlas_shader(atlas,w),sg,xy_regs(),[],192)
                for lane in range(64):
                    coords=tuple(out[1,lane][:3]);v=out[0,lane][:3]
                    need((key,*coords) not in all_out,'duplicate atlas texel')
                    all_out[key,*coords]=v
    rne=rtz=total=0
    for key,x,y,l,native in samples:
        v=list(map(fl,all_out[key,x,y,l]));q=list(map(int,np.asarray(v,'<f2').view('<u2')))
        rne+=sum(a==b for a,b in zip(q,native));rtz+=sum(a==b for a,b in zip(half_rtz(v),native));total+=3
    w=j['coefficient_writer'];i=j['index_words'].index(4957);s=w['source_dispatch'];z=s['probe_indices'].index(4957)
    need(i==rec['z'] and all(w[k][i][:14]==rec['coefficient_words'] for k in ('after_words','before_words','matched_source_coefficients','source_words_after_writer')),'coefficient identity')
    source=s['sampled_compute_writer'];need(source['shader']=='bcbcc6ba1e68c974','wrong radiance producer')
    need(source['radiance_digest']==s['radiance_digest'] and all(source['radiance_view'][k]==s['radiance_view'][k] for k in ('owner','version','width','height','layer')),'radiance version')
    payload=a.z.read(ROOT+'queue-state/'+s['radiance_file'])
    need(len(payload)==s['radiance_bytes']==256*256*8,'radiance bytes')
    raw=np.frombuffer(payload,'<u8');h=14695981039346656037
    for v in raw:h=((h^int(v))*1099511628211)&((1<<64)-1)
    need(format(h,'016x')==s['radiance_digest'],'radiance digest')
    need(z==0 and s['srt'][28:30]==[4,15],'projector captured layout')
    im=np.frombuffer(payload,'<f2').reshape(256,256,4).astype('<f4').view('<u4')
    code=a.z.read(ROOT+'guest-shaders/cs_567d21ad45386c8d_probe.bin')
    need(hashlib.sha256(code).hexdigest()==PROJECTOR_HASH,'projector fingerprint')
    p=bytearray(code[:0xf24])
    for k in range(4):p+=exp(k,list(range(k*4,k*4+4)),k==3)
    p+=END
    for pc,body in ((0x18,smov(16,0x50000)+smov(17,0)),(0x3c,b''),(0xdac,b''),(0xddc,lit(30,4957)),(0xf18,b'')):replace(p,pc,8,body)
    for pc,d,r in ((0x78,4,2),(0x98,19,13),(0xb4,3,12),(0xbc,18,17),(0xc8,20,12),(0xd0,17,17),(0xdc,22,2),(0xe4,12,13)):
        replace(p,pc,8,xor_sign(100,r)+cndmask(d,100,r))
    sites=[(0x1d8,8,6,(31,32),0,0),(0x1e0,12,9,(33,32),8,0),(0x1ec,12,0,(31,34),0,8),(0x210,8,3,(33,34),8,8)]
    vg=xy_regs()
    for k,(pc,n,d,coords,dx,dy) in enumerate(sites):
        replace(p,pc,n,exp(32+k,[*coords,*coords])+b''.join(mov(d+q,256+110+3*k+q) for q in range(3)))
        for l in range(64):
            for q in range(3):vg[(110+3*k+q)*64+l]=int(im[l//8+dy,l%8+dx,q])
    sg=[0]*106;sg[2:5]=[0,0,z]
    o=wave(runner,bytes(p),sg,vg,[(0x50004,pack([4,15]))],192)
    need(all(o[32+k,l][:2]==[l%8+dx,l//8+dy] for k,(_,_,_,_,dx,dy) in enumerate(sites) for l in range(64)),'projector coordinates')
    got=sum((o[k,0] for k in range(4)),[]);want=w['matched_source_coefficients'][i]
    delta=np.asarray(got,'<u4').view('<u2').astype(int)-np.asarray(want,'<u4').view('<u2').astype(int)
    rgb=im[:16,:16,:3].view('<f4')
    return dict(atlas=dict(probes=list(unique),texels=len(samples),rgb_fields=total,exact_rne=rne,exact_rtz=rtz),
        projector=dict(probe=4957,coordinates=256,half_fields=len(delta),max_half_ulp=int(np.max(abs(delta))),zero_rgb_texels=int(np.count_nonzero(~np.any(rgb!=0,axis=2))),max_rgb=float(rgb.max()),mean_rgb=float(rgb.mean()),source=source['shader'],frame=source['frame'],submit=source['submit']),
        scope='observed historical coefficient/texel inputs; inferred producer MODE 192; not a full independent producer certificate')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path);p.add_argument('runner',type=Path);args=p.parse_args()
    a=Archive(args.archive)
    try:print(json.dumps(audit(a,args.runner),indent=2))
    finally:a.close()
