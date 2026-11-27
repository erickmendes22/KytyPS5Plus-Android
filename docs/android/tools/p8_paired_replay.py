#!/usr/bin/env python3
"""Independent HZB and flip-linked compositor replay for the actual F capture.

Not a second Kyty SPIR-V execution. The HZB contract is the exact guest
dbecbbe2051b41ba 2x2 minimum; the compositor executes its original GFX10 ISA.
Native image readbacks are same-input operands, NOT guest-root certificates.
The report must not turn an unlinked local result into a global P8 cut.
"""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import struct
import zipfile

import numpy as np
from p8_guest_graphics import merge, pack, run_wave

HZB = 0xdbecbbe2051b41ba
COMPOSITE = 0x3265b01ed00a5c01
FULLSCREEN = 0x004c650d72ecd57c
HASHES = {HZB:'7836188d7f115e1cfa59884cef662549353deacd8047c666161066bb1c46e9cc',
          COMPOSITE:'ccd268f60f85b6d19cd50d73c5efdefd1d8dfe914ebc607c31c8f75df8136ea0',
          FULLSCREEN:'d6f97e0841790c47c5fc1d3cdc0a255b9f748139799741d7f95278d34b5ac913'}
KEYS=('owner','object','native','version','write_order','format')
P='p8-reference/'
G='ds-lighting-capture/'


def need(x, reason):
    if not x: raise ValueError(reason)


def digest(b):
    return hashlib.sha256(b).hexdigest()


def key(x):
    return tuple(x[k] for k in KEYS)


def subkey(x):
    return key(x)+tuple(x[k] for k in ('mip','layer','aspect'))


def bits(f):
    return struct.unpack('<I',struct.pack('<f',f))[0]


def hzb_minimum(data,width,height):
    need(type(width) is int and type(height) is int and width>=2 and height>=2
         and width%2==height%2==0 and len(data)==width*height*4,'invalid 2x2 extent')
    words=np.frombuffer(data,'<u4')
    a=words.view('<f4').reshape(height,width)
    need(np.isfinite(a).all() and not (words>>31).any() and (a<=1).all(),
         'unsupported nonfinite/negative depth')
    need(not ((words>0)&(words<0x00800000)).any(),'ambiguous denormal MODE')
    return np.minimum(np.minimum(a[::2,::2],a[::2,1::2]),
                      np.minimum(a[1::2,::2],a[1::2,1::2])).astype('<f4').tobytes()


class Archive:
    def __init__(self,path):
        self.z=zipfile.ZipFile(path)
        names=self.z.namelist()
        need(len(names)==len(set(names)),'duplicate archive paths')
        manifest=json.loads(self.z.read(P+'export-manifest.json'))
        self.files={}
        for r in manifest['files']:
            n=r['file']
            need(isinstance(n,str) and PurePosixPath(n).name==n and n not in self.files,'unsafe/duplicate manifest path')
            if r['status']!='complete':
                self.files[n]=None;continue
            need(0<=r['bytes']<=128*1024*1024,'oversized P8 file')
            b=self.z.read(P+n)
            need(len(b)==r['bytes']==r['copied'] and digest(b)==r['sha256'],'manifest integrity: '+n)
            self.files[n]=b
        self.events=[json.loads(x) for x in self.files['observations.jsonl'].splitlines()]
        self.status=json.loads(self.files['capture-status.json'])
        self.frame=self.status['target_frame']
        self.use={}
        self.snapshots={}
        for i,e in enumerate(self.events):
            if e['kind']=='cut_use':
                m=e['marker'];need(m not in self.use,'duplicate use marker');self.use[m]=(i,e)
            elif e['kind']=='snapshot' and e.get('valid'):
                self.snapshots.setdefault(json.dumps(e,sort_keys=True),[]).append(i)

    def close(self):
        self.z.close()

    def module(self,stage,shader):
        data=self.z.read(G+f'guest-shaders/{stage}_{shader:016x}_probe.bin')
        need(digest(data)==HASHES[shader],'unexpected guest module hash')
        return data

    def snapshot(self,s,role,earlier=None,later=None):
        need(s.get('valid') and s['role']==role and s['version']==s['version_after']
             and not s['cpu_dirty'] and not s['buffer_modified'],'unusable '+role)
        rows=self.snapshots.get(json.dumps(s,sort_keys=True),[])
        need(any((earlier is None or earlier<i) and (later is None or i<later) for i in rows),
             'missing/out-of-order snapshot '+role)
        b=self.files.get(s['file']);w,h=s['extent']
        need(b is not None and len(b)==s['expected_bytes']==s['captured_bytes']==w*h*s['texel_bytes'],
             'missing/truncated '+role)
        if s.get('observation_reused'):
            orig=[x for x in self.events if x.get('kind')=='snapshot' and x.get('marker')==s['source_observation']]
            need(len(orig)==1 and orig[0]['file']==s['file'] and subkey(orig[0])==subkey(s),
                 'invalid reused payload')
        return b


