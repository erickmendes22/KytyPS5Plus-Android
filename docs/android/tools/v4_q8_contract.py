#!/usr/bin/env python3
"""V4 q8 local same-input contracts; never a whole-P8 or expected-lighting proof.

Usage: python v4_q8_contract.py diagnostic.zip kyty_p8_guest_ref
The reference uses the original shader bytes from the archive, not Kyty SPIR-V.
Sample values from unsupported/uncaptured textures remain native observations.
"""
import argparse, functools, json, struct, subprocess
from pathlib import Path
import numpy as np
from p8_paired_replay import Archive, need, key, descriptor, digest
from p8_guest_graphics import run_wave, pack, merge
from p8_image_trap import exp

SHADER=0x3dfe402653925fbf
HASH='c99fdd9616b9975702f3fec398d27d29ccde9a76b75483d1248228e1c03fcaa6'
HEADER=8; LANES=64; FIXED=96; OUTER=8; TERMS=1024; TERM=24; IMAGE=28
OUTER_BASE=HEADER+LANES*FIXED; TERM_BASE=OUTER_BASE+32*LANES*OUTER; IMAGE_BASE=TERM_BASE+(1+2*TERMS)*TERM
SITES=[(0x118,0,2,1),(0x150,0,2,1),(0x164,0,3,1),(0x170,0,3,1),(0x17c,0,3,1),(0x1e8,0,2,4),(0x364,0,2,2),(0x57c,0,7,3),(0x5c4,0,2,2),(0xa70,1,7,3),(0x1010,2,7,3),(0x1328,2,7,3),(0x1410,2,4,1),(0x1424,2,4,1),(0x147c,2,4,1),(0x1488,2,4,1),(0x1864,2,4,1),(0x1874,2,4,1),(0x18e8,2,4,1),(0x18f0,2,4,1),(0x1b9c,2,3,3)]
WORDS=IMAGE_BASE+sum((TERMS if loop else 1)*IMAGE for _,loop,_,_ in SITES)

# Schema 3 retains the original wave fields once and duplicates only the
# per-witness tails. Old schema 1/2 captures retain their original layout.
SINGLE_WORDS=WORDS+TERMS*48
WITNESS_STRIDE=SINGLE_WORDS-TERM_BASE
PAIR_WORDS=SINGLE_WORDS+WITNESS_STRIDE

def select_witness(event,raw,witness='dark'):
    need(witness in ('dark','control'),'unknown witness')
    if event['schema'] not in (3,4):
        need(witness=='dark','control not captured by this schema')
        return event,raw
    from v4_q8_brdf import EXTRA_WORDS
    expected=PAIR_WORDS+(EXTRA_WORDS if event['schema']==4 else 0)
    need(len(raw)==expected*4 and event['trace_bytes']==len(raw),'truncated paired trace')
    raw=raw[:PAIR_WORDS*4]
    a=np.frombuffer(raw,'<u4');d=event['lane'];control=event['control'];c=control['lane'];wg=event['wg']
    need(0<=d<64 and 0<=c<64 and c!=d and list(a[:5])==[1,wg,d,0,c],'wrong paired header or overflow')
    need(event['stencil']==control['stencil'] and event['stencil']&64 and event['material_word']&0xffffff and control['material_word']&0xffffff,'invalid material/stencil pair')
    need(event['pixel']!=control['pixel'] and event['dark_luminance_before']<.003 and control['luminance_before']>=event['dark_luminance_before']
         and bool(event['pair_contrast_before'])==(control['luminance_before']>=.006),'invalid pre-HDR selection')
    if witness=='dark':
        return event,raw[:SINGLE_WORDS*4]
    selected=dict(event,lane=c,pixel=control['pixel'],material_word=control['material_word'],stencil=control['stencil'])
    common=a[:TERM_BASE].copy();common[2]=c
    return selected,common.tobytes()+raw[SINGLE_WORDS*4:]

class Divergence(Exception):
    def __init__(self,pc,op,expected,observed,lane=None):
        self.row={'pc':hex(pc),'operation':op,'expected':expected,'observed':observed,'lane':lane}

class Checks:
    def __init__(self):self.counts={}
    def equal(self,pc,op,expected,observed,lane=None):
        if expected!=observed:raise Divergence(pc,op,expected,observed,lane)
        self.counts[op]=self.counts.get(op,0)+1

