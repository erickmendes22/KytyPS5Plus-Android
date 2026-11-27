#!/usr/bin/env python3
"""Match the actual prior GPU cluster writer to q8's consumed mask words.
A byte mismatch is a boundary observation, not proof of illegal mutation.
The writer's native output is NOT an independent oracle of its own math.
"""
import argparse,json
from pathlib import Path
import numpy as np
from p8_paired_replay import Archive,need,digest
import v4_q8_contract as q
WRITER=0x1ce1a858f62b5a11
MAX=2*1024*1024

def writer_key(w):
    need(w and w['shader']==WRITER and w['serial']>0 and not w['indirect'] and not w['fill'],'missing direct cluster writer')
    return tuple(w[k] for k in ('shader','frame','submit','guest','bytes','serial'))

def bound(row,stage,slot,files,written):
    s=stage['buffers'][slot]
    need(row['ok'] and row['slot']==s['slot']==slot and s['access_known'] and bool(s['written'])==written,'buffer access/slot mismatch')
    need(row['guest']==s['guest'] and row['bytes']==s['bytes'] and row['native']==s['native'] and row['binding_offset']==s['native_offset'],'buffer identity mismatch')
    n=row['bytes'];off=row['native_offset']-row['binding_offset']
    need(0<n<=MAX and n%4==0 and 0<=off<=255 and off+n<=s['native_range'],'buffer extent mismatch')
    need(row['tick_before']<=row['tick_after'],'copy order')
    data=files.get(row['file']);need(data is not None and len(data)==n,'missing/truncated payload')
    return data

def compare(produced,consumed,indices):
    need(len(produced)==len(consumed) and len(produced)%4==0 and len(produced)>0,'unpaired mask bytes')
    p=np.frombuffer(produced,'<u4');c=np.frombuffer(consumed,'<u4')
    idx=sorted(set(map(int,indices)))
    need(idx and all(0<=i<len(p) for i in idx),'invalid consumed mask indices')
    diff=np.flatnonzero(p!=c);changed=[i for i in idx if p[i]!=c[i]]
    return dict(status='CHANGED_CONSUMED' if changed else 'MATCH_CONSUMED',
        compared_words=len(p),different_words=len(diff),consumed_words=len(idx),
        different_consumed_words=len(changed),
        first_consumed_differences=[dict(index=i,producer=int(p[i]),consumer=int(c[i])) for i in changed[:64]],
        first_other_differences=[dict(index=int(i),producer=int(p[i]),consumer=int(c[i])) for i in diff[:32]],
        producer_sha256=digest(produced),consumer_sha256=digest(consumed))

