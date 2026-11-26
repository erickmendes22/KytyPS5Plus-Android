#!/usr/bin/env python3
"""Independent, per-draw guest GFX1010 vertex-position replay.

Decode the captured guest ISA with LLVM, resolve S_LOAD from consumed scalar
regions and buffer loads from captured *native GPU* bytes, then evaluate the
backward slice of EXP POS0. Native XYZW, input_x/y/z and matrix probe columns
are observations only and never seed the reference. Missing unrelated normal,
UV, fragment or producer data do not prevent a position comparison.

Usage: python3 prelight_vertex_oracle.py CAPTURE.zip [--record NAME] [--output X]
Requires NumPy and an LLVM shared library with the AMDGPU disassembler (LLVM19
tested). --llvm-library selects a nonstandard installation. No Kyty decoder,
IR, SPIR-V arithmetic, PC renderer or expected visibility is used as reference.

The scope is an already-admitted vertex invocation. NGG/EXEC admission, original
buffer production, complete shader equivalence and P8 are NOT certified.
Numerics use separate IEEE binary32 multiply/add, round-to-nearest, preserving
subnormals; a mismatch is an edge-local candidate, not an automatic production
fix. NaN payload differences and signed zero are reported separately.

Old captures have no scalar address log. For four exact guest binaries only,
the documented legacy flattened-SRT map may be used, with its weaker binding
premise clearly reported. --require-scalar-regions disables that compatibility.
New captures always use vs_scalar_regions when that key is present and never
silently fall back to a legacy map after a missing/ambiguous scalar read.

Exit 0: every selected draw compared and matched. Exit 2: one or more draws
unsupported/incomplete, without a mismatch. Exit 3: a position mismatch exists.
Other draws' results are retained in every case; no integral replay gate.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.util
from dataclasses import dataclass
import fnmatch
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import sys
import zipfile

import numpy as np


class Unsupported(ValueError):
    pass


def require(condition, reason):
    if not condition:
        raise Unsupported(reason)


def words(values, label):
    require(isinstance(values, list) and all(type(v) is int and 0 <= v <= 0xffffffff for v in values),
            label + ' must be uint32 words')
    return np.asarray(values, dtype='<u4')


class Capture:
    """Bounded ZIP/directory reader; archive members are never extracted."""
    def __init__(self, path):
        path = Path(path)
        self.archive = None
        if path.is_dir():
            candidates = [path, path / 'ds-lighting-capture']
            roots = [p for p in candidates if (p / 'queue-state').is_dir()]
            require(len(roots) == 1, 'expected one capture/queue-state directory')
            self.root = roots[0].resolve()
            self.names = sorted(p.relative_to(self.root).as_posix() for p in self.root.rglob('*') if p.is_file())
        else:
            self.archive = zipfile.ZipFile(path)
            names = self.archive.namelist()
            require(len(names) == len(set(names)), 'duplicate ZIP names')
            roots = {n.split('queue-state/', 1)[0] for n in names if 'queue-state/' in n}
            require(len(roots) == 1, 'expected one capture root in ZIP')
            self.root = next(iter(roots))
            self.names = sorted(n[len(self.root):] for n in names if n.startswith(self.root))

    def close(self):
        if self.archive:
            self.archive.close()

    def read(self, name):
        path = PurePosixPath(name)
        require(not path.is_absolute() and '..' not in path.parts and name in self.names,
                'missing/unsafe capture member: ' + str(name))
        if self.archive:
            info = self.archive.getinfo(self.root + name)
            require(info.file_size <= 512 * 1024 * 1024, 'capture member exceeds 512 MiB')
            return self.archive.read(info)
        source = (self.root / name).resolve()
        require(source.is_relative_to(self.root), 'capture member escapes selected directory')
        require(source.stat().st_size <= 512 * 1024 * 1024, 'capture member exceeds 512 MiB')
        return source.read_bytes()

    def json(self, name):
        return json.loads(self.read(name))


class LLVM:
    """LLVM's upstream gfx1010 decoder, independent of the emulator."""
    def __init__(self, library=None):
        names = [library] if library else [ctypes.util.find_library('LLVM-' + str(v)) for v in range(21, 15, -1)]
        names = [n for n in names if n]
        require(names, 'no LLVM AMDGPU shared library found; pass --llvm-library')
        failures = []
        for name in names:
            try:
                lib = ctypes.CDLL(name)
                for suffix in ('TargetInfo', 'Target', 'TargetMC', 'Disassembler'):
                    getattr(lib, 'LLVMInitializeAMDGPU' + suffix)()
                lib.LLVMCreateDisasmCPUFeatures.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                                                         ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p]
                lib.LLVMCreateDisasmCPUFeatures.restype = ctypes.c_void_p
                lib.LLVMDisasmInstruction.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint64,
                                                     ctypes.c_uint64, ctypes.c_char_p, ctypes.c_size_t]
                lib.LLVMDisasmInstruction.restype = ctypes.c_size_t
                lib.LLVMDisasmDispose.argtypes = [ctypes.c_void_p]
                ctx = lib.LLVMCreateDisasmCPUFeatures(b'amdgcn-amd-amdhsa', b'gfx1010', b'', None, 0, None, None)
                require(ctx, 'LLVM rejected gfx1010 disassembler')
                self.lib, self.ctx, self.library = lib, ctx, name
                return
            except (AttributeError, OSError, Unsupported) as error:
                failures.append(str(error))
        raise Unsupported('LLVM initialization failed: ' + '; '.join(failures))

    def decode(self, data):
        raw = ctypes.create_string_buffer(data)
        offset = 0
        while offset < len(data):
            out = ctypes.create_string_buffer(1024)
            size = self.lib.LLVMDisasmInstruction(self.ctx, ctypes.byref(raw, offset), len(data) - offset,
                                                 offset, out, len(out))
            require(size and offset + size <= len(data), f'gfx1010 undecodable bytes at {offset:#x}')
            text = out.value.decode().strip()
            yield offset, size, text
            if text.startswith('exp pos0'):
                return
            offset += size
        raise Unsupported('guest has no EXP POS0')

    def close(self):
        self.lib.LLVMDisasmDispose(self.ctx)


