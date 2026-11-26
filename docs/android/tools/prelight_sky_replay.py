#!/usr/bin/env python3
"""Replay the captured V3 sky raster/depth witness, never the whole P8 prefix.

Usage: python prelight_sky_replay.py diagnostics.zip [--output report.json]
       python prelight_sky_replay.py extracted-diagnostics-or-capture-directory
Requires NumPy and either python-xxhash or the xxhash shared library.

The input is captured native XYZW, not an independent guest-VS result. This
center-sample, float64 raster model does not reproduce a GPU's subpixel grid,
top-left edge ownership, interpolation precision or all fragment operations.
It reports exact changed-pixel masks and numerical D32 differences separately.
--depth-atol is an ANALYSIS threshold, not a Vulkan conformance bound. No result
from this tool certifies P8, guest visibility, a complete shader or gameplay.

Exit 0: local measurements consistent with the stated model/threshold.
Exit 2: BLOCKED (missing, incompatible, corrupt, or unsupported input).
Exit 3: local mismatch candidate; never P8 FAIL or a root-cause certificate.
"""
import argparse
import ctypes
import ctypes.util
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import struct
import zipfile

import numpy as np


class Blocked(ValueError):
    pass


def require(ok, reason):
    if not ok:
        raise Blocked(reason)


class Capture:
    """Read only the selected ZIP/directory; never extract archive paths."""
    def __init__(self, path):
        path = Path(path)
        self.archive = None
        if path.is_dir():
            candidates = [path, path / 'ds-lighting-capture']
            roots = [p for p in candidates if (p / 'queue-state').is_dir()]
            require(len(roots) == 1, 'expected one ds-lighting-capture/queue-state directory')
            self.root = roots[0].resolve()
            self.names = [p.relative_to(self.root).as_posix() for p in self.root.rglob('*') if p.is_file()]
        else:
            self.archive = zipfile.ZipFile(path)
            names = self.archive.namelist()
            require(len(names) == len(set(names)), 'duplicate ZIP member names')
            suffix = 'queue-state/ds_prelight_witness_depth.json'
            roots = [n[:-len(suffix)] for n in names if n.endswith(suffix)]
            require(len(roots) == 1, 'expected one sky depth manifest in ZIP')
            self.root = roots[0]
            self.names = [n[len(self.root):] for n in names if n.startswith(self.root)]

    def close(self):
        if self.archive:
            self.archive.close()

    def read(self, name):
        path = PurePosixPath(name)
        require(not path.is_absolute() and '..' not in path.parts, 'unsafe capture member path')
        require(name in self.names, 'missing capture member: ' + name)
        if self.archive:
            info = self.archive.getinfo(self.root + name)
            require(info.file_size <= 128 * 1024 * 1024, 'capture member exceeds 128 MiB')
            return self.archive.read(info)
        source = (self.root / name).resolve()
        require(source.is_relative_to(self.root), 'capture member escapes selected directory')
        require(source.stat().st_size <= 128 * 1024 * 1024, 'capture member exceeds 128 MiB')
        return source.read_bytes()

    def json(self, name):
        return json.loads(self.read(name))


def xxh3(data):
    try:
        import xxhash
        return xxhash.xxh3_64_hexdigest(data)
    except ImportError:
        name = ctypes.util.find_library('xxhash')
        require(name is not None, 'XXH3 unavailable: install python-xxhash or libxxhash')
        lib = ctypes.CDLL(name)
        require(hasattr(lib, 'XXH3_64bits'), 'shared xxhash has no XXH3_64bits')
        fn = lib.XXH3_64bits
        fn.argtypes = (ctypes.c_void_p, ctypes.c_size_t)
        fn.restype = ctypes.c_uint64
        buffer = ctypes.create_string_buffer(data)
        return f'{fn(buffer, len(data)):016x}'


