#!/usr/bin/env python3
"""Evidence audit for Demon's Souls, with scoped executable decisions.

Usage:
  python docs/android/tools/prelight_v3_gate.py diagnostics.zip --output audit.json
  python docs/android/tools/prelight_v3_gate.py extracted-diagnostics

Exit status is 0 after a valid audit and 3 for unsafe/unreadable input. An unknown
global P8 result is a statement about evidence, not a prohibition on acquiring
the operands of a concrete next operation. This tool does not grant or withhold
permission to build. The observed color loss selects a window; it is not a FAIL.

Local decisions come from executable guest-ISA replay, with the same input bytes
and instruction provenance. Merely supplying expected pixels or certification
booleans is unsupported. Native re-execution of the same SPIR-V is not an
independent guest-ISA oracle. Unsupported operations remain explicit per draw;
they do not erase results from supported operations or demand a full PC capture.

PASS requires closure of the relevant cumulative prefix. FAIL may instead use a
sufficient causal counterexample with its own complete dependency path; it need
not prove unrelated operations correct. Missing inventory data never constitutes
that counterexample. Local position matches do not imply a global P8 PASS.

Archives are inspected without extraction. Traversal, duplicate names, symlinks,
ambiguous capture roots and excessive uncompressed sizes are rejected. Both ZIPs
and directories use the same reader and integrity checks.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import ctypes
import ctypes.util
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import sys
from typing import Any
import zipfile

MAX_MEMBER_BYTES = 128 * 1024 * 1024
MAX_TOTAL_BYTES = 1024 * 1024 * 1024
EVENT_NAME = re.compile(r"queue-state/ds_prelight_events_(\d+)\.jsonl$")
MODULE_NAME = re.compile(r"spv/(vs|ps|cs|ls|hs|ds|gs|ms)_([0-9a-f]{16})_([0-9a-f]{16})\.spv$")
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1


class CaptureError(ValueError):
    """Untrusted or malformed capture input; never a rendering verdict."""


def safe_name(name: str) -> str:
    if not isinstance(name, str) or not name or "\\" in name or "\0" in name:
        raise CaptureError(f"invalid capture path: {name!r}")
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in name.split("/") or ":" in name.split("/", 1)[0]:
        raise CaptureError(f"unsafe capture path: {name!r}")
    normalized = path.as_posix()
    if normalized in ("", "."):
        raise CaptureError(f"invalid capture path: {name!r}")
    return normalized


class Capture:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        self.archive: zipfile.ZipFile | None = None
        self.files: dict[str, Any] = {}
        self.inspected: dict[str, dict[str, Any]] = {}
        total = 0
        if self.path.is_dir():
            self.root = self.path.resolve()
            for file in sorted(self.path.rglob("*")):
                if file.is_symlink():
                    raise CaptureError(f"symlink is not a capture artifact: {file}")
                if not file.is_file():
                    continue
                resolved = file.resolve()
                if not resolved.is_relative_to(self.root):
                    raise CaptureError(f"artifact escapes directory: {file}")
                name = safe_name(file.relative_to(self.path).as_posix())
                size = file.stat().st_size
                self.files[name] = file
                total += size
                self._check_sizes(size, total)
        elif self.path.is_file() and zipfile.is_zipfile(self.path):
            self.archive = zipfile.ZipFile(self.path)
            names_seen: set[str] = set()
            for item in self.archive.infolist():
                name = safe_name(item.filename)
                if name in names_seen:
                    raise CaptureError(f"duplicate archive entry: {name}")
                names_seen.add(name)
                if stat.S_ISLNK(item.external_attr >> 16):
                    raise CaptureError(f"archive symlink is not allowed: {name}")
                if item.is_dir():
                    continue
                if item.flag_bits & 1:
                    raise CaptureError(f"encrypted capture entry: {name}")
                total += item.file_size
                self._check_sizes(item.file_size, total)
                self.files[name] = item
        else:
            raise CaptureError(f"not a diagnostic ZIP or directory: {self.path}")
        roots = {name.removesuffix("queue-state/ds_prelight_status.json")
                 for name in self.files if name.endswith("queue-state/ds_prelight_status.json")}
        if not roots:
            roots = {name[:name.index("queue-state/")]
                     for name in self.files
                     if re.search(r"(^|/)queue-state/ds_prelight_events_\d+\.jsonl$", name)}
        if len(roots) != 1:
            raise CaptureError(f"expected one capture root, found {len(roots)}")
        self.prefix = roots.pop()
        self.names = {name[len(self.prefix):] for name in self.files if name.startswith(self.prefix)}

    @staticmethod
    def _check_sizes(size: int, total: int) -> None:
        if size > MAX_MEMBER_BYTES or total > MAX_TOTAL_BYTES:
            raise CaptureError("capture exceeds the bounded inspection size")

    def exists(self, relative: str) -> bool:
        return safe_name(relative) in self.names

    def read(self, relative: str) -> bytes:
        relative = safe_name(relative)
        full = self.prefix + relative
        item = self.files.get(full)
        if item is None:
            raise CaptureError(f"missing artifact: {relative}")
        try:
            if self.archive:
                data = self.archive.read(item)  # ZipFile verifies CRC; no extraction.
            else:
                resolved = item.resolve()
                if item.is_symlink() or not resolved.is_relative_to(self.root):
                    raise CaptureError(f"artifact escapes directory: {relative}")
                data = item.read_bytes()
        except (OSError, zipfile.BadZipFile, RuntimeError) as error:
            raise CaptureError(f"cannot read {relative}: {error}") from error
        if len(data) > MAX_MEMBER_BYTES:
            raise CaptureError(f"artifact grew beyond the inspection bound: {relative}")
        self.inspected[relative] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        return data

    def json(self, relative: str) -> Any:
        try:
            return json.loads(self.read(relative))
        except (ValueError, UnicodeError) as error:
            raise CaptureError(f"invalid JSON in {relative}: {error}") from error

    def close(self) -> None:
        if self.archive:
            self.archive.close()

    def __enter__(self) -> "Capture":
        return self

    def __exit__(self, *_: Any) -> None:
        self.close()


def xxh3(data: bytes) -> str | None:
    """Match the repository's content fingerprints without guessing their value."""
    if not hasattr(xxh3, "library"):
        library = None
        for candidate in (ctypes.util.find_library("xxhash"), "libxxhash.so.0", "libxxhash.dylib"):
            if not candidate:
                continue
            try:
                library = ctypes.CDLL(candidate)
                library.XXH3_64bits.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
                library.XXH3_64bits.restype = ctypes.c_uint64
                break
            except (OSError, AttributeError):
                library = None
        xxh3.library = library
    if xxh3.library is None:
        return None
    return f"{xxh3.library.XXH3_64bits(data, len(data)):016x}"