def span(start, count):
    return list(range(start, start + count))


# These maps address consumed flattened-SRT DWORDs, not a presumed PC image.
# They are compatibility premises for old captures; the new scalar log removes
# the map from the read path. Every map is tied to the *full* guest binary SHA.
PROFILES = {
    'aff9c7ef505c6253': {
        'sha256': '7fc30d3ac58f3d6e445057e4fedc1b429e93dc38b45dd64d411b586da168aa3a',
        'flat': {0x48: span(0, 2), 0x50: span(2, 4), 0x5c: span(6, 4), 0x64: span(10, 4),
                 0x6c: span(14, 16), 0x74: span(30, 2)}, 'descriptors': {0x5c: 0, 0x64: 1}},
    '868546d85ce5090a': {
        'sha256': 'a7988747383220232e3b94e254101cffe9c4a1c0d87676ff00f5bc5b0390cb00',
        'flat': {0x48: span(2, 4), 0x50: span(0, 2), 0x5c: span(6, 4), 0x64: span(10, 4),
                 0x6c: span(14, 4), 0x74: span(18, 4), 0x7c: span(22, 4), 0x84: span(26, 16),
                 0x8c: span(42, 2), 0x94: span(44, 2), 0x9c: span(46, 1)},
        'descriptors': {0x5c: 0, 0x64: 1, 0x6c: 2, 0x74: 3, 0x7c: 4}},
    '0a472b34f593b99b': {
        'sha256': '61e7e4bfeb7b015dacdfa9fe7ffcf79ee9df1ab1cbff74d14e88dbd65b44dc27',
        'flat': {0x48: span(2, 4), 0x50: span(0, 2), 0x5c: span(14, 4) + span(6, 4),
                 0x64: span(10, 4), 0x6c: span(18, 4), 0x74: span(26, 16), 0x7c: span(22, 4),
                 0x84: span(42, 2), 0x8c: span(44, 2), 0x94: span(46, 1)},
        'descriptors': {0x64: 1, 0x6c: 3, 0x7c: 4}},
    'c96f3fcde8141266': {
        'sha256': '306cc768b6cec4e1eb7658526939925852c9e07156dd25db0170006eea78939f',
        'flat': {0x48: span(0, 2), 0x58: span(2, 4), 0x90: span(6, 2), 0xa0: span(24, 1),
                 0xa8: span(8, 4), 0xc4: span(12, 4), 0xec: span(16, 4), 0xf4: span(20, 4),
                 0xfc: span(25, 16), 0x104: span(41, 8), 0x10c: span(49, 4), 0x114: span(53, 2)},
        'descriptors': {0x58: 0, 0xa8: 1, 0xec: 2, 0xf4: 3}, 'conditional': (0x90, 0xc4)},
    '2337192397a30339': {
        'sha256': 'fc4e429120a33e327860c80e7df15feb20ddaafe08f8d05b2b7968f3c49f0774',
        'flat': {}, 'descriptors': {}},
}

NGG_PREFIX = bytes.fromhex(
    '0300a0bf03ffeb930800080003ff0087ff0000006b8c6a8f006a7c88000080bf'
    '090090bfc06bea81c16afe90410900f800000000c000ea810fff8cbfc16afe90')


@dataclass(eq=False)
class Node:
    op: str
    args: tuple
    pc: int = -1


def node(op, *args, pc=-1):
    return Node(op, args, pc)


def registers(token):
    special = {'vcc': ['s106', 's107'], 'vcc_lo': ['s106'], 'vcc_hi': ['s107'],
               'exec': ['s126', 's127'], 'exec_lo': ['s126'], 'exec_hi': ['s127'], 'm0': ['s124']}
    if token in special:
        return special[token]
    if re.fullmatch('[sv][0-9]+', token):
        return [token]
    match = re.fullmatch(r'([sv])\[(\d+):(\d+)\]', token)
    if match:
        return [match[1] + str(i) for i in range(int(match[2]), int(match[3]) + 1)]
    return []


