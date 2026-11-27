import struct
import sys
from p8_graphics_contract import execute


def pack(words):
    return struct.pack('<%dI'%len(words), *words)


def main(executable):
    code = pack([0xf80008cf,0x03020100,0xbf810000])
    v=[0]*256
    for reg in range(4):
        v[reg*64]=0x3f800000+reg
        v[reg*64+1]=0x40000000+reg
    inputs=[code,struct.pack('<QI',3,0xc0),pack([0]),pack(v)]
    params=dict(wave=64,addresses=[],lds=False,target=12,enabled=15,compressed=0,lanes=3)
    baseline=execute(executable,params,inputs)
    assert baseline==pack([0x3f800000+i for i in range(4)]+[0x40000000+i for i in range(4)])
    v[0]=0x40800000
    changed=execute(executable,params,[*inputs[:3],pack(v)])
    assert changed!=baseline and changed[:4]==pack([0x40800000]) and changed[4:]==baseline[4:]
    # Compressed MRT export: two packed half registers become four half words.
    v=[0]*128;v[0]=0x40003c00;v[64]=0x44004200
    compressed=[pack([0xf8001c0f,0x100,0xbf810000]),struct.pack('<QI',1,0xc0),pack([0]),pack(v)]
    assert execute(executable,{**params,'target':0,'compressed':1,'lanes':1},compressed)==pack([0x3c00,0x4000,0x4200,0x4400])
    coeff=[0x3f800000,0x40000000]+[0]*6+[0x40800000]+[0]*3
    v=[0]*(9*64);v[0]=0x3e800000;v[64]=0x3f000000
    inp=[pack([0xbefc0302,0xc8200000,0xc8210001,0xf8000801,8,0xbf810000]),
         struct.pack('<QI',1,0xc0),pack([0,0,0]),pack(v),pack(coeff)]
    par={**params,'lds':True,'target':0,'enabled':1,'lanes':1}
    assert execute(executable,par,inp)==pack([0x40600000])
    coeff[8]=0x41000000
    assert execute(executable,par,[*inp[:4],pack(coeff)])==pack([0x40b00000])
    for p,i in [({**params,'target':13},inputs),({**params,'enabled':7},inputs),
                ({**params,'lanes':1},inputs),({**params,'lanes':7},inputs),
                ({**params,'compressed':1},inputs),({**params,'addresses':[0x4000]},inputs),
                (par,inp[:4]),({**params,'addresses':[0x4000,0x4002]},[*inputs,b'1234',b'5678'])]:
        try:execute(executable,p,i)
        except ValueError:pass
        else:raise AssertionError('accepted incomplete or mismatched graphics contract')
    print('graphics export: equality, changed vertex, compressed MRT, LDS causality, 8 negative controls passed')

if __name__=='__main__':main(sys.argv[1])
