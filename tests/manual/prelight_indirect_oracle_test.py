#!/usr/bin/env python3
"""Focused mutation fixtures for the CPU indirect-consumption evidence audit."""
from __future__ import annotations

from copy import deepcopy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

SCRIPT = Path(__file__).resolve().parents[2] / 'docs/android/tools/prelight_indirect_oracle.py'
SPEC = importlib.util.spec_from_file_location('indirect_oracle', SCRIPT)
oracle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(oracle)


def write_json(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value) + '\n')


def write_lines(path: Path, values):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(''.join(json.dumps(value) + '\n' for value in values))


def batch_fixture(parent=100, seq=40, first_count=0, owner='GPU'):
    batch = {'kind': 'cp_batch', 'schema': 1, 'parent_command': parent, 'seq': seq,
             'args_guest': oracle.TARGET_ADDRESS, 'count': 3, 'stride': 20,
             'index_base': 0x2e8100000, 'index_limit': 22020096, 'index_type': 1,
             'index_element_bytes': 4, 'frame': 201, 'submit': 5831, 'tick': 39359,
             'stream_id': 1, 'records': []}
    for child in range(3):
        words = [first_count if child == 0 else 0, 1, 0, 0, 0]
        state = {'gpu_dirty': owner == 'GPU', 'pending_publication': False,
                 'pending_tokens': [], 'cpu_modified': owner == 'CPU', 'gpu_modified': owner == 'GPU',
                 'image_gpu_dirty': False, 'registered': True, 'gpu_generation': 17,
                 'unregister_epoch': 0, 'valid': True,
                 'address': oracle.TARGET_ADDRESS + child * 20, 'bytes': 20,
                 'gpu_dirty_ranges': [{'address': oracle.TARGET_ADDRESS + child * 20, 'bytes': 20}]
                    if owner == 'GPU' else [], 'tracker_granularity': 'page',
                 'buffer_guest': oracle.TARGET_ADDRESS, 'buffer_size': 4096, 'native': 713}
        reference = {'kind': 'gpu_native' if owner == 'GPU' else 'cpu_backing',
                     'words': list(words), 'ok': True, 'native': 713,
                     'buffer_guest': oracle.TARGET_ADDRESS, 'buffer_size': 4096,
                     'offset': child * 20, 'tick_before': 39359, 'tick_after': 39359,
                     'queued_after_consumption': True, 'before_next_child': True,
                     'collection_after_all_original_reads': True, 'observation_seq_collected': seq - 1}
        record = {'child': child, 'address': oracle.TARGET_ADDRESS + child * 20,
                  'consumed_words': list(words),
                  'uses_complete': True,
                  'uses': [{'field': field, 'phase': phase, 'value': word} for field, phase, word in zip(
                      ('index_count', 'instance_count', 'first_index', 'base_vertex', 'first_instance'),
                      ('count', 'instances', 'index_address', 'patch', 'patch'), words)],
                  'emitted': {'index_count': words[0], 'instance_count': 1, 'first_index': 0,
                              'base_vertex': 0, 'first_instance': 0, 'index_addr': 0x2e8100000},
                  'state_before': deepcopy(state), 'state_after': deepcopy(state),
                  'state_reference_after': deepcopy(state), 'mutation_epoch_before': 11,
                  'mutation_epoch_after': 11, 'mutation_epoch_reference_after': 11,
                  'observation_seq_before': seq - 9 + child * 2, 'observation_seq_consumed': seq - 9 + child * 2,
                  'observation_seq_reference': seq - 8 + child * 2,
                  'reference': reference, 'backing_after': {'words': list(words), 'ok': True}}
        batch['records'].append(record)
    return batch


def alter_consumer(record, field, value):
    names = ['index_count', 'instance_count', 'first_index', 'base_vertex', 'first_instance']
    record['consumed_words'][names.index(field)] = value & 0xffffffff
    for use in record['uses']:
        if use['field'] == field:
            use['value'] = value
    record['emitted'][field] = value