class Program:
    def __init__(self, decoded, profile, user_data, user_data_base=8):
        require(type(user_data_base) is int and 0 <= user_data_base < 104, 'invalid VS user-data SGPR base')
        self.reg = {f's{i + user_data_base}': node('const', int(v)) for i, v in enumerate(user_data)}
        self.reg.update(v5=node('input', 'vertex'), v8=node('input', 'instance'))
        self.profile, self.export_pc = profile, None
        self.ops = []
        for pc, size, text in decoded:
            if pc < 0x40:  # common, full-fingerprint-validated NGG admission prologue
                continue
            self.pc = pc
            self.ops.append((pc, text))
            if text.startswith('exp pos0'):
                fields = text[len('exp pos0'):].strip().split(',')
                self.output = tuple(self.source(field.strip().split()[0]) for field in fields[:4])
                require(len(self.output) == 4, 'incomplete POS0 export')
                self.export_pc = pc
                break
            self.instruction(text)
        require(self.export_pc is not None, 'no position export reached')

    def source(self, token):
        token = token.strip()
        if token.startswith('-') and registers(token[1:]):
            return node('fneg', self.source(token[1:]), pc=self.pc)
        if token.startswith('|') and token.endswith('|'):
            return node('fabs', self.source(token[1:-1]), pc=self.pc)
        regs = registers(token)
        if len(regs) == 1:
            return self.reg.get(regs[0], node('unknown', 'uninitialized ' + regs[0], pc=self.pc))
        if token == 'null':
            return node('const', 0)
        try:
            bits = struct.unpack('<I', struct.pack('<f', float(token)))[0] if '.' in token else int(token, 0) & 0xffffffff
            return node('const', bits)
        except (ValueError, OverflowError):
            return node('unknown', 'unsupported source ' + token, pc=self.pc)

    def sources(self, token):
        return tuple(self.source(x) for x in registers(token))

    def set_group(self, dest, group):
        for component, reg in enumerate(registers(dest)):
            self.reg[reg] = node('component', group, component, pc=self.pc)

    def instruction(self, text):
        opcode, _, rest = text.partition(' ')
        args = [x.strip().split()[0] for x in rest.split(',') if x.strip()]
        conditional = self.profile.get('conditional')
        if conditional and conditional[0] <= self.pc < conditional[1]:
            if args:
                for r in registers(args[0]):
                    self.reg[r] = node('unknown', 'conditional branch value outside supported position slice', pc=self.pc)
            return
        if opcode.startswith(('s_waitcnt', 's_nop')):
            return
        if opcode.startswith('s_cbranch'):
            require(conditional and self.pc == conditional[0] - 4, f'unsupported control flow at {self.pc:#x}')
            return
        require(not opcode.startswith(('s_branch', 's_setpc', 's_swappc')),
                f'unsupported control flow at {self.pc:#x}: {opcode}')
        if opcode.startswith('s_load_dword'):
            count = len(registers(args[0]))
            group = node('scalar_load', self.sources(args[1]), self.source(args[2]), count, pc=self.pc)
            self.set_group(args[0], group)
            return
        if opcode.startswith('s_buffer_load_dword'):
            group = node('buffer_load', self.sources(args[1]), node('const', 0), self.source(args[2]),
                         len(registers(args[0])), False, False, pc=self.pc)
            self.set_group(args[0], group)
            return
        if opcode.startswith('buffer_load'):
            count = len(registers(args[0]))
            is_format = opcode.startswith('buffer_load_format')
            require('offen' not in text, f'unsupported buffer offen at {self.pc:#x}')
            offset = re.search(r'\boffset:(\d+)\b', text)
            byte_offset = node('uadd', self.source(args[3]), node('const', int(offset[1]) if offset else 0), pc=self.pc)
            index = self.source(args[1]) if 'idxen' in text else node('const', 0)
            group = node('buffer_load', self.sources(args[2]), index, byte_offset, count, is_format,
                         'idxen' in text, pc=self.pc)
            self.set_group(args[0], group)
            return
        if opcode.startswith('s_mov_b'):
            destination, source = registers(args[0]), self.sources(args[1])
            if source and len(source) == len(destination):
                self.reg.update(zip(destination, source))
            else:
                for reg in destination:
                    self.reg[reg] = self.source(args[1])
            return
        if opcode.startswith('v_cmp'):
            # A wave mask is not a float position operand. If it subsequently
            # reaches position, the unknown value fails that draw explicitly.
            require(not opcode.startswith('v_cmpx') or conditional is not None,
                    f'unsupported EXEC modification at {self.pc:#x}')
            self.reg['s106'] = node('unknown', 'wave comparison/EXEC mask', pc=self.pc)
            return
        dest = registers(args[0]) if args else []
        if not dest:
            return
        op = re.sub(r'_(e32|e64|sdwa)$', '', opcode)
        operands = [self.source(a) for a in args[1:]]
        if '_sdwa' in opcode:
            for i in range(min(2, len(operands))):
                select = re.search(rf'\bsrc{i}_sel:(\w+)', text)
                if select and select[1] != 'DWORD':
                    if select[1] == 'WORD_0' and op == 'v_add_nc_u32':
                        operands[i] = node('uand', operands[i], node('const', 0xffff), pc=self.pc)
                    else:
                        operands[i] = node('unknown', 'unsupported SDWA selection ' + select[1], pc=self.pc)
        simple = {'v_add_f32': 'fadd', 'v_sub_f32': 'fsub', 'v_mul_f32': 'fmul',
                  'v_add_nc_u32': 'uadd', 'v_and_b32': 'uand', 'v_sqrt_f32': 'fsqrt'}
        if op in simple and len(operands) == (1 if op == 'v_sqrt_f32' else 2):
            value = node(simple[op], *operands, pc=self.pc)
        elif op == 'v_subrev_f32' and len(operands) == 2:
            value = node('fsub', operands[1], operands[0], pc=self.pc)
        elif op in ('v_mad_f32', 'v_madmk_f32') and len(operands) == 3:
            value = node('fadd', node('fmul', *operands[:2], pc=self.pc), operands[2], pc=self.pc)
        elif op == 'v_mac_f32' and len(operands) == 2:
            value = node('fadd', node('fmul', *operands, pc=self.pc), self.source(args[0]), pc=self.pc)
        elif op == 'v_mov_b32' and len(operands) == 1:
            value = operands[0]
        else:
            value = node('unknown', 'unsupported instruction ' + text, pc=self.pc)
        if re.search(r'\b(clamp|mul:|div:)', text):
            value = node('unknown', 'unsupported output modifier ' + text, pc=self.pc)
        for reg in dest:
            self.reg[reg] = value


