#!/usr/bin/env python3
"""Exercise a real RDNA instruction stream, not a mocked expected result."""
import pathlib
import struct
import subprocess
import sys
import tempfile

def main():
    exe=pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as directory:
        root=pathlib.Path(directory)
        # v_mov_b32 v0, 3; buffer_store_format_x v0, v1, s[0:3], 0 idxen; s_endpgm
        (root/"guest.bin").write_bytes(struct.pack("<4I",0x7e000283,0xe0102000,0x80000001,0xbf810000))
        (root/"data.bin").write_bytes(bytes(256))
        def invoke(addr, descriptor_addr, expected):
            # Explicit scalar/vector launch registers; v1 = 0 for lane 0.
            manifest=root/"manifest.txt"
            manifest.write_text(
                f'P8ISA1 rdna1 10000 1 0 64 4 2 10000 "{root/"guest.bin"}"\n'
                f'{descriptor_addr:x} 40000 40 14004\n' + '0 '*128 +
                f'\n1\n{addr:x} "{root/"data.bin"}"\n1\n{addr:x} 4\n')
            result=subprocess.run([exe,manifest],capture_output=True,text=True,timeout=30)
            print(result.stdout,result.stderr,sep="")
            if expected is None:
                assert result.returncode!=0,"uncaptured write accepted"
            else:
                assert result.returncode==0
                assert f"MEM {addr:x} {expected}" in result.stdout
                header=next(x for x in result.stdout.splitlines() if x.startswith("P8ISA_RESULT1"))
                assert int(header.split()[2])>0,"no architectural memory access was observed"
                assert f"MEM {addr:x} 00000000" not in result.stdout
        invoke(0x4000,0x4000,"03000000")
        invoke(0x4000,0x5000,None)

        # Vertex position export, all four channels, two different lanes.
        # EXP encoding is ordinary GFX10 ISA (no game code).
        def graphics(code, sgprs, vgprs, mask, lds=None):
            (root/"guest.bin").write_bytes(struct.pack("<"+"I"*len(code),*code))
            if lds is not None:(root/"lds.bin").write_bytes(lds)
            m=root/"graphics.txt"
            m.write_text(f'P8ISA2 rdna1 10000 {mask:x} c0 64 {len(sgprs)} {len(vgprs)//64} 10000 "{root/"guest.bin"}"\n'
                +' '.join(f'{v:x}' for v in sgprs)+'\n'+' '.join(f'{v:x}' for v in vgprs)+'\n0\n0\n'
                +('"-"' if lds is None else f'"{root/"lds.bin"}"')+'\n')
            r=subprocess.run([exe,m],capture_output=True,text=True,timeout=30)
            assert r.returncode==0,(r.stdout,r.stderr)
            return [x.split() for x in r.stdout.splitlines() if x.startswith("GFXEXP ")]
        v=[0]*256
        for reg in range(4):
            v[reg*64]=0x3f800000+reg
            v[reg*64+1]=0x40000000+reg
        rows=graphics([0xf80008cf,0x03020100,0xbf810000],[0],v,3)
        assert rows==[['GFXEXP','c','f','0','0','1','3','3f800000','3f800001','3f800002','3f800003','40000000','40000001','40000002','40000003']],rows
        # One primitive, attr0.x: P0=1, P10=2, P20=4. I=.25 J=.5.
        # The two interpolation instructions must calculate 3.5.
        coeff=[0x3f800000,0x40000000]+[0]*6+[0x40800000]+[0]*3
        v=[0]*(9*64);v[0]=0x3e800000;v[64]=0x3f000000
        rows=graphics([0xbefc0302,0xc8200000,0xc8210001,0xf8000801,8,0xbf810000],
                      [0,0,0],v,1,struct.pack('<12I',*coeff))
        assert len(rows)==1 and rows[0][7]=='40600000',rows
        print("Graphics export and explicit interpolation LDS fixtures passed")

if __name__=="__main__":
    main()
