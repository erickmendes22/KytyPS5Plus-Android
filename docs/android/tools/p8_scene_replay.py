#!/usr/bin/env python3
"""Version-linked execution of the real scene producers and compositor.

Local replay never turns a native intermediate into an independent root.
The result names missing provenance and only reports a global cut when the
bilateral decision engine has independently verified the corresponding cone.
"""
import argparse
import json
import math
import struct
from pathlib import Path
import numpy as np
from p8_guest_graphics import merge,pack
from p8_image_trap import execute, floatbits
from p8_scene_textures import Texture,Sampler,VersionedTextures,decode_pixels
from prelight_vertex_oracle import LLVM
from p8_compositor_isa import Replay
import p8_paired_replay as p

POST=0xa5fc1658592e3cba
OVERLAY=0xb4e1b94988dff9ae
VS=0xfe75262ac7cb3aa4
SCENE=0x2b2a40000
HASHES={POST:'95408b30263c04b032bc4b951a4bdba832416883972c87eb8b8c71330e3c71a1',OVERLAY:'ec45fb596ae3111328e08b4eaa5f126e6e0345059429295376084050ad1530db',VS:'e9133b777514763fc7049a226ffbf81387d15a51ef2845c677bdf9307ac94313'}

def shader(a,stage,h):
    code=a.z.read(p.G+f'guest-shaders/{stage}_{h:016x}_probe.bin')
    p.need(p.digest(code)==HASHES[h],'wrong scene guest shader');return code