def bits32(x):return int(np.asarray(x,dtype='<f4').view('<u4'))
def float32(x):return float(np.asarray(x,dtype='<u4').view('<f4'))
def finite(x):return (int(x)&0x7f800000)!=0x7f800000

def normal_or_zero(x):return finite(x) and ((int(x)&0x7f800000)!=0 or (int(x)&0x7fffffff)==0)

def checked_float(checks,pc,op,ref,actual,lane):
    need(normal_or_zero(ref) and normal_or_zero(actual),'nonfinite/denormal arithmetic outside strict contract '+hex(pc))
    # Signed zero is numerically equal here, but not a claim of bitwise equivalence.
    checks.equal(pc,op,0 if ref&0x7fffffff==0 else ref,0 if actual&0x7fffffff==0 else actual,lane)

def bitset(n):return [i for i in range(32) if int(n)>>i&1]
def marker(row,index,pc):need(int(row[index])==pc+1,'missing trace at '+hex(pc))

def parse_trace(raw,wg,lane):
    need(len(raw) in (WORDS*4,(WORDS+TERMS*48)*4),'truncated/wrong trace layout')
    a=np.frombuffer(raw,dtype='<u4');need(list(a[:3])==[1,wg,lane] and a[3]==0,'stale trace header or iteration overflow')
    f=a[HEADER:OUTER_BASE].reshape(LANES,FIXED);o=a[OUTER_BASE:TERM_BASE].reshape(32,LANES,OUTER)
    terms=a[TERM_BASE:IMAGE_BASE].reshape(1+2*TERMS,TERM)
    samples={};off=IMAGE_BASE
    for pc,loop,nc,nr in SITES:
        for k in range(TERMS if loop else 1):
            r=a[off:off+IMAGE];off+=IMAGE
            if r[0] or r[1]:
                marker(r,0,pc);marker(r,1,pc)
                samples[(pc,k)]={'pc':hex(pc),'light':k if loop else None,'coords':list(map(int,r[2:2+nc])),
                    'descriptor':list(map(int,r[9:17])),'sampler':list(map(int,r[17:21])),'result':list(map(int,r[21:21+nr]))}
    return f,o,terms,samples

def bd(words,raw,slot,buf):
    w=list(map(int,words));need(len(w)==4 and w==raw['buffer_descriptors'][slot][:4],'guest buffer descriptor mismatch')
    addr=w[0]|((w[1]&0xffff)<<32)
    need(addr==buf['guest'] and (w[1]>>16)&0x3fff==4 and w[3]==81924 and w[2]*4==buf['bytes'],'unsupported buffer descriptor semantics')
    return addr

def idesc(samples,pc,raw,slot,entry,snap):
    r=samples.get((pc,0));need(r is not None,'missing image descriptor at '+hex(pc))
    need(r['descriptor']==raw['image_descriptors'][slot][:8],'actual descriptor mismatch at '+hex(pc))
    d=descriptor(r['descriptor']);profile={0:(5,2340,9),1:(22,2340,9),2:(20,516,13)}[slot]
    need(d['base'] in (entry['guest'],snap['owner']) and [d['width'],d['height']]==snap['extent'] and (d['format'],d['select'],d['type'])==profile and d['base_mip']==d['last_mip']==d['base_array']==0 and not d['linked'],'unsupported image descriptor at '+hex(pc))

def replay_tile(code,runner,mode,wg,prefix,tiles,ds,shape):
    # PC 0x44..0xf8 is the original guest load/division/bounds sequence. Its
    # scalar descriptors and dimensions are captured at entry and checked
    # against the actual bound bytes. Prologue before 0x44 remains scoped out.
    s=[0]*106;s[4:8]=ds[0];s[8:12]=ds[1];s[12:15]=shape;s[2]=wg;s[3]=0
    v=[0]*(256*64)
    for l in range(64):v[l]=wg*8+l%8;v[64+l]=l//8;v[128+l]=wg;v[192+l]=8
    base=lambda d:d[0]|((d[1]&0xffff)<<32)
    z=run_wave(runner,code[0x44:0xfc]+exp(0,[4,5,3,2],True)+pack([0xbf810000]),s,v,
        merge([(base(ds[0]),prefix),(base(ds[1]),tiles)]),mode,(1<<64)-1)
    return {l:tuple(map(int,t[2][:2])) for (target,l),t in z.items() if target==0}