def acquisition_fixture(root: Path, batches):
    entries, required = [], []
    for batch in batches:
        name = f'queue-state/ds_prelight_cp_args_batch_{batch["parent_command"]}.json'
        write_json(root / name, batch)
        data = (root / name).read_bytes()
        entries.append({'parent_command': batch['parent_command'], 'file': name})
        required.append({'file': name, 'bytes': len(data), 'fnv1a64': oracle.fnv1a64(data)})
    write_lines(root / oracle.WITNESS_EVENTS, batches)
    data = (root / oracle.WITNESS_EVENTS).read_bytes()
    required.append({'file': oracle.WITNESS_EVENTS, 'bytes': len(data), 'fnv1a64': oracle.fnv1a64(data)})
    positive = next((b['seq'] for b in batches if b['records'][0]['emitted']['index_count']), 0)
    zero = next((b['seq'] for b in batches if b['seq'] > positive and
                 not b['records'][0]['emitted']['index_count']), 0) if positive else 0
    manifest = {'schema': 1, 'batches': entries, 'required_files': required,
                'retention': [{'stream_id': 1, 'last_nonzero': positive, 'first_zero': zero}],
                'drops': {'events': 0, 'blobs': 0, 'signals': 0, 'io_errors': 0}, 'prehistory_evicted': 0}
    write_json(root / oracle.MANIFEST, manifest)
    return manifest


