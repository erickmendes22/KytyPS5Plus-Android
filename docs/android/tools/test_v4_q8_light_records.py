#!/usr/bin/env python3
"""Native/guest publication and shader light-filter fixtures, not game results."""
import copy
import numpy as np
import v4_q8_contract as q
import v4_q8_light_records as m

def reject(fn,typ,word):
    try: fn()
    except typ as e: assert word in str(getattr(e,'row',e)),str(e)
    else: raise AssertionError('mutation was accepted: '+word)

def main():
    native=np.zeros((m.RECORD_COUNT,m.RECORD_WORDS),dtype='<u4')
    for i,flag in enumerate((0x1b,0x1b,0x21)):
        native[i,0]=3;native[i,0x5c//4]=flag
        for off in (0xe0,0xe4,0xe8):native[i,off//4]=q.bits32(.00828 if i==0 else .4)
    meta={'guest':0x30000,'bytes':m.BYTES,'native':42,'native_offset':4096,'tick':11}
    state={'valid':True,'registered':True,'native':42,'address':0x30000,'buffer_guest':0x2f000,
        'buffer_size':m.BYTES+4096,'bytes':m.BYTES,'unregister_epoch':2,'gpu_generation':3,**{k:False for k in m.DIRTY}}
    event={name:{'ok':True,'guest':meta['guest'],'bytes':m.BYTES,'file':'cpu','state_before':copy.deepcopy(state),
        'state_after':copy.deepcopy(state),'tick_before':10+i,'tick_after':10+i} for i,name in enumerate(m.BACKINGS)}
    files={'cpu':native.tobytes()}
    assert m.publication(event,meta,native.tobytes(),files)['status']=='MATCH'
    bad=native.copy();bad[2,0x5c//4]=1
    result=m.publication(event,meta,bad.tobytes(),files)
    assert result['status']=='DIVERGENCE' and result['different_fields']==[(2,0x5c)]
    for k in m.DIRTY:
        x=copy.deepcopy(event);x[m.BACKINGS[1]]['state_after'][k]=True
        assert m.publication(x,meta,bad.tobytes(),files)['status']=='INCOMPLETE'
    x=copy.deepcopy(event);x[m.BACKINGS[1]]['state_after']['native']=99
    assert m.publication(x,meta,bad.tobytes(),files)['status']=='INCOMPLETE'
    x=copy.deepcopy(event);x[m.BACKINGS[1]]['file']='changed'
    assert m.publication(x,meta,bad.tobytes(),{**files,'changed':bad.tobytes()})['status']=='INCOMPLETE'
    assert m.publication(event,meta,bad.tobytes(),{'cpu':b'bad'})['status']=='INCOMPLETE'
    r=np.zeros((q.TERMS,m.STRIDE),'<u4');t=np.zeros((1+2*q.TERMS,q.TERM),'<u4')
    d=[meta['guest'],384<<16,m.RECORD_COUNT,0x5204]
    for i in range(3):
        flag=int(native[i,0x5c//4]);mask=32&flag
        r[i,:8]=[0xdf5,i,flag,32,0xdf5,mask,0xdf9,int(not mask)];r[i,34:38]=d
        if not mask:r[i,8]=0xe69;r[i,9:17]=[native[i,off//4] for off in m.FIELDS]
        t[1+q.TERMS+i,0]=0x1d5d;t[1+q.TERMS+i,10]=0x1d65
    result=m.compare_records(q.Checks(),r,t,[0,1,2],42,86,d,native)
    assert [x['admitted'] for x in result]==[True,True,False]
    for col,word in [(2,'record_flag_load'),(3,'material_category_bit'),(5,'category_and'),(7,'category_exec'),(11,'dynamic_scalar_load')]:
        badr=r.copy();badr[0,col]^=1
        reject(lambda:m.compare_records(q.Checks(),badr,t,[0,1,2],42,86,d,native),q.Divergence,word)
    badr=r.copy();badr[2,:]=0
    reject(lambda:m.compare_records(q.Checks(),badr,t,[0,1,2],42,86,d,native),ValueError,'missing/extra')
    badr=r.copy();badr[0,34]^=1
    reject(lambda:m.compare_records(q.Checks(),badr,t,[0,1,2],42,86,d,native),ValueError,'descriptor')
    badt=t.copy();badt[1+q.TERMS+2,2]=q.bits32(.01)
    reject(lambda:m.compare_records(q.Checks(),r,badt,[0,1,2],42,86,d,native),q.Divergence,'rejected_light_zero')
    a=np.zeros(q.WORDS+q.TERMS*m.STRIDE,'<u4');a[:3]=[1,287,42]
    q.parse_trace(a.tobytes(),287,42)
    reject(lambda:q.parse_trace(a[:-1].tobytes(),287,42),ValueError,'truncated')
    print('V4 q8 record contract: PASS (publication, dynamic loads, category and EXEC, authority/identity/truncation)')

if __name__=='__main__':main()