def arithmetic(code,runner,mode,pc,regs,values,dst):
    # Actual single guest op, using its original register numbering and MODE.
    v=[0]*(256*64)
    for r,x in zip(regs,values):v[r*64]=int(x)
    out=run_wave(runner,code[pc:pc+4]+exp(0,[dst]*4,True)+pack([0xbf810000]),[0]*106,v,[],mode,1)
    return int(out[(0,0)][2][0])

def verify_mask(c,f,o,active,buf,raw,meta):
    for l in active:
        r=f[l];marker(r,27,0xcf8);marker(r,34,0xcf8);marker(r,36,0xd4c)
        bd(r[29:33],raw,3,meta);idx=int(r[28]);need(idx<len(buf),'light-list load outside captured range')
        c.equal(0xcf8,'outer_buffer_load',int(buf[idx]),int(r[35]),l)
    union=functools.reduce(lambda a,l:a|int(f[l,35]),active,0)
    lights=[]
    for l in active:c.equal(0xd4c,'outer_wave_or',union,int(f[l,37]),l)
    for k in bitset(union):
        for l in active:
            r=o[k,l];marker(r,0,0xd68);marker(r,2,0xd68);marker(r,4,0xdbc)
            idx=int(f[l,28])+k+1;c.equal(0xd68,'inner_index',idx,int(r[1]),l)
            need(idx<len(buf),'inner light-list outside captured range');c.equal(0xd68,'inner_buffer_load',int(buf[idx]),int(r[3]),l)
        inner=functools.reduce(lambda a,l:a|int(o[k,l,3]),active,0)
        for l in active:c.equal(0xdbc,'inner_wave_or',inner,int(o[k,l,5]),l)
        lights.extend(k*32+b for b in bitset(inner))
    return union,lights

def verify_terms(c,terms,lights,selected,calc):
    sums=[]
    def addrow(r,pcs,op):
        for j,pc in enumerate(pcs):
            a,b=map(int,r[1+j*2:3+j*2]);ref=calc(pc,j,a,b,None)
            checked_float(c,pc,op,ref,int(r[11+j]),selected)
        sums.append([float32(x) for x in r[11:14]])
    r=terms[0]
    if r[0] or r[10]:
        marker(r,0,0x880);marker(r,10,0x888);addrow(r,(0x880,0x884,0x888),'base_add')
    primary=[]
    for i in range(TERMS):
        r=terms[1+i]
        if not np.any(r):continue
        primary.append(i)
        for j,pc in enumerate((0xc10,0xc1c,0xc28)):
            off=j*6;marker(r,off,pc);marker(r,off+4,pc)
            a,b,d=map(int,r[off+1:off+4]);ref=calc(pc,j,a,b,d)
            checked_float(c,pc,'primary_mac',ref,int(r[off+5]),selected)
    seen=[];contributions=[]
    for i in range(TERMS):
        r=terms[1+TERMS+i]
        if not np.any(r):continue
        marker(r,0,0x1d5c);marker(r,10,0x1d64);seen.append(i)
        addrow(r,(0x1d5c,0x1d60,0x1d64),'secondary_add')
        contributions.append({'light':i,'rgb':[float32(r[j]) for j in (2,4,6)]})
    need(seen==lights,'selected lane missing or extra light iterations')
    return {'primary_indices':primary,'secondary_contributions':contributions,'base_and_secondary_results':sums}