class Evaluator:
    def __init__(self, capture, record, profile, rows, require_regions=False):
        self.capture, self.record, self.profile, self.rows = capture, record, profile, rows
        self.cache, self.file_cache, self.loads = {}, {}, []
        self.descriptors = record.get('vs_buffers', record.get('descriptors', []))
        self.sources = record.get('vs_bound', record.get('vertex_sources', []))
        self.scalar_mode = 'consumed_scalar_regions' if 'vs_scalar_regions' in record else 'legacy_flattened_srt_binding_premise'
        require(not require_regions or self.scalar_mode == 'consumed_scalar_regions', 'consumed scalar regions required for this draw')
        self.scalar_words, self.scalar_conflicts = {}, set()
        for region in record.get('vs_scalar_regions', []):
            require(isinstance(region, dict) and region.get('source') == 'consumed_srt', 'scalar region is not a consumed SRT read')
            data = words(region.get('words'), 'scalar region')
            address = region.get('address')
            require(type(address) is int and address >= 0 and len(data) * 4 == region.get('bytes', len(data) * 4),
                    'invalid scalar region address/size')
            for j, value in enumerate(data):
                at, value = address + j * 4, int(value)
                if at in self.scalar_words and self.scalar_words[at] != value:
                    self.scalar_conflicts.add(at)
                self.scalar_words[at] = value

    def scalar(self, group, only_component=None):
        base, offset, count = group.args
        if self.scalar_mode == 'legacy_flattened_srt_binding_premise':
            mapping = self.profile['flat'].get(group.pc)
            require(mapping is not None and len(mapping) == count,
                    f'S_LOAD {group.pc:#x}: no consumed scalar address log or verified legacy mapping')
            flat = words(self.record.get('vs_srt', self.record.get('srt')), 'flattened SRT')
            require(max(mapping) < len(flat), f'S_LOAD {group.pc:#x}: truncated flattened SRT')
            result = flat[mapping]
            slot = self.profile['descriptors'].get(group.pc)
            if slot is not None:
                require(slot < len(self.descriptors) and list(result) == list(self.descriptors[slot][:count]),
                        f'S_LOAD {group.pc:#x}: legacy descriptor mapping disagrees with slot {slot}')
            selected = list(range(count)) if only_component is None else [only_component]
            self.loads.append({'pc': hex(group.pc), 'kind': 'scalar',
                               'flat_word_indices': [mapping[i] for i in selected],
                               'words': [int(result[i]) for i in selected]})
            return result if only_component is None else result[only_component]
        require(len(base) == 2, f'S_LOAD {group.pc:#x}: expected 64-bit scalar base')
        address = self.integer(base[0]) | (self.integer(base[1]) << 32)
        address += self.integer(offset)
        result = []
        selected = list(range(count)) if only_component is None else [only_component]
        for j in selected:
            at = address + j * 4
            require(at not in self.scalar_conflicts, f'S_LOAD {group.pc:#x}: conflicting consumed reads at {at:#x}')
            require(at in self.scalar_words, f'S_LOAD {group.pc:#x}: missing consumed scalar DWORD at {at:#x}')
            result.append(self.scalar_words[at])
        self.loads.append({'pc': hex(group.pc), 'kind': 'scalar', 'instruction_address': address,
                           'instruction_dwords': count, 'components': selected, 'words': result})
        result = np.asarray(result, dtype='<u4')
        return result if only_component is None else result[0]

    def integer(self, value):
        result = np.asarray(self.eval(value))
        require(result.ndim == 0, f'expected scalar at {value.pc:#x}')
        return int(result)

    def payload(self, source):
        if 'gpu_words' in source and source['gpu_words']:
            return words(source['gpu_words'], 'native GPU words').view('u1')
        if source.get('file'):
            name = source['file']
            if name not in self.file_cache:
                self.file_cache[name] = self.capture.read(name)
            start, size = source.get('offset_bytes', 0), source.get('bytes')
            require(type(start) is int and type(size) is int and start >= 0 and size >= 0,
                    'invalid native payload offset/size')
            data = self.file_cache[name]
            require(start + size <= len(data), 'truncated native payload file: ' + name)
            return np.frombuffer(data, dtype='u1', count=size, offset=start)
        return np.empty(0, dtype='u1')

    def buffer(self, group):
        descriptor, index_node, offset_node, count, formatted, idxen = group.args
        require(len(descriptor) == 4, f'buffer load {group.pc:#x}: expected 128-bit descriptor')
        desc = [self.integer(x) for x in descriptor]
        address, stride = desc[0] | ((desc[1] & 0xffff) << 32), (desc[1] >> 16) & 0x3fff
        require(not (desc[1] & (1 << 31)) and not (desc[3] & (1 << 23)),
                f'buffer load {group.pc:#x}: swizzled/addtid descriptor unsupported')
        matching = [i for i, item in enumerate(self.descriptors) if list(item[:4]) == desc]
        require(matching, f'buffer load {group.pc:#x}: guest descriptor absent from consumed bound descriptors')
        index = np.asarray(self.eval(index_node), dtype=np.uint64)
        offset = np.asarray(self.eval(offset_node), dtype=np.uint64)
        require(offset.ndim == 0, f'buffer load {group.pc:#x}: vector byte offsets unsupported')
        fmt = (desc[3] >> 12) & 0x7f
        formats = {22: ('f32', 1), 64: ('f32', 2), 74: ('f32', 3), 77: ('f32', 4),
                   13: ('f16', 1), 29: ('f16', 2), 71: ('f16', 4),
                   50: ('unorm1010102', 4), 56: ('unorm8', 4)}
        require(not formatted or fmt in formats, f'buffer load {group.pc:#x}: unsupported format {fmt}')
        kind, channels = formats[fmt] if formatted else ('raw', count)
        size = channels * (2 if kind == 'f16' else 4)
        if kind in ('unorm1010102', 'unorm8'):
            size = 4
        at = index * stride + int(offset)
        extent = desc[2] * stride if stride else desc[2]
        require(np.all(at + size <= extent), f'buffer load {group.pc:#x}: descriptor OOB read; no invented zero fill')
        candidates = [s for s in self.sources if isinstance(s, dict) and s.get('slot') in matching
                      and s.get('address') == address]
        require(len(candidates) == 1, f'buffer load {group.pc:#x}: missing/ambiguous native snapshot for slots {matching}')
        source, raw = candidates[0], self.payload(candidates[0])
        require(np.all(at + size <= len(raw)),
                f'buffer load {group.pc:#x}: missing native GPU bytes in slot {source["slot"]}; '
                f'requires through byte {int(np.max(at)) + size}, captured {len(raw)}')
        offsets = at[..., None] + np.arange(size, dtype=np.uint64)
        selected = np.ascontiguousarray(raw[offsets.astype(np.int64)])
        if kind in ('f32', 'raw'):
            values = selected.view('<u4').reshape(at.shape + (channels,))
        elif kind == 'f16':
            values = selected.view('<f2').astype('<f4').view('<u4').reshape(at.shape + (channels,))
        else:
            packed = selected.view('<u4').reshape(at.shape)
            widths = [10, 10, 10, 2] if kind == 'unorm1010102' else [8] * 4
            shift, parts = 0, []
            for width in widths:
                maximum = (1 << width) - 1
                part = ((packed >> shift) & maximum).astype('<f4') / np.float32(maximum)
                parts.append(part.view('<u4'))
                shift += width
            values = np.stack(parts, axis=-1)
        if formatted:
            output = []
            for component in range(count):
                select = (desc[3] >> (component * 3)) & 7
                if select in (0, 1):
                    output.append(np.full(at.shape, 0x3f800000 if select else 0, dtype='<u4'))
                else:
                    require(4 <= select < 4 + channels, f'buffer load {group.pc:#x}: unsupported destination selection {select}')
                    output.append(values[..., select - 4])
            values = np.stack(output, axis=-1)
        self.loads.append({'pc': hex(group.pc), 'kind': 'formatted_buffer' if formatted else 'raw_buffer',
                           'slot': source['slot'], 'address': address, 'stride': stride, 'format': fmt if formatted else None,
                           'index_min': int(np.min(index)), 'index_max': int(np.max(index)),
                           'first_byte': int(np.min(at)), 'end_byte': int(np.max(at)) + size,
                           'payload_bytes_present': len(raw),
                           'read_bytes_sha256': hashlib.sha256(selected.tobytes()).hexdigest(),
                           'result_first_words': [int(x) for x in values.reshape(-1)[:16]]})
        return values

    def eval(self, value):
        if value in self.cache:
            return self.cache[value]
        op, args = value.op, value.args
        if op == 'const':
            result = np.asarray(args[0], dtype='<u4')
        elif op == 'unknown':
            raise Unsupported(f'{value.pc:#x}: {args[0]}')
        elif op == 'input':
            result = self.rows[:, 0 if args[0] == 'vertex' else 1]
        elif op == 'component':
            result = (self.scalar(args[0], args[1]) if args[0].op == 'scalar_load'
                      else self.eval(args[0])[..., args[1]])
        elif op == 'scalar_load':
            result = self.scalar(value)
        elif op == 'buffer_load':
            result = self.buffer(value)
        else:
            a = [np.asarray(self.eval(x), dtype='<u4') for x in args]
            if op == 'fneg':
                result = a[0] ^ np.uint32(0x80000000)
            elif op == 'fabs':
                result = a[0] & np.uint32(0x7fffffff)
            elif op in ('uadd', 'uand'):
                result = (a[0] + a[1]) if op == 'uadd' else (a[0] & a[1])
            else:
                f = [x.view('<f4') for x in a]
                with np.errstate(all='ignore'):
                    if op == 'fadd':
                        out = np.add(f[0], f[1], dtype=np.float32)
                    elif op == 'fsub':
                        out = np.subtract(f[0], f[1], dtype=np.float32)
                    elif op == 'fmul':
                        out = np.multiply(f[0], f[1], dtype=np.float32)
                    elif op == 'fsqrt':
                        out = np.sqrt(f[0], dtype=np.float32)
                    else:
                        raise Unsupported(f'{value.pc:#x}: unknown evaluator node {op}')
                result = np.asarray(out, dtype='<f4').view('<u4')
        self.cache[value] = result
        return result

    def position(self, program):
        return np.stack([np.broadcast_to(self.eval(x), (len(self.rows),)) for x in program.output], axis=-1)


