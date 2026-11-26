#!/usr/bin/env python3
"""Reconstruct the retained one-wave CP producer from #746, without native output as input.

This is a local backend calibration, not an authority for the P8 cut.
"""
import argparse
import hashlib
import itertools
import json
import pathlib
import struct
import zipfile

def require(value,message):
    if not value: raise ValueError(message)

def fnv1a(data):
    h=14695981039346656037
    for b in data: h=((h^b)*1099511628211)&((1<<64)-1)
    return h

def merge_segments(segments):
    result=[]
    for addr,data in sorted(segments):
        require(data and 0<=addr<(1<<64)-len(data),'invalid memory segment')
        if not result or addr>result[-1][0]+len(result[-1][1]):
            result.append((addr,data));continue
        a,b=result[-1];off=addr-a;overlap=min(len(data),len(b)-off)
        require(b[off:off+overlap]==data[:overlap],f'conflicting capture bytes at {addr:#x}')
        result[-1]=(a,b+data[overlap:])
    return result

def read_range(segments,addr,size):
    for a,data in segments:
        if a<=addr and addr+size<=a+len(data): return data[addr-a:addr-a+size]
    raise ValueError(f'missing data at {addr:#x}/{size}')

def pack(words): return struct.pack('<'+'I'*len(words),*words)

def check_compaction(segments,user_data,observations):
    # Independent mathematical semantics of the integer-only guest instruction
    # sequence ea2e66ea6ad76baf at 0x0004..0x01b0. Do not import Kyty code.
    # Atomic return order is not specified; enumerate all possible 3-lane orders.
    def read(addr,n=1): return struct.unpack('<'+'I'*n,read_range(segments,addr,n*4))
    def ptr(lo,hi): return lo+(hi<<32)
    def base(desc):
        require(desc[1]>>16==4 and desc[3]==81924,'unexpected buffer descriptor')
        return ptr(desc[0],desc[1]&0xffff)
    p=read(ptr(*user_data),4)
    src_table,dst_table=ptr(*p[:2]),ptr(*p[2:])
    count=read(src_table)[0]
    require(0<count<=3,'unsupported calibration lane count')
    dst=base(read(dst_table+0x270,4));src=base(read(src_table+0x10,4))
    ids=read(src,count)
    require(len(set(ids))==count and all(i>0 for i in ids),'overlapping record IDs')
    limits=read(dst_table+0x1c,3)
    records={i:list(read(dst+96*i,24)) for i in ids}
    initial=read(dst+16,3)
    all_results=[];matching=[]
    for order in itertools.permutations(ids):
        current={i:v.copy() for i,v in records.items()};counter=list(initial)
        for i in order:
            rec=current[i];n,m,k=rec[4:7];old=counter.copy()
            counter=[(v+x)&0xffffffff for v,x in zip(counter,(n>>3,m,k))]
            if any(x>limit for x,limit in zip(counter,limits)):
                rec[4]=rec[6]=0
            else:
                rec[3],rec[5],rec[6],rec[8]=old[0],old[1],k,old[2]
                rec[0]=max(1,((n+63)&0xffffffff)>>6)
        predicted=[]
        for o in observations:
            i,offset=divmod(o['address']-dst,96)
            require(i in current and offset%4==0 and offset+len(bytes.fromhex(o['before']))<=96,'witness outside record')
            predicted.append(pack(current[i])[offset:offset+len(bytes.fromhex(o['before']))].hex())
        all_results.append({'order':order,'predicted':predicted})
        if predicted==[o['observed_after'] for o in observations]:matching.append(order)
    # A match shows a legal common order explains these bytes. A mismatch is
    # not a proof of divergence: each atomic can have a different lane order.
    return {'lane_ids':ids,'matching_common_atomic_orders':matching,'candidate_orders':all_results,
            'scope':'observed record bytes, common atomic lane order, not a global P8 decision'}