class RecordTest(unittest.TestCase):
    def test_zero_is_a_valid_gpu_match(self):
        report = oracle.audit_batch(batch_fixture())
        self.assertEqual(report['status'], 'MATCH')
        self.assertEqual(report['counts'], {'MATCH': 3})

    def test_altered_consumer_is_independent_divergence(self):
        batch = batch_fixture(first_count=98304)
        alter_consumer(batch['records'][0], 'index_count', 0)
        report = oracle.audit_batch(batch)
        self.assertEqual(report['status'], 'DIVERGENCE')
        self.assertEqual(report['first_divergence']['consumed'], 0)
        self.assertEqual(report['first_divergence']['reference'], 98304)
        self.assertEqual(report['first_divergence']['address'], oracle.TARGET_ADDRESS)
        self.assertEqual(report['counts'], {'DIVERGENCE': 1, 'MATCH': 2})

    def test_old_gpu_mirror_of_cpu_dirty_page_is_not_an_error(self):
        batch = batch_fixture(owner='CPU')
        for record in batch['records']:
            record['reference']['kind'] = 'gpu_native'
            record['reference']['words'][0] = 98304
        report = oracle.audit_batch(batch)
        self.assertEqual(report['status'], 'MATCH')
        self.assertTrue(all(record['ignored_non_authoritative_gpu_reference'] for record in report['records']))
        self.assertTrue(all(record['authority'] == 'CPU' for record in report['records']))

    def test_coarse_cpu_modified_flag_does_not_discard_exact_gpu_ledger(self):
        batch = batch_fixture()
        for record in batch['records']:
            for key in ('state_before', 'state_after', 'state_reference_after'):
                record[key]['cpu_modified'] = True
        self.assertEqual(oracle.audit_batch(batch)['status'], 'MATCH')

    def test_pending_publication_prevents_selecting_stale_backing(self):
        batch = batch_fixture(first_count=98304)
        record = batch['records'][0]
        alter_consumer(record, 'index_count', 0)
        record['backing_after']['words'][0] = 0
        for key in ('state_before', 'state_after', 'state_reference_after'):
            record[key].update(gpu_dirty=False, gpu_dirty_ranges=[], pending_publication=True,
                pending_tokens=[{'token': 22, 'ranges': [{'address': record['address'], 'bytes': 20}]}])
        report = oracle.audit_batch(batch)
        self.assertEqual(report['records'][0]['status'], 'DIVERGENCE')
        self.assertEqual(report['records'][0]['authority'], 'GPU')
        self.assertEqual(report['records'][0]['pending_tokens'], [22])

    def test_natural_fault_publication_before_original_reads_can_match(self):
        batch = batch_fixture(first_count=98304, owner='CPU')
        record = batch['records'][0]
        record['state_before'].update(gpu_dirty=True, cpu_modified=False, gpu_modified=True)
        record['state_before']['gpu_dirty_ranges'] = [{'address': record['address'], 'bytes': 20}]
        record['mutation_epoch_before'] = 10
        report = oracle.audit_batch(batch)
        self.assertEqual(report['records'][0]['status'], 'MATCH')
        self.assertTrue(report['records'][0]['read_window_mutation_observed'])

    def test_later_write_is_unstable_not_retroactive_consumer_error(self):
        batch = batch_fixture(first_count=98304)
        alter_consumer(batch['records'][0], 'index_count', 0)
        batch['records'][0]['mutation_epoch_reference_after'] = 12
        report = oracle.audit_batch(batch)
        self.assertEqual(report['records'][0]['status'], 'UNSTABLE')
        self.assertNotIn('first_divergence', report)
        self.assertEqual(report['records'][1]['status'], 'MATCH')

    def test_mutation_during_reads_does_not_assign_a_wrong_version(self):
        batch = batch_fixture(first_count=98304)
        alter_consumer(batch['records'][0], 'index_count', 0)
        batch['records'][0]['mutation_epoch_before'] = 10
        self.assertEqual(oracle.audit_batch(batch)['records'][0]['status'], 'UNSTABLE')

    def test_missing_reference_preserves_other_records(self):
        batch = batch_fixture()
        del batch['records'][1]['reference']['words']
        report = oracle.audit_batch(batch)
        self.assertEqual(report['counts'], {'MATCH': 2, 'INCOMPLETE': 1})

    def test_wrong_parent_and_range_cannot_match(self):
        batch = batch_fixture()
        batch['records'][0]['parent_command'] = 999
        batch['records'][1]['address'] += 4
        report = oracle.audit_batch(batch)
        self.assertEqual(report['counts'], {'INCOMPLETE': 2, 'MATCH': 1})
        self.assertEqual(oracle.audit_batch(batch_fixture(), expected_parent=999)['status'], 'INCOMPLETE')

    def test_native_offset_and_epoch_are_checked(self):
        batch = batch_fixture()
        batch['records'][0]['reference']['offset'] += 4
        batch['records'][1]['state_reference_after']['gpu_generation'] += 1
        report = oracle.audit_batch(batch)
        self.assertEqual([r['status'] for r in report['records']], ['INCOMPLETE', 'UNSTABLE', 'MATCH'])

    def test_reference_before_preserving_values_is_not_a_certificate(self):
        batch = batch_fixture()
        batch['records'][0]['reference']['queued_after_consumption'] = False
        batch['records'][1]['reference']['before_next_child'] = False
        self.assertEqual(oracle.audit_batch(batch)['counts'], {'INCOMPLETE': 2, 'MATCH': 1})

    def test_repeated_original_reads_that_change_are_unstable(self):
        batch = batch_fixture()
        batch['records'][0]['uses'].append({'field': 'instance_count', 'phase': 'draw', 'value': 2})
        self.assertEqual(oracle.audit_batch(batch)['records'][0]['status'], 'UNSTABLE')

    def test_clamped_count_compares_raw_argument_not_clamped_output(self):
        batch = batch_fixture(first_count=98304)
        batch['index_limit'] = 65536
        batch['records'][0]['emitted']['index_count'] = 65536
        self.assertEqual(oracle.audit_batch(batch)['status'], 'MATCH')

    def test_signed_base_vertex_bits_and_index_address(self):
        batch = batch_fixture()
        record = batch['records'][0]
        alter_consumer(record, 'base_vertex', -7)
        record['reference']['words'][3] = 0xfffffff9
        alter_consumer(record, 'first_index', 5)
        record['reference']['words'][2] = 5
        record['emitted']['index_addr'] += 20
        self.assertEqual(oracle.audit_batch(batch)['status'], 'MATCH')

    def test_image_authority_and_missing_mixed_reference_are_explicit(self):
        batch = batch_fixture()
        record = batch['records'][0]
        for key in ('state_before', 'state_after', 'state_reference_after'):
            record[key]['image_gpu_dirty'] = True
        record = batch['records'][1]
        for key in ('state_before', 'state_after', 'state_reference_after'):
            record[key].update(gpu_dirty=False, gpu_dirty_ranges=[], pending_publication=True,
                pending_tokens=[{'token': 1, 'ranges': [{'address': record['address'], 'bytes': 4}]}])
        del record['backing_after']
        self.assertEqual(oracle.audit_batch(batch)['counts'], {'INCOMPLETE': 2, 'MATCH': 1})

    def test_byte_ledger_chooses_backing_for_cpu_bytes_with_old_native_mirror(self):
        batch = batch_fixture()
        record = batch['records'][0]
        for key in ('state_before', 'state_after', 'state_reference_after'):
            record[key]['gpu_dirty_ranges'] = [{'address': record['address'], 'bytes': 4}]
            record[key]['cpu_modified'] = True
        record['reference']['words'][1] = 99
        report = oracle.audit_batch(batch)
        self.assertEqual(report['status'], 'MATCH')
        self.assertEqual(report['records'][0]['authority'], 'MIXED')
        self.assertEqual(report['records'][0]['reference_words'][1], 1)

    def test_partial_pending_can_be_decided_from_both_independent_sources(self):
        batch = batch_fixture()
        record = batch['records'][0]
        for key in ('state_before', 'state_after', 'state_reference_after'):
            record[key].update(gpu_dirty=False, gpu_dirty_ranges=[], pending_publication=True,
                pending_tokens=[{'token': 22, 'ranges': [{'address': record['address'], 'bytes': 4}]}])
        record['reference']['words'][1] = 999
        report = oracle.audit_batch(batch)
        self.assertEqual(report['status'], 'MATCH')
        self.assertEqual(report['records'][0]['authority_by_byte'], ['GPU'] * 4 + ['CPU'] * 16)

    def test_real_emitted_count_disagrees_with_independent_decode_contract(self):
        batch = batch_fixture(first_count=98304)
        batch['records'][0]['emitted']['index_count'] = 0
        report = oracle.audit_batch(batch)
        self.assertEqual(report['status'], 'DIVERGENCE')
        difference = report['first_divergence']
        self.assertEqual(difference['stage'], 'argument_decode_to_emitted_draw')
        self.assertEqual(difference['expected'], 98304)
        self.assertEqual(difference['observed'], 0)

    def test_emitted_index_address_is_checked_without_rejecting_input_evidence(self):
        batch = batch_fixture()
        batch['records'][1]['emitted']['index_addr'] += 4
        report = oracle.audit_batch(batch)
        self.assertEqual(report['records'][1]['status'], 'DIVERGENCE')
        self.assertEqual(report['records'][1]['first_divergence']['field'], 'index_addr')

    def test_recorder_bookkeeping_contradiction_is_not_renderer_divergence(self):
        batch = batch_fixture()
        batch['records'][0]['consumed_words'][0] = 5
        self.assertEqual(oracle.audit_batch(batch)['records'][0]['status'], 'INCOMPLETE')

    def test_cpu_dirty_difference_can_be_a_concurrent_store(self):
        batch = batch_fixture(first_count=98304, owner='CPU')
        alter_consumer(batch['records'][0], 'index_count', 0)
        report = oracle.audit_batch(batch)
        self.assertEqual(report['records'][0]['status'], 'UNSTABLE')
        self.assertEqual(report['records'][0]['observed_difference']['reference'], 98304)
        self.assertNotIn('first_divergence', report)

    def test_native_collection_before_other_children_invalidates_local_capture(self):
        batch = batch_fixture()
        batch['records'][0]['reference']['observation_seq_collected'] = 32
        batch['records'][1]['reference']['collection_after_all_original_reads'] = False
        report = oracle.audit_batch(batch)
        self.assertEqual(report['counts'], {'INCOMPLETE': 2, 'MATCH': 1})

    def test_later_publication_after_frozen_copy_does_not_invalidate_gpu_match(self):
        batch = batch_fixture()
        record = batch['records'][0]
        record['reference']['state_after_collection'] = {'gpu_dirty': False, 'cpu_modified': True}
        self.assertEqual(oracle.audit_batch(batch)['status'], 'MATCH')

    def test_missing_and_duplicate_child_do_not_erase_good_child(self):
        batch = batch_fixture()
        batch['records'] = [batch['records'][0], batch['records'][1], deepcopy(batch['records'][1])]
        report = oracle.audit_batch(batch)
        self.assertEqual(report['records'][0]['status'], 'MATCH')
        self.assertEqual(report['counts'], {'MATCH': 1, 'INCOMPLETE': 3})


