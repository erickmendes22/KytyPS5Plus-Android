#!/usr/bin/env python3
"""Execute original GFX10 arithmetic with independently modeled image reads.

The runner's MIMG implementation is incomplete. Retain original offsets and
branches; use a diagnostic trampoline at each image read to export the actual
address VGPRs, image descriptor and sampler SGPRs. Resolve samples from the
logical, versioned image bytes, then rerun with lane-specific results in
otherwise unused VGPRs. A bounded dynamic-visit counter allows loops. This is not a proof of DCC, driver filtering precision,
fragment admission, or resource producers. Reject unsupported paths.
"""
from dataclasses import dataclass
import re
import struct
import math
import numpy as np
from p8_guest_graphics import decode_all, run_wave, pack
from p8_bilateral import require


@dataclass(frozen=True)
class Read:
    pc:int
    size:int
    op:str
    dst:tuple
    coords:tuple
    image:tuple
    sampler:tuple
    dmask:int
    dim:str


def regs(s,kind):
    s=s.strip()
    if re.fullmatch(kind+r'\d+',s):return (int(s[1:]),)
    m=re.fullmatch(kind+r'\[(\d+):(\d+)\]',s)
    if m:return tuple(range(int(m[1]),int(m[2])+1))
    require(s.startswith('[') and s.endswith(']'),'unrecognized register list '+s)
    return tuple(r for part in s[1:-1].split(',') for r in regs(part,kind))


def operands(text):
    return re.split(r',\s*(?![^\[]*\])',text)


def analyze(decoder,code):
    rows=decode_all(decoder,code);end=rows[-1][0]+4
    # A discard path may follow the first s_endpgm. Decode only reachable
    # termination tails rather than treating data after the program as ISA.
    while True:
        targets=[]
        for pc,t in rows:
            if t.startswith(('s_branch ','s_cbranch_')):
                v=int(t.rsplit(' ',1)[1],0)&0xffff;v=v-0x10000 if v>=0x8000 else v
                targets.append(pc+4+v*4)
        if not any(t>=end for t in targets):break
        require(end<len(code),'branch outside guest binary')
        tail=decode_all(decoder,code[end:]);rows.extend((pc+end,t) for pc,t in tail);end=rows[-1][0]+4
    pcs={pc for pc,t in rows}
    reads=[];used=set();sg_used=set()
    for i,(pc,t) in enumerate(rows):
        for kind,target in (('v',used),('s',sg_used)):
            for m in re.finditer(r'\b'+kind+r'(?:\[(\d+):(\d+)\]|(\d+))',t):
                if m[3]:target.add(int(m[3]))
                else:target.update(range(int(m[1]),int(m[2])+1))
        if t.startswith(('s_branch ','s_cbranch_')):
            v=int(t.rsplit(' ',1)[1],0)&0xffff
            if v>=0x8000:v-=0x10000
            require(pc+4+v*4 in pcs,'branch outside decoded shader')
        require(not t.startswith(('s_setpc','s_swappc','s_call','s_rfe','s_getpc','s_movrel','v_movrel','v_movreld','image_store','image_atomic','buffer_store','global_store','flat_store','scratch_store','ds_write','ds_atomic','ds_add','ds_sub','s_store','s_atomic','s_sendmsg','s_barrier','s_trap','s_endpgm_saved','buffer_atomic','global_atomic','flat_atomic')),'unsupported control or side effect: '+t)
        if not t.startswith('image_'):continue
        m=re.fullmatch(r'(image_sample(?:_lz|_l)?|image_load)\s+(.+) dmask:(0x[0-9a-f]+) dim:(SQ_RSRC_IMG_2D(?:_ARRAY)?|SQ_RSRC_IMG_3D)',t)
        require(m is not None,'unsupported image instruction: '+t)
        op,args,dm,dim=m.groups();args=operands(args);mask=int(dm,16)
        require(len(args)==(3 if op=='image_load' else 4),'unexpected image arguments')
        d,c,r=regs(args[0],'v'),regs(args[1],'v'),regs(args[2],'s');s=regs(args[3],'s') if len(args)==4 else ()
        nc=(2 if dim=='SQ_RSRC_IMG_2D' else 3)+(op=='image_sample_l')
        require(len(d)==mask.bit_count() and len(c)==nc and len(r)==8 and len(s)==(0 if op=='image_load' else 4),'unsupported image register shape')
        reads.append(Read(pc,rows[i+1][0]-pc,op,d,c,r,s,mask,dim))
    require(not any(t.startswith('exp ') and re.search(r'\bparam(?:27|28|29|30|31)\b',t) for _,t in rows),'reserved diagnostic export targets used')
    first=max(used,default=-1)+1;first_s=max(sg_used,default=-1)+1
    require(first+20+max((len(r.dst) for r in reads),default=0)<=256 and first_s+2<=106,'not enough unused registers')
    return code[:end],reads,first,first_s