def descriptor(words):
    need(len(words)>=8 and all(type(v) is int and 0<=v<2**32 for v in words[:8]),'missing guest descriptor')
    n=int.from_bytes(pack(words[:8]),'little')
    f=lambda o,w:(n>>o)&((1<<w)-1)
    return {'raw':n,'base':f(0,40)<<8,'format':f(52,9),'width':f(62,16)+1,'height':f(78,16)+1,
            'select':f(96,12),'base_mip':f(108,4),'last_mip':f(112,4),'tile':f(116,5),
            'type':f(124,4),'base_array':f(144,16),'array_pitch':f(160,4),'max_mip':f(164,4),
            'linked':f(184,1),'compressed':f(212,2)}


def bound_descriptor(words,entry,snap,fmt):
    d=descriptor(words)
    need(d['type']==9 and d['format']==fmt and not d['linked'] and not d['base_array']
         and not d['array_pitch'],'unsupported image descriptor')
    need(d['base'] in (entry['guest'],entry['image']['owner']),'descriptor/guest owner mismatch')
    need(d['base_mip']==d['last_mip']==snap['mip']
         and [max(1,d['width']>>snap['mip']),max(1,d['height']>>snap['mip'])]==snap['extent'],
         'descriptor/view extent mismatch')
    return d


