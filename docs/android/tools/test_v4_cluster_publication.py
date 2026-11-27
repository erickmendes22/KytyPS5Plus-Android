#!/usr/bin/env python3
import numpy as np
from v4_cluster_publication import compare,bound,writer_key,WRITER

def reject(fn):
    try:fn()
    except (ValueError,KeyError):return
    raise AssertionError('invalid acquisition accepted')

def main():
    p=np.array([1,15,0,0,1,3,0,0],'<u4')
    assert compare(p.tobytes(),p.tobytes(),[0,1,4,5])['status']=='MATCH_CONSUMED'
    b=p.copy();b[2]=7
    v=compare(p.tobytes(),b.tobytes(),[0,1,4,5])
    assert v['status']=='MATCH_CONSUMED' and v['different_words']==1 and not v['different_consumed_words']
    b[1]=31
    v=compare(p.tobytes(),b.tobytes(),[0,1,4,5])
    assert v['status']=='CHANGED_CONSUMED' and v['first_consumed_differences']==[dict(index=1,producer=15,consumer=31)]
    for idx in ([],[-1],[8]):reject(lambda:compare(p.tobytes(),b.tobytes(),idx))
    reject(lambda:compare(p.tobytes(),b[:-1].tobytes(),[1]))
    row=dict(ok=True,slot=0,guest=0x1000,bytes=32,native=7,binding_offset=64,native_offset=64,tick_before=3,tick_after=4,file='p')
    stage=dict(buffers=[dict(slot=0,guest=0x1000,bytes=32,native=7,native_offset=64,native_range=32,access_known=True,written=False)])
    assert bound(row,stage,0,{'p':p.tobytes()},False)==p.tobytes()
    for k,x in [('native',8),('guest',0x2000),('native_offset',68),('tick_after',2),('ok',False)]:
        reject(lambda:bound(dict(row,**{k:x}),stage,0,{'p':p.tobytes()},False))
    reject(lambda:bound(row,stage,0,{'p':p.tobytes()[:-4]},False))
    w=dict(shader=WRITER,frame=358,submit=100,guest=0x1000,bytes=32,serial=91,indirect=False,fill=False)
    assert writer_key(w)==(WRITER,358,100,0x1000,32,91)
    reject(lambda:writer_key(dict(w,serial=0)));reject(lambda:writer_key(dict(w,fill=True)))
    print('V4 cluster publication: equal/changed consumed words, unrelated changes, invalid identity/length/order PASS')
if __name__=='__main__':main()
