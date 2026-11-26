#!/usr/bin/env python3
"""Independent position-oracle tests with actual gfx1010 bytecode and analytic inputs.

Run: python3 tests/manual/prelight_vertex_oracle_test.py
Requires NumPy and LLVM's AMDGPU disassembler. The 304-byte guest program is the
actual hash-bound aff9 vertex shader; identity transforms and simple dyadic
coordinates supply an analytic expected answer independent of both emitters.
No diagnostic ZIP, device, network, emulator decoder or captured XYZW is needed.
"""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'docs/android/tools'))
import prelight_vertex_oracle as oracle


GUEST = bytes.fromhex(
    '0300a0bf03ffeb930800080003ff0087ff0000006b8c6a8f006a7c88000080bf090090bf'
    'c06bea81c16afe90410900f800000000c000ea810fff8cbfc16afe90f910084a0a068406'
    '841a04f4100000fa040408f4000000fa7fc08cbf350508f4100000fa080608f4100000fa'
    '090010f4c00000fa891a04f4c00100fa7fc08cbf202038e0040a0580102038e004000580'
    '002038e004060580002008e0050e0680703f8cbf10191a3e040041d502210e041011123e'
    '0f171a3e0f03083e0f0f123e0e151a3e010041d5001d12040e0d123e040041d5021a0e00'
    '050041d50e1a3e00030041d5061a1e00020041d50a1a2e000102083e0d020a3e0502063e'
    '000041d509020a04010041d5001212040c120a3e0412063e0812003e6a0a023e6b0a063e'
    'cf0800f801030005000081bf00000000')
SHADER = 'aff9c7ef505c6253'
GUEST_2337 = bytes.fromhex(
    '0300a0bf03ffeb930800080003ff0087ff0000006b8c6a8f006a7c88000080bf090090bf'
    'c06bea81c16afe90410900f800000000c000ea810fff8cbfc16afe908002067e04060cf4'
    '000000fa7fc08cbf0c0c08f4100000fa0d0808f4000700fa0d000cf4e00600fa0e0d08f4'
    '100000fa8d1a00f4d80200fa0f0908f4140000fa0d0e04f4d00200fa0d0a08f4000200fa'
    '0d020cf4e00100fa0d0b08f4300000fa0d040cf4000000fa8d0e04f4c00100fa7fc08cbf'
    '002008e005000c801a0c28f4200000fa1a062cf4000000faf972080824068686703f8cbf'
    '22040c1006040a1002040e10020041d50143180405020a3e01020e3e2000043e010041d5'
    '0400160400000e3e7fc08cbf32040c101e040a101a040010020041d531021a041d020a3e'
    '1902003e300e043e1c0e0a3e180e003e020041d5ff04aa8100606a460509024000606a46'
    '000041d5ff00e28000606a46020041d527d4080401001fd526080220000041d525700004'
    '2a040a100e0408100a040c10020041d529021604070041d50d02120409020c3e2800043e'
    '0c000e3e08000c3e050041d52e04be00040041d512044e00020041d516045e00010041d5'
    '2d0e1604110e083e000041d5150e0a042c0c023e020041d5100c1204140c003e3a02043e'
    '3b02003ecf0800f802000301000081bf000000000000000000000000')
COLUMNS = ['vertex', 'instance', 'transform', 'clip_x', 'clip_y', 'clip_z', 'clip_w', 'clip_distance',
           'input_x', 'input_y', 'input_z', 'm00', 'm01', 'm02', 'm03', 'm10', 'm11', 'm12', 'm13',
           'm20', 'm21', 'm22', 'm23']


def fwords(values):
    return np.asarray(values, dtype='<f4').view('<u4').tolist()


class MemoryCapture:
    def __init__(self, extra=None):
        self.data = {f'guest-shaders/vs_{SHADER}_probe.bin': GUEST, **(extra or {})}
        self.names = sorted(self.data)

    def read(self, name):
        if name not in self.data:
            raise oracle.Unsupported('missing capture member: ' + name)
        return self.data[name]

    def json(self, name):
        return json.loads(self.read(name))


