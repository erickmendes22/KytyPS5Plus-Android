#!/usr/bin/env python3
"""Same q8/q33 witness: exact probe lookup, buffer read and LOD0 sample contracts.
No native sample is treated as a guest-root reference. An unsupported sampler,
out-of-range footprint or missing trace remains explicitly incomplete.
"""
import argparse,json,math,struct
from pathlib import Path
import numpy as np
from p8_paired_replay import Archive,need,digest,descriptor
from p8_guest_graphics import pack,merge,run_wave,decode_all
from prelight_vertex_oracle import LLVM
from v4_q8_pair_replay import u,mov,lit,jump,exp,NOP,END,smov,scmp,fl,dist,regs,xor_sign,cndmask
SHADER=0xe3daeb1e7426761b
HASH='23f2a3429fae7ab78ab464eb3bd61f5807516faa2a1aade35ae97ffdadd64638'
# pc, binding slot, kind, loop, coordinates, destination, count, descriptor, sampler
SITES=[
 (0x104,0,0,0,(6,7),0,1,4,255),(0x13c,1,0,0,(6,7),11,1,4,255),
 (0x178,2,0,0,(6,7,1),3,1,20,255),(0x184,2,0,0,(6,7,2),10,1,20,255),
 (0x1a0,2,0,0,(6,7,1),2,1,20,255),(0x3e4,2,0,0,(6,7,8),12,1,20,255),
 (0x508,3,1,0,(27,28),0,3,0,16),(0x5a4,4,0,0,(22,23,24),21,1,0,255),
 (0x600,5,0,0,(0,1,2),21,1,0,255),(0x618,2,2,0,(21,),0,3,28,255),
 (0x708,3,2,0,(21,),0,1,36,255),(0x9ec,4,2,1,(31,),5,1,24,255),
 (0xac0,6,1,1,(3,33,35,5),33,1,8,32),(0xacc,7,1,1,(4,34,5),3,3,0,16),
 (0xb44,5,2,0,(21,),25,1,40,255),(0xca4,2,2,0,(2,),0,2,28,255),
 (0xd58,2,2,0,(2,),0,1,28,255),(0xd74,3,2,0,(2,),0,1,36,255),
 (0xfa8,4,2,2,(30,),2,1,24,255),(0x107c,6,1,2,(0,32,34,2),32,1,8,32),
 (0x1088,7,1,2,(1,33,2),0,3,0,16),(0x112c,8,0,0,(6,7),0,3,0,255),
 (0x1174,9,3,0,(6,7),0,4,0,255)]
BY_PC={s[0]:s for s in SITES}
WAVE_BASE=8;WAVE_STRIDE=12;SITE_BASE=WAVE_BASE+64*WAVE_STRIDE
STRIDE=28;WITNESS=sum((129 if s[3] else 1)*STRIDE for s in SITES)
WORDS=SITE_BASE+2*WITNESS
SIGNS={0x250:(5,8,False),0x264:(20,19,True),0x42c:(10,10,False),
       0x440:(21,12,True),0x78c:(30,30,False),0x798:(16,29,True),
       0x884:(5,19,True),0x890:(19,28,True),0xa20:(35,32,True),
       0xa2c:(4,33,True),0xdf4:(28,28,False),0xe00:(2,20,True),
       0xfdc:(34,31,True),0xfe8:(1,32,True)}
def fbits(x):return struct.unpack('<I',struct.pack('<f',float(x)))[0]
def onehot(k):return 0<k<=128 and k&(k-1)==0