def instructions(data, stage):
    require(len(data) >= 20 and len(data) % 4 == 0, 'invalid SPIR-V byte length')
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    require(words[0] == 0x07230203 and words[3] > 0, 'invalid SPIR-V header')
    result, entries, offset = [], [], 5
    while offset < len(words):
        count, op = words[offset] >> 16, words[offset] & 65535
        require(count > 0 and offset + count <= len(words), 'truncated SPIR-V instruction')
        inst = words[offset:offset + count]
        if op == 15:  # OpEntryPoint: preserve original prefix, permit extra probe interface IDs.
            entries.append(inst)
        result.append(inst)
        offset += count
    require(len(entries) == 1 and entries[0][1] == (0 if stage == 'vs' else 4),
            'unexpected SPIR-V entry point stage')
    return words[:5], result


def module(capture, name, stage, guest):
    match = re.fullmatch(r'spv/(vs|ps)_([0-9a-f]{16})_([0-9a-f]{16})\.spv', name)
    require(match is not None and match[1] == stage and match[2] == guest,
            'module path does not identify its recorded guest stage/hash')
    data = capture.read(name)
    require(xxh3(data) == match[3], 'SPIR-V content fingerprint mismatch: ' + name)
    header, ops = instructions(data, stage)
    return {'file': name, 'xxh3': match[3], 'sha256': hashlib.sha256(data).hexdigest()}, header, ops


def vertex_modules(capture, record):
    guest, fp = record['shader'], record['fingerprint']
    require(re.fullmatch('[0-9a-f]{16}', fp) is not None, 'invalid original fingerprint')
    original, oh, oo = module(capture, f'spv/vs_{guest}_{fp}.spv', 'vs', guest)
    bound, bh, bo = module(capture, record['native_vs_file'], 'vs', guest)
    require(oh[:3] == bh[:3] and oh[4] == bh[4] and bh[3] >= oh[3],
            'original/bound SPIR-V headers are incompatible')
    cursor = 0
    for instruction in oo:
        while cursor < len(bo):
            observed = bo[cursor]
            cursor += 1
            same = observed == instruction
            if instruction[0] & 65535 == 15:
                same = observed[0] & 65535 == 15 and observed[1:len(instruction)] == instruction[1:]
            if same:
                break
        else:
            raise Blocked('original instruction missing/changed in bound instrumented module')
    return {'original': original, 'bound': bound, 'original_instruction_subsequence': True,
            'scope': 'identity and retained instruction check; not a proof of all instrumentation effects'}


def uint_words(values, label):
    require(isinstance(values, list) and all(type(x) is int and 0 <= x <= 0xffffffff for x in values),
            label + ' must contain uint32 words')
    return np.asarray(values, dtype='<u4')


def positions(record):
    require(record['missing'] == 0, 'vertex capture reports missing rows')
    require(record['base_vertex'] == record['first_instance'] == 0,
            'only the captured zero-base, first-instance sky case is supported')
    require(record['columns'][:8] == ['vertex','instance','transform','clip_x','clip_y','clip_z','clip_w','clip_distance'],
            'unexpected vertex row schema')
    rows = record['rows']
    require(isinstance(rows, list) and 0 < len(rows) <= 65536, 'invalid vertex row count')
    output = {}
    for row in rows:
        require(isinstance(row, list) and len(row) == len(record['columns']), 'incomplete vertex row')
        words = uint_words(row, 'vertex row')
        require(row[0] not in output and row[1] == 0, 'duplicate vertex or unsupported instance')
        values = words[3:8].view('<f4').astype(np.float64)
        require(np.isfinite(values).all(), 'nonfinite captured position/clip distance')
        require(values[4] == 0, 'nonzero user clip distance is unsupported')
        output[row[0]] = values[:4]
    indices = uint_words(record['indices'], 'indices')
    require(0 < len(indices) <= 196608 and len(indices) % 3 == 0, 'incomplete triangle-list indices')
    require(all(int(i) in output for i in indices), 'index references an uncaptured vertex')
    return output


