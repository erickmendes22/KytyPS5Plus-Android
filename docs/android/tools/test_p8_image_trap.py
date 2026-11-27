#!/usr/bin/env python3
"""Exercise actual guest encodings, repeated dynamic reads, SCC, and failure paths."""
import struct,sys
from prelight_vertex_oracle import LLVM
from p8_guest_graphics import pack,decode_all
from p8_image_trap import execute,exp,floatbits,jump,LogicalImages
import numpy as np

# image_sample v[0:3], v[8:9], s[0:7], s[24:27], dmask=f, 2D.
# Exact instruction encoding is validated by LLVM before use.
MIMG=(0xf0800f08,0xc00008)

def test(runner):
    d=LLVM()
    try:
        # Read twice at the same static PC; SCC=1 on entry, must remain 1.
        # s_mov_b32 s32,0; loop:s_cmp_eq_u32 0,0; image_sample; 
        # s_cbranch_scc0 error; s_add_u32 s32,s32,1; s_cmp_eq_u32 s32,2;
        # s_cbranch_scc0 loop; exp MRT0 v0..3; end; error before final end.
        # Use an error flag in v0 and branch to common final export.
        code=bytearray(pack([0xbea00380,0xbf068080,*MIMG,0xbf840004,0x80208120,0xbf068220,0xbf840000,0xbf820001,0x7e000280]))
        # Branch at word7 loops to word1. Error at word4 jumps to word9.
        code[28:32]=jump(28,4,0xbf840000)
        code+=exp(0,[0,1,2,3],True)+pack([0xbf810000])
        rows=decode_all(d,bytes(code));print(rows);assert rows[2][1].startswith('image_sample '),rows
        sg=[0]*106;vg=[0]*(64*256);seen=[]
        def sample(read,desc,samp,coords):
            seen.append((read.pc,coords));return {lane:(floatbits(float(len(seen))),1,2,3) for lane in coords}
        res,trace=execute(d,runner,bytes(code),sg,vg,[],0x1c0,15,None,sample)
        assert len(seen)==2 and trace[0]['pc']==trace[1]['pc'] and trace[1]['visit']==2,(trace,res)
        assert all(res[0,l][2]==(floatbits(2.),1,2,3) for l in range(4)),res
        try:execute(d,runner,bytes(code),sg,vg,[],0x1c0,15,None,sample,max_visits=1)
        except ValueError:pass
        else:raise AssertionError('loop budget failure accepted')
        try:execute(d,runner,bytes(code),sg,vg,[],0x1c0,15,None,lambda *a:{})
        except ValueError:pass
        else:raise AssertionError('missing lane data accepted')
        print('dynamic image read, SCC preservation, visit budget and missing-lane rejection passed')
    finally:d.close()
if __name__=='__main__':test(sys.argv[1])