def snapshot(a,s,role,earlier=None,later=None):
    p.need(s.get('valid') and s['role']==role and s['version']==s['version_after'] and not s['cpu_dirty'] and not s['buffer_modified'],'unusable '+role)
    rows=a.snapshots.get(json.dumps(s,sort_keys=True),[])
    p.need(any((earlier is None or earlier<i) and (later is None or i<later) for i in rows),'missing or out-of-order '+role)
    w,h=s['extent'];d=s.get('depth',1);b=s.get('block_extent',1);data=a.files.get(s['file'])
    p.need(b in (1,4) and all(type(v) is int and v>0 for v in (w,h,d)),'bad subresource layout')
    p.need(data is not None and len(data)==s['captured_bytes']==s['expected_bytes']==((w+b-1)//b)*((h+b-1)//b)*d*s['texel_bytes'],'truncated '+role)
    if s.get('observation_reused'):
        prior=[e for e in a.events if e.get('marker')==s['source_observation'] and e.get('kind')=='snapshot']
        p.need(len(prior)==1 and p.subkey(prior[0])==p.subkey(s) and prior[0]['file']==s['file'] and prior[0].get('depth',1)==d and prior[0].get('block_extent',1)==b,'bad reuse')
    return data

def sampler(words):
    a,b,c,d=words
    p.need(((a>>29)&3)==0 and not (a&(1<<20)),'unsupported reduction/degamma')
    modes={0:'wrap',1:'mirror',2:'clamp',4:'border',6:'border'}
    clamps=tuple((a>>(3*i))&7 for i in range(3));p.need(all(x in modes for x in clamps),'unsupported sampler clamp')
    mag=(c>>20)&3;mn=(c>>22)&3;mip=(c>>26)&3
    p.need(mip<3,'reserved mip filter');p.need((mag<2 and mn<2) or (a>>9)&7==0,'anisotropic footprint not certified')
    border=(d>>30)&3;p.need(border<3,'table border unsupported')
    minlod=(b&4095)/256;maxlod=((b>>12)&4095)/256;bias=((c&16383)^8192)-8192
    norm=not bool(a&32768)
    return Sampler('linear' if mag&1 else 'nearest','linear' if mn&1 else 'nearest',('none','nearest','linear')[mip],tuple(modes[x] for x in clamps),min(minlod,maxlod) if mip else 0,maxlod if mip else 0,bias/256 if mip else 0,((0.,0.,0.,0.),(0.,0.,0.,1.),(1.,1.,1.,1.))[border],norm,True)

class Inputs:
    def __init__(self,a,stage,ui,extra=()):
        raw=stage['guest_operands'];p.need(raw['scalar_complete'],'incomplete guest scalar operands')
        self.ud=raw['user_data'];self.base=raw['user_data_base'];self.parts=merge([(r['address'],pack(r['words'])) for r in raw['scalar_regions']]+list(extra));self.textures={};self.samplers={};self.identities={}
        self.bad_samplers={}
        for words in raw['sampler_descriptors']:
            k=tuple(words[:4])
            try:self.samplers[k]=sampler(k)
            except ValueError as e:self.bad_samplers[k]=str(e)
        for words in raw['image_descriptors']:
            k=tuple(words[:8]);d=p.descriptor(k)
            dim={9:'SQ_RSRC_IMG_2D',10:'SQ_RSRC_IMG_3D',13:'SQ_RSRC_IMG_2D_ARRAY'}.get(d['type'])
            if not dim or d['linked']:continue
            entries=[e for e in stage['images'] if e.get('read') and e.get('access_known') and not e.get('written') and not e.get('placeholder') and 'view' in e and d['base'] in (e['guest'],e['image']['owner'])]
            for e in entries:
                v=e['view']
                if d['base_mip']!=v['mip'] or d['last_mip']+1!=v['mip']+v['levels'] or d['base_array']!=v['layer']:continue
                levels={};ids={}
                for s in e['snapshots']:
                    if s['role']!='scene_input' or not s.get('valid') or s['aspect']!=v['aspect']:continue
                    p.need(p.key(s)==p.key(e['image']),'scene input version changed')
                    p.need(s['format']==v['format'] or s['aspect']==2 and s['format']==130,'unverified reinterpretation')
                    m,l=s['mip'],s['layer'];w,h=s['extent'];depth=s.get('depth',1)
                    p.need([w,h]==[max(1,d['width']>>m),max(1,d['height']>>m)],'scene descriptor extent mismatch')
                    b=snapshot(a,s,'scene_input',later=ui);levels[m,l]=decode_pixels(b,s['format'],w,h,depth);ids[m,l]=p.subkey(s)
                if not levels:continue
                # A 3D base depth is available at mip0, not derived from an
                # arbitrary downsampled layer count.
                depth=levels[0,v['layer']].shape[0] if dim=='SQ_RSRC_IMG_3D' and (0,v['layer']) in levels else 1
                if dim=='SQ_RSRC_IMG_3D' and (0,v['layer']) not in levels:continue
                t=Texture(levels,d['base_mip'],d['last_mip'],v['layer'],v['layers'],dim,tuple((d['select']>>(3*j))&7 for j in range(4)),(d['width'],d['height'],depth))
                if k in self.textures:p.need(self.identities[k]==ids,'ambiguous descriptor version')
                self.textures[k]=t;self.identities[k]=ids
    def logical(self):return VersionedTextures(self.textures,self.samplers)

def post_pixels(dec,runner,code,inputs,state,w,h,points):
    p.need(p.digest(code)==HASHES[POST] and inputs.base==0 and len(inputs.ud)==2,'post shader/ABI mismatch')
    p.sample_coordinates(state,w,h);p.need(state['output_mode'] in (0,4,5,6,7,8,9),'unknown post export mode')
    # Position VGPRs are validated against the captured interpolation ABI;
    # raw pixel center is necessary for the original integer image_load.
    p.need(state.get('position_xy_vgprs')==[2,3],'unverified fragment position ABI')
    quads=sorted({(x&~1,y&~1) for x,y in points});out={};reads=[]
    for off in range(0,len(quads),16):
        coords=[(x+i,y+j) for x,y in quads[off:off+16] for j in (0,1) for i in (0,1)]
        versions=[]
        for poison in (0,0x7fa54321):
            sg=[poison]*106;sg[:3]=[*inputs.ud,0];vg=[poison]*16384;logical=inputs.logical()
            for lane,(x,y) in enumerate(coords):
                vx,vy,vw,vh,_,_=state['viewport']
                for k,v in enumerate(((x+.5-vx)/vw,1-(y+.5-vy)/vh,x+.5,y+.5)):vg[k*64+lane]=floatbits(v)
            lds=pack([0,floatbits(1.),0,0,0,0,0,0,0,floatbits(1.),0,0])
            result,trace=execute(dec,runner,code,sg,vg,inputs.parts,state['mode'],(1<<len(coords))-1,lds,logical)
            p.need(set(result)=={(0,k) for k in range(len(coords))} and all(r[:2]==(15,1) for r in result.values()),'missing exact post MRT export')
            versions.append((result,logical.reads))
        p.need(versions[0]==versions[1],'post depends on unknown registers')
        for lane,xy in enumerate(coords):out[xy]=struct.pack('<4H',*versions[0][0][0,lane][2])
        reads.extend(versions[0][1])
    return out,reads

def paired(a,h):
    pairs=[(i,e) for i,e in enumerate(a.events) if e.get('kind')=='scene_after' and e.get('shader')==h and e.get('frame')==a.frame]
    p.need(len(pairs)==1,'missing or ambiguous scene pair '+hex(h));i,e=pairs[0];ui,u=a.use[e['use']]
    p.need(ui<i and u['frame']==e['frame'] and u['submit']==e['submit'],'scene pair order')
    states=[x for x in a.events[ui+1:i] if x.get('kind')=='scene_state' and x.get('use')==e['use']]
    p.need(len(states)==1,'missing scene raster state');s=states[0]
    out=[r['full'] for r in e['outputs'] if r['full']['owner']==SCENE];before=[r['full'] for r in e['before'] if r['full']['owner']==SCENE]
    p.need(len(out)==len(before)==1,'ambiguous scene attachment')
    ob=snapshot(a,out[0],'scene_after',earlier=ui,later=i);ib=snapshot(a,before[0],'scene_before',later=ui)
    p.need(out[0]['format']==before[0]['format']==97 and out[0]['extent']==before[0]['extent'] and p.key(out[0])[:3]==p.key(before[0])[:3],'scene attachment version identity')
    return ui,u,s,e,before[0],ib,out[0],ob

def buffer_parts(a,rows,stage):
    p.need(len(rows)==len(stage['buffers']),'missing original buffer binding')
    result=[]
    for r,b in zip(rows,stage['buffers']):
        p.need(r['valid'] and b['access_known'] and not b['written'] and r['slot']==b['slot'] and r['address']==b['guest'] and r['bytes']==b['bytes'] and r['native']==b['native'] and r['native_range']==b['native_range'],'buffer binding identity')
        data=a.files.get(r['file']);p.need(data is not None and len(data)==r['bytes'],'truncated vertex buffer');result.append((r['address'],data))
    return result

def overlay_vertices(dec,runner,code,inputs,state,ids,instances):
    p.need(p.digest(code)==HASHES[VS] and inputs.base==8 and 0<len(inputs.ud)<=98,'overlay VS ABI')
    p.need(state.get('vs_mode') is not None,'missing VS arithmetic mode')
    allv=[(v,i) for i in instances for v in ids];p.need(len(allv)<=200000,'overlay vertex budget')
    out={}
    for off in range(0,len(allv),64):
        batch=allv[off:off+64];versions=[]
        for poison in (0,0x7fa54321):
            sg=[poison]*106;sg[8:8+len(inputs.ud)]=inputs.ud;vg=[poison]*16384
            for l,(v,i) in enumerate(batch):vg[5*64+l]=v;vg[8*64+l]=i
            exports,_=execute(dec,runner,code[0x40:],sg,vg,inputs.parts,state['vs_mode'],(1<<len(batch))-1,None,inputs.logical())
            versions.append(exports)
        p.need(versions[0]==versions[1],'overlay VS depends on unknown launch state')
        for l,vi in enumerate(batch):
            r=versions[0].get((12,l));p.need(r and r[:2]==(15,0),'missing overlay POS0')
            out[vi]=np.frombuffer(pack(r[2]),'<f4').astype(np.float64)
    return out

def excluded(points,positions,indices,instances,state):
    # Conservative coverage proof: outside every primitive's expanded bounding
    # rectangle; clipped primitives are included conservatively, never discarded
    # by an assumed depth test. Any possible overlap remains unresolved.
    p.need(state['topology']==3 and not state['primitive_restart'] and not state['clip_transform'] and state['viewport_count']==state['scissor_count']==1,'unsupported primitive assembly or clip transform')
    p.need(len(indices)%3==0,'incomplete triangle list');x,y,w,h,*_=state['viewport'];boxes=[]
    for i in instances:
        for j in range(0,len(indices),3):
            v=np.array([positions[(k,i)] for k in indices[j:j+3]])
            p.need(np.isfinite(v).all() and np.all(v[:,3]>0),'unresolved clipped triangle')
            q=v[:,:2]/v[:,3:4];q=q*np.array([w,h])/2+np.array([x+w/2,y+h/2]);boxes.append((q.min(0)-1,q.max(0)+1))
    lo=np.array([b[0] for b in boxes]);hi=np.array([b[1] for b in boxes]);p.need(len(boxes)>0,'empty primitive set')
    return {(x,y):not bool(np.any(np.all((lo<=np.array([x+.5,y+.5]))&(hi>=np.array([x+.5,y+.5])),axis=1))) for x,y in points}

def audit(path,runner):
    a=p.Archive(path);dec=LLVM();result={'p8':'BLOCKED','scope':'actual finite scene/compositor replay; no independent prefix authority'}
    try:
        r=Replay(a);x,y=(r.w//2)&~1,(r.h//2)&~1;quad=r.quad(x,y);base,reads,_=r.execute(dec,runner,quad)
        p.need(base[0]==int(r.actual[y,x]),'compositor baseline mismatch')
        points=set(t for row in reads if row['descriptor']==r.slots[1] for ts in row['texels_by_lane'].values() for t in ts)
        p.need(points,'empty scene footprint');result['flip_witness']=[x,y];result['scene_support']=sorted(points)
        ui,u,s,e,bi,ib,ao,ab=paired(a,POST);vj,v,vs,ve,bo,bb,out,ob=paired(a,OVERLAY)
        p.need(ui<vj and p.subkey(ao)==p.subkey(bo) and ab==bb and p.subkey(out)==tuple(r.identities[r.slots[1]][k] for k in p.KEYS+('mip','layer','aspect')) and ob==r.payloads[1],'scene producer/consumer link mismatch')
        w,h=ao['extent'];p.need(len(u['stages'])==2 and u['stages'][1]['shader']==POST and not u['stages'][1]['buffers'],'unsupported post bindings')
        a.module('vs',p.FULLSCREEN);inputs=Inputs(a,u['stages'][1],ui)
        ref,reads=post_pixels(dec,runner,shader(a,'ps',POST),inputs,s,w,h,points)
        native=lambda data,x,y:data[8*(y*w+x):8*(y*w+x)+8]
        result['post']={'match':sum(ref[z]==native(ab,*z) for z in points),'total':len(points),'mismatch':[list(z) for z in sorted(points) if ref[z]!=native(ab,*z)],'support':[{"descriptor":list(q['descriptor']),"pc":q['pc'],"subresources":q['subresources_by_lane']} for q in reads]}
        g=v['scene_geometry'];p.need(g['valid'] and g['indexed'] and not g['indirect'] and not g['mesh'] and len(v['stages'])==2 and v['stages'][0]['shader']==VS and v['stages'][1]['shader']==OVERLAY,'missing complete overlay geometry')
        idx=a.files.get(g['index_payload']['file']);p.need(idx is not None and len(idx)==4*g['indices'],'missing index payload')
        indices=[(int(t)+g['base_vertex'])&0xffffffff for t in np.frombuffer(idx,'<u4')];ids=sorted(set(indices));inst=range(g['first_instance'],g['first_instance']+g['instances'])
        vi=Inputs(a,v['stages'][0],vj,buffer_parts(a,g['vs_buffers'],v['stages'][0]));positions=overlay_vertices(dec,runner,shader(a,'vs',VS),vi,vs,ids,inst)
        outside=excluded(points,positions,indices,inst,vs);result['overlay_coverage']={'excluded':[list(z) for z in sorted(points) if outside[z]],'possible':[list(z) for z in sorted(points) if not outside[z]]}
        p.need(all(outside.values()),'possible overlay coverage requires fragment/depth/blend replay')
        p.need(not vs['load_clear'] and all(native(ab,*z)==native(ob,*z) for z in points),'overlay untouched baseline mismatch')
        changed={k:v.copy() for k,v in r.images.items()};scene=changed[r.slots[1]]
        for (px,py),word in ref.items():
            if (px,py) in points:scene[py,px]=np.frombuffer(word,'<f2').astype(np.float32)
        pred=r.execute(dec,runner,quad,changed)[0]
        result['local_propagation']={'observed':f'{base[0]:08x}','replayed':f'{pred[0]:08x}','changed':base[0]!=pred[0]}
        result['reason']='complete local suffix for this footprint; C1-C8 authority and prior HDR producers still required'
    except (ValueError,KeyError,TypeError,OSError,IndexError) as ex:result['reason']=str(ex)
    finally:dec.close();a.close()
    return result

def main():
    cli=argparse.ArgumentParser(description=__doc__);cli.add_argument('archive',type=Path);cli.add_argument('runner',type=Path);args=cli.parse_args();print(json.dumps(audit(args.archive,args.runner),indent=2))
if __name__=='__main__':main()