def read_depth(capture, name, width, height):
    data = capture.read('queue-state/' + name)
    require(len(data) == width * height * 4, 'depth snapshot size does not match extent: ' + name)
    values = np.frombuffer(data, dtype='<f4').reshape(height, width)
    require(np.isfinite(values).all() and (values >= 0).all() and (values <= 1).all(),
            'depth snapshot has nonfinite/out-of-range values: ' + name)
    return values, hashlib.sha256(data).hexdigest()


def index_digest(indices):
    value = 14695981039346656037
    for word in indices:
        value = ((value ^ word) * 1099511628211) & 0xffffffffffffffff
    return value


def journal_identity(capture, records):
    draws = {r['journal_draw']:r for r in records}
    found = {draw:{} for draw in draws}
    names = [n for n in capture.names if re.fullmatch(r'queue-state/ds_prelight_events_\d+\.jsonl',n)]
    require(bool(names), 'missing execution journal for draw identity')
    for name in names:
        for line in capture.read(name).splitlines():
            event = json.loads(line)
            require(isinstance(event,dict), 'journal event must be an object')
            kind = event.get('kind')
            draw = event.get('seq') if kind == 'draw_prepared' else event.get('draw')
            if kind == 'geometry_query':
                require(isinstance(event.get('result'),dict), 'query result must be an object')
            if kind == 'geometry_query' and event['result'].get('kind') == 'original':
                event = event['result']
                draw,kind = event.get('journal_draw'),'original_query'
            if draw in found and kind in ('draw_prepared','draw_inputs','draw_execute','original_query'):
                require(kind not in found[draw], 'duplicate witness journal operation')
                found[draw][kind] = event
    report = {}
    for draw,record in draws.items():
        events = found[draw]
        require(set(events) == {'draw_prepared','draw_inputs','draw_execute','original_query'},
                'missing witness journal preparation/input/execution/query')
        prepared,inputs,execute,query = (events[k] for k in ('draw_prepared','draw_inputs','draw_execute','original_query'))
        require(not prepared['indirect'] and prepared['instances'] == 1 and prepared['topology'] == 3,
                'journal does not describe a direct, single-instance triangle list')
        require(prepared['indices'] == inputs['native_index_count'] == query['index_count_or_records'] == len(record['indices']),
                'journal index counts disagree with vertex snapshot')
        require(inputs['index_words'] == record['indices'] and inputs['index_words_captured'] == len(record['indices'])
                and inputs['index_digest'] == index_digest(record['indices']), 'journal index payload/digest mismatch')
        require(prepared['index_guest'] == query['index_guest'] == record['index_guest']
                and prepared['command'] == query['command'] == record['command'], 'journal command/index identity mismatch')
        require(prepared['vs'] == int(record['shader'],16) and prepared['ps'] == int(record['pixel_shader'],16)
                and query['vs'] == record['shader'] and query['ps'] == record['pixel_shader'], 'journal shader identity mismatch')
        require(inputs['vs_file'] == query['native_vs_file'] == record['native_vs_file']
                and inputs['ps_file'] == query['native_ps_file'] == record['native_ps_file'], 'journal bound module mismatch')
        require(inputs['depth_image']['owner'] == query['depth_owner'] == record['depth_owner'],
                'journal depth owner mismatch')
        require(inputs['vs_srt'] == record['srt'] and inputs['vs_user_data'] == record['user_data'],
                'journal vertex scalar snapshot mismatch')
        for key in ('depth_test','depth_write','depth_compare'):
            require(execute[key] == query[key] == record[key], 'live execution state mismatch: '+key)
        require(execute['depth_clear'] is False, 'unexpected executed clear on the selected sky draw')
        require(inputs['frame'] == prepared['frame'] == execute['frame'] == query['frame'] == record['frame'],
                'journal frame identity mismatch')
        report[str(draw)] = {'command':record['command'],'index_digest':inputs['index_digest'],
                             'input_depth_version':inputs['depth_image'].get('version'),
                             'query_depth_version':query.get('depth_version_before'),
                             'snapshot_depth_version':record.get('depth_version'),
                             'original_passed_samples':query['passed_samples']}
    return report


