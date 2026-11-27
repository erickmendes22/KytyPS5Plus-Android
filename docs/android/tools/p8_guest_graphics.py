#!/usr/bin/env python3
"""Execute full admitted guest VS waves against recorded bytes, including PARAM exports.

This adapter uses the original GFX10 ISA runner, never translated SPIR-V for
expected values. It does not certify primitive admission or a whole P8 prefix.
Native POS0 is an independent calibration; PARAM exports are reference-only
until matching native observations or a complete material replay exist.
"""
from __future__ import annotations
import argparse, ctypes, hashlib, json, struct, subprocess, tempfile
from pathlib import Path, PurePosixPath
import numpy as np
from prelight_vertex_oracle import (Capture, LLVM, PROFILES, check_guest, merge_inputs,
                                   position_rows, compare_positions, Unsupported)
from p8_bilateral import require, Blocked, digest


def merge(parts):
    segments=[]
    for a,b in sorted(parts):
        require(type(a) is int and a>=0 and b and a+len(b)<=2**64,'invalid operand interval')
        if not segments or a>segments[-1][0]+len(segments[-1][1]):segments.append((a,b));continue
        p,d=segments[-1];off=a-p;n=min(len(d)-off,len(b))
        require(d[off:off+n]==b[:n],'conflicting scalar/native bytes')
        segments[-1]=(p,d+b[n:])
    return segments


def pack(words):return struct.pack('<%dI'%len(words),*words)


def stage_memory(capture, record, stage):
    parts=[]; missing=[]
    require(record.get(stage+'_scalar_capture_complete') is True,'incomplete scalar capture')
    for b in record[stage+'_bound']:
        if b.get('capture_status')!='complete':
            missing.append({'slot':b['slot'],'address':b['address'],'bytes':b['bytes']});continue
        if 'gpu_words' in b:raw=pack(b['gpu_words'])
        else:
            data=capture.read(b['file']);off=b['offset_bytes']
            require(0<=off<=len(data)-b['bytes'],'operand outside retained binary')
            raw=data[off:off+b['bytes']]
        require(len(raw)==b['bytes'],'truncated native buffer')
        parts.append((b['address'],raw))
    for r in record[stage+'_scalar_regions']:
        require(r['source']=='consumed_srt' and r['bytes']==4*len(r['words']),'invalid scalar region')
        parts.append((r['address'],pack(r['words'])))
    return merge(parts),missing


def decode_all(decoder, code):
    raw=ctypes.create_string_buffer(code);off=0;result=[]
    while off<len(code):
        out=ctypes.create_string_buffer(1024)
        n=decoder.lib.LLVMDisasmInstruction(decoder.ctx,ctypes.byref(raw,off),len(code)-off,off,out,len(out))
        require(n and n<=len(code)-off,'undecodable guest instruction at %#x'%off)
        text=out.value.decode().strip();result.append((off,text));off+=n
        if text=='s_endpgm':return result
    raise Blocked('missing guest termination')