def words_valid(words: Any) -> bool:
    return isinstance(words, list) and all(type(word) is int and 0 <= word <= UINT32_MAX for word in words)


def word_digest(words: Any) -> int:
    result = 14695981039346656037
    for word in words:
        result = ((result ^ word) * 1099511628211) & UINT64_MAX
    return result


def image_identity(event: dict[str, Any]) -> tuple[Any, ...]:
    image = event.get("image", {})
    view = event.get("view", {})
    return tuple(image.get(key) for key in ("owner", "object", "native", "format", "width", "height")) + (
        view.get("mip", event.get("mip", 0)), view.get("layer", event.get("layer", 0)))


def spirv_error(data: bytes) -> str | None:
    if len(data) < 20 or len(data) % 4:
        return "invalid SPIR-V byte length"
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if words[0] != 0x07230203 or not words[3]:
        return "invalid SPIR-V header"
    index = 5
    while index < len(words):
        count = words[index] >> 16
        if not count or index + count > len(words):
            return f"invalid SPIR-V instruction at word {index}"
        index += count
    return None


def read_events(capture: Capture) -> list[dict[str, Any]]:
    names = sorted((name for name in capture.names if EVENT_NAME.fullmatch(name)),
                   key=lambda name: int(EVENT_NAME.fullmatch(name).group(1)))
    if not names:
        raise CaptureError("no prelight journal chunks")
    events = []
    for name in names:
        for line_number, line in enumerate(capture.read(name).splitlines(), 1):
            try:
                event = json.loads(line)
            except (ValueError, UnicodeError) as error:
                raise CaptureError(f"invalid journal JSON at {name}:{line_number}") from error
            if not isinstance(event, dict) or type(event.get("seq")) is not int or event["seq"] <= 0:
                raise CaptureError(f"invalid sequence at {name}:{line_number}")
            if not isinstance(event.get("kind"), str):
                raise CaptureError(f"missing event kind at {name}:{line_number}")
            events.append(event)
    if not events:
        raise CaptureError("empty prelight journal")
    return events


def find_loss(events: list[dict[str, Any]]) -> tuple[dict[str, Any], dict[str, Any], str] | None:
    """Only a retention criterion; never an expected-value or visibility test."""
    before: dict[int, dict[str, Any]] = {}
    for event in events:
        if event["kind"] == "draw_before":
            before[event["draw"]] = event
        elif event["kind"] == "draw_after" and event.get("draw") in before:
            prior = before[event["draw"]]
            a, b = prior.get("full"), event.get("full")
            if not isinstance(a, dict) or not isinstance(b, dict):
                continue
            area = b.get("width", 0) * b.get("height", 0)
            same_extent = (a.get("width"), a.get("height")) == (b.get("width"), b.get("height"))
            complete_identity = all(value is not None for value in image_identity(event))
            if (area > 0 and same_extent and complete_identity and
                    image_identity(prior) == image_identity(event) and
                    a.get("rgb_nonzero", -1) >= area / 2 and
                    0 <= b.get("rgb_nonzero", area + 1) <= area / 10):
                basis = "explicit_witness_loss" if event.get("witness_first_material_loss") is True else "same_draw_observed_drop"
                return prior, event, basis
    return None


