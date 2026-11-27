#!/usr/bin/env python3
"""V4: separate observed material population from HDR on the same native version.
Never a global prefix certificate or a healthy-lighting oracle.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from p8_paired_replay import Archive, need, key
from p8_version_link import audit as material_link
from p8_compositor_isa import Replay
from prelight_vertex_oracle import LLVM, run as vertex_run
from prelight_sky_replay import run as sky_run


def unique(rows, reason):
    need(len(rows) == 1, reason)
    return rows[0]


def luminance(rgb):
    return rgb @ np.array([.2126, .7152, .0722], np.float32)


def audit(path, runner, draw):
    a = Archive(path)
    try:
        link = material_link(a.z, draw)
        sky = sky_run(path)
        need(sky['local_status'] != 'BLOCKED' and sky['witness']['material_draw'] == draw,
             'missing paired sky depth witness')
        w, h = sky['witness']['extent']; n = w*h
        mask = (np.frombuffer(a.files[link['payload']], '<u4').reshape(h,w)&0xffffff)!=0
        q = 'ds-lighting-capture/queue-state/ds_prelight_witness_depth_pre_'
        before, after = (np.frombuffer(a.z.read(q+s+'.bin'), '<u4').reshape(h,w) for s in ('before','after'))
        need(mask.any(), 'no nonzero material proxy for HDR witness')
        vertices = vertex_run(path, 'ds_prelight_witness_vertex_*.json')
        need(not vertices.get('capture_error') and vertices['draws'], 'vertex acquisition unavailable')
        counts = vertices['counts']
        vstats = {k:sum(x.get(k,0) for x in vertices['draws']) for k in
                  ('vertices','components','bitwise_equal_components','numeric_equal_components','nan_payload_differences')}
        vstats['counts'] = counts
        vstats['nonmatching_draws'] = [x['identity']['draw'] for x in vertices['draws'] if x['status']!='POSITION_MATCH']
        journal=[json.loads(row) for row in a.z.read('ds-lighting-capture/queue-state/ds_prelight_events_0.jsonl').splitlines()]
        ys,xs=np.where(mask); tiles=(ys//8)*((w+7)//8)+xs//8
        stats=[]; memberships={}; last=None; hdr=None
        for queue in (0,8,33):
            rows=[(i,s) for i,s in enumerate(a.events) if s.get('kind')=='snapshot' and s.get('valid') and
                  s.get('frame')==a.frame and s.get('queue')==queue]
            gi,g=unique([(i,s) for i,s in rows if s['role']=='gbuffer' and s['file']==link['payload']], 'ambiguous queue material')
            hi,s=unique([(i,s) for i,s in rows if s['role']=='hdr'], 'ambiguous HDR checkpoint')
            need(key(g)==tuple(link['resource'][k] for k in ('owner','object','native','version','write_order','format')),
                 'queue material version mismatch')
            need(a.snapshot(g,'gbuffer')==a.files[link['payload']] and gi<hi and g['submit']==s['submit'], 'queue observation order/bytes')
            need(s['format']==97 and s['extent']==[w,h] and s['aspect']==1, 'unsupported HDR extent/format')
            if last: need(key(last)[:3]==key(s)[:3] and last['write_order']<s['write_order'], 'HDR identity/order changed')
            bridge=unique([b for b in journal if b.get('kind')=='light_bridge' and b['frame']==a.frame and b['queue']==queue and b['submit']==s['submit']], 'ambiguous light bridge')
            need(bridge['queue_membership_valid'] and bridge['workgroups_valid'] and bridge['same_dispatch_before_valid'] and
                 key(bridge['before_albedo_identity'])==key(g)==key(bridge['after_albedo_identity']) and
                 key(bridge['after_hdr_identity'])==key(s), 'unlinked queue membership')
            qt=bridge['queue_tiles']; need(len(qt)==bridge['queue_end']-bridge['queue_first']==bridge['workgroups'][0] and
                len(set(qt))==len(qt) and all(type(t) is int and 0<=t<((w+7)//8)*((h+7)//8) for t in qt), 'invalid tile list')
            memberships[queue]=qt
            raw=a.snapshot(s,'hdr'); last=s
            hdr=np.frombuffer(raw,'<f2').reshape(h,w,4)[:,:,:3].astype(np.float32)
            need(np.isfinite(hdr[mask]).all(), 'nonfinite material HDR')
            lum=luminance(hdr[mask]);stats.append(dict(queue=queue,version=s['version'],write_order=s['write_order'],
                mean=float(lum.mean()),at_or_above_006=int((lum>=.006).sum()),maximum=float(lum.max()),
                material_pixels_in_tiles=int(np.isin(tiles,qt).sum()),material_tiles=len(set(tiles)&set(qt)),workgroups=len(qt),shader=bridge['shader']))
        candidates=[s for s in a.events if s.get('kind')=='snapshot' and s.get('valid') and s.get('frame')==a.frame and s['role']=='post_hdr_input' and not s.get('observation_reused')]
        post=unique(candidates,'ambiguous post HDR input')
        need(key(post)[:3]==key(last)[:3] and post['write_order']>=last['write_order'] and post['format']==97 and post['extent']==[w,h],'post HDR identity/order')
        hdr=np.frombuffer(a.snapshot(post,'post_hdr_input'),'<f2').reshape(h,w,4)[:,:,:3].astype(np.float32)
        need(np.isfinite(hdr[mask]).all(),'nonfinite post HDR'); lum=luminance(hdr)
        # Select strongest packed material proxy among dark real pixels. This is
        # a deterministic spatial witness, not proof of physical albedo/radiance.
        words=np.frombuffer(a.files[link['payload']],'<u4').reshape(h,w)
        score=sum((words>>k)&255 for k in (0,8,16)); elig=mask & (lum<.006)
        need(elig.any(),'no dark material witness'); y,x=np.unravel_index(np.argmax(np.where(elig,score,0)),mask.shape)
        tile=int(y//8*((w+7)//8)+x//8)
        selected_queues=[dict(queue=k,workgroup=v.index(tile),local_xy=[int(x%8),int(y%8)]) for k,v in memberships.items() if tile in v]
        # Require a version-matched recorded stencil snapshot for queue 8.
        ui,u=unique([(i,e) for i,e in a.use.values() if e['frame']==a.frame and any(f"{st['shader']:016x}"==stats[1]['shader'] for st in e['stages'])], 'ambiguous queue 8 command')
        stage=unique([st for st in u['stages'] if f"{st['shader']:016x}"==stats[1]['shader']], 'queue 8 shader mismatch')
        se=unique([(entry,ss) for entry in stage['images'] for ss in entry['snapshots'] if ss['role']=='stencil' and ss.get('valid')], 'missing stencil input')
        entry,ss=se;need(key(entry['image'])==key(ss) and ss['extent']==[w,h] and ss['texel_bytes']==1 and ss['aspect']==4,'stencil binding identity')
        stencil=np.frombuffer(a.snapshot(ss,'stencil',later=ui),'u1').reshape(h,w)
        r=Replay(a); sx,sy=r.images[r.slots[1]].shape[1],r.images[r.slots[1]].shape[0]
        need((sx,sy)==(w,h),'unmatched scene extent')
        fx=int((x+.5)*r.w/w);fy=int((y+.5)*r.h/h)
        dec=LLVM()
        try:
            center=r.compare(dec,runner,r.quad((r.w//2)&~1,(r.h//2)&~1))[0]
            selected=r.compare(dec,runner,r.quad(fx&~1,fy&~1))[(fy&1)*2+(fx&1)]
        finally:dec.close()
        def overlap(row):
            pts=[(i,j) for s in row['support'] if s['version']==r.identities[r.slots[1]] for i,j in s['texels']]
            return dict(scene_texels=len(pts),spatial_material_overlap=sum(bool(mask[j,i]) for i,j in pts))
        return dict(method='V4_LOCAL_CONTRACT',frame=a.frame,global_p8='UNRESOLVED_NOT_A_RELEASE_GATE',
            material=dict(link=link,pixels=int(mask.sum()),total=n,sky_depth_changed=int((before!=after).sum()),
                          mask_complement_mismatches=int((mask!=(before==after)).sum())),
            admitted_vertex_positions=vstats,hdr_checkpoints=stats,post_hdr=dict(version=post['version'],mean=float(lum[mask].mean()),
                at_or_above_006=int((lum[mask]>=.006).sum()),below_006=int((lum[mask]<.006).sum())),
            witness=dict(material_xy=[int(x),int(y)],material_word=f'{int(words[y,x]):08x}',post_hdr_rgb=hdr[y,x].tolist(),
                flip=selected,tile=tile,queues=selected_queues,stencil=int(stencil[y,x]),
                stencil_bit64_material_pixels=int(((stencil[mask]&64)!=0).sum()),**overlap(selected)),previous_center=dict(flip=center,**overlap(center)),
            scope='Same native material bytes and observed HDR checkpoints; spatial alignment is not HDR-to-scene lineage. No expected gameplay brightness or global branch cut.')
    finally:a.close()


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path);p.add_argument('runner',type=Path);p.add_argument('--draw',type=int,required=True)
    args=p.parse_args();print(json.dumps(audit(args.archive,args.runner,args.draw),indent=2,allow_nan=False))
