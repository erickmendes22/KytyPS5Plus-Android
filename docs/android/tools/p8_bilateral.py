#!/usr/bin/env python3
"""Finite same-input causal replay for the P8 division.

The decision kernel executes operation contracts; it does not consume PASS/FAIL
flags or expected output files. A replay adapter must supply a closed, independently
enumerated guest operation census and native observations. `inventory` audits the
actual Android raw capture and emits acquisition requirements, not a certificate.
The present raw-only APK schema does not yet produce a closed replay capsule.
"""
from __future__ import annotations
import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import struct
import subprocess
import tempfile
import zipfile


class Blocked(ValueError):
    pass


def require(ok, message):
    if not ok:
        raise Blocked(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


@dataclass(frozen=True, order=True)
class Version:
    owner: int
    allocation: int
    version: int
    order: int
    offset: int
    size: int
    mip: int = 0
    layer: int = 0
    aspect: int = 0
    object_id: int = 0
    format: int = 0

    def __post_init__(self):
        values = (self.owner, self.allocation, self.version, self.order,
                  self.offset, self.size, self.mip, self.layer, self.aspect, self.object_id, self.format)
        require(all(type(x) is int and 0 <= x < 2**64 for x in values), 'invalid resource identity')
        require(self.owner and self.allocation and self.size and self.owner+self.offset+self.size <= 2**64,
                'empty/wrapping resource range')

    def overlaps(self, other):
        # Guest buffer aliases must not be considered independent because
        # their host allocations differ. Images use logical subresources; a
        # buffer/image alias or different image owner needs an adapter proof.
        if self.aspect==other.aspect==0:
            a,b=self.owner+self.offset,other.owner+other.offset
            return a<b+other.size and b<a+self.size
        return ((self.owner, self.mip, self.layer, self.aspect) ==
                (other.owner, other.mip, other.layer, other.aspect)
                and self.offset < other.offset+other.size and other.offset < self.offset+self.size)


@dataclass(frozen=True)
class Operation:
    command: int
    category: int
    kind: str
    inputs: tuple[Version, ...]
    output: Version
    parameters: dict


@dataclass(frozen=True)
class Result:
    status: str
    g0: str
    interval: tuple[int, int]
    pruned: tuple[int, ...]
    compared: tuple[int, ...]
    mismatches: tuple[int, ...]
    reason: str
    counterexample: dict | None = None


def guest_wave(executable, params, inputs):
    """Execute original guest ISA with exact versioned memory, not native SPIR-V.

    Only the actual single-wave/no-LDS ABI of p8_guest_ref.cpp is accepted.
    A missing architectural access or unsupported instruction rejects the replay.
    """
    code = params['code']
    wave, execmask, mode = params['wave'], params['exec'], params['mode']
    sgprs, vgprs = params['sgprs'], params['vgprs']
    addresses = params['addresses']
    out_address, out_size = params['output_address'], params['output_size']
    require(isinstance(code, bytes) and code and len(code)%4 == 0, 'missing original guest code')
    require(wave in (32, 64) and 0 < execmask < 1 << wave and
            params['arch'] in ('rdna1', 'rdna2') and params['lds_bytes'] == 0 and
            params['waves_per_group'] == 1, 'unsupported guest launch')
    require(0 < len(sgprs) <= 106 and len(vgprs)%wave == 0 and 0 < len(vgprs)//wave <= 256,
            'invalid guest register ABI')
    require(all(type(x) is int and 0 <= x <= 0xffffffff for x in [*sgprs, *vgprs, mode]),
            'invalid guest register word')
    require(len(addresses) == len(inputs) and all(inputs), 'missing guest memory input')
    segments = sorted(zip(addresses, inputs))
    require(all(type(a) is int and 0 <= a <= 2**64-len(b) for a,b in segments), 'invalid memory range')
    require(all(a+len(b) <= c for (a,b),(c,d) in zip(segments,segments[1:])), 'overlapping memory segments')
    require(out_size > 0 and any(a <= out_address and out_address+out_size <= a+len(b) for a,b in segments),
            'uncaptured output initial bytes')
    pc = 0x10000
    while any(a < pc+len(code) and pc < a+len(b) for a,b in segments):
        pc += 0x100000
        require(pc+len(code)<2**64, 'guest code allocation failed')
    with tempfile.TemporaryDirectory(prefix='p8-isa-') as directory:
        root = Path(directory)
        codefile = root/'guest.bin'; codefile.write_bytes(code)
        lines = [f'P8ISA1 {params["arch"]} {pc:x} {execmask:x} {mode:x} {wave} {len(sgprs)} {len(vgprs)//wave} 1000000 "{codefile}"',
                 ' '.join(f'{v:x}' for v in sgprs), ' '.join(f'{v:x}' for v in vgprs), str(len(segments))]
        for i,(a,b) in enumerate(segments):
            f=root/f'mem{i}.bin'; f.write_bytes(b); lines.append(f'{a:x} "{f}"')
        lines += ['1', f'{out_address:x} {out_size:x}']
        mf=root/'manifest.txt'; mf.write_text('\n'.join(lines)+'\n')
        result=subprocess.run([str(Path(executable).resolve()), str(mf)], capture_output=True, text=True, timeout=40)
        require(result.returncode==0, 'guest executor: '+result.stderr[-1000:])
        rows=result.stdout.splitlines()
        headers=[r for r in rows if r.startswith('P8ISA_RESULT1 ')]
        require(len(headers)==1 and int(headers[0].split()[2])>0, 'unobserved guest memory access')
        mem=[r.split() for r in rows if r.startswith('MEM ')]
        require(len(mem)==1 and len(mem[0])==3 and int(mem[0][1],16)==out_address, 'wrong guest output identity')
        data=bytes.fromhex(mem[0][2]); require(len(data)==out_size, 'wrong guest output length')
        return data


def operation_reference(op, args, executable):
    p=op.parameters
    # No model that merely returns an asserted reference output is permitted.
    if op.kind=='copy':
        require(len(args)==1 and not p and len(args[0])==op.output.size, 'copy contract')
        out=args[0]
    elif op.kind=='masked_store_u32':
        require(len(args)==3 and not p and all(len(x)==4 for x in args), 'masked store operands')
        # The mask is itself a versioned input from the guest register state.
        old,new,mask=(struct.unpack('<I',x)[0] for x in args)
        out=struct.pack('<I',(old&~mask)|(new&mask))
    elif op.kind=='clear_u32':
        require(len(args)==1 and not p and len(args[0])==4 and op.output.size%4==0, 'clear operands')
        out=args[0]*(op.output.size//4)
    elif op.kind=='depth_test_d32':
        # Vk/guest compare ordering is explicitly decoded by the adapter.
        # This exact contract handles a single finite D32 sample, no stencil,
        # no discard, no depth bias or NaN/implementation-dependent conversion.
        require(len(args)==3 and all(len(x)==4 for x in args) and not p, 'depth test operands')
        a,b=(struct.unpack('<f',x)[0] for x in args[:2]); cmp=struct.unpack('<I',args[2])[0]
        require(0<=a<=1 and 0<=b<=1 and 0<=cmp<=7, 'unsupported D32 sample')
        out=struct.pack('<I',int((False,a<b,a==b,a<=b,a>b,a!=b,a>=b,True)[cmp]))
    elif op.kind=='select_u32':
        require(len(args)==3 and all(len(x)==4 for x in args) and not p, 'select operands')
        mask=struct.unpack('<I',args[0])[0];require(mask in (0,1),'non-boolean predicate')
        out=args[1] if mask else args[2]
    elif op.kind=='guest_export':
        from p8_graphics_contract import execute
        require(executable is not None, 'independent graphics executor unavailable')
        # The first four (or five) operands are explicit launch state, not
        # arbitrary constants embedded in the contract's parameters.
        start = 5 if p.get('lds') else 4
        require(len(op.inputs) >= start and all(v.aspect == 0 for v in op.inputs[start:]),
                'guest graphics input identity')
        require(tuple(p['addresses']) == tuple(v.owner+v.offset for v in op.inputs[start:]),
                'guest graphics memory/resource address mismatch')
        out = execute(executable,p,args)
    elif op.kind=='pack_rgba8_unorm':
        require(len(args)==1 and not p and len(args[0])==op.output.size*4 and op.output.size%4==0,
                'RGBA8 pack contract')
        floats=struct.unpack('<%df'%(len(args[0])//4),args[0])
        require(all(math.isfinite(v) for v in floats),'nonfinite RGBA8 operand')
        # Round to nearest even for the explicitly scoped UNORM contract.
        # Do not use this contract for UINT, sRGB, blend or unknown rounding.
        out=bytes(round(min(1.0,max(0.0,v))*255.0) for v in floats)
    elif op.kind=='guest_wave':
        require(executable is not None, 'independent guest executor unavailable')
        require(tuple(p['addresses'])==tuple(v.owner+v.offset for v in op.inputs), 'guest memory/resource address mismatch')
        require(p['output_address']==op.output.owner+op.output.offset and p['output_size']==op.output.size,
                'guest output/resource address mismatch')
        out=guest_wave(executable,p,args)
    else:
        raise Blocked('unsupported contract: '+op.kind)
    require(isinstance(out,bytes) and len(out)==op.output.size,'reference output size')
    return out


def brightness(data, pixel, fmt):
    # These are explicit storage-format metrics for the same selected output
    # sample, not estimates of perceived brightness or a proof of gameplay.
    require(type(pixel) is int and pixel>=0,'invalid final pixel')
    if fmt in (0,37,43): # 0: test fixture only; R8G8B8A8 UNORM/SRGB
        i=4*pixel;require(i<=len(data)-4 and len(data)%4==0,'invalid RGBA8 extent')
        rgb=[v/255.0 for v in data[i:i+3]]
    elif fmt==64: # A2B10G10R10_UNORM_PACK32 (observed flip in #752/#753)
        i=4*pixel;require(i<=len(data)-4 and len(data)%4==0,'invalid RGB10A2 extent')
        w=struct.unpack_from('<I',data,i)[0];rgb=[((w>>k)&1023)/1023.0 for k in (0,10,20)]
    elif fmt==97: # R16G16B16A16_SFLOAT, linear HDR
        i=8*pixel;require(i<=len(data)-8 and len(data)%8==0,'invalid HDR extent')
        rgb=struct.unpack_from('<4e',data,i)[:3]
        require(all(math.isfinite(x) for x in rgb),'nonfinite final color')
    else:raise Blocked('unsupported final color format')
    return sum(x*y for x,y in zip(rgb,(.2126,.7152,.0722)))


def decide(operations, roots, observed, census, boundary, final, pixel, executable=None):
    """Run the decision on an adapter-validated, finite witness, not whole gameplay.

    `census` must originate from the original guest command stream, not the
    translated draw list. `roots` must be same-input guest source bytes, not
    observed intermediate outputs. The acquisition adapter is responsible for
    those two provenance obligations; this kernel validates coverage and bytes.
    """
    compared=[]; mismatch=[]
    def blocked(why):
        return Result('BLOCKED','BLOCKED',(0,16),(),tuple(compared),tuple(mismatch),str(why))
    try:
        require(operations and list(census)==[op.command for op in operations] and
                len(set(census))==len(census) and list(census)==sorted(census),
                'guest command census mismatch: omitted/duplicate/reordered command')
        require(all(type(op.category) is int and 1<=op.category<=16 for op in operations), 'invalid causal category')
        require(all(isinstance(data,bytes) and len(data)==r.size for r,data in roots.items()), 'incomplete root bytes')
        require(not any(r.overlaps(q) for i,r in enumerate(roots) for q in list(roots)[:i]), 'ambiguous overlapping root versions')
        producers={}
        available=set(roots)
        values=dict(roots)
        live=list(roots)
        for i,op in enumerate(operations):
            require(op.output not in available,'duplicate output version')
            require(all(r in available for r in op.inputs),'missing producer for command '+str(op.command))
            for r in op.inputs:
                require(not any(r.overlaps(q) and q!=r and q.order>=r.order for q in live),'stale/ambiguous read at command '+str(op.command))
                require(r.order<op.output.order,'producer/consumer order mismatch')
            for r in live:
                if op.output.overlaps(r):
                    require(op.output.order>r.order and op.output.version>r.version and
                            (op.output.owner,op.output.allocation,op.output.offset,op.output.size)==(r.owner,r.allocation,r.offset,r.size),
                            'overlapping/partial write lacks explicit range partition')
            live=[r for r in live if not op.output.overlaps(r)]+[op.output]
            producers[op.output]=i;available.add(op.output)
        require(set(observed)<=set(producers),'unmatched native observations')
        require(all(isinstance(data,bytes) and len(data)==r.size for r,data in observed.items()),'invalid observed payload length')
        values.update(observed)
        require(final in producers and operations[producers[final]].category>8,'missing final consumer')
        # Trace the precise ancestor cone of the fixed final version. Require all
        # declared boundary versions crossing into the suffix, not an aggregate.
        needed=set(); stack=[final]
        while stack:
            r=stack.pop()
            if r in needed:continue
            needed.add(r)
            if r in producers:stack.extend(operations[producers[r]].inputs)
        cone=[op for op in operations if op.output in needed]
        cut={r for op in cone if op.category>8 for r in op.inputs
             if r in producers and operations[producers[r]].category<=8}
        require(cut and cut==set(boundary),'boundary version census mismatch')
        require(not any(op.category<=8 and any(r in producers and operations[producers[r]].category>8 for r in op.inputs) for op in cone),
                'temporal feedback needs a versioned prefix partition')
        expected={}; errors={}
        for op in cone:
            try:
                require(op.output in observed,'missing actual output for command '+str(op.command))
                ref=operation_reference(op,[values[r] for r in op.inputs],executable)
                expected[op.output]=ref; compared.append(op.command)
                if ref!=values[op.output]:mismatch.append(op.command)
            except (Blocked,KeyError,TypeError,ValueError,OSError,subprocess.SubprocessError) as e:
                errors[op.output]=str(e)
        actual=brightness(values[final],pixel,final.format)
        require(actual<0.006,'selected observed final pixel is not dark')
        # A local correction must propagate to the same output. Each downstream
        # contract used for propagation has to reproduce its original observed
        # output first. Unrelated unavailable operations do not defeat FAIL.
        def propagate(changes):
            changed=dict(changes); path=[]
            for op in cone:
                if op.output in changes:
                    path.append(op.command); continue
                if not any(r in changed for r in op.inputs):continue
                require(op.output in expected and expected[op.output]==values[op.output],
                        'downstream baseline not reproduced at command '+str(op.command))
                new=operation_reference(op,[changed[r] if r in changed else values[r] for r in op.inputs],executable)
                if new!=values[op.output]:changed[op.output]=new;path.append(op.command)
            if final not in changed:return None
            level=brightness(changed[final],pixel,final.format)
            if level<0.006 or level<=actual:return None
            return {'observed_luminance':actual,'replayed_luminance':level,'pixel':pixel,
                    'observed_final_sha256':digest(values[final]),'replayed_final_sha256':digest(changed[final]),
                    'path':path,'substituted':[operations[producers[x]].command for x in changes],
                    'changed_boundary_versions':[r.__dict__ for r in sorted(cut & changed.keys())]}
        # FAIL needs one real counterexample in the prefix with a causal path.
        for op in cone:
            if op.category>8 or op.output not in expected or expected[op.output]==values[op.output]:continue
            try:proof=propagate({op.output:expected[op.output]})
            except (Blocked,ValueError,KeyError,OSError,subprocess.SubprocessError):continue
            if proof and proof['changed_boundary_versions']:
                proof['command']=op.command;proof['category']=op.category
                proof['expected_output_sha256']=digest(expected[op.output]);proof['observed_output_sha256']=digest(values[op.output])
                return Result('FAIL','DEMONSTRATED',(0,8),tuple(range(9,17)),tuple(compared),tuple(mismatch),
                              'independent prefix counterexample propagated through reproduced baseline',proof)
        # PASS requires every ancestor operation in the prefix to be executed
        # and equal, not merely matching final cut bytes (compensating errors).
        prefix=[op for op in cone if op.category<=8]
        if not prefix or any(op.output not in expected or expected[op.output]!=values[op.output] for op in prefix):
            return blocked('prefix lacks complete independent equality or a propagated counterexample')
        for op in cone:
            if op.category<=8 or op.output not in expected or expected[op.output]==values[op.output]:continue
            try:proof=propagate({op.output:expected[op.output]})
            except (Blocked,ValueError,KeyError,OSError,subprocess.SubprocessError):continue
            if proof:
                proof['command']=op.command;proof['category']=op.category
                proof['expected_output_sha256']=digest(expected[op.output]);proof['observed_output_sha256']=digest(values[op.output])
                return Result('PASS','DEMONSTRATED',(8,16),tuple(range(1,9)),tuple(compared),tuple(mismatch),
                              'complete prefix of finite cone matches; suffix counterexample propagated',proof)
        return blocked('no independently demonstrated final symptom; '+str(errors))
    except (Blocked,KeyError,TypeError,ValueError,OSError,subprocess.SubprocessError) as e:
        return blocked(e)


IMAGE_ID=('owner','object','native','version','write_order','format')
SUBRESOURCE=('mip','layer','aspect')

def identity(row, fields):
    require(all(k in row for k in fields),'missing resource identity')
    return tuple(row[k] for k in fields)


def inventory(path):
    """Inventory actual P8 reads, candidate writers and flip-linked output.

    The raw observation schema records pre-use versions, not complete
    post-operation producer versions or a guest-side command census. Do not
    infer a writer from the most recent candidate, or declare P8 from it.
    """
    with zipfile.ZipFile(path) as z:
        names=z.namelist();require(len(names)==len(set(names)),'duplicate archive member')
        prefix='p8-reference/'
        manifest=json.loads(z.read(prefix+'export-manifest.json'))
        files={};missing=[]
        for x in manifest['files']:
            n=x['file'];require(isinstance(n,str) and PurePosixPath(n).name==n and n not in files,'unsafe/duplicate manifest member')
            if x['status']!='complete':missing.append(n);files[n]=None;continue
            require(0<=x['bytes']<=128*1024*1024,'oversized capture member')
            b=z.read(prefix+n);require(len(b)==x['bytes']==x['copied'] and digest(b)==x['sha256'],'invalid payload '+n);files[n]=b
        events=[json.loads(x) for x in files['observations.jsonl'].splitlines()]
        frame=json.loads(files['capture-status.json'])['target_frame']
        final_matches=[];observations={};physical={}
        def snapshot(s):
            k=identity(s,IMAGE_ID+SUBRESOURCE)
            require(s.get('valid') and s['version']==s['version_after'] and not s['cpu_dirty'] and not s['buffer_modified'],'unusable snapshot')
            require(s['file'] in files and files[s['file']] is not None and
                    len(files[s['file']])==s['expected_bytes']==s['captured_bytes'] and
                    s['extent'][0]*s['extent'][1]*s['texel_bytes']==s['captured_bytes'],'incomplete snapshot bytes')
            return k
        for i,e in enumerate(events):
            if e.get('kind')=='snapshot' and e.get('valid') and e['frame']==frame:
                try:k=snapshot(e)
                except Blocked:continue
                if e.get('observation_reused'):
                    p=physical.get(e.get('source_observation'));require(p and p[0]==k and p[1]['file']==e['file'] and p[1]['submit']==e['source_submit'],'invalid version reuse')
                else:
                    require(e['marker'] not in physical,'duplicate physical observation');physical[e['marker']]=(k,e)
                observations.setdefault(k,[]).append((i,e))
            if e.get('kind')=='flip' and e.get('frame')==frame:
                key=identity(e['image'],IMAGE_ID)
                for k,rows in observations.items():
                    for j,s in rows:
                        if j<i and s.get('role')=='compositor' and k[:len(IMAGE_ID)]==key:
                            final_matches.append((i,j,k,s))
        require(len(final_matches)==1,'no unique flip-linked compositor version')
        flip_i,final_i,final_key,final=final_matches[0]
        uses=[(i,e) for i,e in enumerate(events) if i<flip_i and e.get('kind')=='cut_use' and e.get('frame')==frame]
        require(uses and len({e['marker'] for i,e in uses})==len(uses),'invalid use census')
        required={};cut_reads=[];binding_gaps=[];buffer_census=[];writers=defaultdict(list)
        for i,e in uses:
            for t in e.get('targets',[]):
                if 'image' in t:writers[identity(t['image'],('owner','object','native'))].append((i,e['marker'],'attachment',None))
            if 'depth_target' in e:writers[identity(e['depth_target'],('owner','object','native'))].append((i,e['marker'],'depth_attachment',None))
            for st in e.get('stages',[]):
                for b in st.get('buffers',[]):
                    buffer_census.append({'marker':e['marker'],'shader':st.get('shader'),'slot':b['slot'],'guest':b['guest'],'bytes':b['bytes'],
                                          'missing':['read/write range','pre-use authoritative bytes/version','post-write version if written']})
                for b in st.get('images',[]):
                    im=b.get('image');v=b.get('view')
                    if im and b.get('written'):writers[identity(im,('owner','object','native'))].append((i,e['marker'],'storage_image',v))
                    if not b.get('cut_candidate') or not (b.get('read') or not b.get('access_known')):continue
                    if not im or not v or not b.get('access_known'):
                        binding_gaps.append({'marker':e['marker'],'shader':st.get('shader'),'slot':b['slot'],'reason':'missing image/view/access classification'});continue
                    for mip in range(v['mip'],v['mip']+v['levels']):
                        for layer in range(v['layer'],v['layer']+v['layers']):
                            for aspect in (1,2,4):
                                if not v['aspect']&aspect:continue
                                k=identity(im,IMAGE_ID)+(mip,layer,aspect)
                                exact=[s for s in b.get('snapshots',[]) if s.get('valid') and identity(s,IMAGE_ID+SUBRESOURCE)==k]
                                good=[s for s in exact if s['submit']==e['submit'] and any(j<i and old==s for j,old in observations.get(k,[]))]
                                cut_reads.append((e['marker'],k,bool(good)))
                                entry=required.setdefault(k,{'identity':list(k),'consumers':[], 'first_use_index':i,'captured':True})
                                entry['consumers'].append({'marker':e['marker'],'shader':st.get('shader'),'slot':b['slot'],'submit':e['submit'],'bytes_available':bool(good)})
                                entry['captured'] &= bool(good)
        for k,r in required.items():
            # Raw log has only pre-use identity. Keep candidates explicitly;
            # a later consumer version cannot be attributed by adjacency.
            candidates=[{'marker':m,'type':kind,'view':v} for i,m,kind,v in writers.get(k[:3],[]) if i<r['first_use_index'] and
                        (v is None or v['mip']<=k[-3]<v['mip']+v['levels'] and v['layer']<=k[-2]<v['layer']+v['layers'] and v['aspect']&k[-1])]
            r['writer_candidates']=candidates;r['missing']=['independent operation reference','exact post-write version and guest command origin']
            if not r['captured']:r['missing'].append('consumer bytes for exact view/version')
        # Backward schedule from the actual flip and all observed cut consumers.
        # Pre-use image identities and overlapping buffer ranges are candidates,
        # never authoritative last writers. Resolve them once using explicit
        # post-write versions/access ranges in a future capsule, not one build
        # per candidate. Every attachment is a potential load/blend input until
        # a recorded guest clear/overwrite contract proves otherwise.
        by_marker={e['marker']:(i,e) for i,e in uses}
        buffer_ranges=defaultdict(list)
        for i,e in uses:
            for st in e.get('stages',[]):
                for b in st.get('buffers',[]):
                    if b['guest'] and b['bytes']:
                        buffer_ranges[e['marker']].append((b['guest'],b['guest']+b['bytes']))
        all_bda=[(i,e['marker']) for i,e in uses if any(s.get('bda_writes') for s in e.get('stages',[]))]
        def earlier_image_candidates(im,view,i):
            found=[]
            for j,m,kind,v in writers.get(identity(im,('owner','object','native')),[]):
                if j>=i:continue
                if view and v and not (view['mip']<v['mip']+v['levels'] and v['mip']<view['mip']+view['levels'] and
                                       view['layer']<v['layer']+v['layers'] and v['layer']<view['layer']+view['layers'] and view['aspect']&v['aspect']):continue
                found.append(m)
            return found
        # A bound range lacks read/write access bits in #753; conservatively
        # include potential overlapping earlier bindings. This does not assert
        # they wrote, nor ask to dump all descriptor ranges unconditionally.
        def earlier_buffer_candidates(addr,size,i):
            if not addr or not size:return []
            return [m for m,ranges in buffer_ranges.items() if by_marker[m][0]<i and
                    any(a<addr+size and addr<b for a,b in ranges)]
        final_writers=[{'marker':m,'type':kind,'view':v} for i,m,kind,v in writers.get(final_key[:3],[]) if i<final_i]
        todo=[x['marker'] for x in final_writers]+[m for m,k,g in cut_reads]
        visited=set(); schedule=[]; predecessor_gaps=[]
        while todo:
            m=todo.pop()
            if m in visited:continue
            visited.add(m); i,e=by_marker[m]
            dependencies=set(); input_rows=[]
            for st in e.get('stages',[]):
                for b in st.get('images',[]):
                    if not (b.get('read') or not b.get('access_known')):continue
                    im=b.get('image');v=b.get('view')
                    row={'kind':'image','shader':st.get('shader'),'slot':b['slot'],'guest':b['guest'],'view':v}
                    if im:
                        row['identity']=list(identity(im,IMAGE_ID));candidates=earlier_image_candidates(im,v,i)
                        row['candidate_writers']=candidates;dependencies.update(candidates)
                        row['payloads']=[s['file'] for s in b.get('snapshots',[]) if s.get('valid') and s.get('file') in files and files[s['file']] is not None]
                        if not candidates:predecessor_gaps.append({'marker':m,'input':row['identity'],'view':v,'kind':'image','missing':'source version or prior-frame producer'})
                    else:row['missing']='unresolved actual image binding'
                    input_rows.append(row)
                for b in st.get('buffers',[]):
                    candidates=earlier_buffer_candidates(b['guest'],b['bytes'],i);dependencies.update(candidates)
                    input_rows.append({'kind':'buffer','shader':st.get('shader'),'slot':b['slot'],'guest':b['guest'],'bytes':b['bytes'],'candidate_writers':candidates,'missing':'exact read/write ranges and versioned authoritative bytes'})
                    if not candidates:predecessor_gaps.append({'marker':m,'kind':'buffer','guest':b['guest'],'bytes':b['bytes'],'missing':'source version or prior-frame producer'})
            for t in [x['image'] for x in e.get('targets',[]) if 'image' in x]+([e['depth_target']] if 'depth_target' in e else []):
                candidates=earlier_image_candidates(t,None,i);dependencies.update(candidates)
                input_rows.append({'kind':'attachment_load_or_blend','identity':list(identity(t,IMAGE_ID)),'candidate_writers':candidates,'missing':'executed load/clear/blend and exact subresource write'})
            # BDA writers with no access ranges may alias a later read.
            bda=[w for j,w in all_bda if j<i];dependencies.update(bda)
            todo.extend(dependencies-visited)
            schedule.append({'marker':m,'frame':e['frame'],'submit':e['submit'],'command':e.get('command'),'path':e['path'],
                             'guest_modules':[s.get('guest_file') for s in e.get('stages',[])],
                             'input_bindings':input_rows,'unresolved_bda_writers':bda,'candidate_dependencies':sorted(dependencies),
                             'missing':['exact independent guest command/launch state','post-write identities and payloads','supported independent execution contract']})
        schedule.sort(key=lambda r:by_marker[r['marker']][0])
        # Trace candidates for final producer too, not all later frame uses.
        final_writers=[{'marker':m,'type':kind,'view':v} for i,m,kind,v in writers.get(final_key[:3],[]) if i<final_i]
        return {'schema':1,'scope':'actual archive acquisition inventory; no independent prefix replay yet',
                'capture_sha256':digest(Path(path).read_bytes()),'target_frame':frame,
                'verified_manifest_files':sum(b is not None for b in files.values()),'incomplete_manifest_files':missing,
                'native_uses_before_flip':len(uses),'native_uses_in_frame':sum(e.get('kind')=='cut_use' and e.get('frame')==frame for e in events),
                'cut_reads':len(cut_reads),'cut_reads_with_exact_bytes':sum(g for m,k,g in cut_reads),'unique_cut_versions':len(required),
                'cut_versions_with_all_reads':sum(r['captured'] for r in required.values()),'cut_binding_gaps':binding_gaps,
                'final':{'identity':list(final_key),'file':final['file'],'bytes':final['captured_bytes'],'sha256':digest(files[final['file']]),'writer_candidates':final_writers},
                'cut':list(required.values()),'buffer_bindings_without_version_access_contract':buffer_census,
                'candidate_cone_operations':len(schedule),'candidate_cone':schedule,
                'unresolved_predecessors':predecessor_gaps,
                'candidate_cone_is_complete':False,
                'unavailable_backends':['guest command census including omitted work','complete C1..C8 operation replay with authoritative inputs','same-input suffix replay to flip'],
                'decision':{'G0':'BLOCKED','P8':'BLOCKED','interval':[0,16],'reason':'raw capture is not a closed executable cone'}}


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path);p.add_argument('--output',type=Path);a=p.parse_args()
    try:report=inventory(a.archive)
    except (Blocked,KeyError,TypeError,ValueError,OSError,zipfile.BadZipFile) as e:
        print(json.dumps({'decision':{'P8':'BLOCKED'},'input_error':str(e)}));return 2
    s=json.dumps(report,indent=2)+'\n'
    if a.output:a.output.write_text(s);print(json.dumps({k:v for k,v in report.items() if k not in ('cut','buffer_bindings_without_version_access_contract','cut_binding_gaps','candidate_cone','unresolved_predecessors')},indent=2))
    else:print(s,end='')
    return 0

if __name__=='__main__':raise SystemExit(main())
