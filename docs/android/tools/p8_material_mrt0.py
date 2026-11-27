#!/usr/bin/env python3
"""Independent MRT0 dataflow slice for the exact #753 sky material.

The guest x4 load has a dead fourth result (s3); no unknown byte is supplied.
This is an admitted-fragment export reference, NOT a coverage or P8 certificate.
The full original shader is required for fingerprinting and optional equivalence
fixtures. Never distribute game binaries with the tool.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import zipfile

from p8_guest_graphics import run_wave, stage_memory, decode_all
from prelight_vertex_oracle import Capture, LLVM
from p8_version_link import audit as version_link

SHADER = '552d823dcfcf2dee'
SHA = '83029a3d12b156907caa27aef180f9c338b4611a29b9ab214fb8feb587b245ce'
MODES = (0xc0, 0x1c0, 0x3c0)
# Backward dependency closure of v2 at EXP MRT0:
# s[0:2] -> log -> multiply -> exp -> 255 -> packed byte 2,1,0.
# s4/s5 restore original EXEC before export. The slice starts at an admitted
# fragment; interpolation and other MRTs cannot determine these three bytes.
PCS = (0x170,0x180,0x184,0x18c,0x190,0x1c0,0x1c8,0x1f4,0x1f8,
       0x208,0x228,0x234,0x23c,0x264,0x270,0x278,0x298,0x2a0,0x2c4,
       0x364,0x38c)


def require(ok, reason):
    if not ok: raise ValueError(reason)


def pack(*words):
    return struct.pack('<%dI'%len(words),*words)


def make_slice(original, decoder):
    require(hashlib.sha256(original).hexdigest()==SHA, 'unsupported guest binary')
    decoded=decode_all(decoder,original)
    lengths={pc:(decoded[i+1][0]-pc if i+1<len(decoded) else 4)
             for i,(pc,_) in enumerate(decoded)}
    # The exact original SMEM x4, and no s3 operand after it, are part of
    # the fingerprinted dependency proof. Contract only the dead fourth word.
    require(original[0x184:0x18c]==pack(0xf4080035,0xfa000010), 'unexpected load')
    live=pack(0xf4040035,0xfa000010,0xf40000b5,0xfa000018)
    code=b''.join(live if pc==0x184 else original[pc:pc+lengths[pc]] for pc in PCS)
    ops=decode_all(decoder,code)
    require([s for _,s in ops[:4]]==[
        's_load_dwordx2 vcc, s[0:1], 0x18', 's_waitcnt lgkmcnt(0)',
        's_load_dwordx2 s[0:1], vcc, 0x10', 's_load_dword s2, vcc, 0x18'],
        'contracted live read did not decode')
    require(ops[-2][1]=='exp mrt0 v2, off, off, off vm', 'wrong export')
    return code


def export(executable, code, user_data, segments, mode, poison=0, full=False):
    require(len(user_data)==2,'unexpected PS user-data ABI')
    sg=[poison]*106; sg[:2]=user_data
    vg=[poison]*(64*256)
    lds=None
    if full:
        # Synthetic admitted fragment fixture, not an observation of the game.
        sg[2]=0
        vg[0]=0x3e800000;vg[64]=0x3e800000
        lds=pack(*([0x3f800000]*24))
    result=run_wave(executable,code,sg,vg,segments,mode,1,lds)
    row=result.get((0,0))
    require(row is not None and row[:2]==(1,0),'missing native-width MRT0 export')
    return row[2][0]


def fixture_check(original, sliced, executable):
    # Deliberately artificial input ranges and fourth DWORD; no missing game
    # byte is inferred from them. Compare full independent execution to slice.
    checks=0
    for rgb in ((0,0,0),(0x3f800000,0x3e800000,0x3f000000),
                (0x3e000000,0x3f400000,0x40000000)):
        for dead in (0,0xdeadbeef,0x7fc00000):
            parts=[(0x200008,pack(0x300000,0)),(0x200018,pack(0x400000,0)),
                   (0x3002a8,pack(0,0)),(0x400010,pack(*rgb,dead)),
                   (0x400020,pack(0))]
            live=[(0x200018,pack(0x400000,0)),(0x400010,pack(*rgb))]
            for mode in MODES:
                want=export(executable,original,[0x200000,0],parts,mode,full=True)
                got=export(executable,sliced,[0x200000,0],live,mode)
                require(want==got,'MRT0 full/slice fixture disagreement')
                checks+=1
    try:
        export(executable,sliced,[0x200000,0],
               [(0x200018,pack(0x400000,0)),(0x400010,pack(0,0))],MODES[0])
    except ValueError:
        pass
    else:
        raise ValueError('missing live scalar accepted')
    return checks


def audit(path, executable, check=False):
    cap=Capture(path); dec=LLVM()
    try:
        record=cap.json('queue-state/ds_prelight_witness_foreground_599.json')
        require(record.get('ps_hash')==SHADER and record.get('ps_user_data_base')==0,
                'wrong material identity or launch ABI')
        original=cap.read('guest-shaders/ps_'+SHADER+'_probe.bin')
        code=make_slice(original,dec)
        segments,missing=stage_memory(cap,record,'ps')
        require(not missing,'missing live scalar/buffer ranges')
        results=[export(executable,code,record['ps_user_data'],segments,mode,poison)
                 for mode in MODES for poison in (0,0x7fa54321)]
        require(len(set(results))==1,'MRT0 sensitive to unobserved launch state')
        expected=results[0]
        with zipfile.ZipFile(path) as z:
            link=version_link(z,599)
            raw=z.read('p8-reference/'+link['payload'])
        require(record['frame']==link['producer']['frame'] and
                record['submit']==link['producer']['submit'],'wrong material episode')
        hist=Counter(x[0] for x in struct.iter_unpack('<I',raw))
        return {'status':'MRT0_REFERENCE_EXECUTED','scope':'admitted fragment scalar export only; no coverage, input producer or global P8 certificate',
                'draw':599,'shader':SHADER,'guest_sha256':SHA,'slice_sha256':hashlib.sha256(code).hexdigest(),
                'frame':record['frame'],'submit':record['submit'],'boundary':link['consumer'],
                'expected_word':hex(expected),'actual_pixels_equal_word':hist[expected],
                'actual_pixels_total':len(raw)//4,'actual_other_words':len(raw)//4-hist[expected],
                'actual_payload_sha256':link['sha256'],
                'mode_variants_tested':list(MODES),'unobserved_actual_mode':True,
                'unknown_fourth_word':'not read by slice; no value imputed',
                'full_vs_slice_synthetic_checks':fixture_check(original,code,executable) if check else None,
                'p8_decision':'NOT_A_GLOBAL_CERTIFICATE'}
    finally:
        dec.close();cap.close()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('archive',type=Path);p.add_argument('runner',type=Path)
    p.add_argument('--check-fixtures',action='store_true');p.add_argument('--output',type=Path)
    a=p.parse_args();r=audit(a.archive,a.runner,a.check_fixtures);s=json.dumps(r,indent=2)+'\n'
    if a.output:a.output.write_text(s)
    print(s,end='')


if __name__=='__main__':main()
