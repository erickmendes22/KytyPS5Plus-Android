from copy import deepcopy
from p8_cp_ordered import audit_selected

address=0x21de07c38
index_base=0x2e8100000
consumed=[0,1,0,0,0]
state={'valid':True,'address':address,'bytes':20,'gpu_dirty':True,'pending_publication':False,
       'cpu_modified':False,'gpu_modified':True,'image_gpu_dirty':False,'registered':True,
       'gpu_generation':1,'unregister_epoch':2,'gpu_dirty_ranges':[{'address':address,'bytes':20}],
       'pending_tokens':[],'native':3,'buffer_guest':address-56,'buffer_size':4096}
batch={'kind':'cp_batch','selected_p8':True,'mutation_epoch_tracked':False,'collection_front_ready':True,
       'args_guest':address,'count':1,'stride':20,'index_base':index_base,'index_limit':22020096,
       'index_element_bytes':4,'index_type':1,'frame':407,'submit':12225,'tick':40841,'stream_id':0,
       'parent_command':49,'seq':8,'records':[]}
record={'child':0,'address':address,'bytes':20,'front_reference_ready':True,'consumed_words':consumed,
        'uses':[{'field':name,'phase':'draw','value':word} for name,word in zip(('index_count','instance_count','first_index','base_vertex','first_instance'),consumed)],
        'uses_complete':True,'emitted':{'index_count':0,'instance_count':1,'first_index':0,'base_vertex':0,'first_instance':0,'index_addr':index_base},
        'state_before':deepcopy(state),'state_after':deepcopy(state),'state_reference_after':deepcopy(state),
        'mutation_epoch_before':0,'mutation_epoch_after':0,'mutation_epoch_reference_after':0,
        'observation_seq_before':4,'observation_seq_consumed':5,'observation_seq_reference':6,'backing_after':{'ok':True,'words':consumed},
        'reference':{'address':address,'bytes':20,'kind':'gpu_native','ok':True,'words':consumed,'native':3,'buffer_guest':address-56,
                     'buffer_size':4096,'offset':56,'tick_before':40841,'tick_after':40842,'queued_after_consumption':True,'before_next_child':True,
                     'state_at_copy':deepcopy(state),'collection_after_all_original_reads':True,'observation_seq_collected':7}}
batch['records']=[record]
journal=[{'seq':49,'kind':'guest_command','op':'indirect_indexed','frame':407,'submit':12225,'indirect_guest':address,'count':1,'stride':20,'index_guest':index_base,'index_limit':22020096},
         {'seq':50,'kind':'command_return','command':49,'outcome':'cpu_fallback'},
         {'seq':51,'kind':'guest_command','op':'indexed','count':0,'instances':1,'index_guest':index_base}]
def status(b,j=journal):return audit_selected(b,j,408)['status']
assert status(batch)=='MATCH'
b=deepcopy(batch);b['records'][0]['reference']['words']=[92160,1,0,0,0];assert status(b)=='DIVERGENCE'
c=deepcopy(b);c['records'][0]['state_before']['gpu_generation']=0;assert status(c)=='UNSTABLE'
c=deepcopy(b);c['records'][0]['front_reference_ready']=False;assert status(c)=='INCOMPLETE'
c=deepcopy(b);c['records'][0]['reference']['ok']=False;assert status(c)=='INCOMPLETE'
assert status(b,journal[:2])=='INCOMPLETE'
c=deepcopy(b);c['records'][0]['reference']['collection_after_all_original_reads']=False;assert status(c)=='INCOMPLETE'
print('7 ordered CP comparison tests passed')

# Full archive transport and actual three-child shape, including a missing
# draw on the first child and unchanged second and third records.
from p8_cp_ordered import audit
from pathlib import Path
import tempfile, zipfile, json, hashlib
b=deepcopy(batch); b['count']=3; b['records']=[]
j=deepcopy(journal[:2]); j[0]['count']=3
for i in range(3):
    r=deepcopy(record); r['child']=i; r['address']=address+20*i
    for k in ['state_before','state_after','state_reference_after']:
        r[k]['address']=r['address']
        r[k]['gpu_dirty_ranges']=[{'address':r['address'],'bytes':20}]
    ref=r['reference']; ref['address']=r['address']; ref['offset']=56+20*i
    ref['state_at_copy']=deepcopy(r['state_reference_after'])
    r['observation_seq_before']=4+i*3
    r['observation_seq_consumed']=5+i*3
    r['observation_seq_reference']=6+i*3
    ref['observation_seq_collected']=14
    if i==0: ref['words']=[92160,1,0,0,0]
    b['records'].append(r)
    j.append({'seq':51+2*i,'kind':'guest_command','op':'indexed','count':0,'instances':1,'index_guest':index_base})
b['seq']=15
files={'cp-ordered-batches.jsonl':(json.dumps(b)+'\n').encode(),'capture-status.json':b'{"target_frame":408}'}
manifest={'files':[dict(file=n,status='complete',bytes=len(v),copied=len(v),sha256=hashlib.sha256(v).hexdigest()) for n,v in files.items()]}
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'capture.zip'
    def write_archive():
        with zipfile.ZipFile(p,'w') as z:
            z.writestr('queue-state/ds_prelight_events_0.jsonl','\n'.join(map(json.dumps,j))+'\n')
            for n,v in files.items(): z.writestr('p8-reference/'+n,v)
            z.writestr('p8-reference/export-manifest.json',json.dumps(manifest))
    write_archive()
    assert audit(p)['record_counts']=={'DIVERGENCE':1,'MATCH':2}
    # Corrupted data cannot become a local mismatch verdict.
    files['cp-ordered-batches.jsonl']+=b' '
    write_archive()
    try:
        audit(p)
    except ValueError as e:
        assert 'export integrity mismatch' in str(e)
    else:
        raise AssertionError('corrupt selected-cycle export accepted')
print('Three-record archive transport and corrupt-export rejection passed')

# #752: publication during the original read makes a post-read backing
# alias a downstream observation, not an independent GPU-side reference.
def published_backing():
    b=deepcopy(batch); r=b['records'][0]
    clean=deepcopy(state)
    clean.update(gpu_dirty=False,gpu_modified=False,gpu_dirty_ranges=[])
    r['state_after']=deepcopy(clean); r['state_reference_after']=deepcopy(clean)
    r['reference'].update(kind='cpu_backing',state_at_copy=deepcopy(clean))
    return b

b=published_backing()
r=audit_selected(b,journal,408)['records'][0]
assert r['status']=='INCOMPLETE' and r['post_read_reference_match'] is True
assert r['lost_gpu_authority_offsets']==list(range(20))
b['records'][0]['state_before']=deepcopy(b['records'][0]['state_after'])
assert status(b)=='MATCH'
b=published_backing(); b['records'][0]['state_before']['gpu_dirty_ranges']=[{'address':address+2,'bytes':3}]
r=audit_selected(b,journal,408)['records'][0]
assert r['status']=='INCOMPLETE' and r['lost_gpu_authority_offsets']==[2,3,4]
b=published_backing(); s=b['records'][0]['state_before']
s.update(gpu_dirty=False,gpu_dirty_ranges=[],pending_publication=True,
         pending_tokens=[{'token':1,'ranges':[{'address':address+4,'bytes':4}]}])
r=audit_selected(b,journal,408)['records'][0]
assert r['status']=='INCOMPLETE' and r['lost_gpu_authority_offsets']==[4,5,6,7]
print('Four #752 authority-transition regression tests passed')
