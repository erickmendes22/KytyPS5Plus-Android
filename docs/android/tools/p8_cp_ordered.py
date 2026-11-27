#!/usr/bin/env python3
"""Check selected-cycle CP source and emitted draws, not the global P8 prefix."""
import argparse
from collections import Counter
import hashlib
import json
import struct
from pathlib import Path
from prelight_indirect_oracle import (Capture, audit_batch, bind_guest_journal,
    journal_events, recount_batch, states_different, require)

NAME = 'cp-ordered-batches.jsonl'
PREFIX = 'p8-reference/'


def gpu_offsets(state, address):
    """Exact GPU-owned bytes, including outstanding publications."""
    ranges = state['gpu_dirty_ranges'] + [part for pending in state['pending_tokens']
                                          for part in pending['ranges']]
    return {i for part in ranges for i in range(max(0, part['address']-address),
            min(20, part['address']+part['bytes']-address))}


def audit_selected(batch, journal, target):
    require(batch.get('selected_p8') is True and batch.get('mutation_epoch_tracked') is False,
            'not a selected-cycle reference or falsely tracked epoch')
    require(batch.get('frame') in (target-1,target), 'batch outside selected cycle')
    result = audit_batch(batch)
    bind_guest_journal(batch,result,journal)
    parent = next((x for x in journal if x.get('seq')==batch.get('parent_command')), {})
    fallback = any(x.get('kind')=='command_return' and x.get('command')==batch.get('parent_command')
                   and x.get('outcome')=='cpu_fallback' for x in journal)
    raw_by_child = {x['child']:x for x in batch.get('records',[]) if isinstance(x,dict) and 'child' in x}
    for record in result.get('records',[]):
        raw = raw_by_child.get(record.get('child'),{})
        reason = None
        if parent.get('frame') != batch['frame'] or not fallback or record.get('guest_child_binding') != 'VERIFIED':
            reason = 'missing same-frame original parent/child binding'
        elif not raw.get('front_reference_ready') or not batch.get('collection_front_ready'):
            reason = 'record thread ownership not certified'
        # A post-read backing copy may be downstream of the very GPU->CPU
        # publication we are trying to verify. Equality does not prove that
        # publication preserved the pre-read authoritative bytes.
        if record.get('status') == 'MATCH':
            before, after = raw['state_before'], raw['state_after']
            lost = sorted(gpu_offsets(before, raw['address']) - gpu_offsets(after, raw['address']))
            if lost or before['image_gpu_dirty']:
                record['post_read_reference_match'] = True
                record['lost_gpu_authority_offsets'] = lost
                reason = reason or 'pre-read GPU authority not independently captured'
        if reason:
            record.update(status='INCOMPLETE',reason=reason)
            record.pop('first_divergence',None)
        elif record.get('status')=='DIVERGENCE' and record.get('first_divergence',{}).get('stage')=='memory_reference_to_cp_read':
            # No universal CPU-store trace for newly encountered ring addresses.
            # A mismatch is decisive locally only if every differing byte was
            # GPU-owned, with the exact same authority and mapping throughout.
            a=struct.pack('<5I',*record['consumed_words'])
            b=struct.pack('<5I',*record['reference_words'])
            differing=[i for i in range(20) if a[i]!=b[i]]
            before,after=raw['state_before'],raw['state_after']
            if (not differing or any(record['authority_by_byte'][i]!='GPU' for i in differing)
                or before['cpu_modified'] or after['cpu_modified'] or states_different(before,after)):
                record.update(status='UNSTABLE',reason='untracked CPU mutation or changed authority cannot be excluded')
                record.pop('first_divergence',None)
        record['scope']='local read and emission, not producer correctness or global P8'
    recount_batch(result)
    return result


def audit(path):
    capture=Capture(path)
    try:
        require(capture.archive is not None,'expected diagnostic ZIP')
        z=capture.archive
        journal,errors=journal_events(capture)
        manifest=json.loads(z.read(PREFIX+'export-manifest.json'))
        def verified(name):
            entries=[x for x in manifest['files'] if x['file']==name]
            require(len(entries)==1 and entries[0]['status']=='complete','missing/incomplete export: '+name)
            meta=entries[0];data=z.read(PREFIX+name)
            require(len(data)==meta['bytes']==meta['copied'] and hashlib.sha256(data).hexdigest()==meta['sha256'],
                    'export integrity mismatch: '+name)
            return data
        target=json.loads(verified('capture-status.json'))['target_frame']
        rows=[json.loads(x) for x in verified(NAME).splitlines()]
        batches=[audit_selected(x,journal,target) for x in rows if x.get('kind')=='cp_batch']
        omissions=[x for x in rows if x.get('kind')!='cp_batch']
        counts=Counter(r['status'] for b in batches for r in b.get('records',[]))
        return {'target_frame':target,'batches':batches,'record_counts':dict(counts),
                'omissions':omissions,'journal_errors':errors,'p8_certificate':False,
                'scope':'same-cycle CP argument edge only'}
    finally:
        capture.close()


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path)
    try: print(json.dumps(audit(p.parse_args().archive),indent=2))
    except (ValueError,KeyError,OSError) as e:
        print(json.dumps({'status':'INCOMPLETE','reason':str(e),'p8_certificate':False},indent=2));raise SystemExit(2)
