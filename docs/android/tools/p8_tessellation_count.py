#!/usr/bin/env python3
"""Independent count slice for the retained 7d5a guest ISA, not full P8."""
import argparse, hashlib, json, struct, zipfile
import numpy as np

PREFIX='ds-lighting-capture/'
CODE='4f33500927c2674ac7afb46da35f23420ff2696997c39e071be52a76cafa93b3'

def req(x,msg):
    if not x: raise ValueError(msg)

def fnv(data):
    h=14695981039346656037
    for b in data: h=((h^b)*1099511628211)&0xffffffffffffffff
    return h

def payload(z,e):
    a=e['operand_blob']; i=e['inputs']; b=z.read(PREFIX+a['artifact_name'])
    req(a['status']=='retained' and len(b)==a['bytes']==i['operand_blob_bytes'] and fnv(b)==a['fnv1a64']==i['operand_blob_fnv1a64'],'invalid producer payload')
    out=[]
    for x in i['bindings']:
        if not (x['read'] or x['atomic']):continue
        n,o=x['bytes'],x['offset_bytes']
        req(x['capture_status']=='complete' and x['captured_bytes']==n and 0<=o<=len(b)-n,'incomplete buffer')
        out.append((x['address'],b[o:o+n]))
    for x in i['scalar_regions']:
        req(x['source']=='consumed_srt' and x['bytes']==4*len(x['words']),'incomplete scalar region')
        out.append((x['address'],struct.pack('<'+'I'*len(x['words']),*x['words'])))
    merged=[]
    for a,d in sorted(out):
        if not merged or a>merged[-1][0]+len(merged[-1][1]):merged.append((a,d));continue
        p,v=merged[-1]; off=a-p; n=min(len(d),len(v)-off)
        req(v[off:off+n]==d[:n],'conflicting captured memory')
        merged[-1]=(p,v+d[n:])
    def read(a,n):
        for p,d in merged:
            if p<=a and a+n<=p+len(d):return d[a-p:a-p+n]
        raise ValueError('missing operand %#x+%d'%(a,n))
    return b,read

def u(read,a,n=1):return struct.unpack('<'+'I'*n,read(a,4*n))
def f(read,a,n=1):return np.frombuffer(read(a,4*n),dtype='<f4')
def ptr(w):return int(w[0])+(int(w[1])<<32)
def desc(read,a,stride,word3):
    w=u(read,a,4); req(w[1]>>16==stride and w[3]==word3,'unexpected descriptor')
    return ptr((w[0],w[1]&65535)),w[2]

