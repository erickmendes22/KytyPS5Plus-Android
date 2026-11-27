#!/usr/bin/env python3
"""Real guest post shader, synthetic bound inputs, independent texture model.
This tests execution and propagation mechanics, not the game's P8 outcome.
"""
import sys,struct
from pathlib import Path
from types import SimpleNamespace
import numpy as np
from p8_scene_replay import *

def run(path,runner):
    a=p.Archive(path);dec=LLVM()
    try:
        real=Replay(a);state=dict(real.state);state.update(position_xy_vgprs=[2,3],output_mode=4,viewport=[0,0,64,64,0,1],scissor=[0,0,64,64])
        code=shader(a,'ps',POST)
        root,A,B,G=0x4000000,0x4100000,0x4200000,0x4300000
        c=bytearray(0x1a0);sm=bytearray(0x180);g=bytearray(0xbe0)
        for off in range(0,0xc0,4):struct.pack_into('<f',c,off,1.)
        struct.pack_into('<I',c,0,0)
        textures={}
        for j,off in enumerate((0xc0,0xe0,0x100,0x120,0x140,0x160,0x180,0x1a0)):
            desc=tuple([0x10000+j,0,0,0,0,0,0,0]);v=np.broadcast_to(np.array([.2,.3,.4,.5],np.float32),(1,64,64,4)).copy();textures[desc]=Texture({(0,0):v},extent=(64,64,1))
            if off+32<=len(c):c[off:off+32]=pack(desc)
        g[0xbc0:0xbe0]=pack(list(textures)[-1]);sam=(2|(2<<3),0,0x500000,0)
        for off in (0xc0,0x140,0x170):sm[off:off+16]=pack(sam)
        inp=SimpleNamespace(base=0,ud=[root,0],parts=[(root,struct.pack('<QQQ',G,A,B)),(A,bytes(c)),(B,bytes(sm)),(G,bytes(g))],logical=lambda:VersionedTextures(textures,{sam:Sampler('linear','linear','none',('clamp',)*3)}))
        result,reads=post_pixels(dec,runner,code,inp,state,64,64,[(10,10),(11,10),(10,11),(11,11)])
        assert len(result)==4 and len(reads)==10,(len(result),len(reads))
        # Independent geometry exclusion is conservative, including uncertainty.
        ps={(0,0):[-.9,-.9,.5,1],(1,0):[-.5,-.9,.5,1],(2,0):[-.5,-.5,.5,1]}
        st=dict(topology=3,primitive_restart=False,clip_transform=False,viewport_count=1,scissor_count=1,viewport=[0,0,64,64,0,1])
        assert excluded([(6,6),(40,40)],ps,[0,1,2],[0],st)=={(6,6):False,(40,40):True}
        # Test exact scene version routing with original compositor. A synthetic
        # post-shader replacement is propagated, not asserted as a game defect.
        x,y=960,540;quad=real.quad(x,y);before,reads,_=real.execute(dec,runner,quad)
        changed={k:v.copy() for k,v in real.images.items()};scene=changed[real.slots[1]]
        footprint={t for row in reads if row['descriptor']==real.slots[1] for t in row['texels_by_lane'].get(0,[])}
        word=np.frombuffer(result[(10,10)],'<f2').astype(np.float32)
        for px,py in footprint:scene[py,px]=word
        after=real.execute(dec,runner,quad,changed)[0];assert after[0]!=before[0],(word,before,after)
        print('original post shader: 10 image reads, 4 exports; conservative coverage; real flip replacement propagated',hex(before[0]),hex(after[0]))
        try:post_pixels(dec,runner,code,inp,{**state,'position_xy_vgprs':[3,4]},64,64,[(10,10)])
        except ValueError:pass
        else:raise AssertionError('unknown position ABI accepted')
        print('unverified ABI rejected')
    finally:dec.close();a.close()
if __name__=='__main__':run(sys.argv[1],sys.argv[2])
