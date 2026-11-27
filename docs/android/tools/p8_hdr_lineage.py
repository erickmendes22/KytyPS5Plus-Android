#!/usr/bin/env python3
"""Audit actual direct-scene HDR boundaries; never a guest P8 certificate."""
import argparse
import hashlib
import json
from pathlib import PurePosixPath
import zipfile
import numpy as np

HDR = 0x281300000
SCENE = 0x2b2a40000
FORWARD = 0xab81bfa8ee4e0fee
COMPOSITE = 0x3265b01ed00a5c01
POST = {0xe86bfee195efa5a4, 0x8f52cca9ea3ba4fc}
IDENTITY = ('owner', 'object', 'native', 'version', 'write_order', 'format')
VIEW = ('extent', 'mip', 'layer', 'aspect')

def need(ok, reason):
    if not ok:
        raise ValueError(reason)

def same(a, b, keys=IDENTITY):
    return all(k in a and k in b and a[k] == b[k] for k in keys)

def canonical(row):
    return json.dumps(row, sort_keys=True, separators=(',', ':'))

def load_zip(path):
    data = {}
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        need(len(names) == len(set(names)), 'duplicate archive members')
        prefix = 'p8-reference/'
        m = json.loads(z.read(prefix + 'export-manifest.json'))
        seen = set()
        for f in m['files']:
            name = f['file']
            need(PurePosixPath(name).name == name and name not in seen, 'unsafe/duplicate manifest entry')
            seen.add(name)
            if f['status'] != 'complete':
                continue
            payload = z.read(prefix + name)
            need(len(payload) == f['bytes'] == f['copied'], 'wrong length: ' + name)
            need(hashlib.sha256(payload).hexdigest() == f['sha256'], 'wrong digest: ' + name)
            data[name] = payload
    return data, [json.loads(line) for line in data['observations.jsonl'].splitlines()], json.loads(data['capture-status.json'])['target_frame']

