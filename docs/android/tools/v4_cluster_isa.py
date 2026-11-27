#!/usr/bin/env python3
"""Original GFX10 producer execution for every cluster in the retained dispatch."""
import argparse,json,struct,subprocess,tempfile
from concurrent.futures import ThreadPoolExecutor
from itertools import product
from pathlib import Path
import numpy as np
from p8_paired_replay import Archive,need,digest
from p8_guest_graphics import merge,pack
from v4_cluster_publication import pair,bound,writer_key,WRITER
HASH='6c2e72dfd79a528d48633244ce9387fa460fe27430c01632bb2643d53cca9f0e'

def read(segments,address,size):
    for start,data in segments:
        if start<=address and address+size<=start+len(data):return data[address-start:address-start+size]
    raise ValueError('missing scalar operand')

def execute(runner,code,segments,sg,vg,mode,outputs):
    pc=0x10000
    while any(a<pc+len(code) and pc<a+len(b) for a,b in segments):pc+=0x100000
    with tempfile.TemporaryDirectory(prefix='v4-cluster-') as d:
        root=Path(d);f=root/'code';f.write_bytes(code)
        lines=[f'P8ISA2 rdna1 {pc:x} ffffffffffffffff {mode:x} 64 106 256 1000000 "{f}"',
               ' '.join(f'{v:x}' for v in sg),' '.join(f'{v:x}' for v in vg),str(len(segments))]
        for i,(a,b) in enumerate(segments):
            f=root/f'm{i}';f.write_bytes(b);lines.append(f'{a:x} "{f}"')
        lines += [str(len(outputs))]+[f'{a:x} {n:x}' for a,n in outputs]+['"-"']
        mf=root/'manifest';mf.write_text('\n'.join(lines)+'\n')
        r=subprocess.run([str(Path(runner).resolve()),str(mf)],capture_output=True,text=True,timeout=45)
        need(r.returncode==0,'guest runner: '+r.stderr[-1000:])
        need(sum(x.startswith('P8ISA_RESULT1 ') for x in r.stdout.splitlines())==1,'missing runner result')
        result={}
        for row in r.stdout.splitlines():
            if not row.startswith('MEM '):continue
            _,a,b=row.split();a=int(a,16);need(a not in result,'duplicate output');result[a]=bytes.fromhex(b)
        need(set(result)=={a for a,n in outputs} and all(len(result[a])==n for a,n in outputs),'output census')
        return result

def audit(a,runner,workers=4):
    publication=pair(a);wk=writer_key(publication['writer'])
    e=next(e for e in a.events if e['kind']=='v4_cluster_producer' and writer_key(e['writer'])==wk)
    _,use=a.use[e['use']];stage=use['stages'][0];raw=stage['guest_operands'];launch=use['launch']
    need(launch['threads']==[8,8,1] and launch['wave']==64 and launch['group_pack']==1 and
         launch['group_id']==[True,True,True] and launch['lds_size_dwords']==launch['scratch_size_dwords']==0 and
         raw['scalar_complete'] and len(raw['user_data'])==2,'unsupported producer ABI')
    code=a.z.read('ds-lighting-capture/'+stage['guest_file']);need(digest(code)==HASH,'unrecognized cluster producer')
    parts=[(r['address'],pack(r['words'])) for r in raw['scalar_regions']]
    parts.extend((b['guest'],bound(b,stage,i,a.files,i>=2)) for i,b in enumerate(e['before']))
    seg=merge(parts);root=int.from_bytes(pack(raw['user_data']),'little')
    dims_address=int.from_bytes(read(seg,root+8,8),'little');dims=struct.unpack('<III',read(seg,dims_address,12))
    w,h,z=dims;count=w*h*z;need(0<count<=65536 and use['groups']==[(w+7)//8,(h+7)//8,z],'unexpected producer grid')
    mask,summary=[bound(b,stage,i+2,a.files,True) for i,b in enumerate(e['after'])]
    need(len(mask)==32*count and len(summary)==4*count,'cluster output size')
    ma,su=[b['guest'] for b in e['after']]
    vg=[0]*(256*64)
    for l in range(64):vg[l]=l%8;vg[64+l]=l//8
    def group(g):
        x,y,k=g;sg=[0]*106;sg[:2]=raw['user_data'];sg[2:5]=g
        ids=[(k*h+y*8+l//8)*w+x*8+l%8 for l in range(64) if y*8+l//8<h and x*8+l%8<w]
        outputs=[r for i in ids for r in ((ma+32*i,32),(su+4*i,4))]
        got=execute(runner,code,seg,sg,vg,launch['mode'],outputs)
        return [(i,got[ma+32*i],got[su+4*i]) for i in ids]
    groups=list(product(*(range(n) for n in use['groups'])));seen=set();diff=[]
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for records in pool.map(group,groups):
            for i,m,s in records:
                need(i not in seen,'duplicate cluster');seen.add(i)
                if m!=mask[32*i:32*i+32] or s!=summary[4*i:4*i+4]:
                    diff.append(dict(cluster=i,mask_reference=m.hex(),mask_observed=mask[32*i:32*i+32].hex(),
                                     summary_reference=s.hex(),summary_observed=summary[4*i:4*i+4].hex()))
    need(seen==set(range(count)),'incomplete cluster grid')
    return dict(status='DIVERGENCE_LOCAL' if diff else 'MATCH_LOCAL',workgroups=len(groups),clusters=count,
        mask_words=8*count,summary_words=count,different_clusters=len(diff),first_differences=diff[:16],
        publication=publication['status'],scope='original cluster producer, exact captured native operands; not global P8')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path);p.add_argument('runner',type=Path);args=p.parse_args()
    a=Archive(args.archive)
    try:print(json.dumps(audit(a,args.runner),indent=2))
    finally:a.close()