def fixture():
    transform = [0x5000, 160 << 16, 1, 20996, 0, 0, 0, 0]
    positions = [0x6000, 12 << 16, 3, 304044, 0, 0, 0, 0]
    coords = [[.25, .5, .75], [-.25, .25, .5], [.5, -.5, .25]]
    matrix = fwords([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0] + [0] * 28)
    projection = fwords([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
    def region(address, values):
        return {'address': address, 'bytes': len(values) * 4, 'source': 'consumed_srt', 'read_kind': 'host', 'words': values}
    result = {'journal_draw': 1, 'frame': 1, 'submit': 1, 'shader': SHADER, 'columns': COLUMNS,
              'rows': [[i, 0, 0, *fwords([*p, 1]), *([0] * 16)] for i, p in enumerate(coords)],
              'base_vertex': 0, 'first_instance': 0, 'instances': 1, 'indexed': True,
              'indices': [0, 1, 2], 'missing': 0, 'dx_clip_space': True,
              'vs_user_data': [0x1000, 0, 0], 'vs_user_data_base': 8,
              'vs_buffers': [transform, positions],
              'vs_bound': [{'slot': 0, 'address': 0x5000, 'bytes': 160, 'gpu_words': matrix},
                           {'slot': 1, 'address': 0x6000, 'bytes': 36, 'gpu_words': fwords(np.asarray(coords).flatten())}],
              'vs_scalar_capture_complete': True,
              'vs_scalar_regions': [region(0x1000, [0x2000, 0, 0x3000, 0]), region(0x1010, [0x4000, 0]),
                                    region(0x4010, transform[:4]), region(0x2010, positions[:4]),
                                    region(0x30c0, projection), region(0x31c0, [0, 0])],
              'srt': [0x4000, 0, 0x2000, 0, 0x3000, 0, *transform[:4], *positions[:4], *projection, 0, 0]}
    return result


class VertexOracleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.decoder = oracle.LLVM()

    @classmethod
    def tearDownClass(cls):
        cls.decoder.close()

    def replay(self, record=None, capture=None, strict=True):
        return oracle.replay_draw(capture or MemoryCapture(), record or fixture(), self.decoder, strict)

    def test_actual_guest_bytes_analytic_identity(self):
        result = self.replay()
        self.assertEqual(result['status'], 'POSITION_MATCH', result)
        self.assertEqual(result['bitwise_equal_components'], 12)
        self.assertEqual(result['scalar_binding_mode'], 'consumed_scalar_regions')
        self.assertEqual(result['export_pc'], '0x120')
        self.assertFalse(result['p8_certificate'])
        self.assertEqual(result['invocation_association']['cpu_clip']['expected_triangle_classes']['inside_all_planes'], 1)

    def test_2337_scalar_buffer_sdwa_and_literal_mad_zero_z(self):
        record = fixture()
        record['shader'] = '2337192397a30339'
        regions = {
            0x1000: [0x2000, 0, 0x3000, 0, 0x4000, 0, 0x7000, 0],
            0x2010: record['vs_buffers'][1][:4], 0x4010: record['vs_buffers'][0][:4],
            0x36e0: fwords([1, 0, 0, 0, 0, 1, 0, 0]), 0x3700: fwords([0, 0, 1, 0]),
            0x32d8: [0], 0x7014: [0] * 4, 0x32d0: [0] * 2,
            0x3200: fwords([0, 0, 1, 0]), 0x31e0: fwords([1, 0, 0, 0, 0, 1, 0, 0]),
            0x3030: fwords([0, 0, 0, 1]), 0x3000: fwords([1, 0, 0, 0, 0, 1, 0, 0]), 0x31c0: [0, 0],
        }
        record['vs_scalar_regions'] = [{'address': address, 'bytes': len(values) * 4, 'words': values,
                                        'source': 'consumed_srt'} for address, values in regions.items()]
        for row in record['rows']:
            x, y = np.asarray(row[3:5], dtype='<u4').view('<f4')
            row[3:7] = fwords([x * 15000, y * 15000, 0, 1])
        capture = MemoryCapture({'guest-shaders/vs_2337192397a30339_probe.bin': GUEST_2337})
        result = self.replay(record, capture)
        self.assertEqual(result['status'], 'POSITION_MATCH', result)
        self.assertEqual(result['bitwise_equal_components'], 12)
        self.assertEqual(result['export_pc'], '0x1d8')

    def test_native_output_mutation_is_real_divergence(self):
        record = fixture()
        record['rows'][1][3] = fwords([16.0])[0]
        result = self.replay(record)
        self.assertEqual(result['status'], 'POSITION_DIVERGENCE')
        self.assertEqual(result['different_components'], 1)
        self.assertEqual(result['first_local_divergence']['guest_export_pc'], '0x120')
        self.assertEqual(result['clip']['plane_mask_different_vertices'], 1)
        self.assertFalse(result['first_local_divergence']['earliest_instruction_proven'])

    def test_native_buffer_mutation_changes_reference(self):
        record = fixture()
        record['vs_bound'][1]['gpu_words'][0] = fwords([2.0])[0]
        result = self.replay(record)
        self.assertEqual(result['status'], 'POSITION_DIVERGENCE')
        self.assertEqual(result['mismatch_examples'][0]['expected'], '2.0')
        self.assertEqual(result['mismatch_examples'][0]['observed'], '0.25')

    def test_probe_input_and_matrix_columns_never_seed_reference(self):
        record = fixture()
        for row in record['rows']:
            row[8:] = [0xffffffff] * 15
        result = self.replay(record)
        self.assertEqual(result['status'], 'POSITION_MATCH', result)

    def test_nonindexed_first_vertex_is_not_added_to_builtin_twice(self):
        record = fixture()
        record['indexed'] = False
        record['first_vertex'] = 1
        record['indices'] = [0, 1, 0]
        record['rows'] = record['rows'][1:]
        result = self.replay(record)
        self.assertEqual(result['status'], 'POSITION_MATCH', result)

    def test_scalar_projection_mutation_is_consumed(self):
        record = fixture()
        record['vs_scalar_regions'][-2]['words'][0] = fwords([2.0])[0]
        result = self.replay(record)
        self.assertEqual(result['status'], 'POSITION_DIVERGENCE')
        self.assertEqual(result['different_components'], 3)

    def test_mutating_actual_guest_instruction_changes_reference(self):
        import struct
        from prelight_sky_replay import xxh3
        guest = bytearray(GUEST)
        original = struct.unpack_from('<I', guest, 0xa4)[0]
        self.assertEqual(original >> 25, 0x1f)  # V_MAC_F32
        struct.pack_into('<I', guest, 0xa4, (original & 0x01ffffff) | (3 << 25))  # V_ADD_F32
        changed_hash = xxh3(bytes(guest))
        record = fixture()
        wrong_identity = MemoryCapture()
        wrong_identity.data[f'guest-shaders/vs_{SHADER}_probe.bin'] = bytes(guest)
        self.assertEqual(self.replay(record, wrong_identity)['status'], 'UNSUPPORTED_DRAW')
        record['shader'] = changed_hash
        capture = MemoryCapture({f'guest-shaders/vs_{changed_hash}_probe.bin': bytes(guest)})
        result = self.replay(record, capture)
        self.assertEqual(result['status'], 'POSITION_DIVERGENCE', result)
        self.assertEqual(result['different_components'], 3)
        self.assertEqual(result['mismatch_examples'][0]['component'], 'z')
        self.assertEqual(result['mismatch_examples'][0]['expected'], '1.75')
        self.assertEqual(result['mismatch_examples'][0]['observed'], '0.75')

    def test_missing_scalar_fails_only_its_draw(self):
        record = fixture()
        record['vs_scalar_regions'].pop()
        result = self.replay(record)
        self.assertEqual(result['status'], 'UNSUPPORTED_DRAW')
        self.assertIn('0x74', result['reason'])
        self.assertIn('0x31c0', result['reason'])
        self.assertEqual(self.replay()['status'], 'POSITION_MATCH')

    def test_conflicting_scalar_reads_do_not_pick_convenient_value(self):
        record = fixture()
        bad = copy.deepcopy(record['vs_scalar_regions'][-1])
        bad['words'][0] = 1
        record['vs_scalar_regions'].append(bad)
        result = self.replay(record)
        self.assertEqual(result['status'], 'UNSUPPORTED_DRAW')
        self.assertIn('conflicting', result['reason'])

    def test_unused_scalar_word_does_not_block_position(self):
        decoded = [(0x40, 8, 's_load_dwordx4 s[0:3], s[8:9], null'),
                   (0x48, 4, 'v_mov_b32_e32 v0, s0'), (0x4c, 8, 'exp pos0 v0, v0, v0, v0 done')]
        program = oracle.Program(decoded, {}, [0x1000, 0])
        record = fixture()
        record['vs_scalar_regions'] = [{'address': 0x1000, 'bytes': 4, 'source': 'consumed_srt', 'words': fwords([.5])}]
        evaluator = oracle.Evaluator(MemoryCapture(), record, {}, oracle.position_rows(record), True)
        expected = evaluator.position(program)
        self.assertTrue(np.all(expected == fwords([.5])[0]))

    def test_new_scalar_log_never_falls_back_to_legacy(self):
        record = fixture()
        record['vs_scalar_regions'] = []
        result = self.replay(record, strict=False)
        self.assertEqual(result['status'], 'UNSUPPORTED_DRAW')
        self.assertIn('missing consumed scalar', result['reason'])

    def test_legacy_map_is_explicit_and_optional(self):
        record = fixture()
        del record['vs_scalar_regions']
        result = self.replay(record, strict=False)
        self.assertEqual(result['status'], 'POSITION_MATCH', result)
        self.assertEqual(result['scalar_binding_mode'], 'legacy_flattened_srt_binding_premise')
        self.assertEqual(self.replay(record, strict=True)['status'], 'UNSUPPORTED_DRAW')

    def test_large_native_binary_payload_with_offset(self):
        record = fixture()
        payload = bytearray(b'HEADER-16-BYTES!!')
        for source in record['vs_bound']:
            source['file'] = 'queue-state/ds_prelight_witness_data_1.bin'
            source['offset_bytes'] = len(payload)
            payload.extend(np.asarray(source.pop('gpu_words'), dtype='<u4').tobytes())
        capture = MemoryCapture({'queue-state/ds_prelight_witness_data_1.bin': bytes(payload)})
        self.assertEqual(self.replay(record, capture)['status'], 'POSITION_MATCH')
        capture.data['queue-state/ds_prelight_witness_data_1.bin'] = bytes(payload[:-1])
        result = self.replay(record, capture)
        self.assertEqual(result['status'], 'UNSUPPORTED_DRAW')
        self.assertIn('truncated native payload', result['reason'])

    def test_cpu_backing_is_never_a_missing_gpu_payload_substitute(self):
        record = fixture()
        source = record['vs_bound'][1]
        source['backing_words'] = source.pop('gpu_words')
        result = self.replay(record)
        self.assertEqual(result['status'], 'UNSUPPORTED_DRAW')
        self.assertIn('0x98', result['reason'])
        self.assertIn('captured 0', result['reason'])

    def test_user_data_sgpr_base_is_used(self):
        record = fixture()
        record['vs_user_data_base'] = 4
        record['vs_user_data'] = [0, 0, 0, 0, *record['vs_user_data']]
        self.assertEqual(self.replay(record)['status'], 'POSITION_MATCH')

    def test_effective_vertex_index_does_not_add_base_twice(self):
        record = fixture()
        record['base_vertex'] = 1
        record['indices'] = [0, 1, 0]
        record['rows'] = record['rows'][1:]
        result = self.replay(record)
        self.assertEqual(result['status'], 'POSITION_MATCH', result)

    def test_mismatched_base_or_index_stream_is_rejected(self):
        record = fixture()
        record['base_vertex'] = 1
        result = self.replay(record)
        self.assertEqual(result['status'], 'UNSUPPORTED_DRAW')
        self.assertIn('index stream + base_vertex', result['reason'])
        record = fixture()
        record['indices'] = [1, 1, 2]
        self.assertEqual(self.replay(record)['status'], 'UNSUPPORTED_DRAW')

    def test_nan_propagation_is_not_invented_visibility(self):
        record = fixture()
        record['vs_bound'][1]['gpu_words'][0] = 0x7fc01234
        record['rows'][0][3:7] = [0x7fc00000] * 4
        result = self.replay(record)
        self.assertEqual(result['status'], 'POSITION_MATCH', result)
        self.assertEqual(result['matching_nan_components'], 4)
        self.assertEqual(result['nan_payload_differences'], 4)
        self.assertEqual(result['clip']['expected_nonfinite_vertices'], 1)

    def test_unsupported_operand_has_instruction_pc(self):
        program = oracle.Program([(0x40, 4, 'v_unimplemented v0, v5'),
                                  (0x44, 8, 'exp pos0 v0, v0, v0, v0 done')], {}, [0, 0])
        evaluator = oracle.Evaluator(MemoryCapture(), fixture(), {}, oracle.position_rows(fixture()), True)
        with self.assertRaisesRegex(oracle.Unsupported, '0x40.*unsupported instruction'):
            evaluator.position(program)

    def expression(self, op, values, record=None):
        record = record or fixture()
        evaluator = oracle.Evaluator(MemoryCapture(), record, {}, oracle.position_rows(record), True)
        expr = oracle.node(op, *(oracle.node('const', v) for v in values), pc=0x1234)
        return evaluator.eval(expr), evaluator

    def test_sdwa_comparison_explicit_destination_preserves_vcc_jitter(self):
        # Actual #745 a720 instruction writes S16; the reference must never
        # replace VCC with that unrelated normal/handedness comparison mask.
        program = oracle.Program([(0x40, 8, 's_mov_b32 vcc_lo, 0x3f800000'),
                                  (0x48, 8, 'v_cmp_lt_f32_sdwa s16, 0.5, v17 src0_sel:DWORD src1_sel:DWORD'),
                                  (0x50, 4, 'v_mov_b32_e32 v0, vcc_lo'),
                                  (0x54, 8, 'exp pos0 v0, v0, v0, v0 done')], {}, [])
        evaluator = oracle.Evaluator(MemoryCapture(), fixture(), {}, oracle.position_rows(fixture()), True)
        self.assertTrue(np.all(evaluator.position(program) == fwords([1])[0]))

    def test_lshl_or_uses_low_five_shift_bits_and_uint32_result(self):
        value, _ = self.expression('ulshl_or', [0x80000001, 40, 0x23])
        self.assertEqual(int(value), 0x123)
        value, _ = self.expression('ulshlrev', [33, 0x80000003])
        self.assertEqual(int(value), 6)

    def test_float_to_uint_is_saturating_truncation_with_nan_zero(self):
        for source, expected in [(3.75, 3), (-3.75, 0), (float('inf'), 0xffffffff),
                                 (-float('inf'), 0), (float('nan'), 0), (4294967296.0, 0xffffffff)]:
            value, _ = self.expression('fcvt_u32', fwords([source]))
            self.assertEqual(int(value), expected)

    def test_integer_to_float_uses_signed_source(self):
        value, _ = self.expression('icvt_f32', [0xffffffff])
        self.assertEqual(int(value), fwords([-1.0])[0])

    def test_omod_scales_whole_mac_not_only_product(self):
        record = fixture()
        record.update(vs_float_mode=192, vs_ieee_mode=False)
        program = oracle.Program([(0x40, 4, 'v_mov_b32_e32 v0, 4.0'),
                                  (0x44, 8, 'v_mac_f32_e64 v0, 1.5, 2.0 mul:2'),
                                  (0x4c, 8, 'exp pos0 v0, v0, v0, v0 done')], {}, [])
        evaluator = oracle.Evaluator(MemoryCapture(), record, {}, oracle.position_rows(record), True)
        self.assertTrue(np.all(evaluator.position(program) == fwords([14])[0]))
        self.assertEqual(evaluator.numeric_events['output_modifiers_applied'], 1)

    def test_omod_ignored_with_ieee_or_output_denorms(self):
        for mode, ieee in [(192, True), (0x20, False), (0x30, False)]:
            record = fixture()
            record.update(vs_float_mode=mode, vs_ieee_mode=ieee)
            value, evaluator = self.expression('fomod', [1, fwords([2])[0]], record)
            self.assertEqual(int(value), 1)  # no re-flushing after a disabled modifier
            self.assertEqual(evaluator.numeric_events['output_modifiers_ignored'], 1)

    def test_captured_fp32_mode_flushes_ordinary_denormals(self):
        record = fixture()
        record.update(vs_float_mode=192, vs_ieee_mode=False)
        value, evaluator = self.expression('fmul', [1, fwords([2 ** 126])[0]], record)
        self.assertEqual(int(value), 0)
        self.assertEqual(evaluator.numeric_events['flushed_input_subnormals'], 1)

    def test_mad_flushes_denormals_even_with_allow_mode(self):
        record = fixture()
        record.update(vs_float_mode=0x30, vs_ieee_mode=False)
        value, evaluator = self.expression('fmad', [1, fwords([2 ** 126])[0], 0], record)
        self.assertEqual(int(value), 0)
        self.assertEqual(evaluator.numeric_events['flushed_input_subnormals'], 1)

    def test_skipped_conditional_value_cannot_become_position_proof(self):
        program = oracle.Program([(0x90, 4, 'v_mov_b32_e32 v0, 1.0'),
                                  (0xc4, 8, 'exp pos0 v0, v0, v0, v0 done')],
                                 {'conditional': (0x90, 0xc4)}, [])
        evaluator = oracle.Evaluator(MemoryCapture(), fixture(), {}, oracle.position_rows(fixture()), True)
        with self.assertRaisesRegex(oracle.Unsupported, 'conditional branch value'):
            evaluator.position(program)

    def test_reference_only_never_manufactures_native_observation(self):
        inputs = fixture()
        inputs.update(draw=inputs.pop('journal_draw'), vs_hash=inputs.pop('shader'), index_words=inputs.pop('indices'))
        inputs.pop('rows')
        inputs.pop('columns')
        result = oracle.reference_without_native_position(MemoryCapture(), inputs, self.decoder)
        self.assertEqual(result['status'], 'POSITION_REFERENCE_ONLY', result)
        self.assertFalse(result['native_position_compared'])
        self.assertNotIn('observed_bits', result['position_samples'][0])

    def test_zero_indirect_arguments_do_not_require_native_positions(self):
        inputs = {'draw': 10, 'vs_hash': SHADER, 'indexed': True, 'indirect': True}
        events = [dict(kind='draw_prepared', seq=10, command=9, vs=int(SHADER, 16)),
                  dict(kind='indirect_source', seq=11, command=9, bda=0x1000, indexed=True,
                       max_count=1, words=[8907, 0, 0, 0, 0]),
                  dict(kind='indirect_fixed', seq=12, command=9, source_bda=0x1000, count_bda=0,
                       max_count=1, words=[8907, 0, 0, 0, 0]),
                  dict(kind='draw_execute', seq=13, draw=10)]
        result = oracle.zero_argument_reference(inputs, events)
        self.assertEqual(result['status'], 'ZERO_EXECUTED_INVOCATIONS')
        self.assertFalse(result['native_position_compared'])
        self.assertFalse(result['p8_certificate'])
        events[1]['words'][1] = events[2]['words'][1] = 1
        self.assertIsNone(oracle.zero_argument_reference(inputs, events))

    def test_zip_api_retains_good_draw_when_another_draw_is_missing(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'capture.zip'
            record = fixture()
            sidecar = {k: record.pop(k) for k in list(record) if k.startswith('vs_')}
            sidecar.update(draw=1, frame=1, submit=1, vs_hash=SHADER)
            bad = copy.deepcopy(record)
            bad['journal_draw'] = 2
            bad['shader'] = '0000000000000000'
            with zipfile.ZipFile(path, 'w') as archive:
                archive.writestr(f'ds-lighting-capture/guest-shaders/vs_{SHADER}_probe.bin', GUEST)
                archive.writestr('ds-lighting-capture/queue-state/ds_prelight_witness_vertex_1.json', json.dumps(record))
                archive.writestr('ds-lighting-capture/queue-state/ds_prelight_witness_foreground_1.json', json.dumps(sidecar))
                archive.writestr('ds-lighting-capture/queue-state/ds_prelight_witness_vertex_2.json', json.dumps(bad))
            result = oracle.audit_capture(path)
            self.assertEqual(result['counts'], {'POSITION_MATCH': 1, 'POSITION_DIVERGENCE': 0, 'UNSUPPORTED_DRAW': 1}, result)
            self.assertEqual(result['analysis_state'], 'PARTIAL_WITH_GAPS')
            self.assertFalse(result['p8_certificate'])

    def test_new_per_draw_record_deduplicates_legacy_sky_alias(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'queue-state').mkdir()
            (root / 'guest-shaders').mkdir()
            (root / f'guest-shaders/vs_{SHADER}_probe.bin').write_bytes(GUEST)
            record = fixture()
            for name in ('ds_prelight_witness_vertex_1.json', 'ds_prelight_witness_sky_prepass.json'):
                (root / 'queue-state' / name).write_text(json.dumps(record))
            result = oracle.audit_capture(root)
            self.assertEqual(result['counts']['POSITION_MATCH'], 1)
            self.assertEqual(len(result['draws']), 1)
            self.assertEqual(result['draws'][0]['legacy_record_aliases'], ['queue-state/ds_prelight_witness_sky_prepass.json'])

    def test_invalid_json_record_fails_closed_without_traceback(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'queue-state').mkdir()
            (root / 'queue-state/ds_prelight_witness_vertex_1.json').write_text('[]')
            result = oracle.audit_capture(root)
            self.assertEqual(result['counts']['UNSUPPORTED_DRAW'], 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