def clip_xy(poly):
    # No near/far or user-plane approximation: those cases are rejected below.
    for distance in (lambda p:p[3]+p[0], lambda p:p[3]-p[0],
                     lambda p:p[3]+p[1], lambda p:p[3]-p[1]):
        out = []
        for a, b in zip(poly, poly[1:] + poly[:1]):
            da, db = distance(a), distance(b)
            if da >= 0:
                out.append(a)
            if (da >= 0) != (db >= 0):
                out.append(a + (b-a) * (da/(da-db)))
        poly = out
        if not poly:
            break
    require(all(v[3] > 0 for v in poly), 'clip result touches W=0; finite projection unsupported')
    return poly


def raster(record, pos, width, height):
    viewport, scissor = record['viewport'], record['scissor']
    require(len(viewport) == 6 and all(type(x) in (int,float) and math.isfinite(x) for x in viewport),
            'missing/nonfinite live viewport')
    require(viewport[2] > 0 and viewport[3] != 0 and viewport[4:] == [0.0,1.0],
            'unsupported viewport extent/depth range')
    require(len(scissor) == 4 and all(type(x) is int for x in scissor) and min(scissor) >= 0,
            'invalid live scissor')
    require(record['dx_clip_space'] is True and record['depth_bias'] is False,
            'only DX clip convention with disabled depth bias is supported')
    require(record['depth_test'] is True and record['depth_write'] is True
            and record['depth_compare'] == 6 and record['depth_load_clear'] is False,
            'expected actual prepass GREATER_OR_EQUAL/write/no-load-clear state')
    require(not record['stencil_test'] or record['stencil_compare_front'] == record['stencil_compare_back'] == 7,
            'stencil filtering is not supported by this local raster model')
    sx,sy,sw,sh = scissor
    covered = np.zeros((height,width), dtype=bool)
    incoming = np.zeros((height,width), dtype=np.float64)
    triangles = 0
    for j in range(0,len(record['indices']),3):
        poly = clip_xy([pos[i] for i in record['indices'][j:j+3]])
        for k in range(1,len(poly)-1):
            v = np.stack((poly[0],poly[k],poly[k+1]))
            v = v[:,:3] / v[:,3,None]
            require(((v[:,2] >= 0) & (v[:,2] <= 1)).all(),
                    'near/far depth clip or clamp would be needed; unsupported')
            v[:,0] = viewport[0] + viewport[2]*(v[:,0]+1)*.5
            v[:,1] = viewport[1] + viewport[3]*(v[:,1]+1)*.5
            lo = np.maximum(np.ceil(v[:,:2].min(axis=0)-.5).astype(int), (max(0,sx),max(0,sy)))
            hi = np.minimum(np.floor(v[:,:2].max(axis=0)-.5).astype(int), (min(width-1,sx+sw-1),min(height-1,sy+sh-1)))
            if (hi < lo).any():
                continue
            a,b,c = v
            area = (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])
            if abs(area) < 1e-15:
                continue
            xx,yy = np.meshgrid(np.arange(lo[0],hi[0]+1)+.5,np.arange(lo[1],hi[1]+1)+.5)
            l0 = ((b[0]-xx)*(c[1]-yy)-(b[1]-yy)*(c[0]-xx))/area
            l1 = ((c[0]-xx)*(a[1]-yy)-(c[1]-yy)*(a[0]-xx))/area
            l2 = 1-l0-l1
            mask = (l0 >= -1e-10) & (l1 >= -1e-10) & (l2 >= -1e-10)
            z = l0*a[2]+l1*b[2]+l2*c[2]
            ys,xs = slice(lo[1],hi[1]+1),slice(lo[0],hi[0]+1)
            incoming[ys,xs] = np.where(mask,np.maximum(incoming[ys,xs],z),incoming[ys,xs])
            covered[ys,xs] |= mask
            triangles += 1
    return covered, incoming, triangles


