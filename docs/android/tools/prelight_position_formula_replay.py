#!/usr/bin/env python3
"""Fingerprint-bound symbolic check of one Demon's Souls vertex-position slice.

This is NOT an integral ISA interpreter or a P8 oracle. It compares the ordered
arithmetic expression producing XYZW in guest VS 868546d85ce5090a with the exact
captured SPIR-V variant 7843cbd43b48179b. Loads already delivered to registers
are symbols, and the lane is assumed active. Fetch/addressing, EXEC admission,
floating-point implementation/rounding, rasterization and visibility are outside
this certificate. Every result leaves the global tree unchanged.

The guest instruction encodings were independently decoded with LLVM 19's
gfx1010 disassembler. gfx1030 incorrectly leaves V_MAD_F32/V_MAC_F32 undecoded
for this binary. The small decoder below reads the pinned guest bytes directly;
it never invokes Kyty's decoder, IR or emitter. The SPIR-V reader is standalone.
The load-to-register correspondence is an explicit premise, NOT a fetch proof.

Usage:
  python3 prelight_position_formula_replay.py --capture diagnostics.zip
  python3 prelight_position_formula_replay.py --capture extracted-directory
  python3 prelight_position_formula_replay.py --guest vs.bin --spv vs.spv
  python3 prelight_position_formula_replay.py --capture diagnostics.zip --self-test

Unknown fingerprints, instructions on the output slice, or missing files fail
closed. --self-test mutates an actual SPIR-V OpFMul into OpFAdd in memory and
checks that the local formula comparison detects it; no input file is modified.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zipfile
from pathlib import Path


GUEST_NAME = "vs_868546d85ce5090a_probe.bin"
SPV_NAME = "vs_868546d85ce5090a_7843cbd43b48179b.spv"
GUEST_SHA256 = "a7988747383220232e3b94e254101cffe9c4a1c0d87676ff00f5bc5b0390cb00"
SPV_SHA256 = "92cecc85b88e1ff8171011480689eb067199265307ba41bfff214334d1ad2afe"


class Blocked(ValueError):
    """Evidence does not satisfy the deliberately narrow replay contract."""


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def node(op: str, *args):
    return (op, *args)


def guest_position(data: bytes):
    """Build the ordered position formula after the seven buffer-load sites.

    Registers represent the results of guest loads at 00a8..00d8. The inputs
    are unconstrained symbols, not recovered values or asserted valid data.
    """
    v = {i: f"loaded_v{i}" for i in range(64)}
    s = {i: f"loaded_s{i}" for i in range(64)}
    floats = {240: 0.5, 241: -0.5, 242: 1.0, 243: -1.0,
              244: 2.0, 245: -2.0, 246: 4.0, 247: -4.0}
    pc = 0xE4

    def source(encoded: int):
        if encoded >= 256:
            return v[encoded - 256]
        if encoded < 64:
            return s[encoded]
        if encoded in floats:
            return floats[encoded]
        if encoded == 128:
            return 0.0
        return node("unresolved_source", encoded, pc)

    while pc + 4 <= len(data):
        w = struct.unpack_from("<I", data, pc)[0]
        if w >> 26 == 0x3E:  # EXP
            if pc != 0x288 or (w & 15) != 15 or ((w >> 4) & 63) != 12:
                raise Blocked(f"Unexpected position export at {pc:#x}")
            packed = struct.unpack_from("<I", data, pc + 4)[0]
            return tuple(v[(packed >> (8 * i)) & 255] for i in range(4))
        if w >> 16 == 0xBF8C:  # S_WAITCNT: memory/timing not certified here
            pc += 4
            continue
        if w >> 25 == 0x3E:  # VOPC: writes condition codes, not position VGPRs
            pc += 8 if (w & 511) in (249, 255) else 4
            continue
        if w >> 26 == 0x35:  # VOP3
            w1 = struct.unpack_from("<I", data, pc + 4)[0]
            opcode, dst = (w >> 16) & 1023, w & 255
            raw = [w1 & 511, (w1 >> 9) & 511, (w1 >> 18) & 511]
            args = [source(x) for x in raw]
            absolute, negate = (w >> 8) & 7, (w1 >> 29) & 7
            for i in range(3):
                if (absolute >> i) & 1:
                    args[i] = node("abs", args[i])
                if (negate >> i) & 1:
                    args[i] = node("neg", args[i])
            modifiers = ((w >> 11) & 31) or ((w1 >> 27) & 3)
            if opcode == 0x141 and not modifiers:  # non-fused V_MAD_F32
                value = node("add", node("mul", *args[:2]), args[2])
            elif opcode == 0x11F and not modifiers:  # V_MAC_F32
                value = node("add", node("mul", *args[:2]), v[dst])
            else:
                value = node("unresolved_instruction", pc, opcode, *args)
            v[dst] = value
            pc += 8
            continue
        if w >> 25 == 0x3F:  # VOP1
            opcode, dst = (w >> 9) & 255, (w >> 17) & 255
            arg = source(w & 511)
            v[dst] = node("sqrt", arg) if opcode == 0x33 else node(
                "unresolved_instruction", pc, opcode, arg)
            pc += 4
            continue
        if w >> 31 == 0:  # VOP2
            opcode, dst = w >> 25, (w >> 17) & 255
            encoded = w & 511
            if encoded == 249:  # SDWA cndmask in this slice feeds parameters only
                v[dst] = node("unresolved_instruction", pc, opcode)
                pc += 8
                continue
            a, b = source(encoded), v[(w >> 9) & 255]
            if opcode == 0x1F:
                value = node("add", node("mul", a, b), v[dst])
            elif opcode in (3, 4, 8):
                value = node({3: "add", 4: "sub", 8: "mul"}[opcode], a, b)
            elif opcode == 5:
                value = node("sub", b, a)
            else:
                value = node("unresolved_instruction", pc, opcode, a, b)
            v[dst] = value
            pc += 8 if encoded == 255 else 4
            continue
        raise Blocked(f"Unsupported instruction boundary at {pc:#x}: {w:#010x}")
    raise Blocked("Position export was not reached")


def parse_spirv(data: bytes):
    if len(data) % 4 or len(data) < 20:
        raise Blocked("Malformed SPIR-V size")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if words[0] != 0x07230203:
        raise Blocked("Invalid SPIR-V magic")
    definitions = {}
    offsets = {}
    # Only result-producing opcodes needed on this pinned formula's backward slice.
    typed_result_ops = {12, 43, 61, 80, 124, 127, 129, 131, 133, 169}
    i = 5
    while i < len(words):
        count, opcode = words[i] >> 16, words[i] & 65535
        if count < 1 or i + count > len(words):
            raise Blocked(f"Invalid SPIR-V instruction at word {i}")
        args = words[i + 1:i + count]
        if opcode in typed_result_ops:
            if len(args) < 2:
                raise Blocked("Truncated SPIR-V result")
            definitions[args[1]] = (opcode, args)
            offsets[args[1]] = i
        i += count
    return definitions, offsets


def spv_position(data: bytes):
    instructions, offsets = parse_spirv(data)
    # Conditional load results in the exact pinned SPIR-V. Explicit equal-load
    # premise only: this does NOT certify pointers, descriptors, versions or data.
    seeds = {}
    load_groups = [
        ([237, 239, 241, 243], range(14, 18)),
        ([284, 286, 288, 290], range(0, 4)),
        ([330, 332, 334, 336], range(6, 10)),
        ([376, 378, 380, 382], range(10, 14)),
        ([418, 420, 422], range(18, 21)),
        ([465, 467], range(21, 23)),
        ([502, 504], range(4, 6)),
    ]
    for ids, registers in load_groups:
        seeds.update((i, f"loaded_v{r}") for i, r in zip(ids, registers))
    scalar_ids = [121, 124, 127, 130, 133, 135, 137, 140, 143, 146, 149,
                  152, 155, 158, 161, 164, 167, 170, 173, 176, 179]
    scalar_regs = list(range(16)) + [16, 17, 40, 41, 18]
    seeds.update((i, f"loaded_s{r}") for i, r in zip(scalar_ids, scalar_regs))
    cache = dict(seeds)
    visited = set()

    def visit(i):
        if i in cache:
            return cache[i]
        if i in visited or i not in instructions:
            raise Blocked(f"Unresolved SPIR-V value %{i}")
        visited.add(i)
        opcode, a = instructions[i]
        if opcode == 43:  # OpConstant: only f32 constants reach this formula
            value = struct.unpack("<f", struct.pack("<I", a[2]))[0]
        elif opcode == 124:  # OpBitcast; preserving raw-register interpretation
            value = visit(a[2])
        elif opcode == 169 and a[2] == 114:  # already-admitted active lane premise
            value = visit(a[3])
        elif opcode in (129, 131, 133):
            value = node({129: "add", 131: "sub", 133: "mul"}[opcode],
                         visit(a[2]), visit(a[3]))
        elif opcode == 127:
            value = node("neg", visit(a[2]))
        elif opcode == 80:
            value = tuple(visit(x) for x in a[2:])
        elif opcode == 12 and a[3] == 31:  # GLSL.std.450 Sqrt
            value = node("sqrt", visit(a[4]))
        else:
            raise Blocked(f"Unsupported SPIR-V operation {opcode} at %{i}")
        cache[i] = value
        visited.remove(i)
        return value

    return visit(967), offsets


def has_unresolved(value) -> bool:
    if isinstance(value, tuple):
        if value and isinstance(value[0], str) and value[0].startswith("unresolved"):
            return True
        return any(has_unresolved(x) for x in value)
    return False


def compare_formula(guest: bytes, spv: bytes):
    reference = guest_position(guest)
    observed, offsets = spv_position(spv)
    if has_unresolved(reference) or has_unresolved(observed):
        raise Blocked("Unsupported expression reaches the position output")
    components = [a == b for a, b in zip(reference, observed)]
    return components, offsets


def load_pair(args):
    if args.capture:
        path = Path(args.capture)
        if path.is_dir():
            def read(name):
                matches = sorted(path.rglob(name))
                if len(matches) != 1:
                    raise Blocked(f"Expected one {name}; found {len(matches)}")
                return matches[0].read_bytes(), str(matches[0])
            return read(GUEST_NAME), read(SPV_NAME)
        with zipfile.ZipFile(path) as archive:
            def read(name):
                matches = [x for x in archive.namelist() if Path(x).name == name]
                if len(matches) != 1:
                    raise Blocked(f"Expected one {name}; found {len(matches)}")
                return archive.read(matches[0]), f"{path}!{matches[0]}"
            return read(GUEST_NAME), read(SPV_NAME)
    if not args.guest or not args.spv:
        raise Blocked("Supply --capture or both --guest and --spv")
    return (Path(args.guest).read_bytes(), args.guest), (Path(args.spv).read_bytes(), args.spv)


def run(args):
    report = {
        "schema": "demons-position-formula-v1",
        "guest_hash": "868546d85ce5090a",
        "native_variant": "7843cbd43b48179b",
        "guest_export_pc": "0x288",
        "native_position_id": 967,
        "local_formula_verdict": "BLOCKED",
        "tree_effect": "UNCHANGED",
        "p8_certificate": False,
        "p8_verdict": "BLOCKED",
        "scope": "Ordered XYZW arithmetic with identical already-loaded symbolic operands and an active lane.",
        "excluded": ["fetch/address/descriptor/version correctness", "EXEC admission",
                     "ISA-wide equivalence", "floating-point rounding/FTZ/hardware execution",
                     "raster/depth/material/G-buffer", "expected visibility or final image"],
    }
    try:
        (guest, guest_source), (spv, spv_source) = load_pair(args)
        report["inputs"] = {
            "guest": {"source": guest_source, "bytes": len(guest), "sha256": digest(guest)},
            "spv": {"source": spv_source, "bytes": len(spv), "sha256": digest(spv)},
        }
        if digest(guest) != GUEST_SHA256 or digest(spv) != SPV_SHA256:
            raise Blocked("Unsupported fingerprint; pinned load/result mapping is not applicable")
        components, offsets = compare_formula(guest, spv)
        report["xyzw_formula_equal"] = components
        report["local_formula_verdict"] = "PASS" if all(components) else "FAIL"
        report["reason"] = "Local symbolic formula only; no global prefix is certified."
        if args.self_test:
            changed = bytearray(spv)
            word_offset = offsets[781]  # first multiplication feeding exported X
            original = struct.unpack_from("<I", changed, word_offset * 4)[0]
            if (original & 65535) != 133:
                raise Blocked("Mutation anchor is not the expected OpFMul")
            struct.pack_into("<I", changed, word_offset * 4, (original & 0xFFFF0000) | 129)
            mutated, _ = compare_formula(guest, bytes(changed))
            if mutated == [True, True, True, True] or digest(changed) == SPV_SHA256:
                raise Blocked("Negative control failed to detect a changed arithmetic operation")
            report["negative_control"] = {
                "mutation": "SPIR-V %781 OpFMul -> OpFAdd, in memory only",
                "mutated_sha256": digest(changed),
                "xyzw_formula_equal": mutated,
                "local_formula_verdict": "FAIL",
                "normal_fingerprint_gate": "BLOCKED",
                "tree_effect": "UNCHANGED",
                "p8_certificate": False,
            }
    except (Blocked, OSError, zipfile.BadZipFile, KeyError, struct.error) as error:
        report["local_formula_verdict"] = "BLOCKED"
        report["reason"] = str(error)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--capture", help="Original diagnostic ZIP or extracted directory")
    parser.add_argument("--guest", help="Exact raw guest VS binary")
    parser.add_argument("--spv", help="Exact raw native SPIR-V binary")
    parser.add_argument("--self-test", action="store_true", help="Also run a real-byte arithmetic negative control")
    parser.add_argument("--output", help="Write the same JSON report to this path")
    args = parser.parse_args()
    if args.capture and (args.guest or args.spv):
        parser.error("--capture cannot be combined with --guest/--spv")
    report = run(args)
    result = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        Path(args.output).write_text(result)
    sys.stdout.write(result)
    return 0 if report["local_formula_verdict"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