def parse(data,wg,lane,control):
    need(len(data)==4*WORDS,'trace size');a=np.frombuffer(data,'<u4')
    need(list(a[:5])==[2,wg,lane,0,control] and lane!=control and 0<=lane<64 and 0<=control<64,'trace header')
    wave=a[WAVE_BASE:SITE_BASE].reshape(64,WAVE_STRIDE)
    rows=[]
    for witness in range(2):
        at=SITE_BASE+witness*WITNESS;found={}
        for s in SITES:
            pc,slot,kind,loop,coords,dst,count,desc,samp=s
            for k in range(129 if loop else 1):
                r=a[at:at+STRIDE];at+=STRIDE
                if not np.any(r):continue
                need(r[0]==r[1]==pc+1 and (onehot(k) if loop else k==0),'partial or invalid site record')
                found[pc,k]={'coords':list(map(int,r[2:2+len(coords)])),'descriptor':list(map(int,r[9:9+(4 if kind==2 else 8)])),
                              'sampler':list(map(int,r[17:21])),'result':list(map(int,r[21:21+count]))}
        need(at==SITE_BASE+(witness+1)*WITNESS,'trace offset')
        need(all((pc,0) in found for pc in (0x104,0x13c,0x178,0x184,0x1a0,0x508,0x618,0x112c,0x1174)),'missing admitted pixel operations')
        rows.append(found)
    return wave,rows