def pair(a):
    es=[(i,e) for i,e in enumerate(a.events) if e['kind']=='v4_q8_contract']
    need(len(es)==1,'single q8 capture required');ei,e=es[0];ui,u=a.use[e['use']]
    need(ui<ei and e['frame']==u['frame']==a.frame and e['submit']==u['submit'] and e['shader']==q.SHADER,'consumer identity')
    cs=u['stages'][0];need(cs['shader']==q.SHADER and cs['valid'] and len(cs['buffers'])==4,'consumer stage')
    w=e.get('cluster_writer');wk=writer_key(w)
    mb=e['buffers'][3];cb=cs['buffers'][3]
    need(w['guest']==mb['guest']==cb['guest'] and w['bytes']==mb['bytes']==cb['bytes'] and mb['native']==cb['native'] and mb['binding_offset']==cb['native_offset'] and not cb['written'] and cb['access_known'],'consumer/writer range mismatch')
    need(0<=mb['native_offset']-mb['binding_offset']<=255 and mb['native_offset']-mb['binding_offset']+mb['bytes']<=cb['native_range'],'consumer range')
    need(a.frame-1<=w['frame']<=a.frame and w['submit']<=e['submit'],'writer outside causal window')
    ps=[(i,p) for i,p in enumerate(a.events) if p['kind']=='v4_cluster_producer' and p.get('writer') and tuple(p['writer'].get(k) for k in ('shader','frame','submit','guest','bytes','serial'))==wk]
    need(len(ps)==1,'last writer not retained uniquely');pi,p=ps[0];pui,pu=a.use[p['use']]
    need(pui<pi<ui<ei and p['frame']==pu['frame']==w['frame'] and p['submit']==pu['submit']==w['submit'] and p['shader']==WRITER and p['schema']==1,'producer dispatch order/identity')
    need(pu['path']=='compute_direct' and len(pu['stages'])==1 and all(g>0 for g in pu['groups']),'producer launch')
    st=pu['stages'][0];need(st['valid'] and st['shader']==WRITER and len(st['buffers'])==4 and not st['images'],'producer stage')
    need(len(p['before'])==4 and len(p['after'])==2,'producer snapshot census')
    for i,row in enumerate(p['before']):bound(row,st,i,a.files,i>=2)
    output=bound(p['after'][0],st,2,a.files,True);summary=bound(p['after'][1],st,3,a.files,True)
    need(len(output)==8*len(summary) and p['after'][0]['guest']==mb['guest'] and p['after'][0]['bytes']==mb['bytes'],'writer output extent')
    need(max(x['tick_after'] for x in p['before'])<=min(x['tick_before'] for x in p['after'])<=p['after'][0]['tick_after']<=mb['tick'],'producer/consumer copy ticks')
    consumer=a.files.get(mb['file']);need(consumer is not None and len(consumer)==mb['bytes'],'consumer payload')
    e0,trace=q.select_witness(e,a.files[e['trace_file']],'dark')
    f,o,_,_=q.parse_trace(trace,e['wg'],e['lane'])
    active=[i for i,r in enumerate(f) if r[17]==0x129 and r[18]==1]
    need(e['lane'] in active and e['control']['lane'] in active,'witness not admitted')
    check=q.Checks();union,lights=q.verify_mask(check,f,o,active,np.frombuffer(consumer,'<u4'),cs['guest_operands'],mb)
    indices=[int(f[l,28]) for l in active]
    indices.extend(int(o[k,l,1]) for k in q.bitset(union) for l in active)
    result=compare(output,consumer,indices)
    # Use retained code and all captured inputs for the NEXT independent
    # producer replay; no pretend shader equivalence based on native output.
    guest=st['guest_file'];need(guest=='guest-shaders/cs_1ce1a858f62b5a11_probe.bin','producer guest file identity')
    code=a.z.read('ds-lighting-capture/'+guest);need(code and len(code)%4==0,'missing producer guest code')
    result.update(writer=w,consumer=dict(frame=e['frame'],submit=e['submit'],use=e['use'],pixel=e['pixel'],control=e['control']['pixel'],light_indices=lights),
        producer_guest_sha256=digest(code),producer_launch=pu['launch'],producer_scalar_complete=bool(st['guest_operands']['scalar_complete']),
        same_native=p['after'][0]['native']==mb['native'] and p['after'][0]['native_offset']==mb['native_offset'],
        producer_state=p['after'][0]['state_after'],consumer_state=e['cluster_state_after_copy'],
        transfer_candidates=e.get('cluster_transfer_candidates',[]),mask_checks=check.counts,
        next='Replay actual producer from captured pre-dispatch operands' if result['status']=='MATCH_CONSUMED' else 'Locate first intervening mutation/publication on differing consumed dword; do not assume illegality')
    return result

def audit(path):
    a=Archive(path)
    try:return pair(a)
    except (ValueError,KeyError,IndexError,TypeError,OSError,q.Divergence) as ex:return dict(status='INCOMPLETE',reason=str(ex))
    finally:a.close()

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path);args=p.parse_args()
    print(json.dumps(audit(args.archive),indent=2,allow_nan=False))
