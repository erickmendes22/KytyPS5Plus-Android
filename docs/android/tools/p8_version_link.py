#!/usr/bin/env python3
"""Match retained draw outputs to an actual P8 boundary, across frames.

A link is an observation of the same native version, not proof of guest
correctness or a global P8 decision. The recorded full-image word hash
must agree with the verified raw boundary payload.
"""
import argparse
import hashlib
import json
from pathlib import PurePosixPath
import struct
import zipfile

IDENTITY = ('owner', 'object', 'native', 'version', 'write_order', 'format')
PREFIX = 'p8-reference/'
QUEUE = 'ds-lighting-capture/queue-state/'


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def identity(row):
    result = tuple(row[k] for k in IDENTITY)
    require(all(type(v) is int and v >= 0 for v in result) and
            all(result[i] for i in (0, 1, 2, 3)), 'invalid resource identity')
    return result


def full_words(raw):
    require(len(raw) % 4 == 0, 'unaligned raw image')
    h = 14695981039346656037
    colored = 0
    for (word,) in struct.iter_unpack('<I', raw):
        h = ((h ^ word)*1099511628211) & 0xffffffffffffffff
        colored += bool(word & 0xffffff)
    return h, colored


def load(z, name):
    return json.loads(z.read(name))


def audit(z, draw):
    names = z.namelist()
    require(len(names) == len(set(names)), 'duplicate ZIP member')
    manifest = load(z, PREFIX+'export-manifest.json')
    entries = {}
    for row in manifest['files']:
        n = row['file']
        require(PurePosixPath(n).name == n and n not in entries, 'unsafe/duplicate manifest')
        require(row['status'] == 'complete', 'incomplete export: '+n)
        require(0 <= row['bytes'] <= 128*1024*1024 and
                z.getinfo(PREFIX+n).file_size == row['bytes'] == row['copied'], 'manifest length mismatch')
        require(hashlib.sha256(z.read(PREFIX+n)).hexdigest() == row['sha256'], 'manifest hash mismatch: '+n)
        entries[n] = row
    for n in ('capture-status.json','observations.jsonl'):
        require(n in entries, 'unverified P8 control file')
    status = load(z, PREFIX+'capture-status.json')
    witness = load(z, QUEUE+'ds_prelight_witness_foreground_manifest.json')
    cut = status['boundary']
    require(cut == witness['consumer_boundary'] and cut['frame'] == status['target_frame'], 'boundary mismatch')
    events = [json.loads(x) for x in z.read(PREFIX+'observations.jsonl').splitlines()]
    draws = [json.loads(x) for x in z.read(QUEUE+'ds_prelight_events_0.jsonl').splitlines()]
    rows = [x for x in draws if x.get('draw') == draw]
    def unique(kind):
        hits = [r for r in rows if r.get('kind') == kind]
        require(len(hits) == 1, 'missing/ambiguous '+kind)
        return hits[0]
    inp, before, execute, after = (unique(k) for k in ('draw_inputs','draw_before','draw_execute','draw_after'))
    producer = (inp['frame'], inp['submit'], inp['draw'])
    require(all((r['frame'],r['submit'],r['draw']) == producer for r in (before, execute, after)), 'producer identity mismatch')
    require(inp['seq'] < before['seq'] < execute['seq'] < after['seq'], 'draw order mismatch')
    require(inp['frame'] <= cut['frame'] and inp['submit'] <= cut['submit'], 'draw after consumer')
    a = identity(after['image'])
    require(a == identity(execute['image']), 'draw output changed before observation')
    require((a[0],a[3],a[4]) == (cut['owner'],cut['version'],cut['write_order']), 'draw did not produce boundary version')
    require(not after['image'].get('cpu_dirty') and not after['image'].get('buffer_modified'), 'unstable producer')
    matches = [(i,s) for i,s in enumerate(events) if s.get('kind') == 'snapshot' and s.get('valid') is True and
               s.get('role') == 'gbuffer' and s.get('frame') == cut['frame'] and s.get('submit') == cut['submit'] and
               identity(s) == a and (s['mip'],s['layer'],s['aspect']) == (before['mip'],before['layer'],1)]
    require(matches, 'no exact boundary payload')
    require(len({(s['file'],tuple(s['extent'])) for _,s in matches}) == 1, 'ambiguous boundary payload')
    i,s = matches[0]
    require(s['version_after'] == s['version'] and s['texel_bytes'] == 4 and s['format'] == 98 and
            not s.get('cpu_dirty') and not s.get('buffer_modified'), 'unstable/unsupported boundary')
    require(s['file'] in entries, 'unverified image payload')
    raw = z.read(PREFIX+s['file'])
    w,h = s['extent']
    require(len(raw) == w*h*4 == s['expected_bytes'] == s['captured_bytes'], 'boundary byte length mismatch')
    digest, colored = full_words(raw)
    require(after['full'] == {'width':w,'height':h,'hash_words':digest,'rgb_nonzero':colored}, 'draw output does not match raw boundary')
    uses = []
    for j,use in enumerate(events):
        if j <= i or use.get('kind') != 'cut_use' or use.get('frame') != cut['frame'] or use.get('submit') != cut['submit']:
            continue
        for stage in use['stages']:
            for img in stage.get('images',[]):
                if not img.get('read') or not img.get('image') or identity(img['image']) != a:
                    continue
                if any(x.get('valid') and x.get('file') == s['file'] and identity(x) == a and
                       x['version_after'] == x['version'] and not x.get('cpu_dirty') and not x.get('buffer_modified') and
                       (x['mip'],x['layer'],x['aspect']) == (s['mip'],s['layer'],s['aspect']) for x in img['snapshots']):
                    uses.append({'marker':use['marker'],'shader':format(stage['shader'],'016x'),'slot':img['slot']})
    require(uses, 'no consumer of exact boundary payload')
    return {'status':'LINKED_NATIVE_VERSION','scope':'native lineage only, not guest correctness or P8 PASS/FAIL',
            'producer':{'frame':producer[0],'submit':producer[1],'draw':draw,'vs':inp['vs_hash'],'ps':inp['ps_hash']},
            'consumer':cut,'same_frame':producer[0]==cut['frame'],'resource':dict(zip(IDENTITY,a)),
            'payload':s['file'],'sha256':entries[s['file']]['sha256'],'hash_words':digest,'rgb_nonzero':colored,
            'rgb_before':before['full']['rgb_nonzero'],'uses':uses,'manifest_files':len(entries)}


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive');p.add_argument('--draw',type=int,required=True);p.add_argument('--output');a=p.parse_args()
    with zipfile.ZipFile(a.archive) as z:
        result=audit(z,a.draw)
    out=json.dumps(result,indent=2)+'\n'
    if a.output:
        with open(a.output,'w') as f:f.write(out)
    print(out,end='')

if __name__=='__main__':main()