def read_buffer(pc,row,buffers,raw):
    d=row['descriptor'];need(d in [list(x[:4]) for x in raw['buffer_descriptors']],'actual buffer descriptor not retained')
    base=d[0]|((d[1]&65535)<<32);stride=(d[1]>>16)&0x3fff
    need(stride and not(d[1]>>30),'unsupported swizzled buffer')
    idx=row['coords'][0];n=4*len(row['result']);offset=idx*stride+(8 if pc==0xd58 else 0)
    if pc==0xb44:
        # Actual #763 descriptor: stride=1, DstSelX=4, RawFormat=5
        # (k8UInt). buffer_load_format_x zero-extends the selected byte.
        # The guest immediately consumes the integer as a six-bit face mask.
        need(stride==1 and d[3]==20484 and len(row['result'])==1,'unmodeled formatted buffer load')
        n=1
    need(idx<d[2] and offset+n<=stride*(idx+1),'record bounds')
    matches=[data for addr,data in buffers if addr==base]
    need(len(matches)==1 and offset+n<=len(matches[0]),'buffer range not retained')
    if pc==0xb44:return [matches[0][offset]]
    return list(struct.unpack('<'+'I'*(n//4),matches[0][offset:offset+n]))

def verify_image(row,entry,raw,slot):
    need(row['descriptor']==list(raw['image_descriptors'][slot][:8]),'actual image descriptor differs')
    d=descriptor(row['descriptor']);view=entry['view'];ident=entry['image']
    need(d['base'] in (entry['guest'],ident['owner']) and not d['linked'] and
         d['base_mip']==d['last_mip']==view['mip'] and view['levels']==1,'unmodeled image descriptor/view')
    need(d['width']==ident['width'] and d['height']==ident['height'],'image extent mismatch')
    return d

def footprint_reference(pc,row,fp,entry,raw):
    slot=BY_PC[pc][1];need(fp['ok'] and fp['slot']==slot,'footprint unavailable')
    need(all(fp['identity'][k]==entry['image'][k] for k in ('owner','object','native','version','write_order')) and fp['view']==entry['view'],'footprint identity')
    d=verify_image(row,entry,raw,slot)
    w,h,z=fp['extent'];x,y,zz=fp['xyz'];side=fp['side'];size=fp['texel_bytes'];data=bytes(fp['bytes'])
    need(w==d['width'] and h==d['height'] and len(data)==side*side*size,'footprint dimensions')
    if slot in (4,5):
        need(d['type']==10 and d['format']==20 and d['select']&7==4 and side==1 and size==4 and
             [x,y,zz]==row['coords'] and 0<=x<w and 0<=y<h and 0<=zz<z,'3D lookup contract')
        return [struct.unpack('<I',data)[0]],0.0
    need(slot in (3,6,7) and side==2,'sample footprint')
    sm=fp['sampler'];need(sm['raw']==row['sampler'] and row['sampler'] in [list(x[:4]) for x in raw['sampler_descriptors']],'sampler identity')
    # All fingerprinted sites here are image_sample[_c]_lz, explicit Lod=0,
    # no Grad. Vulkan's derivative image operation defines zero derivatives
    # and the point footprint is isotropic (eta=1); AnisoLinear therefore
    # uses the linear filter, AnisoPoint the nearest filter. This does not
    # model implicit-LOD/gradient anisotropy.
    need(not sm['unnormalized'] and sm['reduction']==0 and sm['mag']==sm['min'] and sm['mag'] in (0,1,2,3) and
         (sm['mip_filter']==0 or sm['min_lod']==0),'unsupported sampler')
    need((slot==3 and d['format']==56) or (slot==6 and d['format']==7) or (slot==7 and d['format']==71),'guest sample format')
    c=[fl(v) for v in row['coords']];u0,v0=c[1:3] if slot==6 else c[:2]
    tx=u0*w-.5;ty=v0*h-.5
    need(math.floor(tx)==x and math.floor(ty)==y and 0<=x<w-1 and 0<=y<h-1,'footprint coordinate')
    if slot in (6,7):
        layer=c[3] if slot==6 else c[2]
        need(d['type']==13 and int(layer)==layer and fp['layer']==entry['view']['layer']+int(layer),'array layer')
    if slot==6:
        need(size==2 and entry['view']['format']==124 and 0<=c[0]<=1,'shadow format/reference')
        v=np.frombuffer(data,'<u2').astype(float).reshape(2,2,1)/65535
        ref=c[0];op=sm['compare'];need(0<=op<=7,'compare enum')
        # Equality near UNORM conversion ambiguity is not proof of a mismatch.
        need(op in (0,7) or np.min(np.abs(v-ref))>1/65535,'shadow threshold ambiguity')
        v=[np.zeros_like(v),ref<v,ref==v,ref<=v,ref>v,ref!=v,ref>=v,np.ones_like(v)][op].astype(float)
    elif slot==3:
        need(size==4 and entry['view']['format']==37 and d['type']==9,'screen format')
        v=np.frombuffer(data,np.uint8).astype(float).reshape(2,2,4)/255
    else:
        need(size==8 and entry['view']['format']==97,'atlas format')
        v=np.frombuffer(data,'<f2').astype(float).reshape(2,2,4)
    need(np.isfinite(v).all(),'nonfinite texel')
    if slot!=6:
        selectors=[(d['select']>>(3*i))&7 for i in range(3)]
        need(all(s in (0,1,4,5,6,7) for s in selectors),'unsupported swizzle')
        v=np.stack([np.zeros((2,2)) if s==0 else np.ones((2,2)) if s==1 else v[:,:,s-4] for s in selectors],axis=2)
    if sm['mag'] in (0,2):
        q=v[math.floor(ty+.5)-y,math.floor(tx+.5)-x];tolerance=1e-6
    else:
        fx,fy=tx-x,ty-y
        q=(1-fy)*((1-fx)*v[0,0]+fx*v[0,1])+fy*((1-fx)*v[1,0]+fx*v[1,1])
        bits=fp['subtexel_bits'];need(4<=bits<=24,'subtexel precision')
        tolerance=float(np.max(np.ptp(v.reshape(4,-1),axis=0)))*2*2**(-bits)+max(1e-7,float(np.max(np.abs(v)))*2e-6)
        if slot==3:tolerance+=1/255 # normalized fixed-point filter output allowance
    return list(map(fbits,q)),tolerance

def sign_code(pc):
    d,r,t=SIGNS[pc];return xor_sign(100,r)+cndmask(d,r,100) if t else xor_sign(100,r)+cndmask(d,100,r)

def check_signs(runner,mode):
    # Existing q8 regression proved SDWA NEG loss in ROCjitsu. Test the exact
    # lowered data path with both VCC states, both halves of a wave and sign bits.
    lanes=(0,31,39,63);mask=sum(1<<l for l in lanes)
    for pc,(d,r,t) in SIGNS.items():
        for truth in (0,1):
            sg=[0]*106;vg=[0]*(256*64)
            # s_mov_b64 vcc, -1 or 0
            code=u(0xbeea04c1 if truth else 0xbeea0480)+sign_code(pc)+exp(0,[d]*4,True)+END
            for l in lanes:vg[r*64+l]=0xbe85a1cb
            out=run_wave(runner,code,sg,vg,[],mode,mask)
            want=0xbe85a1cb^(0x80000000 if truth==t else 0)
            need(all((0,l) in out and out[0,l][2][0]==want for l in lanes),'SDWA lowering control')
    return len(SIGNS)*8

def prefix_reference(code,runner,launch,wg,raw,buffers):
    """Execute the original prefix on the complete wave; no observed coordinates injected."""
    need(digest(code)==HASH and len(raw['user_data'])==2,'q33 prefix identity')
    seg=merge([(r['address'],pack(r['words'])) for r in raw['scalar_regions']]+buffers[:2])
    vg=[0]*(256*64)
    for l in range(64):vg[l]=l%8;vg[64+l]=l//8
    out=[]
    for poison in (0,0x12345678,0xffffffff):
        sg=[poison]*106;sg[:2]=raw['user_data'];sg[2]=wg
        actual=run_wave(runner,code[:0xe8]+exp(0,[6,7,6,7],True)+END,sg,vg,seg,launch['mode'],(1<<64)-1)
        out.append({l:list(x[2][:2]) for (t,l),x in actual.items() if t==0})
    need(out[0]==out[1]==out[2],'prefix depends on unknown launch input')
    return out[0]

def scalar_segments(raw,buffers,poison=0x12345678):
    """Only call for HASH: 0x4c0 loads two dead padding words.
    The original uses s8..10,s12..14; s11/s15 are overwritten at 0x84c
    before any use. The SRT capture records consumed dwords, not padding.
    Never synthesize a live operand or overwrite a captured byte.
    """
    parts=[(r['address'],pack(r['words'])) for r in raw['scalar_regions']]+buffers
    seg=merge(parts)
    def read(addr,n):
        for a,b in seg:
            if a<=addr and addr+n<=a+len(b):return b[addr-a:addr-a+n]
        return None
    need(len(raw['user_data'])==2,'scalar root ABI')
    user=int.from_bytes(pack(raw['user_data']),'little')
    ptr=read(user+8,8);need(ptr is not None,'missing actual SRT pointer')
    root=int.from_bytes(ptr,'little')
    for offset in (0x3c,0x4c):
        addr=root+offset
        if read(addr,4) is not None:continue
        need(read(addr-12,12) is not None,'missing live bounds')
        need(not any(a<addr+4 and addr<a+len(b) for a,b in seg),'partial padding capture')
        parts.append((addr,pack([poison])))
    return merge(parts)

def replay(code,runner,launch,wg,lane,raw,buffers,wave,expected):
    need(digest(code)==HASH and raw['scalar_complete'] and len(raw['user_data'])==2,'q33 ISA/operands')
    dec=LLVM()
    try:rows=decode_all(dec,code)
    finally:dec.close()
    sizes={pc:(rows[i+1][0]-pc if i+1<len(rows) else 4) for i,(pc,t) in enumerate(rows)}
    dis=dict(rows)
    need(not any(set(regs(t,'v'))&set(range(100,124)) or set(regs(t,'s'))&{100,101,102,103} for _,t in rows),'scratch register conflict')
    need(all(sizes.get(p)==8 and 'v_cndmask_b32_sdwa' in dis[p] for p in SIGNS),'SDWA site fingerprint')
    for pc,slot,kind,loop,coords,dst,count,desc,samp in SITES:
        text=dis.get(pc,'');need(text.startswith('buffer_load' if kind==2 else 'image_store' if kind==3 else 'image_'),'site opcode')
    original=code[:rows[-1][0]+4]
    mode=launch['mode'];controls=check_signs(runner,mode)
    need(dis[0x4c0].startswith('s_load_dwordx8 s[8:15]') and dis[0x84c].startswith('s_load_dwordx16 s[0:15]'),'dead padding proof fingerprint')
    seg=scalar_segments(raw,buffers)
    sg=[0]*106;sg[:2]=raw['user_data'];sg[2]=wg;sg[3]=0;sg[102]=(1<<lane)&0xffffffff;sg[103]=(1<<lane)>>32
    vg=[0]*(256*64);vg[lane]=lane%8;vg[64+lane]=lane//8
    reductions={}
    for pc,end,restore,reg,scalar,col in ((0x8d0,0x91c,0x904,26,48,6),(0xe90,0xedc,0xec4,27,36,9)):
        present=wave[:,col]==pc+1
        if not present.any():continue
        need(present.all(),'wave mask census')
        union=int(np.bitwise_or.reduce(wave[:,col+1]));need(union<=255 and np.all(wave[:,col+2]==union),'wave OR mismatch')
        reductions[pc]=(end,restore,reg,scalar,col,union)
    done={};deltas=[]
    def patched():
        p=bytearray(original)
        def replace(pc,body,n=None):
            n=sizes[pc] if n is None else n;start=len(p)
            p[pc:pc+n]=jump(pc,start)+NOP*(n//4-1);p.extend(body(start))
        for pc in SIGNS:replace(pc,lambda start,pc=pc:sign_code(pc)+jump(start+12,pc+8))
        replace(0xe4,lambda start:code[0xe4:0xe8]+u(0x87fe7e66)+jump(start+8,0xe8))
        for pc,(end,restore,reg,scalar,col,union) in reductions.items():
            def body(start,pc=pc,end=end,restore=restore,reg=reg,scalar=scalar,union=union):
                b=bytearray()
                if (pc,0) not in done:
                    b+=NOP+lit(100,pc)+lit(101,0)+mov(104,256+reg)+exp(60,[100,101,104,104],True)+END
                    b[:4]=jump(start,start+len(b),0xbf880000)
                b+=code[pc:pc+4]+code[restore:restore+4]+smov(scalar,union)
                return b+jump(start+len(b),end)
            replace(pc,body,end-pc)
        for pc,slot,kind,loop,coords,dst,count,desc,samp in SITES:
            def body(start,pc=pc,kind=kind,loop=loop,coords=coords,dst=dst,count=count,desc=desc,samp=samp):
                b=bytearray(jump(start,pc+sizes[pc],0xbf880000));cases=[]
                if kind!=3 and loop:
                    b+=u(0x85000000|(101<<16)|(128<<8)|129)
                    for (site,k),vals in done.items():
                        if site==pc:b+=scmp(49 if loop==1 else 37,k);cases.append((len(b),vals));b+=NOP
                    b+=u(0xbf070000|(128<<8)|101)
                elif kind!=3 and (pc,0) in done:
                    b+=b''.join(lit(dst+j,v) for j,v in enumerate(done[pc,0]));return b+jump(start+len(b),pc+sizes[pc])
                b+=lit(100,pc)+(mov(101,49 if loop==1 else 37) if loop else lit(101,0))
                for j in range(4):b+=mov(104+j,256+coords[j]) if j<len(coords) else lit(104+j,0)
                b+=exp(60,[100,101,104,105])+exp(59,[106,107,106,107])
                for j in range(8):b+=mov(112+j,desc+j) if j<(4 if kind==2 else 8) else lit(112+j,0)
                b+=exp(61,[112,113,114,115])+exp(62,[116,117,118,119])
                for j in range(4):b+=mov(120+j,samp+j) if samp!=255 else lit(120+j,0)
                if kind==3:b+=exp(63,[120,121,122,123])+exp(0,[dst,dst+1,dst+2,dst+3],True)+END
                else:b+=exp(63,[120,121,122,123],True)+END
                for loc,vals in cases:
                    b[loc:loc+4]=jump(start+loc,start+len(b),0xbf850000);b+=u(0xbf070000|(128<<8)|101)
                    b+=b''.join(lit(dst+j,v) for j,v in enumerate(vals));b+=jump(start+len(b),pc+sizes[pc])
                return b
            replace(pc,body)
        return bytes(p)
    for iteration in range(len(expected)+4):
        out=run_wave(runner,patched(),sg,vg,seg,mode,1<<lane)
        need((60,lane) in out,'missing trap');pc,k,c0,c1=out[60,lane][2]
        if pc in reductions:
            need((pc,0) not in done and c0==int(wave[lane,reductions[pc][4]+1]),'original lane mask differs')
            done[pc,0]=[];continue
        key=(pc,k);need(key in expected and key not in done,'unexpected original visit')
        r=expected[key];site=BY_PC[pc];kind=site[2];n=len(site[4])
        co=[c0,c1,*out[59,lane][2]][:n]
        desc=list(out[61,lane][2]+out[62,lane][2])[:len(r['descriptor'])]
        sampler=list(out[63,lane][2]);need(desc==r['descriptor'] and sampler==r['sampler'],'original descriptor or sampler differs')
        d=[dist(x,y) if kind==1 else (0 if x==y else 2**32) for x,y in zip(co,r['coords'])]
        if max(d,default=0)>8:return dict(status='DIVERGENCE_CANDIDATE',pc=hex(pc),key=k,reference=co,observed=r['coords'],ulp=d)
        deltas.extend(d)
        if kind==3:
            need(set(expected)==({a for a in done if a[0] in BY_PC}|{key}),'missing native/original visit')
            rgb=list(out[0,lane][2]);ds=[dist(x,y) for x,y in zip(rgb,r['result'])]
            return dict(status='MATCH_CONDITIONAL' if max(ds)<=8 else 'DIVERGENCE_CANDIDATE',
                        final_ulp=ds,reference=rgb,observed=r['result'],visits=len(expected),max_coordinate_ulp=max(deltas,default=0),sign_controls=controls)
        done[key]=r['result']
    raise ValueError('trap budget')

def audit(a,runner=None):
    events=[(i,e) for i,e in enumerate(a.events) if e['kind']=='v4_gi_contract']
    if not events:
        return dict(status='INCOMPLETE',reason='q33 acquisition not completed',
                    acquisition=[e for e in a.events if e['kind']=='v4_gi_incomplete'])
    need(len(events)==1,'single q33 paired event required');ei,e=events[0]
    ui,use=a.use[e['use']];qi,quse=a.use[e['q8_use']]
    need(qi<ui<ei and use['frame']==e['frame']==quse['frame']==a.frame and use['submit']==e['submit'],'dispatch order')
    q8=[x for x in a.events if x.get('kind')=='v4_q8_contract' and x.get('use')==e['q8_use']]
    need(len(q8)==1 and all(e[k]==q8[0][k] for k in ('tile','lane','pixel','control')) and q8[0]['after']==e['q8_after'],'same q8 pair')
    stage=use['stages'][0];raw=stage['guest_operands'];need(stage['shader']==SHADER and len(stage['buffers'])==6 and len(stage['images'])==11,'q33 bindings')
    launch=e['launch'];need(launch['threads']==[8,8,1] and launch['wave']==64 and launch['group_pack'] in (0,1) and launch['workgroup_register']==2 and launch['group_id'][0] and
        launch.get('thread_ids',2)>=2 and not launch.get('dispatch_thread_dimensions',False),'q33 ABI')
    buffers=[]
    for i,b in enumerate(e['buffers']):
        s=stage['buffers'][i];data=a.files.get(b['file'])
        need(b['slot']==i and s['slot']==i and s['access_known'] and not s['written'] and
             all(b[k]==s[k] for k in ('guest','bytes','native')) and b['binding_offset']==s['native_offset'] and
             0<=b['native_offset']-b['binding_offset']<=255 and b['native_offset']-b['binding_offset']+b['bytes']<=s['native_range'] and
             data is not None and len(data)==b['bytes'],'buffer identity/length')
        buffers.append((b['guest'],data))
    need(len(buffers)==6,'buffer census')
    prefix=np.frombuffer(buffers[0][1],'<u4');tiles=np.frombuffer(buffers[1][1],'<u4')
    need(len(prefix)>34 and prefix[33]==e['queue_first'] and prefix[34]==e['queue_end'] and
         e['queue_end']-e['queue_first']==e['groups'][0] and e['wg']<e['groups'][0] and
         tiles[e['queue_first']+e['wg']]==e['tile'],'queue tile binding')
    wave,records=parse(a.files[e['trace_file']],e['wg'],e['lane'],e['control']['lane'])
    fps={}
    for f in e['footprints']:
        key=(f['witness'],f['pc'],f['key']);need(key not in fps,'duplicate footprint');fps[key]=f
    code=a.z.read('ds-lighting-capture/'+stage['guest_file']);need(digest(code)==HASH,'guest fingerprint')
    prefix_result=None
    if runner:
        try:
            coords=prefix_reference(code,runner,launch,e['wg'],raw,buffers)
            expected={l:list(map(int,wave[l,1:3])) for l in range(64) if wave[l,0]==0xe5 and wave[l,3]==1}
            need(len(expected)>0 and all(wave[:,0]==0xe5),'incomplete native prefix')
            prefix_result=dict(status='MATCH_LOCAL' if coords==expected else 'DIVERGENCE_CANDIDATE',
                lanes=len(coords),native_lanes=len(expected),first_differences=[dict(lane=l,reference=coords.get(l),observed=expected.get(l)) for l in sorted(set(coords)|set(expected)) if coords.get(l)!=expected.get(l)][:8])
        except Exception as ex:prefix_result=dict(status='INCOMPLETE',reason=str(ex))
    reports=[]
    for wi,(lane,pixel) in enumerate(((e['lane'],e['pixel']),(e['control']['lane'],e['control']['pixel']))):
        need(list(wave[lane,:6])==[0xe5,*pixel,1,0x115,1],'actual wave pixel/admission')
        r=records[wi];local=[]
        for (pc,k),row in r.items():
            site=BY_PC[pc];slot,kind=site[1:3]
            if kind==2:
                try:
                    expect=read_buffer(pc,row,buffers,raw)
                    local.append(dict(pc=hex(pc),key=k,status='MATCH_LOCAL' if expect==row['result'] else 'DIVERGENCE_LOCAL',expected=expect,observed=row['result']))
                except ValueError as ex:local.append(dict(pc=hex(pc),key=k,status='INCOMPLETE',reason=str(ex)))
            elif slot in (0,1,2):
                try:
                    entry=stage['images'][slot];d=verify_image(row,entry,raw,slot)
                    x,y,*layer=row['coords'];layer=layer[0] if layer else 0
                    role=('stencil','depth','gbuffer')[slot]
                    snap=[s for s in entry['snapshots'] if s['valid'] and s['role']==role and s['layer']==entry['view']['layer']+layer]
                    need(len(snap)==1,'missing exact material/depth/stencil snapshot')
                    snap=snap[0];data=a.snapshot(snap,role)
                    w,h=snap['extent'];n=snap['texel_bytes'];need(0<=x<w and 0<=y<h and n==(1 if slot==0 else 4),'material bounds')
                    need((d['format'],d['select'],d['type'])=={0:(5,2340,9),1:(22,2340,9),2:(20,516,13)}[slot],'unsupported material descriptor')
                    offset=(y*w+x)*n;expect=[int.from_bytes(data[offset:offset+n],'little')]
                    local.append(dict(pc=hex(pc),key=k,status='MATCH_LOCAL' if expect==row['result'] else 'DIVERGENCE_LOCAL',expected=expect,observed=row['result']))
                except (ValueError,KeyError) as ex:local.append(dict(pc=hex(pc),key=k,status='INCOMPLETE',reason=str(ex)))
            elif slot in (3,4,5,6,7):
                try:
                    expect,tol=footprint_reference(pc,row,fps[wi,pc,k],stage['images'][slot],raw)
                    equal=expect==row['result'] if slot in (4,5) else all(abs(fl(x)-fl(y))<=tol for x,y in zip(expect,row['result']))
                    local.append(dict(pc=hex(pc),key=k,status='MATCH_LOCAL' if equal else 'DIVERGENCE_CANDIDATE',expected=expect,observed=row['result'],tolerance=tol))
                except (ValueError,KeyError,OverflowError) as ex:local.append(dict(pc=hex(pc),key=k,status='INCOMPLETE',reason=str(ex)))
        before=e['hdr_before'];after=e['hdr_after'];need(before and after,'native HDR pair')
        need(all(before['identity'][k]==after['identity'][k] for k in ('owner','object','native')) and
             before['mip']==after['mip'] and before['layer']==after['layer'],'HDR identity')
        b=before['half_words'][4*wi:4*wi+4];a0=after['half_words'][4*wi:4*wi+4]
        incoming=[fbits(x) for x in np.asarray(b,'<u2').view('<f2')[:3]]
        from v4_relight_slice import half_rtz
        store_values=[fl(x) for x in r[0x1174,0]['result']]
        stored=list(map(int,np.asarray(store_values,'<f2').view('<u2')))
        truncated=half_rtz(store_values)
        need(r[0x112c,0]['coords']==pixel and r[0x1174,0]['coords']==pixel,'HDR coords')
        local.append(dict(pc='0x112c',status='MATCH_LOCAL' if incoming==r[0x112c,0]['result'] else 'DIVERGENCE_LOCAL'))
        local.append(dict(pc='0x1174',status='MATCH_LOCAL' if a0 in (stored,truncated) else 'DIVERGENCE_CANDIDATE',
            expected_rne=stored,expected_rtz=truncated,observed=a0,
            rounding='RTZ' if a0==truncated and a0!=stored else 'RNE_OR_EXACT' if a0==stored else 'UNEXPLAINED'))
        rr=None
        if runner:
            try:rr=replay(code,runner,launch,e['wg'],lane,raw,buffers,wave,r)
            except Exception as ex:rr=dict(status='INCOMPLETE',reason=str(ex))
        reports.append(dict(pixel=pixel,lane=lane,checks=local,conditional_isa=rr,hdr_before=b,hdr_after=a0,
            lookup=[dict(pc=hex(pc),**r[pc,0]) for pc in (0x5a4,0x600,0x618) if (pc,0) in r]))
    checks=[x for r in reports for x in r['checks']]
    bad=[x for x in checks if x['status'].startswith('DIVERGENCE')]
    incomplete=[x for x in checks if x['status']=='INCOMPLETE']
    replay_bad=[r['conditional_isa'] for r in reports if r['conditional_isa'] and r['conditional_isa']['status']!='MATCH_CONDITIONAL']
    return dict(status='DIVERGENCE_CANDIDATE' if bad or any(x['status'].startswith('DIVERGENCE') for x in replay_bad) or
                (prefix_result and prefix_result['status'].startswith('DIVERGENCE')) else
                'INCOMPLETE' if incomplete or replay_bad or not runner or not prefix_result or prefix_result['status']!='MATCH_LOCAL' else 'MATCH_LOCAL_CONDITIONAL',
                prefix=prefix_result,
                matched=sum(x['status']=='MATCH_LOCAL' for x in checks),differences=len(bad),incomplete=len(incomplete),
                witnesses=reports,scope='paired q33 operations, actual native input bytes; no global P8 certification')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path);p.add_argument('--runner',type=Path);args=p.parse_args()
    a=Archive(args.archive)
    try:print(json.dumps(audit(a,args.runner),indent=2))
    finally:a.close()