def u32(x):return pack([x])
def mov(d,s):return u32(0x7e000200|(d<<17)|s)
def literal(d,value):return mov(d,255)+u32(value)
def jump(src,target,opcode=0xbf820000):
    n=(target-src-4)//4
    require((target-src-4)%4==0 and -32768<=n<=32767,'trampoline branch out of range')
    return u32(opcode|(n&0xffff))
def exp(target,vs,done=False):
    require(len(vs)==4,'export register count')
    return pack([0xf800000f|(target<<4)|(0x800 if done else 0),sum(x<<(8*i) for i,x in enumerate(vs))])


def execute(decoder,runner,code,sg,vg,segments,mode,mask,lds,sampler,max_visits=128):
    original,reads,tmp,counter=analyze(decoder,code)
    require(reads,'no supported image reads');require(0<max_visits<=128,'bad dynamic visit bound')
    saved=counter+1
    # The count is uniform and initialized only in unused SGPRs. Save/restore
    # SCC around comparisons. EXEC=0 reads are skipped without touching SCC.
    # Each dynamic visit gets its own lane-specific inputs. Reject exhausted
    # space or a loop that exceeds the bound; never reuse a previous sample.
    fixed=[];trace=[];reg=tmp+20;launch_s=list(sg);launch_s[counter]=launch_s[saved]=0
    restore=u32(0xbf070000|(128<<8)|saved)
    for attempt in range(max_visits+1):
        patched=bytearray(original);launch=list(vg)
        require(len(launch)==256*64,'expected full VGPR launch file')
        for r in reads:
            start=len(patched);patched[r.pc:r.pc+r.size]=jump(r.pc,start)+u32(0xbf800000)*(r.size//4-1)
            patched+=jump(len(patched),r.pc+r.size,0xbf880000)
            patched+=u32(0x85000000|(saved<<16)|(128<<8)|129) # s_cselect saved,1,0
            patched+=u32(0x80000000|(counter<<16)|(129<<8)|counter)
            cases=[]
            for visit,pc,first,vals in fixed:
                if pc!=r.pc:continue
                patched+=u32(0xbf060000|(255<<8)|counter)+u32(visit)
                cases.append((len(patched),first,vals));patched+=u32(0xbf800000)
            patched+=restore
            patched+=literal(tmp,r.pc)+mov(tmp+1,256+r.coords[0])+mov(tmp+2,256+r.coords[1])+mov(tmp+3,counter)
            patched+=exp(60,[tmp,tmp+1,tmp+2,tmp+3])
            for j in range(2):patched+=mov(tmp+16+j,256+r.coords[j+2]) if j+2<len(r.coords) else literal(tmp+16+j,0)
            patched+=literal(tmp+18,0)+literal(tmp+19,0)+exp(59,list(range(tmp+16,tmp+20)))
            for j,s in enumerate(r.image):patched+=mov(tmp+4+j,s)
            patched+=exp(61,list(range(tmp+4,tmp+8)))+exp(62,list(range(tmp+8,tmp+12)))
            for j in range(4):patched+=mov(tmp+12+j,r.sampler[j]) if r.sampler else literal(tmp+12+j,0)
            patched+=exp(63,list(range(tmp+12,tmp+16)),True)+u32(0xbf810000)
            for loc,first,vals in cases:
                patched[loc:loc+4]=jump(loc,len(patched),0xbf850000);patched+=restore
                for j,d in enumerate(r.dst):
                    for lane,value in vals.items():launch[(first+j)*64+lane]=value[j]
                    patched+=mov(d,256+first+j)
                patched+=jump(len(patched),r.pc+r.size)
        result=run_wave(runner,bytes(patched),launch_s,launch,segments,mode,mask,lds)
        lanes=sorted(l for t,l in result if t==60)
        if not lanes:
            require(all(t<59 for t,l in result),'incomplete trap export')
            return result,trace
        rows={};pcs=set();descs=set();samplers=set();visits=set()
        for l in lanes:
            pc,x,y,visit=result[60,l][2];pcs.add(pc);visits.add(visit)
            descs.add(tuple(result[61,l][2]+result[62,l][2]));samplers.add(tuple(result[63,l][2]));rows[l]=(x,y,*result[59,l][2][:2])
        require(len(pcs)==len(descs)==len(samplers)==len(visits)==1,'ambiguous trap state')
        pc=pcs.pop();visit=visits.pop();r=next((r for r in reads if r.pc==pc),None)
        require(r is not None and visit==len(fixed)+1 and attempt<max_visits,'unexpected/too many dynamic image visits')
        require(reg+len(r.dst)<=256,'dynamic image visit register budget exceeded')
        desc=descs.pop();samp=samplers.pop();rows={l:coords[:len(r.coords)] for l,coords in rows.items()};values=sampler(r,desc,samp,rows)
        require(set(values)==set(rows) and all(len(v)==len(r.dst) for v in values.values()),'missing sampled lanes')
        fixed.append((visit,pc,reg,values));reg+=len(r.dst)
        trace.append({'pc':pc,'visit':visit,'op':r.op,'descriptor':list(desc),'sampler':list(samp),'active_lanes':lanes,'coords':rows})
    raise ValueError('image replay did not terminate')


def floatword(x):return struct.unpack('<f',struct.pack('<I',x))[0]
def floatbits(x):return struct.unpack('<I',struct.pack('<f',x))[0]

class LogicalImages:
    """Exact descriptor keyed, 2D/mip0 RGBA float pixels; no physical decode."""
    def __init__(self,entries,samplers):
        self.entries=entries;self.samplers=samplers;self.reads=[]
    def __call__(self,r,desc,sampler,coords):
        require(desc in self.entries,'missing versioned image descriptor '+str(desc))
        require(r.dim=='SQ_RSRC_IMG_2D' and r.op!='image_sample_l','unsupported 3D, array or explicit LOD logical sampler')
        image=self.entries[desc];h,w,c=image.shape;require(c==4,'invalid logical RGBA image')
        require(r.op!='image_load' or len(sampler)==4 and not any(sampler),'unexpected image_load sampler')
        policy=self.samplers.get(sampler) if r.op!='image_load' else ('load','clamp','clamp')
        require(policy is not None,'unimplemented sampler '+str(sampler))
        filter,clamp_x,clamp_y=policy
        require(filter in ('linear','nearest','load'),'unimplemented filter')
        require(clamp_x in ('wrap','clamp') and clamp_y in ('wrap','clamp'),'unimplemented clamp')
        out={};reads=set();per_lane={}
        def tex(x,y):
            x=x%w if clamp_x=='wrap' else max(0,min(w-1,x))
            y=y%h if clamp_y=='wrap' else max(0,min(h-1,y))
            reads.add((x,y));return image[y,x]
        for l,(a,b) in coords.items():
            reads.clear()
            if filter=='load':
                x=struct.unpack('<i',struct.pack('<I',a))[0];y=struct.unpack('<i',struct.pack('<I',b))[0]
                require(0<=x<w and 0<=y<h,'unsupported out of bounds image load');v=tex(x,y)
            else:
                x=np.float32(floatword(a))*np.float32(w);y=np.float32(floatword(b))*np.float32(h)
                require(math.isfinite(x) and math.isfinite(y),'nonfinite sample coordinate')
                if filter=='nearest':v=tex(math.floor(x),math.floor(y))
                else:
                    x=np.float32(x-.5);y=np.float32(y-.5);ix,iy=math.floor(x),math.floor(y);fx=np.float32(x-ix);fy=np.float32(y-iy)
                    a0,b0,c0,d0=tex(ix,iy),tex(ix+1,iy),tex(ix,iy+1),tex(ix+1,iy+1)
                    v=(a0+(b0-a0)*fx)*(np.float32(1)-fy)+(c0+(d0-c0)*fx)*fy
            out[l]=tuple(floatbits(v[j]) for j in range(4) if r.dmask>>j&1)
            per_lane[l]=sorted(reads)
        self.reads.append({'pc':r.pc,'descriptor':desc,'texels_by_lane':per_lane})
        return out