def prepare(archive,out,seq):
    out.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        prefix='ds-lighting-capture/'
        events=[json.loads(x) for x in z.read(prefix+'queue-state/ds_prelight_cp_args_events.jsonl').splitlines()]
        event=next(x for x in events if x.get('seq')==seq)
        require(event.get('retained') and event.get('phase')=='after' and event.get('same_native_mapping'),'no retained matching producer')
        i=event['inputs']
        require(i['shader_hex']=='ea2e66ea6ad76baf','unsupported launch ABI')
        require(i['wave_size']==64 and i['threads']==[64,1,1] and i['groups']==[1,1,1] and i['groups_valid'] and not i['indirect'],'not a single wave direct launch')
        require(i['group_pack']==1 and i['scratch_size_dwords']==i['lds_size_dwords']==0 and not i['uses_dma'] and not i['images'] and not i['samplers'] and not i['bda_writes'],'unsupported resource or shared state')
        require(all(i[k] for k in ('module_identity_complete','scalar_capture_complete','buffers_complete','bda_operands_complete','gds_operands_complete','image_operands_complete')),'incomplete capture')
        require(i['operand_mapping_epoch_before']==i['operand_mapping_epoch_after'] and i['operand_gpu_dirty_generation_before']==i['operand_gpu_dirty_generation_after'],'operand identity changed')
        matches=[]
        for n in z.namelist():
            if n.startswith(prefix+'queue-state/ds_prelight_cp_args_') and n.endswith('_compute.bin') and z.getinfo(n).file_size==i['operand_blob_bytes']:
                d=z.read(n)
                if fnv1a(d)==i['operand_blob_fnv1a64']: matches.append(d)
        require(matches and all(d==matches[0] for d in matches),'no unambiguous operand blob')
        blob=matches[0];segments=[]
        for b in i['bindings']:
            require(b['capture_status']=='complete' and b['captured_bytes']==b['bytes'] and b['byte_adjustment']==0,'incomplete binding')
            offset=b['offset_bytes']
            require(0<=offset and offset+b['bytes']<=len(blob),'binding out of operand blob')
            segments.append((b['address'],blob[offset:offset+b['bytes']]))
        for s in i['scalar_regions']:
            require(s['source']=='consumed_srt' and s['bytes']==4*len(s['words']),'scalar input incomplete')
            segments.append((s['address'],pack(s['words'])))
        segments=merge_segments(segments);code=z.read(prefix+i['guest_cs_file'])
        require(hashlib.sha256(code).hexdigest()=='6378731ae866d8f6a0576252b5501c2fe826a3b8cb8bff102064c2bae1425985','guest code differs from audited instruction sequence')
        # Integer-only shader uses s[0:1], s2 (group X) and v0 (local X).
        require(i['user_data_base']==0 and len(i['user_data'])==2,'user SGPR ABI')
        sgpr=i['user_data']+[0]*104;vgpr=list(range(64))+[0]*(63*64)
        (out/'guest.bin').write_bytes(code)
        lines=[f'P8ISA1 rdna1 10000 ffffffffffffffff {i["float_mode"]:x} 64 106 64 1000000 "{out/"guest.bin"}"',' '.join(f'{v:x}' for v in sgpr),' '.join(f'{v:x}' for v in vgpr),str(len(segments))]
        for j,(a,data) in enumerate(segments):
            path=out/f'segment{j}.bin';path.write_bytes(data)
            lines.append(f'{a:x} "{path}"')
        witnesses=[x for x in events if x.get('retained') and x.get('phase')=='after' and x.get('submit')==event['submit'] and x.get('shader')==event['shader'] and x.get('frame')==event['frame']]
        observations=[]
        for e in witnesses:
            a,b=e['target_before'],e['target_after']
            require(e['same_native_mapping'] and e['target_bytes_comparable'] and a['address']==b['address'] and a['bytes']==b['bytes'],'witness not comparable')
            require(read_range(segments,a['address'],a['bytes'])==pack(a['native_words']),'blob does not match observed pre-dispatch bytes')
            observations.append({'seq':e['seq'],'address':a['address'],'before':pack(a['native_words']).hex(),'observed_after':pack(b['native_words']).hex()})
        require(observations,'no output witnesses')
        lines.append(str(len(observations)))
        for o in observations: lines.append(f'{o["address"]:x} {len(bytes.fromhex(o["before"])):x}')
        (out/'manifest.txt').write_text('\n'.join(lines)+'\n')
        result={'frame':event['frame'],'submit':event['submit'],'producer':i['shader_hex'],'scope':'local calibration only; atomic ordering may differ; not P8 decision','operand_digest':i['operand_blob_fnv1a64'],'witnesses':observations,'calibration':check_compaction(segments,i['user_data'],observations)}
        (out/'observed.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result['calibration'],indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('archive',type=pathlib.Path);p.add_argument('output',type=pathlib.Path)
    p.add_argument('--seq',type=int,default=4891)
    a=p.parse_args();prepare(a.archive,a.output.resolve(),a.seq)