def inspect_indirect(draw: dict[str, Any], executed: dict[str, Any] | None,
                     command_events: list[dict[str, Any]]) -> dict[str, Any]:
    """Validate the actual fixup contract; zero invocations need no vertex inputs.

    This local result does not prove the producer should have emitted zero, nor
    establish a global G0/P8 verdict. Frame equality is intentionally irrelevant.
    """
    result: dict[str, Any] = {"status": "BLOCKED", "zero_invocations": False}
    fixed = [event for event in command_events if event["kind"] == "indirect_fixed" and
             event.get("args_guest") == draw.get("indirect_guest") and
             executed and event["seq"] < executed["seq"]]
    if not fixed:
        return result | {"reason": "missing actual fixed arguments before execution"}
    fixed_event = fixed[-1]
    sources = [event for event in command_events if event["kind"] == "indirect_source" and
               event.get("args_guest") == fixed_event.get("args_guest") and
               event.get("bda") == fixed_event.get("source_bda") and
               event["seq"] < fixed_event["seq"]]
    if not sources:
        return result | {"reason": "missing same-BDA argument source"}
    source = sources[-1]
    try:
        indexed = source["indexed"]
        count = source["max_count"]
        stride = source["stride"]
        width = 5 if indexed else 4
        if (type(indexed) is not bool or type(count) is not int or not 1 <= count <= 1024 or
                type(stride) is not int or stride % 4 or stride < width * 4 or
                type(source["bda"]) is not int or source["bda"] <= 0 or
                type(fixed_event["count_bda"]) is not int or fixed_event["count_bda"] < 0 or
                fixed_event["max_count"] != count or fixed_event["stride"] != width * 4 or
                source["index_limit"] != fixed_event["index_limit"]):
            raise ValueError("invalid or mismatched argument layout")
        raw, actual = source["words"], fixed_event["words"]
        if (not words_valid(raw) or not words_valid(actual) or
                len(raw) != (count - 1) * (stride // 4) + width or len(actual) != count * width):
            raise ValueError("incomplete argument words")
        active_count = count
        if fixed_event.get("count_bda"):
            counts = [event for event in command_events if event["kind"] == "indirect_count" and
                      event.get("bda") == fixed_event["count_bda"] and event["seq"] < fixed_event["seq"]]
            if not counts or not words_valid(counts[-1].get("words")) or len(counts[-1]["words"]) != 1:
                raise ValueError("missing actual indirect count")
            active_count = min(count, counts[-1]["words"][0])
        expected = []
        for index in range(count):
            words = list(raw[index * (stride // 4):index * (stride // 4) + width])
            if index >= active_count:
                words = [0] * width
            elif indexed:
                limit = source["index_limit"]
                if type(limit) is not int or not 0 <= limit <= UINT32_MAX:
                    raise ValueError("invalid index limit")
                words[0] = min(words[0], limit - min(words[2], limit))
            expected.extend(words)
        if expected != actual:
            return result | {"reason": "fixup mismatch candidate; causal provenance not certified",
                             "source_seq": source["seq"], "fixed_seq": fixed_event["seq"],
                             "expected": expected, "observed": actual}
        zero = all(actual[index * width] == 0 or actual[index * width + 1] == 0 for index in range(count))
        return {"status": "LOCAL_FIXUP_MATCH", "zero_invocations": zero,
                "source_seq": source["seq"], "fixed_seq": fixed_event["seq"],
                "words": actual, "source_bda": source["bda"],
                "scope": "actual fixed args only; guest producer correctness remains uncertified"}
    except (KeyError, TypeError, ValueError) as error:
        return result | {"reason": str(error)}


def audit_capture(path: str | Path) -> dict[str, Any]:
    with Capture(path) as capture:
        return audit(capture)


def attach_vertex_oracle(report: dict[str, Any], path: str | Path) -> None:
    """Recompute the local reference. A JSON assertion is never a substitute."""
    script = Path(__file__).with_name("prelight_vertex_oracle.py")
    spec = importlib.util.spec_from_file_location("prelight_vertex_oracle", script)
    if spec is None or spec.loader is None:
        raise CaptureError("vertex oracle implementation unavailable")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    try:
        spec.loader.exec_module(module)
        report["local_vertex_oracle"] = module.audit_capture(Path(path))
        report["local_vertex_oracle"]["backend_sha256"] = hashlib.sha256(script.read_bytes()).hexdigest()
    except ImportError as error:
        report["local_vertex_oracle"] = {"analysis_state": "DEPENDENCY_UNAVAILABLE", "reason": str(error)}


def audit(capture: Capture) -> dict[str, Any]:
    gaps: list[dict[str, Any]] = []

    def gap(code: str, scope: str, detail: Any) -> None:
        gaps.append({"code": code, "scope": scope, "detail": detail})

    # This global interval is distinct from the concrete per-operation results.
    decision = {"status": "BLOCKED", "G0": "BLOCKED", "P8": "BLOCKED",
                "interval_before": [0, 16], "interval_after": [0, 16],
                "pruned_classes": []}
    report: dict[str, Any] = {"schema": 2, "node": "V3/G0/P8", "decision": decision,
        "purpose": "evidence audit, not a build permission gate",
        "source": str(capture.path), "gaps": gaps,
        "oracle": {"status": "UNAVAILABLE", "backend": None, "backend_sha256": None,
            "reason": "The complete cumulative prefix is not certified by this capture. Local executable decisions have their own scope.",
            "required": ["same-input manifest digest", "guest-contract replay with resource-version dependencies",
                         "causal path to the fixed final symptom", "real complete witness with positive and negative regression"]}}
    events = read_events(capture)
    status_name = "queue-state/ds_prelight_status.json"
    status = capture.json(status_name) if capture.exists(status_name) else {}
    if not isinstance(status, dict):
        raise CaptureError("journal status is not an object")
    report["journal"] = {"events": len(events), "first_seq": events[0]["seq"],
        "last_seq": events[-1]["seq"], "reported": status}
    seqs = [event["seq"] for event in events]
    for key, observed in (("first_retained_seq", seqs[0]), ("last_seq", seqs[-1])):
        if status.get(key) is not None and status[key] != observed:
            gap("journal_status_sequence_mismatch", "capture", {"field": key,
                "reported": status[key], "observed": observed})
    duplicates = [seq for seq, count in Counter(seqs).items() if count > 1]
    if duplicates or seqs != sorted(seqs):
        gap("journal_order_invalid", "capture", {"duplicates": duplicates, "ordered": seqs == sorted(seqs)})
    for key in ("dropped_events", "sample_dropped", "full_dropped"):
        if status.get(key, 0):
            gap("reported_capture_drops", "capture", {key: status[key]})
    if not status:
        gap("journal_status_missing", "capture", status_name)
    chosen = find_loss(events)
    if not chosen:
        gap("witness_loss_missing", "G0", "No complete same-draw same-image observed loss pair is available.")
    else:
        before, loss, basis = chosen
        prepared = {event["seq"]: event for event in events if event["kind"] == "draw_prepared"}
        clears = [event for event in events if event["kind"] == "draw_execute" and
                  event.get("depth_clear") is True and event["seq"] <= loss["seq"] and
                  prepared.get(event.get("draw"), {}).get("depth_target") is True]
        if not clears:
            gap("executed_clear_missing", "capture", "Prepared clear flags are not a cycle boundary.")
        else:
            clear = clears[-1]
            first = prepared[clear["draw"]]
            start = first.get("command", first["seq"])
            consumers = [event for event in events if event["kind"] == "consumer" and
                         event["seq"] > loss["seq"] and image_identity(event) == image_identity(loss) and
                         event.get("full", {}).get("hash_words") == loss["full"].get("hash_words")]
            consumer = consumers[0] if consumers else None
            end = consumer["seq"] if consumer else loss["seq"]
            if not consumer:
                gap("matching_consumer_missing", "C8", "No same-subresource consumer with the loss output fingerprint.")
            active_events = [event for event in events if start <= event["seq"] <= end]
            retained = [event["seq"] for event in active_events]
            discontinuities = [[a, b] for a, b in zip(retained, retained[1:]) if b != a + 1]
            if not retained or retained[0] != start or retained[-1] != end or discontinuities:
                gap("active_sequence_gap", "capture", {"start": start, "end": end, "gaps": discontinuities})
            report["journal"]["active_interval"] = [start, end]
            report["journal"]["active_sequence_gaps"] = discontinuities
            report["journal"]["prehistory_eviction_outside_active_interval"] = (
                status.get("prehistory_evicted", 0) > 0 and events[0]["seq"] <= start and not discontinuities)
            report["witness"] = {"selection_basis": basis, "selection_is_oracle": False,
                "first_draw": first["seq"], "first_frame": first["frame"], "first_submit": first["submit"],
                "executed_clear_seq": clear["seq"], "loss_draw": loss["draw"], "loss_frame": loss["frame"],
                "loss_submit": loss["submit"], "loss_seq": loss["seq"], "image_identity": list(image_identity(loss)),
                "before": before["full"], "after": loss["full"],
                "consumer_seq": consumer["seq"] if consumer else None}
            selected = [event for event in events if event["kind"] == "draw_prepared" and
                        first["seq"] <= event["seq"] <= loss["draw"]]
            inspect_cycle(capture, events, selected, report, gap)
            inspect_anchor(capture, loss, gap, report)
            inspect_depth_witness(capture, loss, gap, report)
    gap("resource_version_dag_unavailable", "C1..C8",
        "Target draw inventory does not close upstream CPU/compute producers, actual image subresources, aliases and versions.")
    gap("independent_oracle_unavailable", "G0/P8", report["oracle"]["reason"])
    report["capture_closure"] = {"global_prefix_certified": False,
        "reason": "Global closure is distinct from completeness for a selected operation."}
    inspected = dict(sorted(capture.inspected.items()))
    manifest = json.dumps(inspected, sort_keys=True, separators=(",", ":")).encode()
    report["inspected_artifacts"] = inspected
    report["input_manifest_sha256"] = hashlib.sha256(manifest).hexdigest()
    report["gap_counts"] = dict(sorted(Counter(item["code"] for item in gaps).items()))
    return report


def inspect_cycle(capture: Capture, events: list[dict[str, Any]], selected: list[dict[str, Any]],
                  report: dict[str, Any], gap: Any) -> None:
    by_draw: dict[int, dict[str, list[dict[str, Any]]]] = defaultdict(lambda: defaultdict(list))
    by_command: dict[int, list[dict[str, Any]]] = defaultdict(list)
    by_seq = {event["seq"]: event for event in events}
    for event in events:
        if "draw" in event:
            by_draw[event["draw"]][event["kind"]].append(event)
        if event.get("command"):
            by_command[event["command"]].append(event)
    manifest_name = "queue-state/ds_prelight_witness_foreground_manifest.json"
    manifest = capture.json(manifest_name) if capture.exists(manifest_name) else {}
    if not isinstance(manifest, dict):
        raise CaptureError("foreground manifest is not an object")
    saved_inputs: dict[int, dict[str, Any]] = {}
    saved_bytes = 0
    saved_binary_files: set[str] = set()
    if not manifest:
        gap("foreground_manifest_missing", "capture", manifest_name)
    elif (manifest.get("active") is not True or manifest.get("first_draw") != report["witness"]["first_draw"] or
          manifest.get("loss_draw") != report["witness"]["loss_draw"]):
        gap("foreground_manifest_inactive_or_wrong_cycle", "capture", {
            key: manifest.get(key) for key in ("active", "first_draw", "loss_draw", "bytes", "dropped")})
    if manifest.get("dropped", 0):
        gap("foreground_inputs_dropped", "capture", manifest["dropped"])
    manifest_inputs = manifest.get("inputs", [])
    if not isinstance(manifest_inputs, list):
        raise CaptureError("foreground manifest inputs are not a list")
    for item in manifest_inputs:
        if not isinstance(item, dict) or type(item.get("draw")) is not int or not item.get("file"):
            raise CaptureError("invalid foreground manifest input")
        relative = "queue-state/" + safe_name(item["file"])
        if not capture.exists(relative):
            gap("required_artifact_missing", "capture", relative)
            continue
        payload = capture.json(relative)
        saved_bytes += capture.inspected[relative]["bytes"]
        if not isinstance(payload, dict) or payload.get("draw") != item["draw"]:
            gap("foreground_input_identity_mismatch", "capture", relative)
            continue
        if item.get("saved") is not True:
            gap("foreground_save_status_failed", "capture", relative)
        if item["draw"] in saved_inputs:
            gap("foreground_input_duplicate", "capture", item["draw"])
        saved_inputs[item["draw"]] = payload
        for stage in ("vs", "ps"):
            for binding in payload.get(stage + "_bound", []):
                filename = binding.get("file") if isinstance(binding, dict) else None
                if filename and filename not in saved_binary_files and capture.exists(safe_name(filename)):
                    capture.read(filename)
                    saved_binary_files.add(filename)
                    saved_bytes += capture.inspected[filename]["bytes"]
    required_files = manifest.get("required_files", [])
    if not isinstance(required_files, list):
        raise CaptureError("required_files is not a list")
    for item in required_files:
        if not isinstance(item, dict) or not isinstance(item.get("file"), str):
            raise CaptureError("invalid required file")
        filename = safe_name(item["file"])
        if not capture.exists(filename):
            gap("required_artifact_missing", "capture", filename)
        else:
            data = capture.read(filename)
            if item.get("bytes") != len(data) or item.get("present") is False:
                gap("required_artifact_size_mismatch", "capture", {"file": filename,
                    "expected_bytes": item.get("bytes"), "actual_bytes": len(data)})
    if manifest.get("position_dropped", 0):
        gap("native_positions_dropped", "C3", manifest["position_dropped"])
    selected_ids = {draw["seq"] for draw in selected}
    if manifest:
        missing = sorted(selected_ids - saved_inputs.keys())
        outside = sorted(saved_inputs.keys() - selected_ids)
        if missing:
            gap("foreground_file_coverage_incomplete", "capture", {"missing_draws": missing})
        if outside:
            gap("foreground_files_outside_selected_cycle", "capture", outside)
        if manifest.get("bytes") != saved_bytes:
            gap("foreground_manifest_bytes_mismatch", "capture", {
                "reported": manifest.get("bytes"), "read": saved_bytes})

    shaders: dict[tuple[str, int], dict[str, Any]] = {}
    rows = []
    total_declared = total_present = required_declared = required_missing = 0
    for draw in selected:
        draw_id = draw["seq"]
        scope = f"draw:{draw_id}"
        records = by_draw[draw_id]
        executions = records.get("draw_execute", [])
        executed = executions[0] if len(executions) == 1 else None
        if not executed:
            gap("draw_execution_missing_or_ambiguous", scope, len(executions))
        command = by_seq.get(draw.get("command"), {})
        if command.get("kind") != "guest_command":
            gap("guest_command_missing", scope, draw.get("command"))
        returned = [event for event in by_command[draw.get("command", 0)] if event["kind"] == "command_return"]
        if not returned:
            gap("command_return_missing", scope, draw.get("command"))
        inputs_records = records.get("draw_inputs", [])
        payload = saved_inputs.get(draw_id) or (inputs_records[0] if len(inputs_records) == 1 else {})
        if not payload:
            gap("draw_inputs_missing_or_ambiguous", scope, len(inputs_records))
        elif (payload.get("frame") != draw.get("frame") or payload.get("submit") != draw.get("submit")):
            gap("draw_input_identity_mismatch", scope, {"frame": payload.get("frame"), "submit": payload.get("submit")})
        indirect = inspect_indirect(draw, executed, by_command[draw.get("command", 0)]) if draw.get("indirect") else None
        zero = indirect is not None and indirect["zero_invocations"]
        if indirect and indirect["status"] == "BLOCKED":
            gap("indirect_arguments_incomplete_or_mismatched", scope, indirect["reason"])
        row = {"draw": draw_id, "command": draw.get("command"), "frame": draw.get("frame"),
            "submit": draw.get("submit"), "depth_target": draw.get("depth_target"),
            "target_layer0": draw.get("target_layer0"), "indirect": indirect,
            "zero_invocations": zero, "buffer_gaps": [], "source": "foreground_file" if draw_id in saved_inputs else "journal"}
        if not zero:
            if not records.get("witness_dynamic_state"):
                gap("exact_dynamic_state_missing", scope, "Viewport/scissor/cull/depth/stencil state is not fully bound to this draw.")
            if draw.get("indirect"):
                gap("indirect_vertex_stream_unsupported", scope, "Nonzero indirect draws require actual subdraw index ranges and instances.")
            elif draw.get("index_guest", 0):
                indices = payload.get("index_words")
                expected_count = draw.get("indices", 0)
                if not words_valid(indices) or len(indices) != expected_count:
                    gap("index_stream_incomplete", scope, {"expected_words": expected_count,
                        "captured_words": len(indices) if isinstance(indices, list) else 0})
                elif payload.get("index_digest") is not None and word_digest(indices) != payload["index_digest"]:
                    gap("index_digest_mismatch", scope, payload["index_digest"])
            for stage in ("vs", "ps"):
                shader = draw.get(stage, 0)
                if not shader:
                    continue
                if type(shader) is not int or not 0 < shader <= UINT64_MAX:
                    raise CaptureError(f"invalid {stage} shader hash in {scope}")
                entry = shaders.setdefault((stage, shader), {"stage": stage, "guest_hash": f"{shader:016x}",
                    "draws": [], "bound_files": set()})
                entry["draws"].append(draw_id)
                entry["bound_files"].add(payload.get(stage + "_file", ""))
        for stage in ("vs", "ps"):
            bindings = payload.get(stage + "_bound", [])
            if not isinstance(bindings, list) or any(not isinstance(binding, dict) for binding in bindings):
                raise CaptureError(f"invalid bound-buffer list in {scope}")
            for binding in bindings:
                size = binding.get("bytes")
                address = binding.get("address")
                if type(size) is not int or size < 0 or type(address) is not int or address < 0:
                    raise CaptureError(f"invalid buffer range in {scope}")
                if not size:
                    continue  # A null/empty descriptor is legal, not an error.
                words = binding.get("gpu_words")
                present = len(words) * 4 if words_valid(words) else 0
                binary = binding.get("file")
                if binary:
                    if not isinstance(binary, str):
                        raise CaptureError(f"invalid bound-buffer file in {scope}")
                    filename = safe_name(binary)
                    if capture.exists(filename):
                        data = capture.read(filename)
                        offset = binding.get("offset_bytes", 0)
                        if type(offset) is not int or offset < 0:
                            raise CaptureError(f"invalid bound-buffer offset in {scope}")
                        present = max(0, min(size, len(data) - offset))
                        words = list(struct.unpack(f"<{size // 4}I", data[offset:offset+size])) if present == size and size % 4 == 0 else None
                total_declared += size
                total_present += min(size, present)
                if zero:
                    continue
                required_declared += size
                complete = address > 0 and address + size <= UINT64_MAX and present == size
                if not complete:
                    missing = max(0, size - present)
                    required_missing += missing
                    row["buffer_gaps"].append({"stage": stage, "slot": binding.get("slot"),
                        "address": address, "expected_bytes": size, "captured_bytes": present,
                        "missing_bytes": missing, "reported_status": binding.get("capture_status")})
                elif binding.get("capture_status") not in (None, "complete"):
                    gap("buffer_capture_status_conflict", scope, {"stage": stage, "slot": binding.get("slot"),
                        "status": binding["capture_status"]})
                if complete and binding.get("gpu_digest") is not None and word_digest(words) != binding["gpu_digest"]:
                    gap("buffer_digest_mismatch", scope, {"stage": stage, "slot": binding.get("slot")})
        if row["buffer_gaps"]:
            gap("bound_buffer_bytes_missing", scope, row["buffer_gaps"])
        rows.append(row)
    module_rows = []
    for (_, _), entry in sorted(shaders.items()):
        guest_file = f"guest-shaders/{entry['stage']}_{entry['guest_hash']}_probe.bin"
        entry["guest_file"] = guest_file
        entry["guest_present"] = capture.exists(guest_file)
        if not entry["guest_present"]:
            gap("guest_shader_missing", "C3/C6", guest_file)
        else:
            data = capture.read(guest_file)
            actual = xxh3(data)
            entry["guest_content_fingerprint"] = actual
            if not data or len(data) % 4 or actual != entry["guest_hash"]:
                gap("guest_shader_integrity_unverified", "C3/C6", {"file": guest_file, "actual": actual})
        entry["bound_files"] = sorted(entry["bound_files"])
        for filename in entry["bound_files"]:
            match = MODULE_NAME.fullmatch(filename)
            if not match or match[1] != entry["stage"] or match[2] != entry["guest_hash"]:
                gap("bound_module_identity_missing", "C3/C6", {"guest": entry["guest_hash"], "file": filename})
                continue
            if not capture.exists(filename):
                gap("bound_module_missing", "C3/C6", filename)
                continue
            data = capture.read(filename)
            actual = xxh3(data)
            error = spirv_error(data)
            if error or actual != match[3]:
                gap("bound_module_integrity_unverified", "C3/C6", {"file": filename, "actual": actual, "error": error})
        module_rows.append(entry)
    report["operations"] = {"draw_count": len(rows), "draws": rows,
        "zero_invocation_indirect_draws": [row["draw"] for row in rows if row["zero_invocations"]],
        "scope": "Target depth/layer0 draws in the selected cycle; not a proof of complete upstream emission."}
    report["shaders_consumed_by_nonzero_draws"] = module_rows
    report["buffers"] = {"declared_bytes_all_draws": total_declared,
        "captured_bytes_all_draws": total_present, "missing_bytes_all_draws": total_declared - total_present,
        "required_bytes_nonzero_draws": required_declared, "missing_bytes_nonzero_draws": required_missing,
        "note": "Ranges are counted per binding/use, not deduplicated; actual access and producer versions still require replay."}
    gap("full_gbuffer_and_images_unverified", "C2/C5/C6/C7/C8",
        "Descriptors, 64-point grids and hashes do not supply all sampled image bytes, MRT/depth states and output contracts.")
    gap("full_pipeline_state_unavailable", "C3..C7",
        "A viewport event alone does not certify all stage interfaces, stencil masks/ops, sample state, blend and attachment formats.")


def inspect_anchor(capture: Capture, loss: dict[str, Any], gap: Any, report: dict[str, Any]) -> None:
    name = "queue-state/ds_prelight_anchor.json"
    if not capture.exists(name):
        gap("consumer_anchor_missing", "C8", name)
        return
    anchor = capture.json(name)
    if not isinstance(anchor, dict):
        raise CaptureError("consumer anchor is not an object")
    records = {}
    for side in ("before", "after"):
        item = anchor.get(side, {})
        filename = anchor.get(side + "_file")
        if not isinstance(filename, str):
            gap("consumer_anchor_payload_missing", "C8", side)
            continue
        path = "queue-state/" + safe_name(filename)
        if not capture.exists(path):
            gap("required_artifact_missing", "C8", path)
            continue
        data = capture.read(path)
        full = item.get("full", {})
        expected_bytes = full.get("width", 0) * full.get("height", 0) * 4
        valid = bool(expected_bytes and len(data) == expected_bytes and len(data) % 4 == 0)
        digest = 14695981039346656037 if valid else None
        colored = 0
        if valid:
            for (word,) in struct.iter_unpack("<I", data):
                digest = ((digest ^ word) * 1099511628211) & UINT64_MAX
                colored += (word & 0xFFFFFF) != 0
        valid = valid and digest == full.get("hash_words") and colored == full.get("rgb_nonzero")
        if not valid or anchor.get(side + "_saved") is not True:
            gap("consumer_anchor_integrity_mismatch", "C8", {"file": path, "bytes": len(data),
                "expected_bytes": expected_bytes, "word_digest": digest, "reported_digest": full.get("hash_words"),
                "rgb_nonzero": colored, "reported_rgb_nonzero": full.get("rgb_nonzero")})
        records[side] = {"file": path, "bytes": len(data), "word_digest": digest,
                         "rgb_nonzero": colored, "valid": valid}
    after = anchor.get("after", {})
    matches = (image_identity(after) == image_identity(loss) and
               after.get("image", {}).get("version") == loss.get("image", {}).get("version") and
               after.get("full", {}).get("hash_words") == loss.get("full", {}).get("hash_words"))
    if not matches:
        gap("anchor_loss_identity_mismatch", "C8", "Anchor after is not the same captured loss output version.")
    report["anchor"] = {"payloads": records, "after_matches_loss": matches,
        "scope": "Observed transport integrity only; the prior frame is not an expected-output oracle."}


def inspect_depth_witness(capture: Capture, loss: dict[str, Any], gap: Any,
                          report: dict[str, Any]) -> None:
    """Check the current sky/depth artifact contract without replaying the sky."""
    name = "queue-state/ds_prelight_witness_depth.json"
    if not capture.exists(name):
        gap("full_depth_witness_missing", "C5", name)
        return
    metadata = capture.json(name)
    if not isinstance(metadata, dict):
        raise CaptureError("depth witness metadata is not an object")
    if metadata.get("draw") != loss["draw"] or metadata.get("same_pair_serial") is not True:
        gap("depth_witness_identity_unverified", "C5", {
            key: metadata.get(key) for key in ("draw", "pair", "same_pair_serial")})
    payloads = {}
    for label, dimensions, flag in (("pre_before", "pre", "pre_before_saved"),
                                    ("pre_after", "pre", "pre_after_saved"),
                                    ("material_before", "material", "material_before_saved")):
        equal_alias = label == "material_before" and metadata.get("material_before_equals_pre_after") is True
        filename = f"queue-state/ds_prelight_witness_depth_{'pre_after' if equal_alias else label}.bin"
        expected = metadata.get(dimensions + "_width", 0) * metadata.get(dimensions + "_height", 0) * 4
        if not capture.exists(filename):
            gap("required_artifact_missing", "C5", filename)
            continue
        data = capture.read(filename)
        valid = expected > 0 and len(data) == expected and metadata.get(flag) is True
        if not valid:
            gap("depth_payload_incomplete", "C5", {"file": filename, "expected_bytes": expected,
                "bytes": len(data), "reported_saved": metadata.get(flag)})
        payloads[label] = {"file": filename, "bytes": len(data), "valid": valid,
                           "aliases_pre_after": equal_alias}
        if equal_alias:
            explicit = "queue-state/ds_prelight_witness_depth_material_before.bin"
            if capture.exists(explicit) and capture.read(explicit) != data:
                gap("depth_equality_claim_contradicted", "C5", explicit)
    positions = {}
    for side in ("sky_prepass", "sky_material"):
        filename = f"queue-state/ds_prelight_witness_{side}.json"
        if not capture.exists(filename):
            gap("required_artifact_missing", "C3", filename)
            continue
        position = capture.json(filename)
        if not isinstance(position, dict):
            raise CaptureError(f"invalid position witness: {filename}")
        indices, rows = position.get("indices"), position.get("rows")
        valid = words_valid(indices) and isinstance(rows, list) and all(
            isinstance(row, list) and len(row) == 23 and words_valid(row) for row in rows)
        if valid:
            expected_vertices = {index + position.get("base_vertex", 0) for index in indices}
            valid = expected_vertices == {row[0] for row in rows} and position.get("missing") == 0
        if not valid:
            gap("position_witness_incomplete", "C3", filename)
        positions[side] = {"file": filename, "valid": valid,
            "original_fingerprint": position.get("fingerprint"),
            "bound_module": position.get("native_vs_file"),
            "note": "Original fingerprint and instrumented bound-module fingerprint are distinct identities."}
    report["depth_witness"] = {"payloads": payloads, "positions": positions,
        "scope": "File closure and byte lengths only; not an independent geometry/depth certificate."}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", type=Path, help="diagnostic ZIP or extracted capture directory")
    parser.add_argument("--output", type=Path, help="write the complete JSON readiness report")
    parser.add_argument("--vertex-oracle", action="store_true", help="execute the independent per-draw guest-ISA position reference")
    args = parser.parse_args(argv)
    try:
        report = audit_capture(args.capture)
        if args.vertex_oracle:
            attach_vertex_oracle(report,args.capture)
    except (CaptureError, OSError, KeyError, TypeError, ValueError) as error:
        print(json.dumps({"decision": "BLOCKED", "input_error": str(error)}), file=sys.stderr)
        return 3
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
        print(json.dumps({"report": str(args.output), "decision": report["decision"], "gap_counts": report["gap_counts"]}))
    else:
        print(text, end="")
    return 0  # A readable audit may contain unknowns without prohibiting work.


if __name__ == "__main__":
    raise SystemExit(main())
