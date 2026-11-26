#!/usr/bin/env python3
"""Audit one captured indirect-argument consumption edge, independently.

The operands are the values preserved at the CP's original point of use and a
separately obtained memory reference. An app verdict or a copy of consumed_words
is never a reference. Zero is an ordinary value. A result is local to one record;
missing records do not erase valid results from another record or batch.

Usage: python3 prelight_indirect_oracle.py CAPTURE.zip --output report.json
Only the Python standard library is required. Archives are read, not extracted.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import sys
from typing import Any
import zipfile

TARGET_ADDRESS = 0x21DE00078
RECORD_BYTES = 20
TARGET_RECORDS = 3
MANIFEST = 'queue-state/ds_prelight_cp_args_manifest.json'
WITNESS_EVENTS = 'queue-state/ds_prelight_cp_args_events.jsonl'
MAX_MEMBER_BYTES = 128 * 1024 * 1024
MAX_TOTAL_BYTES = 2 * 1024 * 1024 * 1024
EVENT_NAME = re.compile(r'queue-state/ds_prelight_events_(\d+)\.jsonl$')
FIELDS = ('index_count', 'instance_count', 'first_index', 'base_vertex_bits', 'first_instance')


class EvidenceError(ValueError):
    """A specific malformed/missing acquisition, never a renderer mismatch."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def uint(value: Any, label: str, bits: int = 64) -> int:
    require(type(value) is int and 0 <= value < (1 << bits), f'{label}: expected uint{bits}')
    return value


def object_value(value: Any, label: str) -> dict:
    require(isinstance(value, dict), f'{label}: expected object')
    return value


def word_values(value: Any, label: str, count: int = 5) -> list[int]:
    require(isinstance(value, list) and len(value) == count, f'{label}: expected {count} DWORDs')
    return [uint(word, f'{label}[{i}]', 32) for i, word in enumerate(value)]


def safe_name(name: Any) -> str:
    require(isinstance(name, str) and name and '\\' not in name and '\0' not in name,
            f'invalid artifact name: {name!r}')
    path = PurePosixPath(name)
    require(not path.is_absolute() and '..' not in name.split('/') and ':' not in path.parts[0],
            f'unsafe artifact name: {name!r}')
    require(path.as_posix() not in ('', '.'), 'empty artifact name')
    return path.as_posix()


def json_object_pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f'duplicate JSON key: {key!r}')
        result[key] = value
    return result


