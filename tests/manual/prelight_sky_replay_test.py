#!/usr/bin/env python3
"""Analytic raster fixture and fail-closed reader tests; no guest/P8 oracle claim.

Run: python tests/manual/prelight_sky_replay_test.py
Optional actual-data check: python docs/android/tools/prelight_sky_replay.py CAPTURE.zip
The tiny fixture is a constant-depth square with a known 4x4 center-sample mask.
Its SPIR-V only exercises artifact identity checks, not shader interpretation.
"""
import copy
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

SCRIPT = Path(__file__).resolve().parents[2] / 'docs/android/tools/prelight_sky_replay.py'
SPEC = importlib.util.spec_from_file_location('prelight_sky_replay', SCRIPT)
replay = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(replay)


def module_bytes(stage):
    # Valid instruction framing and a stage entry point; no numerical shader oracle.
    words = [0x07230203,0x00010300,0,5,0,
             (5<<16)|15,0 if stage == 'vs' else 4,1,0x6e69616d,0,
             (2<<16)|19,2,(3<<16)|33,3,2,
             (5<<16)|54,2,1,0,3,(2<<16)|248,4,
             (1<<16)|253,(1<<16)|56]
    return struct.pack('<'+'I'*len(words),*words)


def float_word(x):
    return struct.unpack('<I',struct.pack('<f',x))[0]


class SkyReplayTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)/'ds-lighting-capture'
        (self.root/'queue-state').mkdir(parents=True)
        (self.root/'spv').mkdir()
        hashes = [('vs','aff9c7ef505c6253'),('vs','c96f3fcde8141266'),
                  ('ps','651f53f5932d8794'),('ps','552d823dcfcf2dee')]
        paths = {}
        for stage,guest in hashes:
            data = module_bytes(stage)
            fp = replay.xxh3(data)
            name = f'spv/{stage}_{guest}_{fp}.spv'
            (self.root/name).write_bytes(data)
            paths[guest] = name
        rows = [[i,0,0]+[float_word(x) for x in p]+[0] for i,p in enumerate(
                ((-1.,-1.,.5,1.),(1.,-1.,.5,1.),(-1.,1.,.5,1.),(1.,1.,.5,1.)))]
        self.pre = {
            'shader':'aff9c7ef505c6253','pixel_shader':'651f53f5932d8794',
            'fingerprint':replay.xxh3(module_bytes('vs')),
            'native_vs_file':paths['aff9c7ef505c6253'],'native_ps_file':paths['651f53f5932d8794'],
            'material':False,'pair_id':9,'depth_owner':4096,'index_guest':8192,
            'indices':[0,1,2,2,1,3],'viewport':[0.,4.,4.,-4.,0.,1.],
            'scissor':[0,0,4,4],'base_vertex':0,'first_instance':0,'journal_draw':10,
            'command':9,'srt':[],'user_data':[],
            'frame':3,'missing':0,'columns':['vertex','instance','transform','clip_x','clip_y','clip_z','clip_w','clip_distance'],
            'rows':rows,'dx_clip_space':True,'depth_bias':False,'depth_test':True,
            'depth_write':True,'depth_compare':6,'depth_load_clear':False,
            'stencil_test':False,'stencil_compare_front':7,'stencil_compare_back':7}
        self.mat = copy.deepcopy(self.pre)
        self.mat.update(shader='c96f3fcde8141266',pixel_shader='552d823dcfcf2dee',
                        native_vs_file=paths['c96f3fcde8141266'],native_ps_file=paths['552d823dcfcf2dee'],
                        material=True,journal_draw=20,command=19,depth_write=False,depth_compare=2)
        self.meta = {'pair':9,'draw':20,'frame':3,'same_pair_serial':True,
                     'pre_before_saved':True,'pre_after_saved':True,'material_before_saved':True,
                     'material_position_saved':True,'pre_width':4,'pre_height':4,
                     'material_width':4,'material_height':4,'material_indices':[0,1,2,2,1,3],
                     'material_before_equals_pre_after':True}
        self.save()
        self.before = self.root/'queue-state/ds_prelight_witness_depth_pre_before.bin'
        self.after = self.root/'queue-state/ds_prelight_witness_depth_pre_after.bin'
        self.before.write_bytes(struct.pack('<16f',*([0.]*16)))
        self.after.write_bytes(struct.pack('<16f',*([.5]*16)))

    def tearDown(self):
        self.temp.cleanup()

    def save(self):
        for filename,obj in (('sky_prepass',self.pre),('sky_material',self.mat),('depth',self.meta)):
            (self.root/f'queue-state/ds_prelight_witness_{filename}.json').write_text(json.dumps(obj))
        journal = []
        for r in (self.pre,self.mat):
            draw = r['journal_draw']
            journal.append({'kind':'draw_prepared','seq':draw,'command':r['command'],
                            'indirect':False,'instances':1,'topology':3,'indices':len(r['indices']),
                            'index_guest':r['index_guest'],'vs':int(r['shader'],16),
                            'ps':int(r['pixel_shader'],16),'frame':r['frame']})
            journal.append({'kind':'draw_inputs','draw':draw,'frame':r['frame'],
                            'index_words':r['indices'],'native_index_count':len(r['indices']),
                            'index_words_captured':len(r['indices']),'index_digest':replay.index_digest(r['indices']),
                            'vs_file':r['native_vs_file'],'ps_file':r['native_ps_file'],
                            'depth_image':{'owner':r['depth_owner']},'vs_srt':r['srt'],'vs_user_data':r['user_data']})
            journal.append({'kind':'draw_execute','draw':draw,'frame':r['frame'],'depth_clear':False,
                            'depth_test':r['depth_test'],'depth_write':r['depth_write'],'depth_compare':r['depth_compare']})
            journal.append({'kind':'geometry_query','result':{
                'kind':'original','journal_draw':draw,'frame':r['frame'],'command':r['command'],
                'index_count_or_records':len(r['indices']),'index_guest':r['index_guest'],
                'vs':r['shader'],'ps':r['pixel_shader'],'native_vs_file':r['native_vs_file'],
                'native_ps_file':r['native_ps_file'],'depth_owner':r['depth_owner'],
                'depth_test':r['depth_test'],'depth_write':r['depth_write'],
                'depth_compare':r['depth_compare'],'passed_samples':16}})
        (self.root/'queue-state/ds_prelight_events_0.jsonl').write_text('\n'.join(json.dumps(x) for x in journal))

    def result(self):
        return replay.run(self.root)

    def blocked(self):
        result = self.result()
        self.assertEqual(result['local_status'],'BLOCKED',result)
        self.assertEqual(result['tree_decision'],'BLOCKED')

    def test_analytic_square_and_zip(self):
        result = self.result()
        self.assertEqual(result['local_status'],'LOCAL_MEASUREMENT_CONSISTENT',result)
        self.assertEqual(result['raster']['covered_pixels'],16)
        self.assertEqual(result['raster']['observed_depth_changed'],16)
        self.assertEqual(result['depth_numerics']['maximum_absolute_error'],0)
        self.assertFalse(result['depth_numerics']['hardware_contract_certified'])
        self.assertEqual(result['tree_decision'],'BLOCKED')
        path = Path(self.temp.name)/'fixture.zip'
        with zipfile.ZipFile(path,'w') as archive:
            for source in self.root.rglob('*'):
                if source.is_file(): archive.write(source,source.relative_to(self.root.parent))
        self.assertEqual(replay.run(path),result)

    def test_missing_snapshot(self):
        self.after.unlink()
        self.blocked()

    def test_truncated_snapshot(self):
        self.after.write_bytes(b'\x00'*12)
        self.blocked()

    def test_undefined_vertex(self):
        self.pre['indices'][0] = self.mat['indices'][0] = self.meta['material_indices'][0] = 99
        self.save()
        self.blocked()

    def test_missing_viewport(self):
        del self.pre['viewport']
        self.save()
        self.blocked()

    def test_wrong_pair(self):
        self.meta['pair'] += 1
        self.save()
        self.blocked()

    def test_unsupported_fragment_state(self):
        self.pre['depth_bias'] = True
        self.save()
        self.blocked()

    def test_module_tamper(self):
        path = self.root/self.pre['native_vs_file']
        data = bytearray(path.read_bytes());data[9] ^= 1;path.write_bytes(data)
        self.blocked()

    def test_missing_journal(self):
        (self.root/'queue-state/ds_prelight_events_0.jsonl').unlink()
        self.blocked()

    def test_nonobject_journal_row_is_blocked(self):
        path = self.root/'queue-state/ds_prelight_events_0.jsonl'
        with self.subTest(kind='event'):
            path.write_text('[]\n')
            self.blocked()
        with self.subTest(kind='query result'):
            path.write_text('{"kind":"geometry_query","result":[]}\n')
            self.blocked()

    def test_stale_execution_state(self):
        path = self.root/'queue-state/ds_prelight_events_0.jsonl'
        events = [json.loads(x) for x in path.read_text().splitlines()]
        next(x for x in events if x['kind'] == 'draw_execute')['depth_compare'] = 7
        path.write_text('\n'.join(json.dumps(x) for x in events))
        self.blocked()

    def test_local_mask_mismatch_never_p8_fail(self):
        self.after.write_bytes(struct.pack('<16f',0.,*([.5]*15)))
        result = self.result()
        self.assertEqual(result['local_status'],'LOCAL_MISMATCH_CANDIDATE',result)
        self.assertEqual(result['raster']['changed_mask_mismatches'],1)
        self.assertEqual(result['tree_decision'],'BLOCKED')

    def test_local_numeric_mismatch_with_same_mask(self):
        self.after.write_bytes(struct.pack('<16f',.75,*([.5]*15)))
        result = self.result()
        self.assertEqual(result['local_status'],'LOCAL_MISMATCH_CANDIDATE',result)
        self.assertEqual(result['raster']['changed_mask_mismatches'],0)
        self.assertEqual(result['depth_numerics']['pixels_above_threshold'],1)
        self.assertEqual(result['tree_decision'],'BLOCKED')

    def test_snapshot_contradicts_dedup_manifest(self):
        path = self.root/'queue-state/ds_prelight_witness_depth_material_before.bin'
        path.write_bytes(struct.pack('<16f',*([0.]*16)))
        self.blocked()


if __name__ == '__main__':
    unittest.main()