def analyze(capture, depth_atol=1e-8):
    require(math.isfinite(depth_atol) and depth_atol >= 0, 'invalid analysis depth threshold')
    pre = capture.json('queue-state/ds_prelight_witness_sky_prepass.json')
    mat = capture.json('queue-state/ds_prelight_witness_sky_material.json')
    meta = capture.json('queue-state/ds_prelight_witness_depth.json')
    require(pre['shader'] == 'aff9c7ef505c6253' and mat['shader'] == 'c96f3fcde8141266'
            and pre['pixel_shader'] == '651f53f5932d8794' and mat['pixel_shader'] == '552d823dcfcf2dee',
            'capture is not the fixed V3 sky witness')
    require(pre['material'] is False and mat['material'] is True, 'swapped prepass/material records')
    for key in ('pair_id','depth_owner','index_guest','indices','viewport','scissor','base_vertex','first_instance','dx_clip_space'):
        require(pre[key] == mat[key], 'prepass/material identity mismatch: ' + key)
    require(meta['pair'] == pre['pair_id'] and meta['draw'] == mat['journal_draw']
            and meta['frame'] == mat['frame'] and pre['journal_draw'] < mat['journal_draw'],
            'depth manifest does not identify the captured draw pair')
    require(all(meta[k] is True for k in ('same_pair_serial','pre_before_saved','pre_after_saved',
                                        'material_before_saved','material_position_saved')),
            'depth/position manifest reports missing or unpaired snapshots')
    width,height = meta['pre_width'],meta['pre_height']
    require(type(width) is int and type(height) is int and width > 0 and height > 0
            and width*height <= 16777216, 'unsupported snapshot extent')
    require([width,height] == [meta['material_width'],meta['material_height']]
            and meta['material_indices'] == mat['indices'], 'depth/index manifest extent or identity mismatch')
    require(mat['depth_test'] is True and mat['depth_write'] is False and mat['depth_compare'] == 2
            and mat['depth_bias'] is False and mat['depth_load_clear'] is False,
            'material is not the captured read-only EQUAL/no-clear pass')
    require(not mat['stencil_test'] or mat['stencil_compare_front'] == mat['stencil_compare_back'] == 7,
            'material stencil filtering is unsupported')
    ppos,mpos = positions(pre),positions(mat)
    require(ppos.keys() == mpos.keys() and all(np.array_equal(ppos[v],mpos[v]) for v in ppos),
            'prepass/material captured positions differ')
    modules = {'prepass':vertex_modules(capture,pre),'material':vertex_modules(capture,mat)}
    for label, record in (('prepass',pre),('material',mat)):
        modules[label]['pixel'] = module(capture,record['native_ps_file'],'ps',record['pixel_shader'])[0]
    identity = journal_identity(capture,(pre,mat))
    before,before_sha = read_depth(capture,'ds_prelight_witness_depth_pre_before.bin',width,height)
    after,after_sha = read_depth(capture,'ds_prelight_witness_depth_pre_after.bin',width,height)
    material_same = meta['material_before_equals_pre_after'] is True
    if not material_same or 'queue-state/ds_prelight_witness_depth_material_before.bin' in capture.names:
        material_before,_ = read_depth(capture,'ds_prelight_witness_depth_material_before.bin',width,height)
        actual_same = np.array_equal(after.view('<u4'),material_before.view('<u4'))
        require(not material_same or actual_same, 'deduplicated depth claim contradicts material snapshot')
        material_same = actual_same
    covered,incoming,triangles = raster(pre,ppos,width,height)
    expected_change = covered & (incoming >= before) & (incoming != before)
    observed_change = after.view('<u4') != before.view('<u4')
    mismatch = expected_change != observed_change
    expected = np.where(covered,np.maximum(incoming,before),before)
    error = np.abs(expected-after)
    numerical = int(np.count_nonzero(error > depth_atol))
    examples = [[int(x),int(y)] for y,x in np.argwhere(mismatch)[:16]]
    result = {
        'schema':1,'scope':'LOCAL_RASTER_FROM_CAPTURED_NATIVE_XYZW','tree_node':'V3/G0/P8',
        'tree_decision':'BLOCKED','tree_interval':[0,16],
        'local_status':'LOCAL_MEASUREMENT_CONSISTENT' if not mismatch.any() and not numerical and material_same
                       else 'LOCAL_MISMATCH_CANDIDATE',
        'witness':{'prepass_draw':pre['journal_draw'],'material_draw':mat['journal_draw'],
                   'pair':pre['pair_id'],'depth_owner':pre['depth_owner'],'extent':[width,height]},
        'module_identity':modules,
        'journal_identity':identity,
        'live_state':{'viewport':pre['viewport'],'scissor':pre['scissor'],
                      'depth_bias':pre['depth_bias'],'prepass_depth_compare':pre['depth_compare'],
                      'material_depth_compare':mat['depth_compare']},
        'snapshots':{'before_sha256':before_sha,'after_sha256':after_sha,
                     'material_before_equals_pre_after':material_same},
        'raster':{'vertices':len(ppos),'input_triangles':len(pre['indices'])//3,
                  'clipped_triangles':triangles,'covered_pixels':int(covered.sum()),
                  'expected_depth_changed':int(expected_change.sum()),
                  'observed_depth_changed':int(observed_change.sum()),
                  'changed_mask_mismatches':int(mismatch.sum()),'mismatch_xy_examples':examples,
                  'same_captured_clip_positions':True},
        'depth_numerics':{'maximum_absolute_error':float(error.max()),
                          'median_absolute_error':float(np.median(error)),
                          'analysis_absolute_threshold':depth_atol,'pixels_above_threshold':numerical,
                          'hardware_contract_certified':False},
        'limitations':[
            'Native captured XYZW is the input; guest VS arithmetic and foreground expected visibility are not certified.',
            'Float64 center samples, no-cull union and approximate inclusive edges; not GPU top-left/subpixel/bit-exact rasterization.',
            'The numerical threshold is for analysis only, not a Vulkan precision guarantee; mismatch is a local candidate.',
            'Original fingerprints and instrumented bound-module fingerprints intentionally differ.',
            'No complete PS, G-buffer, producer, publication, lighting or final-output oracle; this result never changes P8.']}
    return result


