#!/usr/bin/env python3
import copy
import numpy as np
import v4_q8_contract as q

def bad(e,a,role='dark'):
    try:q.select_witness(e,a,role)
    except ValueError:return
    raise AssertionError('bad paired trace accepted')

def main():
    a=np.zeros(q.PAIR_WORDS,'<u4');a[:5]=[1,17,0,0,2]
    a[q.TERM_BASE]=0x881;a[q.SINGLE_WORDS]=0xc11
    a[q.OUTER_BASE]=0xd69
    e=dict(schema=3,trace_bytes=a.nbytes,wg=17,lane=0,pixel=[752,664],stencil=86,
        material_word=0xfba3b4c6,dark_luminance_before=0.,pair_contrast_before=True,
        control=dict(lane=2,pixel=[754,664],stencil=86,material_word=0xfdb0c9e0,luminance_before=.074618))
    d,dt=q.select_witness(e,a.tobytes(),'dark');c,ct=q.select_witness(e,a.tobytes(),'control')
    assert d['lane']==0 and c['lane']==2 and c['pixel']==[754,664]
    f,o,t,s=q.parse_trace(dt,17,0);assert t[0,0]==0x881 and o[0,0,0]==0xd69
    f,o,t,s=q.parse_trace(ct,17,2);assert t[0,0]==0xc11 and o[0,0,0]==0xd69
    assert len(dt)==len(ct)==q.SINGLE_WORDS*4
    bad(e,a[:-1].tobytes())
    b=a.copy();b[3]=1;bad(e,b.tobytes())
    b=a.copy();b[4]=0;bad(e,b.tobytes())
    for k,v in [('lane',0),('pixel',[752,664]),('stencil',90),('material_word',0)]:
        z=copy.deepcopy(e);z['control'][k]=v;bad(z,a.tobytes())
    z=copy.deepcopy(e);z['dark_luminance_before']=.074618;bad(z,a.tobytes())
    print('V4 pair trace: PASS (both disjoint tails and shared wave, wrong lane/contrast/overflow/truncation rejection)')
if __name__=='__main__':main()
