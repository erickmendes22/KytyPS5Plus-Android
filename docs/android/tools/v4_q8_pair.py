#!/usr/bin/env python3
"""Paired q8 local comparison; same dispatch, workgroup, stencil and resources."""
import argparse,json
from pathlib import Path
import numpy as np
from p8_paired_replay import Archive,need
import v4_q8_contract as q
import v4_q8_light_records as m

def audit(path,runner):
    a=Archive(path)
    try:
        events=[e for e in a.events if e.get('kind')=='v4_q8_contract']
        need(len(events)==1 and events[0]['schema']==3,'paired q8 acquisition required')
        e=events[0]
        q.select_witness(e,a.files[e['trace_file']],'dark')
        q.select_witness(e,a.files[e['trace_file']],'control')
        contrast=e['pair_contrast_before']
    finally:a.close()
    rows={}
    for role in ('dark','control'):
        rows[role]={'operations':q.audit(path,runner,role,conditional=True),'records':m.audit(path,role)}
    for role,row in rows.items():
        for k in ('hdr_before','hdr_after','selected_accumulation'):
            if k in row['operations']:
                row[k+'_luminance']=float(np.array(row['operations'][k][:3])@np.array([.2126,.7152,.0722]))
    statuses=[r['operations']['status'] for r in rows.values()]+[r['records']['status'] for r in rows.values()]
    replay_statuses=[r['operations'].get('conditional_isa',{}).get('status','INCOMPLETE') for r in rows.values()]
    if 'DIVERGENCE' in statuses:
        decision='DIVERGENCE_LOCAL'
        next_step='Correct the first independently demonstrated load, filter, accumulation or publication mismatch.'
    elif 'DIVERGENCE_CANDIDATE' in replay_statuses:
        decision='DIVERGENCE_CANDIDATE'
        next_step='Reduce the differing original instruction/sample address or light term with the exact captured operands; no lighting fix by assumption.'
    elif all(s=='MATCH' for s in statuses) and all(s=='MATCH_CONDITIONAL' for s in replay_statuses):
        decision='MATCH_CONDITIONAL'
        next_step='Follow the input that determines the low term (sample source or material), or the producer of the incoming HDR contrast. Native samples are not an independent sampling proof.'
    else:
        decision='INCOMPLETE'
        next_step='Repair the explicitly reported comparison gap, not the already matched contracts.'
    return dict(status=decision,contrast_before=contrast,contrast_after=(rows['dark'].get('hdr_after_luminance',1)<.003 and rows['control'].get('hdr_after_luminance',0)>=.006),witnesses=rows,next=next_step,scope='local arithmetic conditional on observed samples and verified wave masks; no whole-P8 or healthy-lighting proof')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path);p.add_argument('runner',type=Path);a=p.parse_args()
    try:print(json.dumps(audit(a.archive,a.runner),indent=2,allow_nan=False))
    except (ValueError,KeyError,IndexError,TypeError,OSError) as e:print(json.dumps(dict(status='INCOMPLETE',reason=str(e)),indent=2))