def audit(path,runner,witness='dark',conditional=False):
    a=Archive(path);c=Checks();report={'scope':'V4 local q8 contract, not complete shader arithmetic, shadow sampling or P8'}
    try:
        events=[(i,e) for i,e in enumerate(a.events) if e.get('kind')=='v4_q8_contract']
        need(len(events)==1,'exactly one V4 q8 dispatch required; '+str([e.get('reason') for e in a.events if e.get('kind')=='v4_q8_incomplete']))
        ei,e=events[0];ui,u=a.use[e['use']];need(ui<ei and e['frame']==u['frame']==a.frame and e['submit']==u['submit'] and e['shader']==SHADER,'dispatch identity mismatch')
        st=u['stages'][0];need(st['shader']==SHADER and st['valid'] and len(st['images'])==11 and len(st['buffers'])==4,'unsupported stage')
        raw=st['guest_operands'];code=a.z.read('ds-lighting-capture/'+st['guest_file']);need(digest(code)==HASH,'unrecognized guest q8 binary')
        mode=e['launch']['mode'];need(e['launch']['threads']==[8,8,1] and e['launch']['wave']==64 and e['launch']['group_pack']<=1 and e['launch']['workgroup_register']==2 and e['groups'][1:]==[1,1],'launch mismatch')
        wg=e['wg'];lane=e['lane'];need(0<=wg<e['groups'][0] and 0<=lane<64,'witness outside launch')
        full_trace=a.files[e['trace_file']]
        e,trace=select_witness(e,full_trace,witness)
        brdf_rows=None
        if e['schema']==4:
            from v4_q8_brdf import extract
            brdf_rows=extract(full_trace,PAIR_WORDS,witness)
        lane=e['lane']
        f,o,terms,samples=parse_trace(trace,wg,lane)
        metas=e['buffers'];buffers=[]
        need(len(metas)==4,'incomplete captured buffer list')
        for slot,b in enumerate(metas):
            src=st['buffers'][slot];need(b['slot']==src['slot']==slot and b['guest']==src['guest'] and b['native']==src['native'] and b['binding_offset']==src['native_offset'] and b['bytes']==src['bytes'] and not src['written'],'buffer identity mismatch')
            data=a.files[b['file']];need(len(data)==b['bytes'] and b['native_offset']>=b['binding_offset'] and b['native_offset']-b['binding_offset']+len(data)<=src['native_range'],'bad buffer range');buffers.append(data)
        p,t=np.frombuffer(buffers[0],'<u4'),np.frombuffer(buffers[1],'<u4');first,end=int(p[8]),int(p[9]);need(0<=first<end<=len(t) and end-first==e['groups'][0] and first==e['queue_first'] and end==e['queue_end'],'queue extent mismatch')
        tile=int(t[first+wg]);need(tile==e['tile'] and len(set(map(int,t[first:end])))==end-first,'queue identity mismatch')
        def snap(slot,role,layer,fmt):
            entry=st['images'][slot];ss=[s for s in entry['snapshots'] if s.get('valid') and s['role']==role and s['layer']==layer]
            need(len(ss)==1 and key(ss[0])==key(entry['image']) and ss[0]['format']==fmt,'snapshot identity '+role)
            return ss[0],a.snapshot(ss[0],role,later=ui)
        g=[snap(2,'gbuffer',i,98) for i in range(3)];ds=snap(1,'depth',0,130);ss=snap(0,'stencil',0,130)
        w,h=g[0][0]['extent'];need(all(s['extent']==[w,h] for s,_ in g+[ds,ss]),'mixed image extent')
        mat=[np.frombuffer(b,'<u4').reshape(h,w) for _,b in g];depth=np.frombuffer(ds[1],'<u4').reshape(h,w);stencil=np.frombuffer(ss[1],'u1').reshape(h,w)
        cols=(w+7)//8;need(tile<cols*((h+7)//8),'tile outside extent')
        x=(tile%cols)*8+lane%8;y=(tile//cols)*8+lane//8;need([x,y]==e['pixel'] and int(mat[0][y,x])==e['material_word'] and int(stencil[y,x])==e['stencil'],'wrong selected material')
        report['witness']={'frame':e['frame'],'submit':e['submit'],'wg':wg,'lane':lane,'pixel':[x,y],'material_word':e['material_word'],'stencil':e['stencil']}
        descs=[list(map(int,f[lane,56:60])),list(map(int,f[lane,60:64]))]
        for slot in (0,1):bd(descs[slot],raw,slot,metas[slot])
        ref=replay_tile(code,runner,mode,wg,buffers[0],buffers[1],descs,[cols,w,h]);active=[]
        idesc(samples,0x118,raw,0,st['images'][0],ss[0])
        for l,r in enumerate(f):
            marker(r,0,0);marker(r,6,0x44);marker(r,8,0x80);marker(r,11,0xf8)
            c.equal(0x44,'prefix_index',8,int(r[64]),l);c.equal(0x44,'prefix_load',first,int(r[7]),l);c.equal(0x80,'tile_index',first+wg,int(r[10]),l);c.equal(0x80,'tile_load',tile,int(r[9]),l)
            need(list(r[53:56])==[cols,w,h] and list(r[56:60])==descs[0] and list(r[60:64])==descs[1] and list(r[1:6])==[l%8,l//8,wg,0,1],'unexpected prologue register operands')
            xy=((tile%cols)*8+l%8,(tile//cols)*8+l//8);inside=xy[0]<w and xy[1]<h
            c.equal(0xf8,'reference_bounds',inside,l in ref,l);c.equal(0xf8,'bounds',int(inside),int(r[14]),l)
            if not inside:continue
            c.equal(0xf8,'tile_coordinates',list(ref[l]),list(map(int,r[12:14])),l)
            ix,iy=xy;marker(r,15,0x118);marker(r,17,0x128)
            c.equal(0x118,'stencil_load',int(stencil[iy,ix]),int(r[16]),l)
            admitted=bool(int(stencil[iy,ix])&64);c.equal(0x128,'stencil_exec',int(admitted),int(r[18]),l)
            if not admitted:continue
            active.append(l)
        if lane in active:
            for pc,slot,snap0 in [(0x150,1,ds[0]),(0x164,2,g[1][0]),(0x170,2,g[0][0]),(0x17c,2,g[2][0])]:
                idesc(samples,pc,raw,slot,st['images'][slot],snap0)
            for l in active:
                r=f[l];ix,iy=ref[l]
                for pc,m,v in [(0x150,19,depth),(0x164,21,mat[1]),(0x170,23,mat[0]),(0x17c,25,mat[2])]:
                    marker(r,m,pc);c.equal(pc,'depth_load' if pc==0x150 else 'material_load',int(v[iy,ix]),int(r[m+1]),l)
        b,aft=e['before'],e['after'];need(key(b)[:3]==key(aft)[:3]==key(st['images'][10]['image'])[:3]==key(st['images'][9]['image'])[:3] and b['format']==aft['format']==97 and not b.get('observation_reused') and not aft.get('observation_reused') and b['extent']==aft['extent']==[w,h],'unpaired HDR image')
        before=np.frombuffer(a.snapshot(b,'q8_hdr_before',later=ui),'<f2').reshape(h,w,4);after=np.frombuffer(a.snapshot(aft,'q8_hdr_after',earlier=ui,later=ei),'<f2').reshape(h,w,4)
        report['hdr_before']=before[y,x].astype(float).tolist();report['hdr_after']=after[y,x].astype(float).tolist();report['admitted_lanes']=active
        if e['schema'] in (3,4):
            observed_lum=float(before[y,x,:3].astype(np.float64)@np.array([.2126,.7152,.0722]))
            want=e['dark_luminance_before'] if witness=='dark' else e['control']['luminance_before']
            need(abs(observed_lum-want)<=1e-12,'selected incoming HDR does not match snapshot')
            need(lane in active,'selected pair not admitted')
        if lane not in active:
            c.equal(0x128,'rejected_pixel_preserved',before[y,x].view('<u2').tolist(),after[y,x].view('<u2').tolist(),lane)
            report['next']='producer of stencil/material admission for this pixel';return dict(report,status='MATCH',checks=c.counts)
        union,lights=verify_mask(c,f,o,active,np.frombuffer(buffers[3],'<u4'),raw,metas[3]);report['outer_mask']=union;report['light_indices']=lights
        addregs={0x880:(38,40,29),0x884:(34,41,30),0x888:(35,39,31),0x1d5c:(29,0,29),0x1d60:(30,50,30),0x1d64:(31,1,31)}
        def calc(pc,j,a,b,d):
            if d is None:r0,r1,dst=addregs[pc];return arithmetic(code,runner,mode,pc,[r0,r1],[a,b],dst)
            return arithmetic(code,runner,mode,pc,[29+j,0 if j else 52,j],[a,b,d],29+j)
        report['terms']=verify_terms(c,terms,lights,lane,calc)
        for l in active:
            r=f[l];marker(r,38,0x1d84);marker(r,47,0x1d98);marker(r,52,0x1d9c)
            ix,iy=map(int,r[39:41]);c.equal(0x1d84,'hdr_coordinates',list(ref[l]),[ix,iy],l)
            need(list(r[68:76])==raw['image_descriptors'][9][:8]==raw['image_descriptors'][10][:8],'HDR guest descriptor mismatch')
            hd=descriptor(list(map(int,r[68:76])));need(hd['base'] in (st['images'][9]['guest'],b['owner']) and (hd['format'],hd['select'],hd['type'])==(71,4012,9) and [hd['width'],hd['height']]==[w,h] and not hd['linked'],'unsupported HDR format/view')
            for j,pc in enumerate((0x1d90,0x1d98,0x1d94)):
                old=bits32(before[iy,ix,j]);need(normal_or_zero(old),'subnormal or nonfinite input HDR')
                c.equal(0x1d84,'hdr_load',old,int(r[41+j]),l)
                expected=arithmetic(code,runner,mode,pc,[j,29+j],[int(r[41+j]),int(r[44+j])],j)
                checked_float(c,pc,'hdr_add',expected,int(r[48+j]),l)
                expected_half=np.float16(float32(int(r[48+j])));actual=after[iy,ix,j]
                need(np.isfinite(expected_half) and np.isfinite(actual),'HDR conversion nonfinite')
                # Vulkan half conversion rounding: allow either adjacent half;
                # do not classify half-subnormal flushing as a proven defect.
                if int(r[48+j])&0x7fffffff==0:c.equal(0x1d9c,'zero_store',0.0,float(actual),l)
                elif abs(float(expected_half))<2**-14:
                    lo=np.nextafter(expected_half,np.float16(-np.inf));hi=np.nextafter(expected_half,np.float16(np.inf))
                    if float(actual)==0.0 and not lo<=actual<=hi:
                        need(False,'subnormal flushing ambiguous')
                    c.equal(0x1d9c,'hdr_subnormal_store_within_1ulp',True,bool(lo<=actual<=hi),l)
                else:
                    lo=np.nextafter(expected_half,np.float16(-np.inf));hi=np.nextafter(expected_half,np.float16(np.inf))
                    c.equal(0x1d9c,'hdr_store_within_1ulp',True,bool(lo<=actual<=hi),l)
            c.equal(0x1d9c,'alpha_store',0,int(after[iy,ix,3].view('<u2')),l)
        report['samples']=list(samples.values());report['selected_accumulation']=[float32(v) for v in f[lane,44:47]]
        if conditional:
            from v4_q8_pair_replay import replay
            # Every native memory load/mask/HDR operation above must match
            # before using its observed wave boundary in the scoped replay.
            report['conditional_isa']=replay(code,runner,mode,wg,lane,raw,
                [(b['guest'],data) for b,data in zip(metas,buffers)],f,o,samples,union,lights,terms,
                brdf=brdf_rows,records=np.frombuffer(trace,'<u4')[WORDS:].reshape(TERMS,48) if brdf_rows is not None else None)
        if not lights:report['next']='producer of the empty clustered light mask, exact slot 3 and captured indices'
        elif report['terms']['secondary_contributions']:
            term=max(report['terms']['secondary_contributions'],key=lambda q:sum(abs(v) for v in q['rgb']))
            report['next']={'contract':'trace actual light term inputs and sample descriptors, not globally correct lighting','light':term['light'],'rgb':term['rgb'],'sample_pcs':[v['pc'] for v in samples.values() if v['light']==term['light']]}
        else:report['next']='first unmodeled base irradiance sample/producer; same captured descriptor and coordinates'
        return dict(report,status='MATCH',checks=c.counts)
    except Divergence as ex:return dict(report,status='DIVERGENCE',first=ex.row,checks=c.counts)
    except (ValueError,KeyError,TypeError,IndexError,OSError,subprocess.SubprocessError) as ex:return dict(report,status='INCOMPLETE',reason=str(ex),checks=c.counts)
    finally:a.close()

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path);p.add_argument('runner',type=Path);a=p.parse_args()
    print(json.dumps(audit(a.archive,a.runner),indent=2,allow_nan=False))
if __name__=='__main__':main()