def run_wave(executable, code, sg, vg, segments, mode, mask, lds=None):
    require(len(code)%4==0 and code and len(sg)==106 and len(vg)%64==0 and 0<len(vg)//64<=256,'invalid wave ABI')
    require(0<mask<2**64,'invalid EXEC')
    pc=0x10000
    while any(a<pc+len(code) and pc<a+len(b) for a,b in segments):pc+=0x100000
    with tempfile.TemporaryDirectory(prefix='p8-gfx-') as d:
        root=Path(d);codefile=root/'guest.bin';codefile.write_bytes(code)
        lines=[f'P8ISA2 rdna1 {pc:x} {mask:x} {mode:x} 64 106 {len(vg)//64} 1000000 "{codefile}"',
               ' '.join(f'{v:x}' for v in sg),' '.join(f'{v:x}' for v in vg),str(len(segments))]
        for i,(a,b) in enumerate(segments):
            f=root/f'mem{i}.bin';f.write_bytes(b);lines.append(f'{a:x} "{f}"')
        lines.append('0')
        if lds is None:lines.append('"-"')
        else:
            require(0<len(lds)<=65536 and len(lds)%4==0,'invalid LDS bytes')
            f=root/'lds.bin';f.write_bytes(lds);lines.append(f'"{f}"')
        mf=root/'manifest.txt';mf.write_text('\n'.join(lines)+'\n')
        r=subprocess.run([str(Path(executable).resolve()),str(mf)],capture_output=True,text=True,timeout=40)
        require(r.returncode==0,'guest runner: '+r.stderr[-1000:])
        rows=r.stdout.splitlines();require(sum(x.startswith('P8ISA_RESULT1 ') for x in rows)==1,'missing result header')
        exports={}
        for row in rows:
            if not row.startswith('GFXEXP '):continue
            target,enabled,compressed,valid,done,lanes,*values=map(lambda x:int(x,16),row.split()[1:])
            require(0<=target<=63 and 0<enabled<=15 and compressed in (0,1) and valid in (0,1) and done in (0,1),'invalid export ABI')
            require(lanes&~mask==0 and len(values)==4*lanes.bit_count(),'unbound export lanes')
            for i,lane in enumerate(x for x in range(64) if lanes>>x&1):
                key=(target,lane);require(key not in exports,'multiple exports of same target/lane')
                exports[key]=(enabled,compressed,tuple(values[i*4:i*4+4]))
        require(exports,'no graphics exports')
        return exports


def replay_vertex(capture, record, executable, decoder, max_waves=1024):
    shader=record['shader'];profile=PROFILES.get(shader,{})
    code=capture.read('guest-shaders/vs_'+shader+'_probe.bin')
    fingerprint=check_guest(code,shader,profile)
    # Only admitted vertex execution: original NGG prologue is verified, not
    # reinterpreted as proof of the vertex/primitive admission decision.
    decoded=decode_all(decoder,code[0x40:])
    unsupported=('image_','ds_','flat_','global_','scratch_','buffer_store','buffer_atomic','s_store','s_sendmsg','s_barrier','s_setpc','s_swappc')
    require(not any(text.startswith(unsupported) for _,text in decoded),'unsupported side effect or image/LDS operand in admitted VS')
    require(record.get('vs_user_data_base')==8 and 0<len(record['vs_user_data'])<=98,'unsupported NGG scalar ABI')
    rows=position_rows(record);require(record['missing']==0 and record['instances']==1,'incomplete native invocation census')
    require(all(int(r[1])==record['first_instance'] for r in rows),'native instance mismatch')
    raw=record.get('index_words',record['indices']);base=record['base_vertex'] if record['indexed'] else record['first_vertex']
    ids=set((int(x)+base)&0xffffffff for x in raw)
    require(ids==set(map(int,rows[:,0])),'native index/vertex census mismatch')
    segments,missing=stage_memory(capture,record,'vs')
    mode=int(record['vs_float_mode'])|int(bool(record['vs_dx10_clamp']))<<8|int(bool(record['vs_ieee_mode']))<<9
    require((len(rows)+63)//64<=max_waves,'requested scope exceeds wave budget')
    outputs={}
    for first in range(0,len(rows),64):
        vertices=rows[first:first+64];mask=(1<<len(vertices))-1
        versions=[]
        # Unknown pre-admission registers are not silently assumed zero. Two
        # poison patterns are a sensitivity check, NOT exhaustive ABI proof.
        for poison in (0,0x7fa54321):
            sg=[poison]*106;vg=[poison]*(64*256)
            sg[8:8+len(record['vs_user_data'])]=record['vs_user_data']
            for lane,r in enumerate(vertices):vg[5*64+lane]=int(r[0]);vg[8*64+lane]=int(r[1])
            versions.append(run_wave(executable,code[0x40:],sg,vg,segments,mode,mask))
        require(versions[0]==versions[1],'reference depends on unknown launch register')
        for (target,lane),value in versions[0].items():outputs[(target,int(vertices[lane,0]))]=value
    predicted=[]
    for row in rows:
        v=outputs.get((12,int(row[0])));require(v and v[:2]==(15,0),'missing complete POS0 export');predicted.append(v[2])
    expected=np.array(predicted,dtype='<u4')
    compare=compare_positions(expected,rows[:,3:7].copy(),rows,record.get('dx_clip_space',True),0)
    target_counts={str(t):sum(k[0]==t for k in outputs) for t in sorted({x[0] for x in outputs})}
    return {'draw':record['journal_draw'],'frame':record['frame'],'submit':record['submit'],'shader':shader,'guest_sha256':fingerprint,
            'scope':'admitted VS original ISA export, not NGG admission or prefix correctness','reference_status':'EXECUTED',
            'position_comparison':compare,'export_counts':target_counts,'unretained_bound_ranges':missing,
            'operands_sha256':digest(b''.join(pack([a&0xffffffff,a>>32,len(b)])+b for a,b in segments)),
            'exports':[{'target':t,'vertex':v,'enabled':x[0],'compressed':x[1],'words':x[2]} for (t,v),x in sorted(outputs.items())]}


def audit(path,executable,draws=None,max_waves=1024):
    capture=Capture(path);decoder=LLVM();reports=[]
    try:
        names=sorted(n for n in capture.names if PurePosixPath(n).name.startswith('ds_prelight_witness_vertex_') and n.endswith('.json'))
        for n in names:
            row=capture.json(n);draw=row.get('journal_draw')
            if draws is not None and draw not in draws:continue
            try:reports.append(replay_vertex(capture,merge_inputs(capture,row),executable,decoder,max_waves))
            except (Blocked,Unsupported,KeyError,TypeError,ValueError,OSError,subprocess.SubprocessError) as e:
                reports.append({'draw':draw,'shader':row.get('shader'),'reference_status':'INCOMPLETE','reason':str(e)})
    finally:capture.close();decoder.close()
    return {'scope':'real capture full admitted VS reference, local calibration not global P8','draws':reports,
            'counts':{'executed':sum(x['reference_status']=='EXECUTED' for x in reports),'incomplete':sum(x['reference_status']!='EXECUTED' for x in reports)}}


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path);p.add_argument('runner',type=Path);p.add_argument('--draw',type=int,action='append');p.add_argument('--max-waves',type=int,default=1024);p.add_argument('--output',type=Path);a=p.parse_args()
    result=audit(a.archive,a.runner,a.draw,a.max_waves);s=json.dumps(result,indent=2)+'\n'
    if a.output:a.output.write_text(s);print(json.dumps({**result,'draws':[{k:v for k,v in x.items() if k!='exports'} for x in result['draws']]},indent=2))
    else:print(s,end='')
if __name__=='__main__':main()
