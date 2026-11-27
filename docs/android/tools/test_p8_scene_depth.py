#!/usr/bin/env python3
"""Original overlay PS, synthetic depth and launch; actual #756 flip consumer.
No synthetic input is an expected output or an independent game reference.
"""
import sys,struct
from pathlib import Path
from p8_scene_replay import *
from p8_image_trap import floatbits

def run(archive,runner):
    a=p.Archive(archive);dec=LLVM()
    try:
        r=Replay(a);code=shader(a,'ps',OVERLAY)
        root,A,B,C=0x4000000,0x4100000,0x4200000,0x4300000
        cb=bytearray(0x268);b=bytearray(0xa60);s=bytearray(0xd0)
        def f(buf,off,*vs):struct.pack_into('<%df'%len(vs),buf,off,*vs)
        f(cb,0x48,1/64,1/64);f(cb,0x80,1);f(cb,0x90,1,0);f(b,0x60,0,0,1,0,0,0,0,1);f(b,0x490,1,1,1,0);f(b,0x4a0,1/64,1/64)
        D,V,Q=(tuple([j]+[0]*7) for j in (0x10000,0x10001,0x10002));sam=(146,0,0x500000,0)
        cb[0x138:0x158]=pack(D)
        for off in range(0x158,0x258,32):cb[off:off+32]=pack(Q)
        cb[0x258:0x268]=pack(sam);cb[0x118:0x138]=pack(D);b[0xa40:0xa60]=pack(V)
        for off in (0xa0,0xb0,0xc0):s[off:off+16]=pack(sam)
        textures={Q:Texture({(0,0):np.full((1,64,64,4),.5,np.float32)},dim='SQ_RSRC_IMG_2D_ARRAY',extent=(64,64,1)),D:Texture({(0,0):np.full((1,64,64,4),.5,np.float32)},extent=(64,64,1)),V:Texture({(0,0):np.full((2,2,2,4),.5,np.float32)},dim='SQ_RSRC_IMG_3D',extent=(2,2,2))}
        parts=[(root,struct.pack('<QQQ',A,B,C)),(A,bytes(cb)),(B,bytes(b)),(C,bytes(s))]
        params={0:[floatbits(1.)]*4,1:[floatbits(.5)]*4,2:[floatbits(.5)]*4,3:[0]*4,4:[0,floatbits(1),floatbits(1),floatbits(1)],5:[0]*4,6:[0,0,8,4<<24]}
        lds=[0]*(7*12)
        for attr,values in params.items():
            for c,word in enumerate(values):lds[12*attr+2*c]=word
        sg=[0]*106;sg[:2]=[root,0];vg=[0]*16384
        for lane in range(64):
            for k,value in enumerate((.2,.2,32.5,32.5,.1)):vg[k*64+lane]=floatbits(value)
        def sample_depth(value):
            textures[D].levels[(0,0)][:]=value
            logical=VersionedTextures(textures,{sam:Sampler('linear','linear','none',('clamp',)*3)})
            exp,trace=execute(dec,runner,code,sg,vg,parts,0xc0,2**64-1,pack(lds),logical)
            assert [q['pc'] for q in trace]==[0x4a8,0x4b0,0x6d8,0x898],trace
            assert exp[0,0][:2]==(15,1) and exp[1,0][:2]==(15,1)
            assert logical.reads[2]['descriptor']==D
            return exp[0,0][2]
        initial=sample_depth(.5);same=sample_depth(.5);other=sample_depth(.75)
        assert initial==same and initial!=other,(initial,same,other)
        quad=r.quad(960,540);base,reads,_=r.execute(dec,runner,quad)
        support={t for q in reads if q['descriptor']==r.slots[1] for t in q['texels_by_lane'].get(0,[])}
        def flip(word):
            ims={k:v.copy() for k,v in r.images.items()};color=np.frombuffer(struct.pack('<4H',*word),'<f2').astype(np.float32)
            for x,y in support:ims[r.slots[1]][y,x]=color
            return r.execute(dec,runner,quad,ims)[0][0]
        first=flip(initial);second=flip(other)
        assert first!=second,(first,second)
        print('original overlay PS: depth/array/volume reads, both MRTs; same-input equality and changed-depth propagation through actual flip shader',hex(first),hex(second))
        del textures[D]
        try:sample_depth(.5)
        except (ValueError,KeyError):pass
        else:raise AssertionError('missing depth accepted')
    finally:dec.close();a.close()
if __name__=='__main__':run(Path(sys.argv[1]),sys.argv[2])