def audit(data, events, frame):
    physical, recorded = {}, {}
    for i, e in enumerate(events):
        if e.get('kind') != 'snapshot' or not e.get('valid') or e['frame'] != frame:
            continue
        need(e['version'] == e['version_after'], 'version changed during capture')
        if e.get('observation_reused'):
            prev = physical.get(e['source_observation'])
            need(prev is not None and same(e, prev, IDENTITY + VIEW + ('file', 'captured_bytes')), 'reuse identity mismatch')
            need(e['source_submit'] == prev['submit'] and not e['cpu_dirty'] and not e['buffer_modified'], 'invalid reuse')
        else:
            need(e['marker'] not in physical, 'duplicate physical marker')
            physical[e['marker']] = e
        recorded.setdefault(canonical(e), []).append(i)
    uses = [(i,e) for i,e in enumerate(events) if e.get('kind') == 'cut_use' and e.get('frame') == frame]

    def pixels(s, role, owner):
        need(s.get('valid') and s.get('role') == role and s.get('owner') == owner, 'missing/invalid ' + role)
        need(canonical(s) in recorded, 'unrecorded ' + role)
        need(s.get('format') == 97 and s.get('aspect') == 1 and s.get('texel_bytes') == 8, 'unsupported HDR format')
        raw = data.get(s['file'])
        w,h = s['extent']
        need(raw is not None and len(raw) == w*h*8 == s['captured_bytes'] == s['expected_bytes'], 'HDR payload incomplete')
        return np.frombuffer(raw, '<f2').reshape(-1,4)[:,:3].astype(np.float32)

    def stats(s, role, owner):
        a = pixels(s, role, owner)
        finite = np.isfinite(a).all(axis=1)
        valid = a[finite]
        lum = valid @ np.array([0.2126,0.7152,0.0722],np.float32)
        return dict(version=s['version'], write_order=s['write_order'], extent=s['extent'],
                    nonzero=int(np.any(valid != 0,axis=1).sum()), nonfinite=int((~finite).sum()),
                    lit=int((lum >= .006).sum()), mean_luminance=float(lum.mean()) if len(lum) else None)

    result = {'target_frame': frame, 'scope': 'observed native values only', 'p8_certificate': False,
              'forward_pairs': [], 'post_inputs': [], 'compositor_inputs': [], 'missing': []}
    for pi, pair in enumerate(events):
        if pair.get('kind') != 'radiance_draw_pair' or pair.get('frame') != frame:
            continue
        try:
            matched = [(i,e) for i,e in uses if e.get('radiance_pair') == pair['pair']]
            need(len(matched) == 1, 'missing/ambiguous forward use')
            ui,use = matched[0]
            need(use['submit'] == pair['submit'] and pair['shader'] == FORWARD and any(s['shader']==FORWARD for s in use['stages']), 'forward command mismatch')
            b,a = pair['before'],pair['after']
            before=pixels(b,'forward_hdr_before',HDR); after=pixels(a,'forward_hdr_after',HDR)
            need(b['submit']==a['submit']==pair['submit'] and any(bi<ui<ai<pi for bi in recorded[canonical(b)] for ai in recorded[canonical(a)]), 'forward observation order mismatch')
            need(same(b,a,('owner','object','native','format')+VIEW) and a['version']>=b['version'] and a['write_order']>=b['write_order'], 'forward changed resource identity')
            target=[t['image'] for t in use['targets'] if t['slot']==pair['slot']]
            need(len(target)==1 and same(target[0],b), 'forward attachment identity mismatch')
            next_use=None
            for ni,ne in uses:
                if ni<=pi: continue
                images=[t['image'] for t in ne.get('targets',[])]
                images += [j['image'] for s in ne['stages'] for j in s.get('images',[]) if j.get('image')]
                candidates=[im for im in images if same(im,a,('owner','object','native'))]
                if candidates:
                    next_use={'marker':ne['marker'],'same_produced_version':any(same(im,a) for im in candidates)};break
            if not next_use or not next_use['same_produced_version']:
                result['missing'].append('forward pair '+str(pair['pair'])+': next-use version not established')
            result['forward_pairs'].append({'pair':pair['pair'],'before':stats(b,'forward_hdr_before',HDR),
                'after':stats(a,'forward_hdr_after',HDR),'changed_rgb':int(np.any(before!=after,axis=1).sum()),
                'load_clear':pair['raster']['load_clear'],'native_color_write':pair['raster']['native_color_write'],
                'next_use':next_use})
        except (KeyError,ValueError) as exc:
            result['missing'].append('forward pair '+str(pair.get('pair'))+': '+str(exc))
    for ui,use in uses:
        for stage in use['stages']:
            shader=stage.get('shader')
            for entry in stage.get('images',[]):
                owner=entry.get('image',{}).get('owner')
                role = 'post_hdr_input' if shader in POST and owner==HDR else 'compositor_scene_input' if shader==COMPOSITE and owner==SCENE else None
                if not role or not entry.get('read') or entry.get('written'):continue
                try:
                    snapshots=[s for s in entry['snapshots'] if s.get('role')==role and s.get('mip')==entry['view']['mip'] and s.get('layer')==entry['view']['layer']]
                    need(len(snapshots)==1,'missing/ambiguous actual input')
                    snap=snapshots[0]
                    need(same(snap,entry['image']) and snap['submit']==use['submit'] and canonical(snap) in recorded and any(si<ui for si in recorded[canonical(snap)]),'consumer identity/order mismatch')
                    result['post_inputs' if role=='post_hdr_input' else 'compositor_inputs'].append({'marker':use['marker'],'shader':f'{shader:016x}',**stats(snap,role,owner)})
                except (KeyError,ValueError) as exc:
                    result['missing'].append(use['marker']+' '+role+': '+str(exc))
    for key in ('forward_pairs','post_inputs','compositor_inputs'):
        if not result[key]:result['missing'].append('no verified '+key)
    result['status']='OBSERVED_CHAIN_INCOMPLETE' if result['missing'] else 'OBSERVED_BOUNDARIES_AVAILABLE'
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive');args=p.parse_args()
    print(json.dumps(audit(*load_zip(args.archive)),indent=2))

if __name__=='__main__':main()