class AcquisitionTest(unittest.TestCase):
    def test_same_retained_transition_in_directory_and_zip(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'capture'
            acquisition_fixture(root, [batch_fixture(54823, 40, 98304), batch_fixture(55986, 80, 0)])
            report = oracle.audit_capture(root)
            self.assertEqual(report['analysis_state'], 'MATCH')
            self.assertEqual(report['record_counts'], {'MATCH': 6})
            self.assertEqual(report['retention']['streams'][0]['last_nonzero_parent'], 54823)
            self.assertEqual(report['retention']['streams'][0]['first_zero_parent'], 55986)
            archive = Path(temp) / 'capture.zip'
            with zipfile.ZipFile(archive, 'w') as dest:
                for source in root.rglob('*'):
                    if source.is_file():
                        dest.write(source, 'ds-lighting-capture/' + source.relative_to(root).as_posix())
            self.assertEqual(oracle.audit_capture(archive)['record_counts'], {'MATCH': 6})

    def test_dropped_data_is_reported_without_erasing_valid_records(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = acquisition_fixture(root, [batch_fixture()])
            manifest['drops']['signals'] = 1
            write_json(root / oracle.MANIFEST, manifest)
            report = oracle.audit_capture(root)
            self.assertEqual(report['analysis_state'], 'INCOMPLETE')
            self.assertEqual(report['record_counts'], {'MATCH': 3})

    def test_evicted_prehistory_does_not_invalidate_retained_operands(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = acquisition_fixture(root, [batch_fixture(100, 40, 98304), batch_fixture(200, 80, 0)])
            manifest['prehistory_evicted'] = 10000
            write_json(root / oracle.MANIFEST, manifest)
            self.assertEqual(oracle.audit_capture(root)['analysis_state'], 'MATCH')

    def test_missing_batch_keeps_other_batch(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = acquisition_fixture(root, [batch_fixture(100, 40, 98304), batch_fixture(200, 80, 0)])
            (root / manifest['batches'][0]['file']).unlink()
            report = oracle.audit_capture(root)
            self.assertEqual(report['analysis_state'], 'INCOMPLETE')
            self.assertEqual(report['record_counts'], {'MATCH': 3})

    def test_interleaved_signal_uses_observed_order_not_drain_order(self):
        batch = batch_fixture(first_count=98304)
        alter_consumer(batch['records'][0], 'index_count', 0)
        report = oracle.audit_batch(batch)
        oracle.check_interleaving(batch, report, [{'kind': 'write_backing', 'seq': 90,
            'observed_seq': 32, 'address': oracle.TARGET_ADDRESS, 'bytes': 20}])
        self.assertEqual(report['records'][0]['status'], 'UNSTABLE')
        self.assertNotIn('first_divergence', report)
        self.assertEqual(report['records'][1]['status'], 'MATCH')

    def test_loss_binding_allows_inflight_batch_and_is_not_a_global_oracle(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = acquisition_fixture(root, [batch_fixture(100, 40, 98304), batch_fixture(200, 80, 0)])
            manifest['observed_loss'] = {'frame': 201, 'queue': 0, 'seq': 78}
            manifest['retention'][0].update(sealed=True,
                last_seen={'frame': 201, 'submit': 5831, 'parent_command': 200, 'seq': 80})
            write_json(root / oracle.MANIFEST, manifest)
            report = oracle.audit_capture(root)
            self.assertEqual(report['analysis_state'], 'MATCH')
            self.assertEqual(report['retention']['loss_binding'], 'OBSERVED_WINDOW')
            self.assertTrue(report['retention']['streams'][0]['batch_finished_after_loss_observation'])

    def test_external_dependency_without_size_uses_actual_export_inventory(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = acquisition_fixture(root, [batch_fixture()])
            name = 'spv/cs_module.spv'
            (root / 'spv').mkdir()
            (root / name).write_bytes(b'captured module')
            manifest['required_files'].append({'file': name})
            write_json(root / oracle.MANIFEST, manifest)
            write_json(root / 'EXPORT-CLOSURE.json', {'required_files': [name], 'included_required': [name]})
            report = oracle.audit_capture(root)
            self.assertEqual(report['analysis_state'], 'MATCH')
            self.assertFalse(report['file_integrity'][name]['declared_bytes_checked'])

    def test_real_child_journal_rejects_conflicting_emission_bookkeeping(self):
        batch = batch_fixture()
        batch['records'][0]['emitted']['index_count'] = 7
        report = oracle.audit_batch(batch)
        self.assertEqual(report['records'][0]['status'], 'DIVERGENCE')
        journal = [{'seq': 100, 'kind': 'guest_command', 'op': 'indirect_indexed',
                    'indirect_guest': oracle.TARGET_ADDRESS, 'count': 3, 'stride': 20,
                    'index_guest': batch['index_base'], 'index_limit': batch['index_limit'],
                    'submit': batch['submit']}]
        for child in range(3):
            journal.append({'seq': 102 + child, 'kind': 'guest_command', 'op': 'indexed',
                            'count': 0, 'instances': 1, 'index_guest': batch['index_base']})
        oracle.bind_guest_journal(batch, report, journal)
        self.assertEqual(report['records'][0]['status'], 'INCOMPLETE')
        self.assertEqual(report['records'][1]['status'], 'MATCH')


class ReaderTest(unittest.TestCase):
    def test_reference_binary_range_and_hash(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'queue-state').mkdir()
            blob = struct.pack('<5I', 0, 1, 0, 0, 0)
            (root / 'queue-state/source.bin').write_bytes(b'PREFIX00' + blob + b'SUFFIX00')
            capture = oracle.Capture(root)
            payload = {'file': 'queue-state/source.bin', 'offset_bytes': 8, 'bytes': 20,
                       'sha256': hashlib.sha256(blob).hexdigest()}
            self.assertEqual(oracle.read_payload(capture, payload, 'reference'), [0, 1, 0, 0, 0])
            payload['offset_bytes'] = 20
            with self.assertRaisesRegex(oracle.EvidenceError, 'truncated'):
                oracle.read_payload(capture, payload, 'reference')
            capture.close()

    def test_export_truncated_file_does_not_erase_valid_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'queue-state').mkdir()
            (root / 'queue-state/good.bin').write_bytes(b'a' * 20)
            (root / 'queue-state/bad.bin').write_bytes(b'a' * 16)
            capture = oracle.Capture(root)
            files, errors = oracle.verify_required_files(capture, {'required_complete': True,
                'required_files': [{'file': 'queue-state/good.bin', 'bytes': 20,
                                    'sha256': hashlib.sha256(b'a' * 20).hexdigest()},
                                   {'file': 'queue-state/bad.bin', 'bytes': 20}]})
            self.assertEqual(files['queue-state/good.bin']['status'], 'VERIFIED')
            self.assertEqual(files['queue-state/bad.bin']['status'], 'INCOMPLETE')
            self.assertEqual(len(errors), 1)
            capture.close()

    def test_export_same_size_corruption_is_detected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'queue-state').mkdir()
            (root / 'queue-state/source.bin').write_bytes(b'b' * 20)
            capture = oracle.Capture(root)
            _, errors = oracle.verify_required_files(capture, {'required_files': [
                {'file': 'queue-state/source.bin', 'bytes': 20,
                 'sha256': hashlib.sha256(b'a' * 20).hexdigest()}]})
            self.assertIn('SHA-256 mismatch', errors[0]['reason'])
            capture.close()

    def test_app_verdict_cannot_supply_reference_words(self):
        with self.assertRaises(oracle.EvidenceError):
            oracle.read_payload(None, {'verdict': 'MATCH', 'expected_from_consumed': True}, 'reference')

    def test_json_array_journal_line_does_not_crash_or_hide_valid_event(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            write_lines(root / 'queue-state/ds_prelight_events_0.jsonl',
                        [{'kind': 'guest_command', 'seq': 7}, []])
            capture = oracle.Capture(root)
            events, errors = oracle.journal_events(capture)
            self.assertEqual([e['seq'] for e in events], [7])
            self.assertEqual(len(errors), 1)
            capture.close()

    def test_zip_reader_uses_same_bytes_and_rejects_traversal(self):
        with tempfile.TemporaryDirectory() as temp:
            archive = Path(temp) / 'capture.zip'
            with zipfile.ZipFile(archive, 'w') as dest:
                dest.writestr('diagnostics/ds-lighting-capture/queue-state/source.bin', b'1234')
            capture = oracle.Capture(archive)
            self.assertEqual(capture.read('queue-state/source.bin'), b'1234')
            capture.close()
            with zipfile.ZipFile(archive, 'w') as dest:
                dest.writestr('../queue-state/source.bin', b'1234')
            with self.assertRaisesRegex(oracle.EvidenceError, 'unsafe'):
                oracle.Capture(archive)

    def test_transition_is_observed_not_an_expected_positive_count(self):
        def batch(seq, count):
            result = [{'kind': 'guest_command', 'op': 'indirect_indexed', 'seq': seq,
                       'indirect_guest': oracle.TARGET_ADDRESS, 'count': 3, 'stride': 20,
                       'index_limit': 22020096, '_file': 'journal', '_line': seq},
                      {'kind': 'command_return', 'seq': seq + 1, 'command': seq,
                       'outcome': 'cpu_fallback'}]
            for child in range(3):
                result.append({'kind': 'guest_command', 'op': 'indexed',
                               'seq': seq + 2 + child, 'count': count if child == 0 else 0,
                               'instances': 1, '_file': 'journal', '_line': seq + 2 + child})
            return result
        report = oracle.legacy_history(batch(54823, 98304) + batch(55986, 0))
        self.assertEqual(report['last_nonzero']['parent_command'], 54823)
        self.assertEqual(report['first_zero']['parent_command'], 55986)
        self.assertEqual(report['classification'], 'INCOMPLETE')
        self.assertNotIn('expected_count', report)


if __name__ == '__main__':
    unittest.main(verbosity=2)
