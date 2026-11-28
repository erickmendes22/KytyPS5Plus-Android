#!/usr/bin/env python3
import numpy as np
import v4_relight_slice as r
def reject(fn):
    try:fn()
    except (ValueError,KeyError,AssertionError):return
    raise AssertionError('invalid data accepted')
def main():
    assert r.WORDS==466952
    assert r.valid_layer(r.bits(19)) and r.valid_layer(r.bits(0))
    assert not r.valid_layer(r.bits(1.4503871202468872))
    assert not r.valid_layer(r.bits(-1)) and not r.valid_layer(0x7f800000)
    assert sorted(r.wave_lane(i) for i in range(256))==[(g,l) for g in range(4) for l in range(64)]
    assert r.half_rtz([2.5017981897690333e-5,2.7074676836491562e-5])==[419,454]
    assert r.half_rtz([-2.5017981897690333e-5])==[0x8000+419]
    d=[0,71<<20,0,(13<<28)|0xfac,359,0,0,0];sam=[0]*4
    record=[r.bits(.5),r.bits(.5),r.bits(19),*d,*sam]
    rep=dict(descriptor=d,sampler=sam,extent=[8,8],base_layer=0,subtexel_bits=8,sampler_state=dict(mag=1,min=1,reduction=0,unnormalized=False,mip_filter=0,min_lod=0,compare=0))
    fp=dict(xy=[3,3],layer=19,texels=[419,419,419,0]*4)
    want,tol=r.sample_reference(0x172c,record,fp,rep)
    assert all(abs(x-float(np.array(419,'<u2').view('<f2')))<tol for x in want)
    reject(lambda:r.sample_reference(0x172c,record,dict(fp,layer=20),rep))
    reject(lambda:r.sample_reference(0x172c,record,dict(fp,texels=[0]),rep))
    other,_=r.sample_reference(0x172c,record,dict(fp,texels=[2000,2000,2000,0]*4),rep)
    assert np.max(abs(want-other))>tol
    raw=[0]*512;meta=dict(schema=1,probe=r.PROBE,position=r.POSITION,groups=[2,2,1],z=0,map=[r.PROBE],ray_words=raw,bytes=r.WORDS*4)
    a=np.zeros(r.WORDS,'<u4');a[:5]=[3,0,r.PROBE,0,1]
    for ray in range(256):
        off=8+ray*r.RAY_WORDS
        for pc,corner,regs in r.SITES:
            if pc in (0x120,0x17bc,0x17f8,0x18a8,0x18c0,0x1b60,0x1b88):
                a[off]=pc+1
                if pc==0x120:a[off+2:off+6]=[r.PROBE,r.PROBE*256+ray,0,0]
            off+=(8 if corner else 1)*32
    assert len(r.parse(a.tobytes(),meta))==256
    b=a.copy();b[8+4]=1;reject(lambda:r.parse(b.tobytes(),meta))
    reject(lambda:r.parse(a[:-1].tobytes(),meta))
    b=a.copy();b[3]=1;reject(lambda:r.parse(b.tobytes(),meta))
    print('V4 relight: 256 unique rays, trace and raw binding, exact signed FP16 truncation, footprint equality/mutation and missing-data controls PASS')
if __name__=='__main__':main()