def clip_codes(bits, dx_clip):
    f = bits.view('<f4').astype(np.float64)
    x, y, z, w = f.T
    planes = [w + x, w - x, w + y, w - y, z if dx_clip else w + z, w - z]
    result = np.zeros(len(f), dtype=np.uint8)
    for i, distance in enumerate(planes):
        result |= ((distance < 0).astype(np.uint8) << i)
    result |= ((~np.isfinite(f).all(axis=1)).astype(np.uint8) << 6)
    return result


def compare_positions(expected, observed, rows, dx_clip=True, max_ulp=0):
    require(type(max_ulp) is int and 0 <= max_ulp <= 4096, 'invalid ULP analysis threshold')
    e, o = expected.view('<f4'), observed.view('<f4')
    both_nan = np.isnan(e) & np.isnan(o)
    numeric_equal = (e == o) | both_nan
    finite = np.isfinite(e) & np.isfinite(o)
    ordered = lambda b: np.where(b & np.uint32(0x80000000), ~b, b ^ np.uint32(0x80000000)).astype(np.int64)
    ulp = np.abs(ordered(expected) - ordered(observed))
    match = numeric_equal | (finite & (ulp <= max_ulp))
    error = np.abs(e.astype(np.float64) - o.astype(np.float64))
    differences = np.argwhere(~match)
    examples = []
    for row, component in differences[:16]:
        examples.append({'vertex': int(rows[row, 0]), 'instance': int(rows[row, 1]),
                         'component': 'xyzw'[component], 'expected_bits': f'{int(expected[row, component]):08x}',
                         'observed_bits': f'{int(observed[row, component]):08x}',
                         'expected': str(float(e[row, component])), 'observed': str(float(o[row, component])),
                         'ulp_distance': int(ulp[row, component]) if finite[row, component] else None})
    ec, oc = clip_codes(expected, dx_clip), clip_codes(observed, dx_clip)
    return {
        'status': 'POSITION_MATCH' if match.all() else 'POSITION_DIVERGENCE',
        'vertices': len(rows), 'components': int(match.size), 'different_components': int((~match).sum()),
        'different_vertices': int((~match).any(axis=1).sum()),
        'bitwise_equal_components': int((expected == observed).sum()),
        'numeric_equal_components': int(numeric_equal.sum()), 'matching_nan_components': int(both_nan.sum()),
        'nan_payload_differences': int((both_nan & (expected != observed)).sum()),
        'maximum_finite_absolute_error': float(error[finite].max()) if finite.any() else None,
        'maximum_finite_ulp_distance': int(ulp[finite].max()) if finite.any() else None,
        'analysis_ulp_threshold': max_ulp, 'mismatch_examples': examples,
        'clip': {'convention': 'DX zero-to-one' if dx_clip else 'negative-one-to-one',
                 'plane_mask_different_vertices': int((ec != oc).sum()),
                 'expected_nonfinite_vertices': int(((ec & 64) != 0).sum()),
                 'observed_nonfinite_vertices': int(((oc & 64) != 0).sum()),
                 'expected_outside_any_plane': int(((ec & 63) != 0).sum()),
                 'observed_outside_any_plane': int(((oc & 63) != 0).sum()),
                 'scope': 'homogeneous clip predicates; not raster coverage or expected visibility'},
    }