def hzb_pair(a,i,e):
    ui,u=a.use[e['use']]
    need(ui<i and u['frame']==e['frame'] and u['submit']==e['submit'] and e['shader']==HZB,'wrong HZB execution')
    need(u['path']=='compute_direct' and len(u['stages'])==1,'unexpected HZB launch')
    a.module('cs',HZB)
    st=u['stages'][0]; l=u['launch']
    need(st['shader']==HZB and st['valid'] and not st['buffers'] and not st['bda_writes']
         and not st['uses_gds'] and not st['uses_dma'],'HZB resource side effects')
    need(l['threads']==[8,8,1] and l['wave']==64 and l['group_pack']==1 and
         l['lds_size_dwords']==l['scratch_size_dwords']==0 and l['group_id'][:2]==[True,True],
         'unsupported HZB ABI')
    need(len(st['images'])==2 and len(e['outputs'])==1,'wrong HZB resources')
    inp,out=st['images']
    need(inp['slot']==0 and inp['read'] and not inp['written'] and out['slot']==1
         and out['written'] and not out['read'] and e['outputs'][0]['slot']==1
         and inp['access_known'] and out['access_known'] and not inp['placeholder'] and not out['placeholder'],
         'HZB access classification')
    need(len(inp['snapshots'])==1 and e['outputs'][0]['valid'],'missing paired HZB observations')
    s=inp['snapshots'][0];t=e['outputs'][0]['snapshot']
    before=a.snapshot(s,'hzb_input',later=ui)
    after=a.snapshot(t,'hzb_output',earlier=ui,later=i)
    need(key(s)==key(inp['image']) and key(t)[:3]==key(out['image'])[:3]
         and t['version']>=out['image']['version'] and t['write_order']>=out['image']['write_order'],
         'HZB resource version mismatch')
    need(s['aspect'] in (1,2) and s['format'] in (100,126,130) and t['aspect']==1
         and t['format']==100 and s['layer']==t['layer']==0,'unsupported HZB native format')
    raw=st['guest_operands'];ds=raw['image_descriptors']
    need(raw['scalar_complete'] and len(ds)==2,'missing HZB guest descriptors')
    di=bound_descriptor(ds[0],inp,s,22);do=bound_descriptor(ds[1],out,t,22)
    need((di['select']&7)==4 and (do['select']&7)==4,'non-X HZB swizzle')
    w,h=s['extent'];ow,oh=t['extent']
    need([w,h]==[2*ow,2*oh] and u['groups']==[ow//8,oh//8,1] and ow%8==oh%8==0,
         'HZB launch does not cover exact output extent')
    ref=hzb_minimum(before,w,h)
    changed=np.frombuffer(ref,'<u4')!=np.frombuffer(after,'<u4')
    return {'use':e['use'],'frame':e['frame'],'submit':e['submit'],
            'status':'MATCH' if not changed.any() else 'DIVERGENCE_LOCAL',
            'input':dict(zip(KEYS+('mip','layer','aspect'),subkey(s))),
            'output':dict(zip(KEYS+('mip','layer','aspect'),subkey(t))),
            'input_sha256':digest(before),'expected_sha256':digest(ref),'observed_sha256':digest(after),
            'different_texels':int(changed.sum()),'total_texels':ow*oh,
            'scope':'same-input HZB reduction; source and final causality not certified'}


def scalar_segments(raw):
    need(raw['scalar_complete'] and raw['user_data_base']==0,'missing original scalar operands')
    return merge([(r['address'],pack(r['words'])) for r in raw['scalar_regions']])


def read(parts,address,count):
    for a,b in parts:
        if a<=address and address+count<=a+len(b):return b[address-a:address-a+count]
    raise ValueError(f'unretained scalar load {address:#x}+{count}')


def replace_region(parts,address,data):
    out=[]
    old=read(parts,address,len(data))
    need(len(old)==len(data),'scalar region not captured')
    for a,b in parts:
        if a<=address and address+len(data)<=a+len(b):
            off=address-a;b=b[:off]+data+b[off+len(data):]
        out.append((a,b))
    return out


def ptr(data):
    return struct.unpack('<Q',data)[0]


def linearize(words,entry,snap,data,new_base):
    d=bound_descriptor(words,entry,snap,71)
    need(snap['format']==97 and snap['aspect']==1 and snap['mip']==snap['layer']==0
         and d['max_mip']==0 and d['select']==0xfac and not d['compressed'],
         'unsupported RGBA16F view or guest component mapping')
    w,h=snap['extent']
    need(len(data)==w*h*8 and not new_base%256 and new_base<2**48,'invalid image bytes/address')
    # RDNA1 linear 2D rows use 256 byte alignment. Rebase only physical
    # addressing; all sampling/filtering/format controls remain original.
    pitch=(w*8+255)&~255
    rows=np.frombuffer(data,np.uint8).reshape(h,w*8)
    padded=np.zeros((h,pitch),np.uint8);padded[:,:w*8]=rows
    n=(d['raw']&~((1<<40)-1)&~(31<<116))|(new_base>>8)
    return n.to_bytes(32,'little'),padded.tobytes()


def pack10(words):
    need(len(words)==4 and all(0<=x<=0xffff for x in words),'invalid half export')
    v=struct.unpack('<4e',struct.pack('<4H',*words))
    need(all(math.isfinite(f) for f in v),'nonfinite compositor export')
    q=[round(max(0.,min(1.,f))*limit) for f,limit in zip(v,(1023,1023,1023,3))]
    return q[0]|(q[1]<<10)|(q[2]<<20)|(q[3]<<30)


def sample_coordinates(state,w,h):
    need(not any(state[k] for k in ('indexed','indirect','mesh','cull_front','cull_back','depth',
         'stencil','blend','sample_shading','kill','depth_export','sample_mask_export',
         'clip_transform','fp16_overflow')),'unsupported compositor raster state')
    need(state['vs']==FULLSCREEN and state['first_vertex']==state['first_instance']==0
         and state['instances']==1 and (state['rectlist'] and state['indices']==3 or
          state['indices']==4 and state['topology']==4) and state['wave']==64
         and state['perspective_center']==0 and (state['interpolator0']&63)==0
         and state['samples']==1 and state['color_count']==1 and state['color_write']
         and state['target_mask']&15==15 and state['shader_mask']&15==15
         and state['output_shader_mask']&15==15,'unsupported fullscreen export/ABI')
    x,y,vw,vh,_,_=state['viewport']
    need(state['viewport_count']==state['scissor_count']==1 and x==0 and vw==w and abs(vh)==h
         and y==(h if vh<0 else 0) and state['scissor']==[0,0,w,h],'unsupported viewport/scissor')
    # Four quads on each axis, away from edges. The VS exports
    # POS0=(2*u-1,1-2*v,1,1), PARAM0=(u,v); both triangles are affine.
    coords=[]
    for j in (1,3,5,7):
        for k in (1,3,5,7):
            px=(w*k//8)&~1;py=(h*j//8)&~1
            need(px+1<w and py+1<h,'quad outside framebuffer')
            coords += [(px+dx,py+dy,(px+dx+.5-x)/vw,
                        1-(py+dy+.5-y)/vh) for dy in (0,1) for dx in (0,1)]
    return coords


def run_composite(executable,code,raw,state,images,w,h):
    need(digest(code)==HASHES[COMPOSITE],'wrong original compositor binary')
    parts=scalar_segments(raw);ud=raw['user_data'];need(len(ud)==2,'unexpected PS user-data launch')
    root=ptr(pack(ud));a,b=struct.unpack('<QQ',read(parts,root,16))
    read(parts,a,16);read(parts,a+20,8)
    read(parts,b+0xa0,16);read(parts,b+0xc0,16)
    desc=struct.unpack('<16I',read(parts,a+0x20,64))
    need(len(images)==2,'compositor needs two inputs')
    for slot,(entry,snap,data) in enumerate(images):
        address=0x600000000000+slot*0x100000000
        new,padded=linearize(desc[slot*8:slot*8+8],entry,snap,data,address)
        parts=replace_region(parts,a+0x20+slot*32,new)
        parts=merge([*parts,(address,padded)])
    coords=sample_coordinates(state,w,h)
    lds=pack([0,bits(1.),0,0,0,0,0,0,0,bits(1.),0,0])
    variants=[]
    for poison in (0,0x7fa54321):
        sg=[poison]*106;sg[:3]=[*ud,0]
        vg=[poison]*(64*256)
        for lane,(_,_,u,v) in enumerate(coords):
            vg[lane]=bits(u);vg[64+lane]=bits(v)
        exp=run_wave(executable,code,sg,vg,parts,state['mode'],(1<<len(coords))-1,lds)
        need(len(exp)==len(coords) and all(exp[(0,k)][:2]==(15,1) for k in range(len(coords))),
             'unexpected compositor export')
        variants.append([pack10(exp[(0,k)][2]) for k in range(len(coords))])
    need(variants[0]==variants[1],'compositor depends on unspecified launch register')
    return coords,variants[0]


def composite_pair(a,i,e,exe):
    ui,u=a.use[e['use']]
    need(ui<i and u['frame']==e['frame']==a.frame and u['submit']==e['submit']
         and e['shader']==COMPOSITE,'wrong compositor execution')
    states=[x for j,x in enumerate(a.events) if ui<j<i and x.get('kind')=='composite_state' and x.get('use')==e['use']]
    need(len(states)==1,'missing actual compositor raster state');state=states[0]
    need(len(u['stages'])==2 and u['stages'][0]['shader']==FULLSCREEN
         and u['stages'][1]['shader']==COMPOSITE,'wrong fullscreen pipeline')
    a.module('vs',FULLSCREEN);code=a.module('ps',COMPOSITE)
    st=u['stages'][1]
    need(not st['buffers'] and not st['bda_writes'] and len(st['images'])==2,'unsupported compositor bindings')
    imgs=[]
    for k,entry in enumerate(st['images']):
        role=('compositor_overlay_input','compositor_scene_input')[k]
        need(entry['slot']==k and entry['access_known'] and entry['read'] and not entry['written']
             and len(entry['snapshots'])==1 and not entry['placeholder'],'missing exact compositor input')
        s=entry['snapshots'][0];need(key(s)==key(entry['image']),'compositor input version mismatch')
        imgs.append((entry,s,a.snapshot(s,role,later=ui)))
    need(len(e['outputs'])==1,'ambiguous compositor output')
    out=e['outputs'][0]['full'];native=a.snapshot(out,'compositor',earlier=ui,later=i)
    need(out['format']==64 and out['aspect']==1 and out['texel_bytes']==4
         and len(u['targets'])==1 and key(u['targets'][0]['image'])[:3]==key(out)[:3],
         'compositor target mismatch')
    flips=[x for j,x in enumerate(a.events) if j>i and x.get('kind')=='flip' and
           x.get('frame') in (a.frame,a.frame+1) and key(x['image'])==key(out)]
    need(flips,'output is not the flip version')
    w,h=out['extent'];coords,pred=run_composite(exe,code,st['guest_operands'],state,imgs,w,h)
    actual=[struct.unpack_from('<I',native,4*(y*w+x))[0] for x,y,_,_ in coords]
    def maxdelta(p,q):return max(abs(((p>>k)&1023)-((q>>k)&1023)) for k in (0,10,20))
    samples=[{'x':x,'y':y,'expected':f'{p:08x}','observed':f'{q:08x}','rgb_lsb_delta':maxdelta(p,q)}
             for (x,y,_,_),p,q in zip(coords,pred,actual)]
    return {'use':e['use'],'status':'MATCH_EXACT' if pred==actual else
            'MATCH_WITHIN_1_LSB' if all(maxdelta(p,q)<=1 for p,q in zip(pred,actual)) else 'DIVERGENCE_LOCAL',
            'frame':a.frame,'submit':u['submit'],'samples':samples,'native_flip_sha256':digest(native),
            'scope':'original guest compositor on identical logical native input texels; not prefix authority or a P8 cut'}


def audit(path,exe):
    a=Archive(path)
    try:
        result={'frame':a.frame,'hzb':[],'compositor':[],'p8':'BLOCKED',
                'interval':[0,16],'scope':'local same-input comparisons, actual P8 requires a closed causal propagation path'}
        for i,e in enumerate(a.events):
            if e['kind'] not in ('hzb_after','composite_after'):continue
            dest='hzb' if e['kind']=='hzb_after' else 'compositor'
            try:row=hzb_pair(a,i,e) if dest=='hzb' else composite_pair(a,i,e,exe)
            except (ValueError,KeyError,TypeError,OSError,IndexError) as err:
                row={'use':e.get('use'),'status':'INCOMPLETE','reason':str(err)}
            result[dest].append(row)
        result['acquisition_gaps']=[name for name in ('hzb','compositor') if not result[name]]
        result['local_counterexamples']=[x for name in ('hzb','compositor') for x in result[name] if x['status']=='DIVERGENCE_LOCAL']
        return result
    finally:a.close()


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path)
    p.add_argument('runner',type=Path);p.add_argument('--output',type=Path);a=p.parse_args()
    r=audit(a.archive,a.runner);s=json.dumps(r,indent=2)+'\n'
    if a.output:a.output.write_text(s)
    print(s,end='')


if __name__=='__main__':main()
