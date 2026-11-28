#!/usr/bin/env python3
"""V4: exact relight producer slice on the same captured probe and ray.
Original guest ISA for arithmetic; independent native texture footprints.
MATCH is conditional to captured inputs, not an assumption of healthy probes.
"""
import argparse,json,hashlib,math,struct,zipfile
from pathlib import Path
import numpy as np
from p8_guest_graphics import run_wave
from v4_q8_pair_replay import smov,exp,END,dist,fl
from p8_paired_replay import need
HASH='d69e3d40292a18fd7791453fee05c2c896d4791ccd067096ed878b5934efcba2'
PROBE=4957
POSITION=[3246391296,3288334336,3246391296,1098907648]
SITES=[
 (0x120,0,[2,3,8,9]),
 (0x15bc,1,[3,284,285,286,287]),(0x16a4,1,[4]),
 (0x172c,1,[43,44,45,*range(256,264),296,297,298,299]),
 (0x1734,1,[34,39,40,45,*range(264,272),300,301,302,303]),
 (0x1744,1,[3,4,5,34,37,36,28,29,30,31]),(0x177c,1,[28,29,30,31]),
 (0x1788,0,[28,29,30,31]),(0x17b8,0,[27,22,5]),
 (0x17bc,0,[5,8,13,14,15,16,20,21,22,27,9,10,12]),(0x17f8,0,[27,28,16]),
 (0x18a8,0,[3,4,5,13,16,27,28,272,362,363]),(0x18c0,0,[27,28,16]),
 (0x1b60,0,[8,18,15,17,14,3,27,28,16]),(0x1b88,0,[0,1,2,3,6,7])]
REGS={p:reg for p,k,reg in SITES}
RAY_WORDS=sum((8 if k else 1)*32 for _,k,_ in SITES)
WORDS=8+256*RAY_WORDS
PAIRS=((0x1744,0x177c),(0x1788,0x17b8),(0x17bc,0x17f8),(0x18a8,0x18c0),(0x1b60,0x1b88))
def bits(x):return struct.unpack('<I',struct.pack('<f',float(x)))[0]
def half_rtz(x):
    x=np.asarray(x,'<f4')
    need(np.isfinite(x).all(),'nonfinite store')
    q=x.astype('<f2')
    mask=abs(q.astype('<f4'))>abs(x)
    q[mask]=np.nextafter(q[mask],np.float16(0))
    return list(map(int,q.view('<u2')))
