#!/usr/bin/env python3
"""Independent POS0 slice of 00d5229e9ce98fd9, using exact forward draw inputs.

This compares admitted vertices, not expected scene content or the entire P8.
It deliberately refuses missing scalar/descriptor bytes and reports unresolved
rounding instead of treating an arbitrary tolerance as a semantic fix.
"""
import argparse, hashlib, json, struct, zipfile
import numpy as np

SHA='e631468e69972826e7b5265347b72c4907d3da3888ab6bbb018e37e6ffb26046'
VS=0x00d5229e9ce98fd9

def require(x,msg):
    if not x:raise ValueError(msg)

def words(b):return np.frombuffer(b,dtype='<u4')
def f32bits(v):return np.asarray(v,dtype='<u4').view('<f4')
def ptr(v):return int(v[0])+(int(v[1])<<32)
def merge(parts):
    out=[]
    for a,b in sorted(parts):
        require(b and a>=0,'empty memory segment')
        if not out or a>out[-1][0]+len(out[-1][1]):out.append((a,b));continue
        p,d=out[-1];o=a-p;n=min(len(b),len(d)-o)
        require(d[o:o+n]==b[:n],'conflicting scalar/native operand')
        out[-1]=(p,d+b[n:])
    def read(a,n):
        for p,b in out:
            if p<=a and a+n<=p+len(b):return b[a-p:a-p+n]
        raise ValueError('missing operand %#x+%d'%(a,n))
    return read

def payloads(z):
    m=json.loads(z.read('p8-reference/export-manifest.json')); entries={x['file']:x for x in m['files']}
    require(len(entries)==len(m['files']),'duplicate manifest entry')
    def get(row):
        require(row.get('valid') and row.get('file') in entries,'missing payload')
        x=entries[row['file']]; b=z.read('p8-reference/'+row['file'])
        require(x['status']=='complete' and len(b)==row['bytes']==x['bytes']==x['copied'] and hashlib.sha256(b).hexdigest()==x['sha256'],'payload identity mismatch')
        return b
    return get

def clip_masks(p,dx):
    x,y,z,w=p.astype(np.float64).T
    planes=[w+x,w-x,w+y,w-y,z if dx else w+z,w-z]
    mask=np.zeros(len(p),dtype=np.uint8)
    for i,v in enumerate(planes):mask|=(v<0).astype(np.uint8)<<i
    return mask

