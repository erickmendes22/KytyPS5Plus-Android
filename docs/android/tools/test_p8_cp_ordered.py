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