def position_rows(record):
    columns = record.get('columns')
    require(isinstance(columns, list) and columns[:8] == ['vertex', 'instance', 'transform', 'clip_x', 'clip_y',
                                                        'clip_z', 'clip_w', 'clip_distance'], 'unexpected position row schema')
    values = record.get('rows')
    require(isinstance(values, list) and 0 < len(values) <= 1048576, 'missing/invalid native position rows')
    require(all(isinstance(x, list) and len(x) == len(columns) for x in values), 'truncated native position row')
    rows = np.stack([words(x, 'native row') for x in values])
    require(len({(int(x[0]), int(x[1])) for x in rows}) == len(rows), 'duplicate native invocation rows')
    return rows


def draw_identity(record):
    return {'draw': record.get('journal_draw', record.get('draw')), 'frame': record.get('frame'),
            'submit': record.get('submit'), 'shader': record.get('shader', record.get('vs_hash')),
            'native_vs_file': record.get('native_vs_file'), 'original_fingerprint': record.get('fingerprint'),
            'base_vertex': record.get('base_vertex'), 'first_instance': record.get('first_instance')}


def invocation_association(record, rows, expected, observed):
    """Indices establish invocation identity; they never determine position."""
    indices = record.get('index_words', record.get('indices'))
    if indices is None:
        return {'status': 'INDEX_ASSOCIATION_UNAVAILABLE', 'reason': 'no captured index stream'}
    indices = words(indices, 'captured index words')
    indexed = record.get('indexed', True)
    base = record.get('base_vertex', 0) if indexed else record.get('first_vertex', 0)
    require(type(base) is int, 'invalid base_vertex')
    effective = (indices.astype(np.int64) + base) & 0xffffffff
    wanted_vertices = {int(v) for v in effective}
    instances = {int(v) for v in rows[:, 1]}
    row_map = {(int(r[0]), int(r[1])): i for i, r in enumerate(rows)}
    require(all(int(r[0]) in wanted_vertices for r in rows), 'native row vertex absent from captured index stream + base_vertex')
    first, count = record.get('first_instance', 0), record.get('instances')
    if type(count) is int and count >= 0:
        instances = set(range(first, first + count))
        require(all(int(r[1]) in instances for r in rows), 'native row instance outside original draw range')
    missing = [(v, i) for i in sorted(instances) for v in sorted(wanted_vertices) if (v, i) not in row_map]
    result = {'status': 'CAPTURED_INDEX_ASSOCIATION_MATCH', 'raw_indices': len(indices),
              'indexed': indexed, 'base_vertex_or_first_vertex_applied_once': base, 'effective_instances': sorted(instances),
              'unique_index_vertices': len(wanted_vertices), 'missing_position_invocations': len(missing),
              'full_instance_range_captured': type(count) is int}
    if missing or len(indices) % 3:
        result['clip_primitive_status'] = 'INCOMPLETE_ROWS_OR_NON_TRIANGLE_LIST'
        return result
    ec, oc = clip_codes(expected, record.get('dx_clip_space', True)), clip_codes(observed, record.get('dx_clip_space', True))
    counts = {'inside_all_planes': 0, 'rejected_by_shared_plane': 0, 'requires_polygon_clipping': 0, 'nonfinite': 0}
    changed = 0
    for instance in sorted(instances):
        at = np.asarray([row_map[(int(v), instance)] for v in effective], dtype=np.int64).reshape(-1, 3)
        a, b = ec[at], oc[at]
        changed += int((a != b).any(axis=1).sum())
        nonfinite = ((a & 64) != 0).any(axis=1)
        rejected = ((a[:, 0] & a[:, 1] & a[:, 2] & 63) != 0) & ~nonfinite
        inside = ((a & 63) == 0).all(axis=1) & ~nonfinite
        counts['nonfinite'] += int(nonfinite.sum())
        counts['rejected_by_shared_plane'] += int(rejected.sum())
        counts['inside_all_planes'] += int(inside.sum())
        counts['requires_polygon_clipping'] += int((~nonfinite & ~rejected & ~inside).sum())
    result['cpu_clip'] = {'expected_triangle_classes': counts, 'triangles_with_different_vertex_clip_codes': changed,
                          'scope': 'homogeneous clip predicates of indexed triangle list; intersecting polygons not asserted visible'}
    return result


def sky_raster_from_expected(capture, record, rows, expected):
    """Existing positive-control raster, now fed by guest-derived positions."""
    alias, metadata = 'queue-state/ds_prelight_witness_sky_prepass.json', 'queue-state/ds_prelight_witness_depth.json'
    if alias not in capture.names or metadata not in capture.names:
        return None
    try:
        saved = capture.json(alias)
        if record.get('journal_draw') != saved.get('journal_draw') or record.get('shader') != saved.get('shader'):
            return None
        from prelight_sky_replay import raster, read_depth
        meta = capture.json(metadata)
        require(meta.get('pair') == record.get('pair_id') and meta.get('frame') == record.get('frame')
                and meta.get('same_pair_serial') and meta.get('pre_before_saved') and meta.get('pre_after_saved'),
                'sky depth snapshots are not paired to this draw')
        width, height = meta['pre_width'], meta['pre_height']
        require(type(width) is int and type(height) is int and width > 0 and height > 0
                and width * height <= 16777216, 'unsupported sky depth extent')
        require(all(int(r[1]) == 0 for r in rows) and record.get('base_vertex') == 0, 'unsupported sky instance/base')
        pos = {int(r[0]): expected[i].view('<f4').astype(np.float64) for i, r in enumerate(rows)}
        require(all(np.isfinite(v).all() for v in pos.values()), 'nonfinite guest position in raster control')
        before, before_sha = read_depth(capture, 'ds_prelight_witness_depth_pre_before.bin', width, height)
        after, after_sha = read_depth(capture, 'ds_prelight_witness_depth_pre_after.bin', width, height)
        covered, incoming, triangles = raster(record, pos, width, height)
        predicted = np.where(covered, np.maximum(before, incoming), before)
        changed = covered & (incoming >= before) & (incoming != before)
        observed = before.view('<u4') != after.view('<u4')
        error = np.abs(predicted - after)
        mismatch = int((changed != observed).sum())
        return {'status': 'LOCAL_RASTER_MEASUREMENT_CONSISTENT' if mismatch == 0 and np.max(error) <= 1e-8
                else 'LOCAL_RASTER_MISMATCH', 'position_source': 'independent guest ISA and captured operands',
                'covered_pixels': int(covered.sum()), 'clipped_triangles': triangles,
                'expected_depth_changed': int(changed.sum()), 'observed_depth_changed': int(observed.sum()),
                'changed_mask_mismatches': mismatch, 'maximum_absolute_depth_error': float(np.max(error)),
                'analysis_depth_threshold': 1e-8, 'before_sha256': before_sha, 'after_sha256': after_sha,
                'hardware_raster_contract_certified': False,
                'scope': 'same float64 center-sample, inclusive-edge, no-cull control as prelight_sky_replay; no hardware precision or visibility guarantee'}
    except (ValueError, KeyError, TypeError, OSError) as error:
        return {'status': 'UNSUPPORTED_LOCAL_RASTER', 'reason': str(error)}