def compute(vertices,read,user,fused):
    # Exact original guest instruction slice: 0040..0110, 0154..0164,
    # 01b0..01c4, 03a0..0414, 0454, 05ac..05c0, 05d8, 05f4..072c.
    def u(a,n=1):return words(read(a,4*n))
    def f(a,n=1):return u(a,n).view('<f4')
    def descriptor(a,stride,fmt):
        d=u(a,4);require(int(d[1])>>16==stride and int(d[3])==fmt,'unsupported position descriptor')
        return ptr((d[0],int(d[1])&65535)),int(d[2])
    p=ptr(user[:2]);top=u(p,4);p32,p34=ptr(top[:2]),ptr(top[2:]);p40=ptr(u(p+16,2))
    pa,pn=descriptor(p32+16,12,304044);ta,tn=descriptor(p40+16,160,20996);require(tn>=1,'missing transform')
    pos=np.frombuffer(read(pa,12*pn),dtype='<f4').reshape(-1,3)
    ids=vertices&np.uint32(0x7fffffff);gen=(vertices>>31)!=0
    local=np.zeros((len(ids),3),dtype=np.float32);offset=np.zeros_like(local)
    require(not np.any(ids[~gen]>=pn),'original vertex outside descriptor');local[~gen]=pos[ids[~gen]]
    def mad(a,b,c):
        a,b,c=map(lambda v:np.asarray(v,dtype=np.float32),(a,b,c))
        return (a.astype(np.float64)*b.astype(np.float64)+c.astype(np.float64)).astype(np.float32) if fused else a*b+c
    if gen.any():
        ga,gn=descriptor(p34+0xc50,12,20996);ia,ni=descriptor(p32,2,45060)
        g=ids[gen];require(int(g.max())<gn,'generated vertex outside descriptor')
        lo,hi=int(g.min()),int(g.max());r=words(read(ga+12*lo,12*(hi-lo+1))).reshape(-1,3)[g-lo]
        stream=np.frombuffer(read(ia,2*ni),dtype='<u2');require(np.all(r[:,0].astype(np.uint64)+2<ni),'generated triangle outside source index stream')
        tri=stream[r[:,0,None]+np.arange(3,dtype=np.uint32)];require(np.all(tri<pn),'source triangle outside position descriptor')
        a,b,c=pos[tri[:,0]],pos[tri[:,1]],pos[tri[:,2]];s=r[:,1];t=r[:,2]
        w1=((s>>24)&255).astype(np.float32)*np.float32(1/128);w2=((s>>16)&255).astype(np.float32)*np.float32(1/128)
        local[gen]=mad(w2[:,None],c-a,mad(w1[:,None],b-a,a))
        k=f32bits([0x374cce67])[0];bias=f32bits([0xbecccccd])[0]
        offset[gen]=mad(np.stack((s&65535,t>>16,t&65535),axis=-1).astype(np.float32),k,bias)
    t=f(ta,12).reshape(3,4);m=f(p34+0xc0,16).reshape(4,4);j=f(p34+0x1c0,2)
    def mat(v,m,o):
        return np.stack([mad(v[:,0],r[0],mad(v[:,1],r[1],mad(v[:,2],r[2],r[3]+o[:,i]))) for i,r in enumerate(m)],axis=-1)
    world=mat(local,t,offset);clip=mat(world,m,np.zeros((len(world),4),dtype=np.float32))
    clip[:,:2]=mad(clip[:,3,None],j,clip[:,:2]);return clip

def compare(expected,alternate,observed,triangle_rows,dx):
    def eq(a,b):return (a==b)|(np.isnan(a)&np.isnan(b))
    m0,m1=eq(expected,observed),eq(alternate,observed)
    robust=~m0&~m1
    def outside(p):return np.bitwise_and.reduce(clip_masks(p,dx)[triangle_rows],axis=1)!=0
    a,b,o=map(outside,(expected,alternate,observed));stable=a==b
    return {'status':'MATCH' if m0.all() or m1.all() else ('ROUNDING_AMBIGUOUS' if not robust.any() else 'DIVERGENCE_CANDIDATE'),
            'vertices':len(observed),'generated_model0_numeric_equal':int(m0.sum()),'model1_numeric_equal':int(m1.sum()),
            'components':int(m0.size),'components_differing_from_both_models':int(robust.sum()),
            'triangles':len(triangle_rows),'trivial_clip_rejection_model0':int(a.sum()),'trivial_clip_rejection_model1':int(b.sum()),'trivial_clip_rejection_native':int(o.sum()),
            'stable_clip_predicate_difference':int((stable&(a!=o)).sum()),
            'scope':'actual admitted invocations and homogeneous clip predicates, not coverage or whole P8'}

