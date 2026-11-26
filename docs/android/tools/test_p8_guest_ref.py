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

if __name__=="__main__":
    main()
