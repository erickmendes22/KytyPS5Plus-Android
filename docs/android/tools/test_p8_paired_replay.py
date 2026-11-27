#!/usr/bin/env python3
import struct,sys
from p8_paired_replay import hzb_minimum, pack10, linearize, bits
from p8_bilateral import Version,Operation,decide,operation_reference
def req(x):
    assert x
def main():
    d=struct.pack('<16f',.2,.3,.5,.6,.1,.4,.7,.8,.9,.8,.7,.6,.5,.4,.3,.2)
    req(struct.unpack('<4f',hzb_minimum(d,4,4))==struct.unpack('<4f',struct.pack('<4f',.1,.5,.4,.2)))
    for bad in [b'',struct.pack('<16f',*([float('nan')]*16)),struct.pack('<16f',*([-1.]*16)),struct.pack('<16f',*([2.]*16))]:
        try:hzb_minimum(bad,4,4)
        except ValueError:pass
        else:raise AssertionError('bad HZB source accepted')
    req(pack10((0x3800,0x3800,0x3800,0x3c00))==0xe0080200)
    req(pack10((0,0,0,0x3c00))==0xc0000000)
    owner=0x100000;new=0x200000
    n=(owner>>8)|(71<<52)|(7<<62)|(7<<78)|(0xfac<<96)|(9<<124)
    words=struct.unpack('<8I',n.to_bytes(32,'little'))
    snap=dict(format=97,aspect=1,mip=0,layer=0,extent=[8,8])
    ent=dict(guest=owner,image=dict(owner=owner))
    raw=struct.pack('<256H',*range(256))
    desc,padded=linearize(words,ent,snap,raw,new)
    req(len(padded)==8*256 and padded[:64]==raw[:64] and padded[256:320]==raw[64:128])
    req((int.from_bytes(desc,'little')&((1<<40)-1))==(new>>8))
    try:linearize(words,ent,snap,raw[:-2],new)
    except ValueError:pass
    else:raise AssertionError('truncated image accepted')
    print('paired HZB min2x2, invalid source rejection, RGBA16F linearization, RGB10 packing passed')
if __name__=='__main__':main()