def wave_lane(ray):
    y,x=divmod(ray,16)
    return (y//8)*2+x//8,(y%8)*8+x%8
def parse(data,meta):
    need(meta['schema']==1 and meta['probe']==PROBE and meta['position']==POSITION,'wrong probe/position')
    need(meta['groups'][:2]==[2,2] and 0<=meta['z']<meta['groups'][2] and meta['map'][meta['z']]==PROBE and meta['map'].count(PROBE)==1,'map')
    need(len(meta['ray_words'])==512 and len(data)==meta['bytes']==WORDS*4,'trace extent')
    a=np.frombuffer(data,'<u4');need(list(a[:5])==[3,meta['z'],PROBE,0,1],'trace header')
    rows=[]
    for ray in range(256):
        at=8+ray*RAY_WORDS;records={}
        for pc,corner,regs in SITES:
            for k in range(8 if corner else 1):
                r=a[at:at+32];at+=32
                if not np.any(r):continue
                need(r[0]==pc+1 and r[1]==(1<<k if corner else 0),'record marker')
                records[pc,k]=list(map(int,r[2:2+len(regs)]))
        need(all((p,0) in records for p in (0x120,0x17bc,0x17f8,0x18a8,0x18c0,0x1b60,0x1b88)),'missing ray checkpoint')
        raw=records[0x120,0]
        need(raw==[PROBE,PROBE*256+ray,*meta['ray_words'][2*ray:2*ray+2]],'native raw ray does not match bound row')
        for k in range(8):
            visited=[(p,k) in records for p in (0x15bc,0x16a4,0x172c,0x1734,0x1744,0x177c)]
            need(all(visited) or not any(visited),'incomplete corner')
        need(((0x1788,0) in records)==((0x17b8,0) in records),'normalization pair')
        rows.append(records)
    return rows
def slice_code(code,pc,end):
    need(hashlib.sha256(code).hexdigest()==HASH,'unrecognized relight ISA')
    if pc==0x1b60:
        # s_load image descriptor and wait do not influence vector arithmetic.
        body=code[0x1b60:0x1b70]+code[0x1b78:0x1b84]
    else:body=code[pc:end]
    regs=REGS[end][:4] if end==0x1b88 else REGS[end]
    return body+exp(0,(regs+[regs[-1]]*4)[:4],True)+END
def compare_slice(code,runner,mode,pc,end,inputs,outputs):
    regs=REGS[pc];outregs=REGS[end][:4] if end==0x1b88 else REGS[end]
    vg=[0]*(256*64);sg=[0]*106;scalars={};mask=0
    for lane,values in inputs.items():
        need(len(values)==len(regs),'operand count');mask|=1<<lane
        for reg,val in zip(regs,values):
            if reg<256:vg[reg*64+lane]=val
            else:
                old=scalars.setdefault(reg-256,val)
                need(old==val,'nonuniform scalar input')
    prefix=b''
    for r,v in scalars.items():
        if r<106:sg[r]=v
        else:prefix+=smov(r,v)
    out=run_wave(runner,prefix+slice_code(code,pc,end),sg,vg,[],mode,mask)
    need(set(outputs)==set(inputs),'output census')
    result=[]
    for lane,want in outputs.items():
        need((0,lane) in out,'missing original export')
        got=list(out[0,lane][2][:len(outregs)]);want=want[:len(outregs)]
        result.append((lane,[dist(a,b) for a,b in zip(got,want)],got,want))
    return result
def sample_reference(pc,record,fp,report):
    shadow=pc==0x1734
    coords=list(map(fl,record[:4 if shadow else 3]))
    u,v,z=coords[1:4] if shadow else coords
    n=4 if shadow else 3
    need(record[n:n+8]==report['descriptor'] and record[n+8:n+12]==report['sampler'],'sample descriptor')
    w,h=report['extent'];x,y=fp['xy'];tx=u*w-.5;ty=v*h-.5
    need(math.floor(tx)==x and math.floor(ty)==y and 0<=x<w-1 and 0<=y<h-1 and z==int(z) and fp['layer']==report['base_layer']+int(z),'footprint coordinates')
    sm=report['sampler_state'];need(not sm['unnormalized'] and sm['reduction']==0 and sm['mag']==sm['min'] and sm['mag'] in (0,1,2,3) and (sm['mip_filter']==0 or sm['min_lod']==0),'sampler state')
    d=report['descriptor'];fmt=(d[1]>>20)&511;need((d[3]>>28)&15==13,'array descriptor')
    if shadow:
        need(fmt==7 and len(fp['texels'])==4,'D16 footprint')
        a=np.array(fp['texels'],float).reshape(2,2,1)/65535
        ref=coords[0];op=sm['compare'];need(0<=op<=7 and 0<=ref<=1,'Dref/compare')
        need(op in (0,7) or np.min(np.abs(a-ref))>1/65535,'threshold ambiguity')
        a=[np.zeros_like(a),ref<a,ref==a,ref<=a,ref>a,ref!=a,ref>=a,np.ones_like(a)][op].astype(float)
    else:
        need(fmt==71 and len(fp['texels'])==16 and d[3]&0xfff==0xfac,'RGBA16 atlas descriptor')
        a=np.asarray(fp['texels'],'<u2').view('<f2').astype(float).reshape(2,2,4)[:,:,:3]
    need(np.isfinite(a).all(),'nonfinite footprint')
    if sm['mag'] in (0,2):
        want=a[math.floor(ty+.5)-y,math.floor(tx+.5)-x];tol=1e-7
    else:
        fx,fy=tx-x,ty-y
        want=(1-fy)*((1-fx)*a[0,0]+fx*a[0,1])+fy*((1-fx)*a[1,0]+fx*a[1,1])
        tol=float(np.max(np.ptp(a.reshape(4,-1),axis=0)))*2*2**(-report['subtexel_bits'])+max(1e-9,float(np.max(abs(a)))*2e-6)
    return want,tol
def audit(code,runner,meta,data):
    rows=parse(data,meta);bad=[];incomplete=[];matched=0;checks=[]
    def result(kind,pc,ray,k,ok,detail=None):
        nonlocal matched
        if ok:matched+=1
        else:bad.append(dict(kind=kind,pc=hex(pc),ray=ray,corner=k,detail=detail))
    for report in meta['footprints']:
        pc=report['pc'];need(pc in (0x172c,0x1734),'unknown footprint')
        seen=set()
        for fp in report['samples']:
            ray,k=fp['ray'],fp['corner'];key=(ray,k);need(key not in seen and 0<=ray<256 and 0<=k<8,'duplicate footprint');seen.add(key)
            r=rows[ray]
            try:
                ref,tol=sample_reference(pc,r[pc,k],fp,report)
                val=r[0x1744,k][:3] if pc==0x172c else r[0x1744,k][3:4]
                val=list(map(fl,val));result('footprint',pc,ray,k,all(abs(a-b)<=tol for a,b in zip(ref,val)),dict(reference=ref.tolist(),observed=val,tol=tol))
            except (ValueError,KeyError,OverflowError) as ex:incomplete.append(dict(pc=hex(pc),ray=ray,corner=k,reason=str(ex)))
        expected={(i,k) for i,r in enumerate(rows) for k in range(8) if (pc,k) in r}
        if seen!=expected or report['status'] not in ('captured','no_visits'):incomplete.append(dict(pc=hex(pc),reason='footprint coverage',observed=len(seen),expected=len(expected),status=report['status']))
    need(sorted(x['pc'] for x in meta['footprints'])==[0x172c,0x1734],'sample census')
    for ray,r in enumerate(rows):
        corners=[k for k in range(8) if (0x1744,k) in r]
        prior=[0]*4
        for k in corners:
            result('accumulator_chain',0x1744,ray,k,r[0x1744,k][6:10]==prior)
            prior=r[0x177c,k]
        if (0x1788,0) in r:
            result('normalization_entry',0x1788,ray,0,r[0x1788,0]==prior)
            result('normalization_publication',0x17bc,ray,0,[r[0x17bc,0][9],r[0x17bc,0][8],r[0x17bc,0][0]]==r[0x17b8,0])
        else:
            result('inactive_recursive',0x17bc,ray,0,not corners and all(fl(r[0x17bc,0][i])==0 for i in (0,8,9)))
        # Original shader's RGB gamma decoding and high nibble emission.
        raw=r[0x120,0][2];a=r[0x17bc,0]
        ref=[((raw>>(8*i))&255)/255 for i in range(3)]
        got=[fl(a[3]),fl(a[4]),fl(a[5])]
        result('ray_albedo_decode',0x17bc,ray,0,all(abs(x**2.2-y)<=2e-6 for x,y in zip(ref,got)),dict(reference=[x**2.2 for x in ref],observed=got))
        result('ray_emission_decode',0x17bc,ray,0,abs(fl(a[2])-(raw>>28)/15)<=1e-7)
        result('material_publication',0x18a8,ray,0,[r[0x18a8,0][5],r[0x18a8,0][6],r[0x18a8,0][4]]==r[0x17f8,0])
        result('environment_publication',0x1b60,ray,0,r[0x1b60,0][6:9]==r[0x18c0,0])
        out=meta.get('native_output',{}).get('half_words',[])
        if len(out)==256 and out[ray] is not None:
            values=list(map(fl,r[0x1b88,0][:4]))
            result('native_store',0x1b88,ray,0,half_rtz(values)==out[ray],dict(reference=half_rtz(values),observed=out[ray]))
        else:incomplete.append(dict(ray=ray,reason='native output absent'))
    for pc,end in PAIRS:
        for k in range(8 if pc==0x1744 else 1):
            for group in range(4):
                ins={};outs={};ids={}
                for ray,r in enumerate(rows):
                    g,l=wave_lane(ray)
                    if group!=g or (pc,k) not in r:continue
                    need((end,k) in r,'missing slice end');ins[l]=r[pc,k];outs[l]=r[end,k];ids[l]=ray
                if not ins:continue
                if not runner:incomplete.append(dict(pc=hex(pc),reason='original ISA runner required'));continue
                try:
                    for l,delta,got,want in compare_slice(code,runner,meta['mode'],pc,end,ins,outs):
                        result('original_ISA',pc,ids[l],k,max(delta,default=0)<=8,dict(ulp=delta,reference=got,observed=want))
                except (ValueError,KeyError,RuntimeError,OSError,AssertionError) as ex:incomplete.append(dict(pc=hex(pc),corner=k,group=group,reason=str(ex)))
    return dict(status='DIVERGENCE_CANDIDATE' if bad else 'INCOMPLETE' if incomplete else 'MATCH_LOCAL_CONDITIONAL',
                frame=meta['frame'],submit=meta['submit'],probe=PROBE,matched=matched,differences=len(bad),first_differences=bad[:24],
                incomplete=len(incomplete),first_incomplete=incomplete[:24],
                scope='actual ray material factors, native footprint samples, original corner/normalization/material/environment/store arithmetic; prior input producers not certified')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path);p.add_argument('--runner',type=Path);args=p.parse_args()
    with zipfile.ZipFile(args.archive) as z:
        prefix='ds-lighting-capture/queue-state/';names=[n for n in z.namelist() if n.startswith(prefix+'ds_v4_relight_') and n.endswith('.json')]
        need(names,'no targeted producer capture');code=z.read('ds-lighting-capture/guest-shaders/cs_bcbcc6ba1e68c974_probe.bin')
        need(hashlib.sha256(code).hexdigest()==HASH,'guest hash')
        for n in names:
            meta=json.loads(z.read(n));need(prefix+meta['binary']==n[:-5]+'.bin','binary identity')
            try:print(json.dumps(audit(code,args.runner,meta,z.read(prefix+meta['binary'])),indent=2))
            except (ValueError,KeyError,AssertionError,OverflowError) as ex:print(json.dumps(dict(status='INCOMPLETE',file=n,reason=str(ex))))