def run(path, depth_atol=1e-8):
    capture = None
    try:
        capture = Capture(path)
        return analyze(capture,depth_atol)
    except (Blocked,KeyError,TypeError,ValueError,OSError,zipfile.BadZipFile,struct.error) as exc:
        return {'schema':1,'scope':'LOCAL_RASTER_FROM_CAPTURED_NATIVE_XYZW',
                'tree_node':'V3/G0/P8','tree_decision':'BLOCKED','tree_interval':[0,16],
                'local_status':'BLOCKED','reason':str(exc)}
    finally:
        if capture:
            capture.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('capture',help='diagnostics ZIP, extracted diagnostics directory or capture directory')
    parser.add_argument('--depth-atol',type=float,default=1e-8,help='analysis threshold only; default 1e-8, not a conformance bound')
    parser.add_argument('--output',type=Path,help='write the same JSON report to this path')
    args = parser.parse_args()
    report = run(args.capture,args.depth_atol)
    rendered = json.dumps(report,indent=2,allow_nan=False) + '\n'
    if args.output:
        args.output.write_text(rendered,encoding='utf-8')
    print(rendered,end='')
    return 2 if report['local_status'] == 'BLOCKED' else 3 if report['local_status'] == 'LOCAL_MISMATCH_CANDIDATE' else 0


if __name__ == '__main__':
    raise SystemExit(main())