class Capture:
    """Small independent stdlib reader; no renderer/oracle implementation used."""
    def __init__(self, path: str | Path):
        self.path = Path(path)
        self.archive = None
        self.files = {}
        self.inspected = {}
        total = 0
        if self.path.is_dir():
            self.root = self.path.resolve()
            for source in sorted(self.path.rglob('*')):
                require(not source.is_symlink(), f'symlink artifact: {source.name}')
                if not source.is_file():
                    continue
                name = safe_name(source.relative_to(self.path).as_posix())
                size = source.stat().st_size
                self.files[name] = source
                total += size
                require(size <= MAX_MEMBER_BYTES and total <= MAX_TOTAL_BYTES, 'capture size bound exceeded')
        else:
            require(self.path.is_file() and zipfile.is_zipfile(self.path), 'expected diagnostic ZIP or directory')
            self.archive = zipfile.ZipFile(self.path)
            seen = set()
            for entry in self.archive.infolist():
                name = safe_name(entry.filename)
                require(name not in seen, f'duplicate ZIP member: {name}')
                seen.add(name)
                require(not stat.S_ISLNK(entry.external_attr >> 16), f'ZIP symlink: {name}')
                require(not entry.flag_bits & 1, f'encrypted ZIP member: {name}')
                if entry.is_dir():
                    continue
                total += entry.file_size
                require(entry.file_size <= MAX_MEMBER_BYTES and total <= MAX_TOTAL_BYTES,
                        'capture size bound exceeded')
                self.files[name] = entry
        roots = {name[:name.index('queue-state/')] for name in self.files
                 if re.search(r'(^|/)queue-state/', name)}
        require(len(roots) == 1, f'expected one capture root, found {len(roots)}')
        self.prefix = roots.pop()
        self.names = {name[len(self.prefix):] for name in self.files if name.startswith(self.prefix)}

    def read(self, name: str) -> bytes:
        name = safe_name(name)
        require(name in self.names, f'missing artifact: {name}')
        source = self.files[self.prefix + name]
        try:
            if self.archive:
                data = self.archive.read(source)
            else:
                require(not source.is_symlink() and source.resolve().is_relative_to(self.root),
                        f'artifact escaped capture root: {name}')
                data = source.read_bytes()
        except (OSError, zipfile.BadZipFile, RuntimeError) as error:
            raise EvidenceError(f'unreadable artifact {name}: {error}') from error
        require(len(data) <= MAX_MEMBER_BYTES, f'artifact too large: {name}')
        self.inspected[name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        return data

    def json(self, name: str):
        try:
            return json.loads(self.read(name), object_pairs_hook=json_object_pairs)
        except (ValueError, UnicodeError) as error:
            raise EvidenceError(f'invalid JSON in {name}: {error}') from error

    def close(self):
        if self.archive:
            self.archive.close()


def journal_events(capture: Capture) -> tuple[list[dict], list[dict]]:
    """Keep usable chunks when another chunk is corrupt; expose exact gaps."""
    events, errors = [], []
    names = sorted((name for name in capture.names if EVENT_NAME.fullmatch(name)),
                   key=lambda name: int(EVENT_NAME.fullmatch(name).group(1)))
    seen = set()
    for name in names:
        try:
            lines = capture.read(name).splitlines()
        except EvidenceError as error:
            errors.append({'file': name, 'reason': str(error)})
            continue
        for line_number, line in enumerate(lines, 1):
            if not line.strip():
                continue
            try:
                event = object_value(json.loads(line, object_pairs_hook=json_object_pairs), 'journal event')
                seq = uint(event.get('seq'), 'journal seq')
                require(seq not in seen, f'duplicate journal seq: {seq}')
                seen.add(seq)
                event = dict(event, _file=name, _line=line_number)
                events.append(event)
            except (EvidenceError, ValueError, UnicodeError) as error:
                errors.append({'file': name, 'line': line_number, 'reason': str(error)})
    return sorted(events, key=lambda e: e['seq']), errors


def legacy_history(events: list[dict], address: int = TARGET_ADDRESS) -> dict:
    """Derive the old journal's partial consumption, not missing source bytes."""
    returns = {e.get('command'): e for e in events if e.get('kind') == 'command_return'}
    cases = []
    for index, event in enumerate(events):
        if (event.get('kind') != 'guest_command' or event.get('op') != 'indirect_indexed'
                or event.get('indirect_guest') != address):
            continue
        returned = returns.get(event['seq'], {})
        if returned.get('outcome') != 'cpu_fallback':
            continue
        count = event.get('count')
        if type(count) is not int or not 0 < count <= 64:
            continue
        children = []
        for child in events[index + 1:]:
            if child.get('kind') != 'guest_command':
                continue
            if child.get('op') != 'indexed':
                break
            children.append({'command': child['seq'], 'count': child.get('count'),
                             'instances': child.get('instances'), 'index_guest': child.get('index_guest'),
                             'file': child['_file'], 'line': child['_line']})
            if len(children) == count:
                break
        cases.append({'parent_command': event['seq'], 'frame': event.get('frame'),
                      'submit': event.get('submit'), 'tick': event.get('tick'),
                      'args_guest': address, 'count_guest': event.get('count_guest'),
                      'count': count, 'stride': event.get('stride'),
                      'index_base': event.get('index_guest'), 'index_limit': event.get('index_limit'),
                      'children': children, 'complete_child_inventory': len(children) == count,
                      'file': event['_file'], 'line': event['_line']})
    last_nonzero = None
    first_zero = None
    for case in cases:
        if not case['complete_child_inventory']:
            continue
        values = [child.get('count') for child in case['children']]
        if not all(type(value) is int and value >= 0 for value in values):
            continue
        if any(values):
            if first_zero is None:
                last_nonzero = case
        elif last_nonzero is not None and first_zero is None:
            first_zero = case
    return {'scope': 'observed child counts; no independent source/reference bytes',
            'args_guest': address, 'cases': cases, 'last_nonzero': last_nonzero,
            'first_zero': first_zero,
            'classification': 'INCOMPLETE',
            'missing': ['raw five-DWORD values preserved at each original read',
                        'same-version independent native GPU or separate CPU backing bytes',
                        'authority, pending-publication, version and observation order']}


def verify_required_files(capture: Capture, manifest: dict) -> tuple[dict, list[dict]]:
    """Recompute file integrity; a producer's required_complete flag is ignored."""
    results, errors = {}, []
    entries = manifest.get('required_files')
    if not isinstance(entries, list):
        return {}, [{'reason': 'manifest.required_files: expected list'}]
    for index, item in enumerate(entries):
        name = None
        try:
            item = object_value(item, f'required_files[{index}]')
            name = safe_name(item.get('file'))
            require(name not in results, f'duplicate required artifact: {name}')
            expected_bytes = uint(item['bytes'], f'{name}.bytes') if 'bytes' in item else None
            data = capture.read(name)
            if expected_bytes is not None:
                require(len(data) == expected_bytes,
                        f'{name}: expected {expected_bytes} bytes, found {len(data)}')
            else:
                # RegisterRequired() names external modules without a size.
                # Their presence must also be declared by the actual exporter;
                # do not manufacture a producer-supplied byte-size certificate.
                require('EXPORT-CLOSURE.json' in capture.names,
                        f'{name}: no declared size or exporter closure inventory')
                closure = object_value(capture.json('EXPORT-CLOSURE.json'), 'EXPORT-CLOSURE')
                require(name in closure.get('required_files', [])
                        and name in closure.get('included_required', []),
                        f'{name}: external dependency absent from exporter closure inventory')
            digest = hashlib.sha256(data).hexdigest()
            expected_hash = item.get('sha256')
            if expected_hash is not None:
                require(isinstance(expected_hash, str) and re.fullmatch(r'[0-9a-fA-F]{64}', expected_hash),
                        f'{name}: invalid SHA-256')
                require(digest == expected_hash.lower(), f'{name}: SHA-256 mismatch')
            if 'fnv1a64' in item:
                expected_fnv = uint(item['fnv1a64'], f'{name}.fnv1a64')
                require(fnv1a64(data) == expected_fnv, f'{name}: FNV1a64 mismatch')
            if 'present' in item:
                require(item['present'] is True, f'{name}: manifest marked absent')
            results[name] = {'status': 'VERIFIED', 'bytes': len(data), 'sha256': digest,
                             'declared_sha256_checked': expected_hash is not None,
                             'declared_fnv1a64_checked': 'fnv1a64' in item,
                             'declared_bytes_checked': expected_bytes is not None}
        except (EvidenceError, TypeError) as error:
            issue = {'index': index, 'file': name, 'reason': str(error)}
            errors.append(issue)
            if name:
                results[name] = dict(issue, status='INCOMPLETE')
    return results, errors


def fnv1a64(data: bytes) -> int:
    result = 14695981039346656037
    for value in data:
        result = ((result ^ value) * 1099511628211) & ((1 << 64) - 1)
    return result


def read_payload(capture: Capture, item: dict, label: str, size: int = RECORD_BYTES) -> list[int]:
    """Read independent inline DWORDs or an exact, integrity-checked binary slice."""
    require(size > 0 and size % 4 == 0, label + ': invalid DWORD byte range')
    if 'words' in item:
        return word_values(item['words'], label + '.words', size // 4)
    name = safe_name(item.get('file'))
    offset = uint(item.get('offset_bytes', 0), label + '.offset_bytes')
    require(uint(item.get('bytes'), label + '.bytes') == size, label + ': wrong payload byte range')
    data = capture.read(name)
    require(offset + size <= len(data), label + ': truncated binary payload')
    data = data[offset:offset + size]
    if 'sha256' in item:
        digest = item['sha256']
        require(isinstance(digest, str) and hashlib.sha256(data).hexdigest() == digest.lower(),
                label + ': payload SHA-256 mismatch')
    return list(struct.unpack('<' + 'I' * (size // 4), data))


def first_difference(consumed: list[int], reference: list[int], address: int) -> dict | None:
    for index, (actual, expected) in enumerate(zip(consumed, reference)):
        if actual != expected:
            return {'dword': index, 'field': FIELDS[index], 'address': address + 4 * index,
                    'consumed': actual, 'reference': expected,
                    'consumed_hex': f'0x{actual:08x}', 'reference_hex': f'0x{expected:08x}'}
    return None


def state_value(value: Any, label: str) -> dict:
    value = object_value(value, label)
    for key in ('gpu_dirty', 'pending_publication', 'cpu_modified', 'gpu_modified',
                'image_gpu_dirty', 'registered'):
        require(type(value.get(key)) is bool, f'{label}.{key}: expected bool')
    for key in ('gpu_generation', 'unregister_epoch'):
        uint(value.get(key), f'{label}.{key}')
    ranges = value.get('gpu_dirty_ranges')
    require(isinstance(ranges, list), label + '.gpu_dirty_ranges: exact byte ledger missing')
    for part in ranges:
        part = object_value(part, label + '.gpu_dirty_ranges entry')
        start = uint(part.get('address'), label + '.GPU range address')
        size = uint(part.get('bytes'), label + '.GPU range bytes')
        require(size > 0 and start + size < 1 << 64, label + ': invalid GPU dirty range')
    require(isinstance(value.get('pending_tokens'), list), f'{label}.pending_tokens: expected list')
    for item in value['pending_tokens']:
        item = object_value(item, label + '.pending_tokens entry')
        uint(item.get('token'), label + '.pending token')
        ranges = item.get('ranges')
        require(isinstance(ranges, list), label + '.pending ranges: expected list')
        for part in ranges:
            part = object_value(part, label + '.pending range')
            start = uint(part.get('address'), label + '.pending range address')
            size = uint(part.get('bytes'), label + '.pending range bytes')
            require(size > 0 and start + size < 1 << 64, label + ': invalid pending range')
    return value


def pending_overlap(state: dict, address: int, size: int) -> tuple[list[int], bool]:
    tokens, intervals = [], []
    for item in state['pending_tokens']:
        overlaps = False
        for part in item['ranges']:
            low = max(address, part['address'])
            high = min(address + size, part['address'] + part['bytes'])
            if low < high:
                intervals.append((low, high))
                overlaps = True
        if overlaps:
            tokens.append(item['token'])
    end = address
    for low, high in sorted(intervals):
        if low > end:
            break
        end = max(end, high)
    return sorted(tokens), end >= address + size


def states_different(left: dict, right: dict) -> list[str]:
    keys = ('gpu_dirty', 'pending_publication', 'cpu_modified', 'gpu_modified',
            'image_gpu_dirty', 'registered', 'gpu_generation', 'unregister_epoch', 'pending_tokens',
            'gpu_dirty_ranges')
    keys += tuple(key for key in ('buffer_guest', 'buffer_size', 'native') if key in left or key in right)
    return [key for key in keys if left.get(key) != right.get(key)]


def signed32(value: int) -> int:
    return value - (1 << 32) if value & (1 << 31) else value


def check_consumption(batch: dict, record: dict, consumed: list[int]) -> dict:
    """Validate identity of every preserved read and the actual draw operands."""
    uses = record.get('uses')
    require(isinstance(uses, list) and uses, 'record.uses: original point-of-use values missing')
    if 'uses_complete' in record:
        require(record['uses_complete'] is True, 'original point-of-use recorder overflow/incomplete')
    field_names = ('index_count', 'instance_count', 'first_index', 'base_vertex', 'first_instance')
    values_by_field = {field: [] for field in field_names}
    validated = []
    for index, use in enumerate(uses):
        use = object_value(use, f'uses[{index}]')
        field = use.get('field')
        require(field in values_by_field, f'uses[{index}]: unsupported field {field!r}')
        require(use.get('phase') in ('patch', 'index_address', 'count', 'instances', 'empty', 'draw', 'log'),
                f'uses[{index}]: unsupported phase {use.get("phase")!r}')
        value = use.get('value')
        if field == 'base_vertex' and type(value) is int and -(1 << 31) <= value < 0:
            value &= 0xffffffff
        value = uint(value, f'uses[{index}].value', 32)
        values_by_field[field].append(value)
        validated.append({'field': field, 'phase': use['phase'], 'value': value})
    absent = [field for field, values in values_by_field.items() if not values]
    require(not absent, 'original point-of-use fields missing: ' + ', '.join(absent))
    changed = [field for field, values in values_by_field.items() if len(set(values)) != 1]
    if changed:
        return {'status': 'UNSTABLE', 'reason': 'repeated original reads differ', 'fields': changed,
                'uses': validated}
    for index, field in enumerate(field_names):
        require(values_by_field[field][0] == consumed[index],
                f'consumed_words and original use disagree for {field}')
    emitted = object_value(record.get('emitted'), 'record.emitted')
    limit = uint(batch.get('index_limit'), 'batch.index_limit', 32)
    expected = {'index_count': min(consumed[0], limit) if limit else consumed[0],
                'instance_count': consumed[1], 'first_index': consumed[2],
                'base_vertex': signed32(consumed[3]), 'first_instance': consumed[4],
                'index_addr': uint(batch.get('index_base'), 'batch.index_base')
                              + consumed[2] * batch['index_element_bytes']}
    mismatches = []
    for field, value in expected.items():
        actual = emitted.get(field)
        if field == 'base_vertex' and type(actual) is int and 0 <= actual <= 0xffffffff:
            actual = signed32(actual)
        require(type(actual) is int, f'emitted {field}: expected captured integer')
        if field == 'base_vertex':
            require(-(1 << 31) <= actual < 1 << 31, 'emitted base_vertex outside int32')
        else:
            uint(actual, 'emitted.' + field, 64 if field == 'index_addr' else 32)
        if field == 'first_index':
            # first_index is duplicate bookkeeping in the recorder; the actual
            # DrawIndexArgs output carries index_addr, not first_index.
            require(actual == value, 'emitted first_index bookkeeping differs from original uses')
        elif actual != value:
            dword = {'index_count': 0, 'instance_count': 1, 'index_addr': 2,
                     'base_vertex': 3, 'first_instance': 4}[field]
            mismatches.append({'stage': 'argument_decode_to_emitted_draw', 'field': field,
                               'dword': dword, 'observed': actual, 'expected': value,
                               'operation': 'min(raw_count,index_limit) when limit!=0' if field == 'index_count'
                               else 'index_base+first_index*index_element_bytes' if field == 'index_addr'
                               else 'int32(base_vertex_bits)' if field == 'base_vertex' else 'identity'})
    return {'status': 'VALID', 'uses': validated, 'emitted': emitted,
            'emission_mismatches': mismatches}


def audit_record(batch: dict, record: dict, capture: Capture | None = None) -> dict:
    """Pure record comparator. App MATCH/PASS labels are deliberately ignored."""
    result = {'status': 'INCOMPLETE', 'parent_command': batch.get('parent_command'),
              'child': record.get('child') if isinstance(record, dict) else None}
    try:
        record = object_value(record, 'record')
        parent = uint(batch.get('parent_command'), 'batch.parent_command')
        require(parent > 0, 'batch.parent_command must identify the original command')
        child = uint(record.get('child'), 'record.child', 32)
        require(child < batch['count'], 'record child is outside the parent count')
        if 'parent_command' in record:
            require(record['parent_command'] == parent, 'record parent_command mismatch')
        address = uint(record.get('address'), 'record.address')
        result['address'] = address
        expected_address = batch['args_guest'] + child * batch['stride']
        require(address == expected_address, 'record address/child/stride mismatch')
        require(record.get('bytes', RECORD_BYTES) == RECORD_BYTES, 'record must cover exactly 20 bytes')
        consumed = word_values(record.get('consumed_words'), 'record.consumed_words')
        result['consumed_words'] = consumed
        binding = check_consumption(batch, record, consumed)
        if binding['status'] == 'UNSTABLE':
            return dict(result, **binding)
        result['emitted'] = binding['emitted']

        before = state_value(record.get('state_before'), 'record.state_before')
        after = state_value(record.get('state_after'), 'record.state_after')
        reference_after = state_value(record.get('state_reference_after'), 'record.state_reference_after')
        for state, label in ((before, 'state_before'), (after, 'state_after'),
                             (reference_after, 'state_reference_after')):
            if 'valid' in state:
                require(state['valid'] is True, label + ' is invalid')
            if 'address' in state:
                require(state['address'] == address and state.get('bytes') == RECORD_BYTES,
                        label + ' address/byte range mismatch')
        epoch_before = uint(record.get('mutation_epoch_before'), 'record.mutation_epoch_before')
        epoch_after = uint(record.get('mutation_epoch_after'), 'record.mutation_epoch_after')
        epoch_reference = uint(record.get('mutation_epoch_reference_after'), 'record.mutation_epoch_reference_after')
        require(epoch_before <= epoch_after <= epoch_reference, 'non-monotonic mutation epochs')
        result['versions'] = {'before': epoch_before, 'after_consumption': epoch_after,
                              'after_reference': epoch_reference,
                              'gpu_generation': after['gpu_generation'],
                              'unregister_epoch': after['unregister_epoch']}
        changed = states_different(after, reference_after)
        if epoch_after != epoch_reference or changed:
            return dict(result, status='UNSTABLE', reason='source changed between consumed values and reference',
                        changed_state_fields=changed)
        if before['unregister_epoch'] != after['unregister_epoch']:
            return dict(result, status='UNSTABLE', reason='mapping changed during original reads')
        if after['image_gpu_dirty']:
            return dict(result, status='INCOMPLETE', reason='image authority is not represented by a native buffer/backing reference')

        tokens, _ = pending_overlap(after, address, RECORD_BYTES)
        require(after['pending_publication'] == bool(tokens),
                'pending-publication flag and overlapping tokens disagree')
        gpu_bytes = [False] * RECORD_BYTES
        ledger_bytes = [False] * RECORD_BYTES
        for part in after['gpu_dirty_ranges']:
            for offset in range(max(address, part['address']) - address,
                                min(address + RECORD_BYTES, part['address'] + part['bytes']) - address):
                ledger_bytes[offset] = gpu_bytes[offset] = True
        require(after['gpu_dirty'] == any(ledger_bytes), 'GPU dirty flag and exact byte ledger disagree')
        for pending in after['pending_tokens']:
            for part in pending['ranges']:
                for offset in range(max(address, part['address']) - address,
                                    min(address + RECORD_BYTES, part['address'] + part['bytes']) - address):
                    gpu_bytes[offset] = True
        gpu_authority = any(gpu_bytes)
        cpu_authority = not all(gpu_bytes)
        result['pending_tokens'] = tokens
        # cpu_modified/gpu_modified are page-granular tracker flags. The byte
        # ledger and pending-publication ranges take precedence; a coarse CPU
        # flag must not discard a native reference for exact GPU-dirty bytes.
        reference = object_value(record.get('reference'), 'record.reference')
        if 'address' in reference:
            require(reference['address'] == address and reference.get('bytes') == RECORD_BYTES,
                    'independent reference address/byte range mismatch')
        if 'state_at_copy' in reference:
            require(reference['state_at_copy'] == reference_after,
                    'reference state_at_copy disagrees with record state_reference_after')
        if 'mutation_epoch_at_copy' in reference:
            require(reference['mutation_epoch_at_copy'] == epoch_reference,
                    'reference mutation epoch disagrees with record')
        require(reference.get('queued_after_consumption') is True,
                'reference was not ordered after preservation of original consumed values')
        require(reference.get('before_next_child') is True,
                'reference was not frozen before the next child')
        tick_before = uint(reference.get('tick_before'), 'reference.tick_before')
        tick_after = uint(reference.get('tick_after'), 'reference.tick_after')
        require(tick_before <= tick_after, 'reference tick order is reversed')
        result['reference_ticks'] = [tick_before, tick_after]

        native_expected = None
        backing_expected = None
        if gpu_authority:
            require(after['registered'], 'GPU authority has no registered native buffer')
            require(reference.get('kind') == 'gpu_native' and reference.get('ok') is True,
                    'authoritative GPU reference is missing; CPU backing cannot replace it')
            native = uint(reference.get('native'), 'reference.native')
            require(native > 0, 'reference.native has no existing buffer identity')
            buffer_guest = uint(reference.get('buffer_guest'), 'reference.buffer_guest')
            buffer_size = uint(reference.get('buffer_size'), 'reference.buffer_size')
            offset = uint(reference.get('offset'), 'reference.offset')
            require(buffer_guest + offset == address and offset + RECORD_BYTES <= buffer_size,
                    'native reference address/offset/extent mismatch')
            for key in ('native', 'buffer_guest', 'buffer_size'):
                if key in after:
                    require(reference[key] == after[key], 'reference native identity differs from state_after: ' + key)
            native_expected = read_payload(capture, reference, 'reference')
            result['reference_identity'] = {'kind': 'gpu_native', 'native': native,
                                             'buffer_guest': buffer_guest, 'buffer_size': buffer_size,
                                             'offset': offset}
        if cpu_authority:
            # The separate backing alias is authoritative even if the recorder
            # also retained an old Vulkan mirror. Never compare that stale mirror.
            if reference.get('kind') == 'cpu_backing':
                selected = reference
            else:
                selected = object_value(record.get('backing_after'), 'record.backing_after')
            require(selected.get('ok') is True, 'independent CPU backing reference is unavailable')
            backing_expected = read_payload(capture, selected, 'cpu_backing')
            if not gpu_authority:
                result['reference_identity'] = {'kind': 'cpu_backing', 'address': address,
                                                 'source': 'separate_backing_alias'}
            else:
                result['reference_identity']['cpu_bytes_source'] = 'separate_backing_alias'
            if reference.get('kind') == 'gpu_native' and not gpu_authority:
                result['ignored_non_authoritative_gpu_reference'] = True
        native_data = struct.pack('<5I', *native_expected) if native_expected is not None else bytes(RECORD_BYTES)
        backing_data = struct.pack('<5I', *backing_expected) if backing_expected is not None else bytes(RECORD_BYTES)
        expected_bytes = bytes(native_data[i] if gpu_bytes[i] else backing_data[i] for i in range(RECORD_BYTES))
        expected = list(struct.unpack('<5I', expected_bytes))
        result['reference_words'] = expected
        result['authority'] = 'MIXED' if gpu_authority and cpu_authority else 'GPU' if gpu_authority else 'CPU'
        result['authority_by_byte'] = ['GPU' if owned else 'CPU' for owned in gpu_bytes]
        mismatch = first_difference(consumed, expected, address)
        if mismatch is not None and epoch_before != epoch_after:
            return dict(result, status='UNSTABLE', reason='mutation during original reads prevents assigning mismatched values to one version',
                        observed_difference=mismatch)
        if mismatch is not None and after['cpu_modified']:
            used_bytes = struct.pack('<5I', *consumed)
            cpu_differences = [i for i in range(RECORD_BYTES)
                               if not gpu_bytes[i] and used_bytes[i] != expected_bytes[i]]
            if cpu_differences:
                return dict(result, status='UNSTABLE',
                            reason='CPU-dirty bytes differ after the original read; an unobserved concurrent guest store is not excluded',
                            observed_difference=mismatch, changed_cpu_byte_offsets=cpu_differences)
        result['status'] = 'DIVERGENCE' if mismatch else 'MATCH'
        if mismatch:
            mismatch['stage'] = 'memory_reference_to_cp_read'
            result['first_divergence'] = mismatch
        elif binding['emission_mismatches']:
            result['status'] = 'DIVERGENCE'
            result['emission_mismatches'] = binding['emission_mismatches']
            result['first_divergence'] = dict(binding['emission_mismatches'][0],
                                              address=address + 4 * binding['emission_mismatches'][0]['dword'])
        result['scope'] = 'captured original reads versus independent reference in the recorded stable mutation epoch'
        result['producer_certified'] = False
        result['read_window_mutation_observed'] = epoch_before != epoch_after
        return result
    except (EvidenceError, KeyError, TypeError, AttributeError) as error:
        return dict(result, status='INCOMPLETE', reason=str(error))


def witness_events(capture: Capture) -> tuple[list[dict], list[dict]]:
    events, errors = [], []
    if WITNESS_EVENTS not in capture.names:
        return [], [{'file': WITNESS_EVENTS, 'reason': 'witness events missing'}]
    try:
        lines = capture.read(WITNESS_EVENTS).splitlines()
    except EvidenceError as error:
        return [], [{'file': WITNESS_EVENTS, 'reason': str(error)}]
    seen = set()
    for line_number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            event = object_value(json.loads(line, object_pairs_hook=json_object_pairs), 'witness event')
            seq = uint(event.get('seq'), 'witness seq')
            require(isinstance(event.get('kind'), str), 'witness kind must be a string')
            require(seq not in seen, 'duplicate witness seq')
            seen.add(seq)
            if 'observed_seq' in event:
                uint(event['observed_seq'], 'witness observed_seq')
                require(event['observed_seq'] <= seq, 'signal observed_seq is after its exported seq')
            events.append(dict(event, _line=line_number))
        except (EvidenceError, ValueError, UnicodeError) as error:
            errors.append({'file': WITNESS_EVENTS, 'line': line_number, 'reason': str(error)})
    return sorted(events, key=lambda item: item.get('observed_seq', item['seq'])), errors


def check_interleaving(batch: dict, result: dict, events: list[dict]) -> None:
    """Cross-check the version counters with the separately retained event order."""
    records = batch.get('records', [])
    consumed_orders = [record.get('observation_seq_consumed') for record in records if isinstance(record, dict)]
    latest_consumed = max(consumed_orders) if consumed_orders and all(type(value) is int for value in consumed_orders) else None
    for raw, compared in zip(records, result.get('records', [])):
        if not isinstance(raw, dict) or compared['status'] not in ('MATCH', 'DIVERGENCE'):
            continue
        keys = ('observation_seq_before', 'observation_seq_consumed', 'observation_seq_reference')
        present = [key in raw for key in keys]
        if not any(present):
            compared.update(status='INCOMPLETE', reason='record observation order is missing')
            compared.pop('first_divergence', None)
            continue
        try:
            if compared.get('authority') in ('GPU', 'MIXED'):
                native = raw['reference']
                require(native.get('collection_after_all_original_reads') is True,
                        'native collection was not deferred until all original children were consumed')
                collected = uint(native.get('observation_seq_collected'), 'reference.observation_seq_collected')
                require(latest_consumed is not None and collected >= latest_consumed,
                        'native collection precedes another child original consumption')
                compared['collection_order'] = collected
            before, consumed, reference = [uint(raw.get(key), 'record.' + key) for key in keys]
            require(before <= consumed <= reference, 'non-monotonic observation order')
            require(reference < batch['seq'], 'record reference order must precede the saved batch')
            if 'collection_order' in compared:
                require(compared['collection_order'] >= reference, 'native collection precedes its queued copy')
            following = next((item for item in records if isinstance(item, dict)
                              and item.get('child') == raw['child'] + 1), None)
            if following is not None:
                require(reference <= uint(following.get('observation_seq_before'), 'next child observation order'),
                        'reference was frozen after the next child original reads began')
            compared['observation_order'] = {'before': before, 'consumed': consumed, 'reference': reference}
            changed = []
            for event in events:
                order = event.get('observed_seq', event['seq'])
                if not consumed < order <= reference:
                    continue
                if event.get('kind') not in ('mutation', 'write_backing', 'fault_write_candidate',
                                             'publication_queued', 'publication_done'):
                    continue
                address, size = event.get('address'), event.get('bytes')
                if type(address) is not int or type(size) is not int:
                    continue
                if address < raw['address'] + RECORD_BYTES and raw['address'] < address + size:
                    changed.append({'seq': order, 'kind': event['kind'], 'path': event.get('path'),
                                    'address': address, 'bytes': size})
            if changed:
                compared.update(status='UNSTABLE', reason='overlapping write/publication event between consumption and reference',
                                interleaved_events=changed)
                compared.pop('first_divergence', None)
        except (EvidenceError, TypeError, KeyError) as error:
            compared.update(status='INCOMPLETE', reason=str(error))
            compared.pop('first_divergence', None)
    recount_batch(result)


def recount_batch(result: dict) -> None:
    if not result.get('records'):
        return
    statuses = Counter(record['status'] for record in result['records'])
    result['counts'] = dict(statuses)
    result['status'] = ('DIVERGENCE' if statuses['DIVERGENCE'] else 'INCOMPLETE' if statuses['INCOMPLETE']
                        else 'UNSTABLE' if statuses['UNSTABLE'] else 'MATCH')
    differences = [dict(record['first_divergence'], child=record['child']) for record in result['records']
                   if record['status'] == 'DIVERGENCE']
    if differences:
        result['first_divergence'] = min(differences, key=lambda item: (item['child'], item['dword']))
    else:
        result.pop('first_divergence', None)


def audit_retention(manifest: dict, batches: list[dict]) -> dict:
    """Derive the nonzero-to-zero transition; validate the pinned slot identities."""
    loss = manifest.get('observed_loss')
    result = {'status': 'VERIFIED', 'streams': [], 'errors': [], 'observed_loss': loss,
              'loss_binding': 'NOT_OBSERVED' if loss is None else 'OBSERVED_WINDOW'}
    if loss is not None:
        try:
            loss = object_value(loss, 'observed_loss')
            for key in ('frame', 'queue', 'seq'):
                uint(loss.get(key), 'observed_loss.' + key)
        except EvidenceError as error:
            result['errors'].append({'reason': str(error)})
            result['loss_binding'] = 'INCOMPLETE'
    slots = manifest.get('retention')
    if not isinstance(slots, list):
        return dict(result, status='INCOMPLETE', errors=[{'reason': 'retention: expected stream list'}])
    for slot in slots:
        try:
            slot = object_value(slot, 'retention stream')
            stream = uint(slot.get('stream_id'), 'retention.stream_id')
            positive = uint(slot.get('last_nonzero'), 'retention.last_nonzero')
            zero = uint(slot.get('first_zero'), 'retention.first_zero')
            row = {'stream_id': stream, 'last_nonzero_seq': positive, 'first_zero_seq': zero}
            if 'sealed' in slot:
                require(type(slot['sealed']) is bool, 'retention.sealed must be bool')
                row['sealed'] = slot['sealed']
                if loss is not None and not slot['sealed']:
                    result['loss_binding'] = 'INCOMPLETE'
                    result['errors'].append({'reason': 'observed loss stream still has an unfinished batch', 'stream_id': stream})
            if 'last_seen' in slot:
                last = object_value(slot['last_seen'], 'retention.last_seen')
                for key in ('frame', 'submit', 'parent_command', 'seq'):
                    uint(last.get(key), 'retention.last_seen.' + key)
                require(last['seq'] >= max(positive, zero), 'last_seen precedes retained endpoint')
                row['last_seen'] = last
                if isinstance(loss, dict) and type(loss.get('seq')) is int:
                    row['batch_finished_after_loss_observation'] = last['seq'] > loss['seq']
            selected = [batch for batch in batches if batch.get('stream_id') == stream]
            by_seq = {batch.get('seq'): batch for batch in selected}
            for seq, nonzero in ((positive, True), (zero, False)):
                if not seq:
                    continue
                require(seq in by_seq, f'pinned batch seq {seq} missing from stream {stream}')
                batch = by_seq[seq]
                records = batch.get('records')
                require(isinstance(records, list) and len(records) == batch.get('count'),
                        f'pinned batch seq {seq} has incomplete child inventory')
                observed = any(record.get('emitted', {}).get('index_count', 0) != 0
                               and record.get('emitted', {}).get('instance_count', 0) != 0
                               for record in records if isinstance(record, dict))
                require(observed == nonzero, f'pinned batch seq {seq} does not match its retention slot')
                row['last_nonzero_parent' if nonzero else 'first_zero_parent'] = batch['parent_command']
            require(not zero or (positive and positive < zero), 'first_zero must follow retained nonzero batch')
            if positive and zero:
                left, right = by_seq[positive], by_seq[zero]
                for key in ('args_guest', 'count', 'stride', 'index_base', 'index_limit', 'index_type'):
                    require(left.get(key) == right.get(key), 'retained transition changed parent geometry: ' + key)
                row['transition_observed'] = True
            else:
                row['transition_observed'] = False
            result['streams'].append(row)
        except (EvidenceError, TypeError, AttributeError) as error:
            result['errors'].append({'reason': str(error), 'stream': slot})
    if result['errors']:
        result['status'] = 'INCOMPLETE'
    return result


def audit_pinned_producer(batch: dict, capture: Capture, integrity: dict) -> dict:
    """Check immutable producer copies without confusing current source aliases."""
    result = {'status': 'AVAILABLE', 'files': [], 'errors': [], 'producer_certified': False}
    producer = batch.get('last_observed_producer')
    result['last_observed_producer'] = producer
    if isinstance(producer, dict):
        result['candidate_only'] = (producer.get('candidate') is True
                                    or producer.get('candidate_only') is True
                                    or producer.get('path') == 'compute_descriptor_candidate')
    else:
        result['candidate_only'] = True
    files = batch.get('pinned_producer_files', [])
    if not isinstance(files, list):
        return dict(result, status='INCOMPLETE', errors=[{'reason': 'pinned_producer_files: expected list'}])
    for item in files:
        try:
            item = object_value(item, 'pinned producer file')
            name = safe_name(item.get('file'))
            original = safe_name(item.get('source_name', item.get('source_file')))
            require(name in integrity and integrity[name]['status'] == 'VERIFIED',
                    'pinned producer file absent from verified export closure: ' + name)
            data = capture.read(name)
            require(len(data) == uint(item.get('bytes'), 'pinned producer bytes'),
                    'pinned producer size mismatch: ' + name)
            require(fnv1a64(data) == uint(item.get('fnv1a64'), 'pinned producer digest'),
                    'pinned producer FNV1a64 mismatch: ' + name)
            result['files'].append({'file': name, 'source_file_at_pin': original, 'bytes': len(data),
                                    'sha256': hashlib.sha256(data).hexdigest()})
        except (EvidenceError, TypeError) as error:
            result['errors'].append({'reason': str(error)})
    if result['errors']:
        result['status'] = 'INCOMPLETE'
    elif not files and producer is None:
        result['status'] = 'UNOBSERVED'
    return result


def bind_guest_journal(batch: dict, result: dict, journal: list[dict]) -> None:
    """Use retained real child commands to cross-check emitted observations."""
    parent_index = next((i for i, event in enumerate(journal)
                         if event['seq'] == batch.get('parent_command')), None)
    if parent_index is None:
        result['guest_journal_binding'] = 'PARENT_NOT_RETAINED'
        return
    parent = journal[parent_index]
    checks = {'indirect_guest': batch.get('args_guest'), 'count': batch.get('count'),
              'stride': batch.get('stride'), 'index_guest': batch.get('index_base'),
              'index_limit': batch.get('index_limit'), 'submit': batch.get('submit')}
    identity_matches = (parent.get('kind') == 'guest_command' and parent.get('op') == 'indirect_indexed'
                        and all(parent.get(key) == value for key, value in checks.items()))
    if not identity_matches:
        result['guest_journal_binding'] = 'PARENT_IDENTITY_MISMATCH'
        for record in result.get('records', []):
            record.update(status='INCOMPLETE', reason='batch parent identity disagrees with retained guest command')
            record.pop('first_divergence', None)
        recount_batch(result)
        return
    children = []
    for event in journal[parent_index + 1:]:
        if event.get('kind') != 'guest_command':
            continue
        if event.get('op') != 'indexed':
            break
        children.append(event)
        if len(children) == batch['count']:
            break
    result['guest_journal_binding'] = 'PARENT_VERIFIED'
    for record in result.get('records', []):
        child = record.get('child')
        if type(child) is not int or not 0 <= child < len(children) or 'emitted' not in record:
            continue
        observed = children[child]
        record['guest_child_command'] = observed['seq']
        keys = {'count': 'index_count', 'instances': 'instance_count', 'index_guest': 'index_addr'}
        if any(observed.get(key) != record['emitted'].get(field) for key, field in keys.items()):
            record.update(status='INCOMPLETE', reason='captured emitted tuple disagrees with retained original child command')
            record.pop('first_divergence', None)
        else:
            record['guest_child_binding'] = 'VERIFIED'
    recount_batch(result)


def audit_capture(path: str | Path) -> dict:
    """Public API. Return local results even when another retained batch is bad."""
    report = {'schema': 1, 'scope': 'local CPU indirect-argument consumption; no global G0/P8 decision',
              'analysis_state': 'INCOMPLETE', 'batches': [], 'errors': [], 'first_divergence': None}
    capture = None
    try:
        capture = Capture(path)
        journal, journal_errors = journal_events(capture)
        report['journal_errors'] = journal_errors
        report['legacy_observation'] = legacy_history(journal)
        if MANIFEST not in capture.names:
            report['errors'].append({'file': MANIFEST, 'reason': 'independent CP-consumption acquisition absent'})
            return report
        manifest = object_value(capture.json(MANIFEST), 'manifest')
        require(manifest.get('schema') == 1, 'unsupported CP witness schema')
        integrity, integrity_errors = verify_required_files(capture, manifest)
        report['file_integrity'] = integrity
        report['errors'].extend(integrity_errors)
        events, event_errors = witness_events(capture)
        report['errors'].extend(event_errors)
        report['event_inventory'] = dict(Counter(event.get('kind', 'unknown') for event in events))
        drops = object_value(manifest.get('drops'), 'manifest.drops')
        for key in ('events', 'blobs', 'signals', 'io_errors'):
            uint(drops.get(key), 'manifest.drops.' + key)
        report['drops'] = drops
        report['prehistory_evicted'] = uint(manifest.get('prehistory_evicted', 0), 'manifest.prehistory_evicted')
        entries = manifest.get('batches')
        require(isinstance(entries, list), 'manifest.batches: expected list')
        raw_batches = []
        seen = set()
        for entry in entries:
            name = None
            parent = None
            try:
                entry = object_value(entry, 'manifest batch')
                parent = uint(entry.get('parent_command'), 'manifest parent_command')
                name = safe_name(entry.get('file'))
                require(name not in seen, 'duplicate manifest batch file')
                seen.add(name)
                require(name in integrity and integrity[name]['status'] == 'VERIFIED',
                        'batch file missing from verified export closure')
                batch = object_value(capture.json(name), 'batch file')
                raw_batches.append(batch)
                result = audit_batch(batch, capture, parent)
                result['file'] = name
                result['seq'] = batch.get('seq')
                check_interleaving(batch, result, events)
                bind_guest_journal(batch, result, journal)
                result['producer_evidence'] = audit_pinned_producer(batch, capture, integrity)
                report['errors'].extend(dict(issue, parent_command=parent)
                                        for issue in result['producer_evidence']['errors'])
                # A retained journal event and its pinned batch must agree on
                # the actual operands; pinning metadata may legitimately differ.
                twins = [event for event in events if event.get('kind') == 'cp_batch'
                         and event.get('seq') == batch.get('seq')]
                if twins:
                    require(len(twins) == 1, 'ambiguous retained batch event')
                    require(all(twins[0].get(key) == batch.get(key)
                                for key in ('parent_command', 'args_guest', 'stream_id', 'records')),
                            'pinned batch and witness journal disagree')
                result['journal_copy_present'] = bool(twins)
                report['batches'].append(result)
            except (EvidenceError, TypeError, KeyError) as error:
                report['batches'].append({'status': 'INCOMPLETE', 'parent_command': parent,
                                          'file': name, 'records': [], 'reason': str(error)})
        report['retention'] = audit_retention(manifest, raw_batches)
        report['batch_counts'] = dict(Counter(batch['status'] for batch in report['batches']))
        report['record_counts'] = dict(Counter(record['status'] for batch in report['batches']
                                              for record in batch.get('records', [])))
        differences = [dict(batch['first_divergence'], parent_command=batch['parent_command'], seq=batch.get('seq'))
                       for batch in report['batches'] if batch.get('first_divergence')]
        if differences:
            report['first_divergence'] = min(differences,
                key=lambda item: (item['parent_command'], item['child'], item['dword']))
            report['analysis_state'] = 'DIVERGENCE'
        elif (report['errors'] or any(drops.values()) or report['retention']['status'] != 'VERIFIED'
              or report['batch_counts'].get('INCOMPLETE') or not report['batches']):
            report['analysis_state'] = 'INCOMPLETE'
        elif report['batch_counts'].get('UNSTABLE'):
            report['analysis_state'] = 'UNSTABLE'
        else:
            report['analysis_state'] = 'MATCH'
        report['limits'] = ['same recorded version/observation window, not a universal CPU-store trace',
                            'descriptor/dispatch candidates are not proofs of executed producer stores',
                            'local equality does not certify the original producer or visibility']
        return report
    except (EvidenceError, OSError, zipfile.BadZipFile, TypeError, KeyError) as error:
        report['errors'].append({'reason': str(error)})
        return report
    finally:
        if capture is not None:
            report['inspected_files'] = capture.inspected
            capture.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    report = audit_capture(args.capture)
    rendered = json.dumps(report, indent=2, sort_keys=True) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered)
    sys.stdout.write(rendered)
    return 3 if report['analysis_state'] == 'DIVERGENCE' else 0 if report['analysis_state'] == 'MATCH' else 2


def audit_batch(batch: Any, capture: Capture | None = None,
                expected_parent: int | None = None) -> dict:
    """A missing/bad child retains the other children's independently valid results."""
    result = {'status': 'INCOMPLETE', 'records': []}
    try:
        batch = object_value(batch, 'batch')
        require(batch.get('kind') == 'cp_batch', 'expected kind=cp_batch')
        parent = uint(batch.get('parent_command'), 'batch.parent_command')
        if 'seq' in batch:
            uint(batch['seq'], 'batch.seq')
        result['parent_command'] = parent
        if expected_parent is not None:
            require(parent == expected_parent, 'batch parent_command differs from manifest')
        for key in ('args_guest', 'count', 'stride', 'index_base', 'index_limit', 'frame', 'submit', 'tick', 'stream_id'):
            uint(batch.get(key), 'batch.' + key)
        require(batch['count'] > 0 and batch['count'] <= 64, 'batch count outside captured scope')
        require(batch['stride'] >= RECORD_BYTES and batch['stride'] % 4 == 0, 'invalid indirect stride')
        require(type(batch.get('index_element_bytes')) is int and batch['index_element_bytes'] in (2, 4),
                'invalid index element size')
        require(type(batch.get('index_type')) is int and batch['index_type'] in (0, 1),
                'invalid captured index type')
        require(batch['index_element_bytes'] == (4 if batch['index_type'] == 1 else 2),
                'index type and element size disagree')
        result.update({key: batch[key] for key in ('args_guest', 'count', 'stride', 'frame', 'submit', 'tick', 'stream_id')})
        records = batch.get('records')
        require(isinstance(records, list), 'batch.records: expected list')
        seen = set()
        for record in records:
            item = audit_record(batch, record, capture)
            child = item.get('child')
            if type(child) is int and child in seen:
                item = dict(item, status='INCOMPLETE', reason='duplicate child identity')
                for previous in result['records']:
                    if previous.get('child') == child:
                        previous.update(status='INCOMPLETE', reason='duplicate child identity')
            seen.add(child if type(child) is int else None)
            result['records'].append(item)
        for child in range(batch['count']):
            if child not in seen:
                result['records'].append({'status': 'INCOMPLETE', 'parent_command': parent,
                                          'child': child, 'reason': 'child record missing from batch'})
        statuses = Counter(record['status'] for record in result['records'])
        result['counts'] = dict(statuses)
        result['status'] = ('DIVERGENCE' if statuses['DIVERGENCE'] else 'INCOMPLETE' if statuses['INCOMPLETE']
                            else 'UNSTABLE' if statuses['UNSTABLE'] else 'MATCH')
        divergences = [record['first_divergence'] | {'child': record['child']}
                       for record in result['records'] if record['status'] == 'DIVERGENCE']
        if divergences:
            result['first_divergence'] = min(divergences, key=lambda item: (item['child'], item['dword']))
        check_interleaving(batch, result, [])
        return result
    except (EvidenceError, TypeError, KeyError) as error:
        return dict(result, status='INCOMPLETE', reason=str(error))


if __name__ == '__main__':
    raise SystemExit(main())