def audit(z):
    get=payloads(z);m=json.loads(z.read('p8-reference/export-manifest.json'));obs=next(x for x in m['files'] if x['file']=='observations.jsonl')
    events=[json.loads(l) for l in get({'valid':True,**obs}).splitlines()];results=[]
    for e in events:
        if e.get('kind')!='radiance_draw_pair' or e.get('shader')!=0xab81bfa8ee4e0fee:continue
        ident={k:e[k] for k in ('frame','submit','command','pair')}
        try:
            g=e['geometry'];require(g['valid'] and g['vs']==VS and g['instances']==1 and g['first_instance']==0 and not g['primitive_restart'] and g['topology']==3,'unsupported draw identity')
            code=z.read('ds-lighting-capture/guest-shaders/vs_00d5229e9ce98fd9_probe.bin');require(hashlib.sha256(code).hexdigest()==SHA,'guest shader fingerprint differs')
            raw=words(get(g['index_payload']));require(len(raw)==g['indices'] and len(raw)%3==0,'incomplete triangle index stream')
            inv=(raw+np.uint32(g['base_vertex']&0xffffffff)).astype(np.uint32);vertices,invmap=np.unique(inv,return_inverse=True)
            values=words(get(g['position_payload']));require(g['position_header_words']==5 and g['position_stride_words']==23,'unexpected recorder layout')
            base,count=g['position_window_base'],g['position_window_count'];require(list(map(int,values[:5]))==[1,base[0],count[0],base[1],count[1]] and sum(count)<=65536 and len(values)==5+23*sum(count),'recorder identity mismatch')
            bank=vertices>>31;off=vertices.astype(np.int64)-np.asarray(base,dtype=np.int64)[bank];require(np.all(off>=0) and np.all(off<np.asarray(count)[bank]),'vertex outside recorder window')
            rows=values[5:].reshape(-1,23)[off+np.where(bank!=0,count[0],0)];require(np.all(rows[:,0]==1) and np.all(rows[:,1]==0),'missing native invocations')
            s=g['vs_inputs'];require(s['shader']==VS and s['scalar_capture_complete'] and s['user_data_base']==8 and len(s['user_data'])>=2,'incomplete scalar ABI')
            parts=[(r['address'],struct.pack('<'+'I'*len(r['words']),*r['words'])) for r in s['scalar_regions']]
            for r in s['buffers']:
                if r.get('valid') and not r.get('not_accessed'):parts.append((r['address']+r['slice_offset'],get(r)))
            read=merge(parts);expected=compute(vertices,read,s['user_data'],False);alt=compute(vertices,read,s['user_data'],True)
            results.append({**ident,**compare(expected,alt,rows[:,3:7].copy().view('<f4'),invmap.reshape(-1,3),g['dx_clip_space']),'generated_vertices':int((vertices>>31).sum())})
        except (KeyError,ValueError,TypeError,IndexError,struct.error) as err:results.append({**ident,'status':'INCOMPLETE','reason':str(err)})
    require(results,'no direct forward pass in capture');return results

def selftest():
    # Two exact scalar tables, original and generated vertex records, identity
    # transforms, binary-exact half barycentric weights. Displacement is
    # independently calculated from the immediate guest constants.
    p,p32,p34,p40,pa,ta,ga,ia=0x1000,0x2000,0x3000,0x5000,0x6000,0x7000,0x8000,0x9000
    def pack(*v):return struct.pack('<'+'I'*len(v),*v)
    def d(a,stride,n,fmt):return pack(a,stride<<16,n,fmt)
    t=np.zeros((3,4),'<f4');t[:,:3]=np.eye(3,dtype=np.float32);m=np.eye(4,dtype='<f4');
    a=bytearray(0xd00);a[0xc0:0x100]=m.tobytes();a[0xc50:0xc60]=d(ga,12,1,20996)
    b=bytearray(0x30);b[:16]=pack(p32,0,p34,0);b[16:24]=pack(p40,0)
    read=merge([(p,bytes(b)),(p32,d(ia,2,3,45060)+d(pa,12,3,304044)),(p34,bytes(a)),(p40+16,d(ta,160,1,20996)),(pa,np.array([[0,0,0],[2,0,0],[0,2,0]],'<f4').tobytes()),(ta,t.tobytes()),(ga,pack(0,64<<24|32<<16,0)),(ia,struct.pack('<3H',0,1,2))])
    out=compute(np.array([0,0x80000000],dtype='<u4'),read,[p,0],False);require(np.allclose(out,[[0,0,0,1],[.6,.1,-.4,1]]),'fixture calculation')
    t=np.array([[0,0,0],[1,1,1]]);require(compare(out,out,out,t,True)['status']=='MATCH','match fixture')
    bad=out.copy();bad[0,0]=2;require(compare(out,out,bad,t,True)['stable_clip_predicate_difference']==1,'clip divergence fixture')
    print('forward position selftest: mixed indices, source operands, match and clip divergence passed')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('zip',nargs='?');p.add_argument('--selftest',action='store_true');a=p.parse_args()
    if a.selftest:selftest();return
    with zipfile.ZipFile(a.zip) as z:print(json.dumps(audit(z),indent=2))
if __name__=='__main__':main()