def replay_draw(capture, record, decoder, require_regions=False, max_ulp=0):
    report = {'identity': draw_identity(record) if isinstance(record, dict) else {}, 'status': 'UNSUPPORTED_DRAW',
              'scope': 'guest fetch and binary32 arithmetic to captured POS0 for already-admitted invocations',
              'p8_certificate': False}
    evaluator = None
    try:
        require(isinstance(record, dict), 'position record must be an object')
        shader = record.get('shader', record.get('vs_hash'))
        require(isinstance(shader, str) and re.fullmatch('[0-9a-f]{16}', shader), 'invalid guest VS identity')
        profile = PROFILES.get(shader, {'flat': {}, 'descriptors': {}})
        guest_names = [n for n in capture.names if PurePosixPath(n).name in
                       (f'vs_{shader}_probe.bin', f'vs_{shader}.bin')]
        require(len(guest_names) == 1, 'missing/ambiguous guest instruction bytes for ' + shader)
        guest = capture.read(guest_names[0])
        digest = hashlib.sha256(guest).hexdigest()
        from prelight_sky_replay import xxh3
        require(xxh3(guest) == shader, 'guest bytes do not match the recorded shader hash')
        require(guest[:0x40] == NGG_PREFIX, 'unsupported NGG admission prologue / initial register convention')
        if 'sha256' in profile:
            require(digest == profile['sha256'], 'guest bytes differ from the supported full fingerprint')
        report['guest'] = {'file': guest_names[0], 'sha256': digest, 'decoder': decoder.library, 'target': 'gfx1010'}
        # The independent guest reference never consumes native module arithmetic.
        # Verify the recorded bound module's filename/content identity if present.
        module_file = record.get('native_vs_file')
        report['native_module_identity_verified'] = False
        if module_file:
            from prelight_sky_replay import module
            report['native_module'] = module(capture, module_file, 'vs', shader)[0]
            report['native_module_identity_verified'] = True
        rows = position_rows(record)
        user_data = words(record.get('vs_user_data', record.get('user_data')), 'VS user data')
        require(len(user_data) >= 2, 'missing initial S8:S9 user data')
        program = Program(decoder.decode(guest), profile, user_data, record.get('vs_user_data_base', 8))
        evaluator = Evaluator(capture, record, profile, rows, require_regions)
        expected = evaluator.position(program)
        result = compare_positions(expected, rows[:, 3:7].copy(), rows, record.get('dx_clip_space', True), max_ulp)
        report.update(result)
        report['invocation_association'] = invocation_association(record, rows, expected, rows[:, 3:7].copy())
        local_raster = sky_raster_from_expected(capture, record, rows, expected)
        if local_raster is not None:
            report['raster'] = local_raster
        report['export_pc'] = hex(program.export_pc)
        report['expected_position_sha256'] = hashlib.sha256(expected.astype('<u4').tobytes()).hexdigest()
        report['observed_position_sha256'] = hashlib.sha256(rows[:, 3:7].astype('<u4').tobytes()).hexdigest()
        report['captured_missing_rows'] = record.get('missing')
        report['all_invocations_certified'] = False
        report['input_scalar_capture_complete'] = record.get('vs_scalar_capture_complete')
        report['captured_float_controls'] = {k: v for k, v in record.items()
                                             if k.startswith('vs_') and any(x in k for x in ('float_mode', 'ieee', 'dx10'))}
        report['position_samples'] = [{'vertex': int(rows[i, 0]), 'instance': int(rows[i, 1]),
                                       'expected_bits': [int(x) for x in expected[i]],
                                       'observed_bits': [int(x) for x in rows[i, 3:7]]}
                                      for i in range(min(8, len(rows)))]
        if result['mismatch_examples']:
            report['first_local_divergence'] = {'edge': 'guest loads/arithmetic -> native POS0',
                                               'guest_export_pc': hex(program.export_pc),
                                               **result['mismatch_examples'][0],
                                               'earliest_instruction_proven': False}
        report['reason'] = ('Captured invocations agree with the stated numeric model; follow input producers on this edge.'
                            if result['status'] == 'POSITION_MATCH' else
                            'Same captured operands produce different POS0 under the stated numeric model; inspect mismatch magnitude and rounding/FTZ before claiming a semantic fix.')
    except (Unsupported, OSError, KeyError, TypeError, ValueError, struct.error, zipfile.BadZipFile) as error:
        report['status'], report['reason'] = 'UNSUPPORTED_DRAW', str(error)
    if evaluator is not None:
        report['scalar_binding_mode'] = evaluator.scalar_mode
        report['consumed_loads_on_position_slice'] = sorted(evaluator.loads, key=lambda x: int(x['pc'], 16))
    return report


