#!/usr/bin/env python3
"""Boundary linkage controls, including different frame and resource identities."""
import io
import json
import hashlib
import struct
import zipfile
from p8_version_link import audit, full_words, PREFIX, QUEUE

def fixture(case='valid'):
    raw=struct.pack('<2I',0xff000000,0xff000001)
    h,n=full_words(raw)
    img=dict(owner=100,object=200,native=300,version=4,write_order=9,format=98)
    cut=dict(owner=100,version=4,write_order=9,frame=21,submit=12)
    snap=dict(**img,kind='snapshot',valid=True,role='gbuffer',frame=21,submit=12,mip=0,layer=0,aspect=1,
              file='P8_FULL_1.bin',extent=[2,1],version_after=4,texel_bytes=4,expected_bytes=8,captured_bytes=8)
    use=dict(kind='cut_use',frame=21,submit=12,marker='P8_USE_1',stages=[dict(shader=123,images=[dict(read=True,image=img.copy(),snapshots=[snap.copy()],slot=0)])])
    rows=[dict(kind=k,draw=9,frame=20,submit=11,seq=i+1) for i,k in enumerate(('draw_inputs','draw_before','draw_execute','draw_after'))]
    rows[0].update(vs_hash='vs',ps_hash='ps')
    rows[1].update(mip=0,layer=0,full={'rgb_nonzero':2})
    rows[2]['image']=img.copy()
    rows[3].update(image=img.copy(),full=dict(width=2,height=1,hash_words=h,rgb_nonzero=n))
    if case=='wrong_version':rows[2]['image']['version']=rows[3]['image']['version']=3
    if case=='wrong_allocation':snap['native']=999
    if case=='wrong_hash':rows[3]['full']['hash_words']^=1
    if case=='wrong_layer':snap['layer']=1
    if case=='future_producer':
        for r in rows:r['frame']=22
    if case=='wrong_consumer':use['stages'][0]['images'][0]['image']['version']=5
    if case=='unstable':snap['version_after']=5
    files={'capture-status.json':json.dumps(dict(boundary=cut,target_frame=21)).encode(),
           'observations.jsonl':b'\n'.join(json.dumps(x).encode() for x in (snap,use)),
           'P8_FULL_1.bin':raw}
    manifest={'files':[dict(file=k,status='complete',bytes=len(v),copied=len(v),sha256=hashlib.sha256(v).hexdigest()) for k,v in files.items()]}
    if case=='corrupt':files['P8_FULL_1.bin']=b'12345678'
    buf=io.BytesIO()
    with zipfile.ZipFile(buf,'w') as z:
        for k,v in files.items():z.writestr(PREFIX+k,v)
        z.writestr(PREFIX+'export-manifest.json',json.dumps(manifest))
        z.writestr(QUEUE+'ds_prelight_witness_foreground_manifest.json',json.dumps(dict(consumer_boundary=cut)))
        z.writestr(QUEUE+'ds_prelight_events_0.jsonl','\n'.join(json.dumps(x) for x in rows))
    return buf

if __name__=='__main__':
    with zipfile.ZipFile(fixture()) as z:
        r=audit(z,9)
    assert r['status']=='LINKED_NATIVE_VERSION' and not r['same_frame'] and len(r['uses'])==1
    for c in ('wrong_version','wrong_allocation','wrong_hash','wrong_layer','future_producer','wrong_consumer','unstable','corrupt'):
        try:
            with zipfile.ZipFile(fixture(c)) as z:audit(z,9)
        except ValueError:continue
        raise AssertionError(c)
    print('cross-frame native linkage + 8 negative controls')
