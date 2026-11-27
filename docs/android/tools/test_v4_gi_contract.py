#!/usr/bin/env python3
import struct
import numpy as np
from v4_gi_contract import *
def rejects(fn):
    try:fn()
    except (ValueError,KeyError):return
    raise AssertionError('invalid acquisition accepted')
def main():
    # Exact 3D lookup, mutation and wrong-coordinate controls.
    n=(0x300000000>>8)|(20<<52)|(31<<62)|(23<<78)|(4<<96)|(10<<124)
    d=list(struct.unpack('<8I',n.to_bytes(32,'little')))
    ident=dict(owner=0x300000000,object=1,native=2,version=3,write_order=4,width=32,height=24)
    view=dict(mip=0,levels=1,layer=0,layers=1,format=98)
    entry=dict(guest=ident['owner'],image=ident,view=view)
    raw=dict(image_descriptors=[[]]*4+[d],buffer_descriptors=[])
    row=dict(descriptor=d,coords=[1,2,3],sampler=[0]*4,result=[123])
    fp=dict(ok=True,slot=4,identity=ident,view=view,extent=[32,24,17],xyz=[1,2,3],side=1,texel_bytes=4,bytes=list(pack([123])))
    assert footprint_reference(0x5a4,row,fp,entry,raw)==([123],0)
    assert footprint_reference(0x5a4,row,dict(fp,bytes=list(pack([124]))),entry,raw)[0]!=row['result']
    rejects(lambda:footprint_reference(0x5a4,row,dict(fp,xyz=[1,2,4]),entry,raw))
    rejects(lambda:footprint_reference(0x5a4,row,dict(fp,identity=dict(ident,version=4)),entry,raw))
    rejects(lambda:footprint_reference(0x5a4,row,dict(fp,bytes=[0]),entry,raw))
    desc=[0,3|(12<<16),2,81924];raw['buffer_descriptors']=[desc]
    br=dict(descriptor=desc,coords=[1],result=[4,5,6])
    assert read_buffer(0x618,br,[(0x300000000,pack([1,2,3,4,5,6]))],raw)==br['result']
    assert read_buffer(0x618,br,[(0x300000000,pack([1,2,3,4,5,7]))],raw)!=br['result']
    rejects(lambda:read_buffer(0x618,dict(br,coords=[2]),[(0x300000000,pack([1,2,3,4,5,6]))],raw))
    a=np.zeros(WORDS,'<u4');a[:5]=[2,3,4,0,5]
    for wi in range(2):
        at=SITE_BASE+wi*WITNESS
        for site in SITES:
            pc=site[0]
            if pc in (0x104,0x13c,0x178,0x184,0x1a0,0x3e4,0x508,0x618,0x112c,0x1174):a[at:at+2]=pc+1
            at+=(129 if site[3] else 1)*STRIDE
    assert len(parse(a.tobytes(),3,4,5)[1])==2
    b=a.copy();b[SITE_BASE+1]=0;rejects(lambda:parse(b.tobytes(),3,4,5))
    rejects(lambda:parse(a[:-1].tobytes(),3,4,5))
    print('V4 q33 lookup: byte equality/mutation, identity/range, paired trace census PASS')
if __name__=='__main__':main()