def merge_inputs(capture, record):
    """Join only a unique same-draw/frame/submit input record, never nearest draw."""
    draw = record.get('journal_draw', record.get('draw'))
    candidates = [n for n in capture.names if PurePosixPath(n).name in
                  (f'ds_prelight_witness_foreground_{draw}.json', f'ds_prelight_witness_inputs_{draw}.json')]
    if not candidates:
        return record
    require(len(candidates) == 1, 'ambiguous draw-input sidecar')
    source = capture.json(candidates[0])
    require(isinstance(source, dict) and source.get('draw') == draw, 'draw-input sidecar identity mismatch')
    for key in ('frame', 'submit'):
        require(source.get(key) == record.get(key), 'draw-input sidecar ' + key + ' mismatch')
    require(source.get('vs_hash') == record.get('shader'), 'draw-input sidecar VS hash mismatch')
    for key, old in (('vs_user_data', 'user_data'), ('vs_srt', 'srt'), ('vs_buffers', 'descriptors')):
        if key in record and key in source:
            require(source[key] == record[key], 'draw-input sidecar ' + key + ' mismatch')
        if old in record and key in source:
            require(source[key] == record[old], 'draw-input sidecar ' + key + ' mismatch')
    for key in ('base_vertex', 'first_instance', 'first_vertex', 'indexed', 'instances'):
        if key in record and key in source:
            require(source[key] == record[key], 'draw-input sidecar ' + key + ' mismatch')
    if 'indices' in record and 'index_words' in source:
        require(record['indices'] == source['index_words'], 'draw-input sidecar index stream mismatch')
    fields = {'base_vertex', 'first_instance', 'first_vertex', 'indexed', 'instances', 'index_words',
              'native_index_count', 'index_words_synthetic'}
    return {**record, **{k: v for k, v in source.items() if k.startswith('vs_') or k in fields}}


def run(path, record_pattern=None, require_regions=False, max_ulp=0, llvm_library=None):
    report = {'schema': 'demons-independent-vertex-oracle-v1', 'tree_effect': 'LOCAL_EDGE_RESULTS_ONLY',
              'p8_certificate': False, 'draws': [],
              'numeric_model': 'IEEE binary32 RNE; separate multiply/add; preserved subnormals; NaN class and signed-zero numeric equality',
              'excluded': ['NGG/EXEC invocation admission', 'upstream production/version correctness',
                           'complete VS/PS or G-buffer equivalence', 'hardware rounding/FTZ conformance',
                           'expected visibility and final image']}
    capture, decoder = None, None
    try:
        capture, decoder = Capture(path), LLVM(llvm_library)
        names = [n for n in capture.names if n.endswith('.json') and
                 (PurePosixPath(n).name.startswith(('ds_vertex_position_', 'ds_prelight_witness_vertex_'))
                  or PurePosixPath(n).name in ('ds_prelight_witness_sky_prepass.json', 'ds_prelight_witness_sky_material.json'))]
        if record_pattern:
            names = [n for n in names if fnmatch.fnmatch(n, record_pattern) or fnmatch.fnmatch(PurePosixPath(n).name, record_pattern)]
        require(names, 'no selected native position records')
        general = {}
        # The exporter retains sky aliases for backwards compatibility. A new
        # per-draw record is authoritative for this audit and is counted once.
        ordered = sorted(names, key=lambda n: (not PurePosixPath(n).name.startswith('ds_prelight_witness_vertex_'), n))
        for name in ordered:
            try:
                record = capture.json(name)
                require(isinstance(record, dict), 'position record must be an object')
                draw = record.get('journal_draw', record.get('draw'))
                if PurePosixPath(name).name.startswith('ds_prelight_witness_sky_') and draw in general:
                    previous = general[draw]
                    require(draw_identity(record) == previous['identity'], 'legacy sky alias has conflicting draw identity')
                    previous.setdefault('legacy_record_aliases', []).append(name)
                    continue
                result = replay_draw(capture, merge_inputs(capture, record), decoder, require_regions, max_ulp)
            except (Unsupported, KeyError, ValueError, TypeError, OSError) as error:
                result = {'status': 'UNSUPPORTED_DRAW', 'reason': str(error), 'p8_certificate': False}
            complete = {'record': name, **result}
            report['draws'].append(complete)
            if PurePosixPath(name).name.startswith('ds_prelight_witness_vertex_') and complete.get('identity', {}).get('draw') is not None:
                general[complete['identity']['draw']] = complete
    except (Unsupported, OSError, ValueError, zipfile.BadZipFile) as error:
        report['capture_error'] = str(error)
    finally:
        if capture:
            capture.close()
        if decoder:
            decoder.close()
    report['counts'] = {name: sum(x['status'] == name for x in report['draws'])
                        for name in ('POSITION_MATCH', 'POSITION_DIVERGENCE', 'UNSUPPORTED_DRAW')}
    divergences = [x for x in report['draws'] if x.get('first_local_divergence')]
    if divergences:
        first = min(divergences, key=lambda x: x.get('identity', {}).get('draw') or 0)
        report['first_local_divergence'] = {'identity': first['identity'], **first['first_local_divergence']}
    report['analysis_state'] = ('LOCAL_DIVERGENCE' if divergences else
                                'PARTIAL_WITH_GAPS' if report.get('capture_error') or report['counts']['UNSUPPORTED_DRAW']
                                else 'SELECTED_POSITION_EDGES_MATCH')
    return report


def audit_capture(path: Path) -> dict:
    """Importable gate integration: recompute evidence; never accept an external PASS."""
    return run(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('capture')
    parser.add_argument('--record', help='Position-record basename/path glob')
    parser.add_argument('--require-scalar-regions', action='store_true')
    parser.add_argument('--max-ulp', type=int, default=0, help='Analysis threshold only; default requires exact numeric equality')
    parser.add_argument('--llvm-library')
    parser.add_argument('--output')
    args = parser.parse_args()
    result = run(args.capture, args.record, args.require_scalar_regions, args.max_ulp, args.llvm_library)
    output = json.dumps(result, indent=2, allow_nan=False) + '\n'
    if args.output:
        Path(args.output).write_text(output)
    sys.stdout.write(output)
    if result['counts']['POSITION_DIVERGENCE']:
        return 3
    return 2 if result.get('capture_error') or result['counts']['UNSUPPORTED_DRAW'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