def replay(z,e,nxt):
    i=e['inputs']; req(e['phase']=='after' and e['retained'] and e['same_native_mapping'] and e['target_bytes_comparable'],'unstable target')
    req(i['shader_hex']=='7d5ad40fca3137e2' and hashlib.sha256(z.read(PREFIX+i['guest_cs_file'])).hexdigest()==CODE,'unverified guest code')
    req(i['groups_valid'] and i['wave_size']==64 and i['threads']==[64,1,1] and i['group_pack']==1 and i['groups'][1:]==[1,1] and i['float_mode']==192,'unsupported launch')
    req(not i['uses_dma'] and not i['bda_writes'] and i['lds_size_dwords']==i['scratch_size_dwords']==0 and i['user_data_base']==0 and len(i['user_data'])==2,'unsupported ABI')
    req(all(i[k] for k in ('module_identity_complete','scalar_capture_complete','buffers_complete','bda_operands_complete','gds_operands_complete','image_operands_complete')),'incomplete capture')
    req(i['operand_mapping_epoch_before']==i['operand_mapping_epoch_after'] and i['operand_gpu_dirty_generation_before']==i['operand_gpu_dirty_generation_after'],'unstable operands')
    blob,read=payload(z,e); _,after=payload(z,nxt)
    req(nxt['shader']==0xea2e66ea6ad76baf and nxt['frame']==e['frame'] and nxt['submit']==e['submit'] and nxt['stream_id']==e['stream_id'] and nxt['target_before']['native_buffer']==e['target_after']['native_buffer'] and nxt['target_before']['native_words']==e['target_after']['native_words'],'missing next consumer identity')
    req(len(i['image_operands'])==1,'unexpected HZB count'); im=i['image_operands'][0]
    req(im['status']=='complete' and im['image']['format']==100 and im['view']['base_mip']==0 and im['view']['mip_count']==9 and im['version_after']==im['image']['version'] and len(im['subresources'])==9,'unsupported HZB')
    for j,x in enumerate(im['subresources']):
        o,n=x['offset_bytes'],x['bytes']; req(x['mip']==j and x['texel_bytes']==4 and 0<=o<=len(blob)-n and fnv(blob[o:o+n])==x['fnv1a64'] and not any(blob[o:o+n]),'HZB not uniform zero')
    w=u(read,ptr(i['user_data']),12);p28,p30,p32,p34,p50,p48=[ptr(w[j:j+2]) for j in range(0,12,2)]
    start,count,rid=u(read,p48,3); req(count>0 and count<=i['groups'][0]*64 and start+count<2**31,'dispatch extent')
    ia,ni=desc(read,p28,2,45060);va,nv=desc(read,p28+16,12,304044)
    ta,nt=desc(read,p32+16,160,20996); req(nt>=1 and 3*(start+count)<=ni,'index range')
    fa,nf=desc(read,p48+32,0,805539756); req(nf==4 and read(fa,4)==b'\xff'*4,'nonconstant vertex flags')
    ca,nc=desc(read,p50+0x270,4,81924); req(24*rid+24<=nc and ca+96*rid+24==e['target_before']['address'],'counter identity')
    req(u(read,ca+96*rid+24,15)==tuple(e['target_before']['native_words']) and u(after,ca+96*rid+24,15)==tuple(e['target_after']['native_words']),'counter bytes not bound')
    ix=np.frombuffer(read(ia+6*start,6*count),dtype='<u2').reshape(-1,3);req(int(ix.max())<nv,'vertex index range')
    xyz=np.frombuffer(read(va,12*nv),dtype='<f4').reshape(-1,3)[ix]; transform=f(read,ta,12).reshape(3,4);matrix=f(read,p30+192,16).reshape(4,4)
    d=f(read,p50+0x80,2);eps=f(read,p50+0x88)[0];minw=f(read,p50+0x90)[0];factor=f(read,p50,2);scale=f(read,p48+16)[0];dist=f(read,p50+12,2);maxlevel=u(read,p50+8)[0]
    req(0<minw and 0<=maxlevel<=15 and np.isfinite(xyz).all() and np.isfinite(transform).all() and np.isfinite(matrix).all(),'unsupported numeric domain')
    def calc(fused):
        def mad(x,y,z):return (x.astype(np.float64)*np.float64(y)+z.astype(np.float64)).astype(np.float32) if fused else x*y+z
        def mat(v,m):
            return np.stack([mad(v[...,0],r[0],mad(v[...,1],r[1],mad(v[...,2],r[2],np.full(v.shape[:-1],r[3],dtype=np.float32)))) for r in m],axis=-1)
        pos=mat(xyz,transform); clip=mat(pos,matrix); W=clip[...,3];rcp=1/np.maximum(W,minw);ndc=clip[...,:3]*rcp[...,None];pad=rcp.max(axis=1)[:,None]*np.array([d[0],d[1],eps],dtype=np.float32)
        lo=ndc.min(axis=1)-pad;hi=ndc.max(axis=1)+pad
        keep=(lo.max(axis=1)<=1)&(hi.min(axis=1)>=-1)&(W.max(axis=1)>minw)&(hi[:,2]>=0)
        p=pos[keep];w=W[keep];weight=np.clip(mad(w,factor[0],np.full(w.shape,factor[1],dtype=np.float32)),0,1)
        lv=[]
        for x,y in ((0,1),(1,2),(2,0)):
            delta=p[:,x]-p[:,y]; length=np.sqrt((delta[:,2]*delta[:,2]+delta[:,1]*delta[:,1])+delta[:,0]*delta[:,0]); base=length*scale*np.maximum(weight[:,x],weight[:,y]); mean=np.maximum(minw,(w[:,x]+w[:,y])*np.float32(.5)); lim=np.floor(np.log2(np.maximum(1,dist[0]*base)))+np.frombuffer(struct.pack('<I',0x3f7d70a4),dtype='<f4')[0];adapt=np.log2(np.maximum(1,dist[1]/mean*base));lv.append(np.minimum(lim,adapt))
        level=np.minimum(maxlevel,np.ceil(np.maximum.reduce(lv))).astype(np.int64);level[weight.max(axis=1)<=0]=0;n=1<<level;prod=(n+1)*(n+2)
        return np.flatnonzero(keep),level,[int(len(level)),int((((prod>>2)+7)&~7).sum()),int((prod>>1).sum()),int((3*n*n).sum())]
    a,l,pred=calc(False);b,k,pred2=calc(True);req(np.array_equal(a,b) and np.array_equal(l,k) and pred==pred2,'FMA variants change decision')
    locations=[ca+12,ca+96*rid+16,ca+96*rid+20,ca+96*rid+24]
    observed=[(u(after,x)[0]-u(read,x)[0])&0xffffffff for x in locations]
    return {'scope':'local visibility/tessellation atomic counts, zero-HZB constant flags; not P8','frame':e['frame'],'submit':e['submit'],'seq':e['seq'],'record':rid,'input_triangles':count,'levels':{int(k):int(v) for k,v in zip(*np.unique(l,return_counts=True))},'expected':pred,'observed':observed,'status':'MATCH' if pred==observed else 'DIVERGENCE'}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('zip');p.add_argument('--seq',type=int,required=True);a=p.parse_args()
    with zipfile.ZipFile(a.zip) as z:
        es=[json.loads(x) for x in z.read(PREFIX+'queue-state/ds_prelight_cp_args_events.jsonl').splitlines()];e=next(x for x in es if x['seq']==a.seq);nxt=next(x for x in es if x['seq']>a.seq and x.get('phase')=='after' and x.get('stream_id')==e['stream_id'] and x.get('shader')==0xea2e66ea6ad76baf)
        print(json.dumps(replay(z,e,nxt),indent=2))
if __name__=='__main__':main()
