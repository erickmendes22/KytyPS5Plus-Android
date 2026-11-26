#!/usr/bin/env python3
"""Readiness regressions; optionally require the two real #741/#744 captures.

  python tests/manual/test_prelight_v3_gate.py \
      --real-capture /path/to/113031.zip --real-capture /path/to/115532.zip

The small fixtures test missing/inconsistent evidence, never pretend to be the
independent guest oracle. Real-capture tests assert the previously measured cycle,
missing-byte census, zero-work indirect args and unchanged V3 interval.
"""
from __future__ import annotations

import argparse
import copy
import importlib.util
import json
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "docs/android/tools/prelight_v3_gate.py"
spec = importlib.util.spec_from_file_location("prelight_v3_gate", SCRIPT)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)
REAL_CAPTURES: list[Path] = []


def fixture(indirect: bool = False) -> tuple[list[dict], dict]:
    events = []
    ids = {}
    image = {"owner": 0x287AE0000, "object": 101, "native": 202, "format": 98,
             "width": 2, "height": 2, "version": 5}

    def add(kind, **values):
        seq = 100 + len(events)
        events.append({"kind": kind, "seq": seq, **values})
        return seq

    def start_draw(indirect_draw=False, material=False):
        # Guest commands and prepared work may cross a host frame boundary.
        command = add("guest_command", frame=6, submit=1, count=3, instances=1)
        draw = add("draw_prepared", command=command, frame=7, submit=1, vs=1, ps=2,
                   index_guest=0x2000, index_bytes=6, indices=3, instances=1,
                   depth_target=True, target_layer0=material, indirect=indirect_draw,
                   indirect_guest=0x3000 if indirect_draw else 0)
        return command, draw

    def inputs(draw):
        return add("draw_inputs", draw=draw, frame=7, submit=1, depth_load_clear=False,
                   index_words=[0, 1, 2], vs_bound=[], ps_bound=[], vs_file="", ps_file="")

    command, draw = start_draw()
    ids["first_draw"] = draw
    ids["clear_inputs"] = inputs(draw)
    ids["clear_execute"] = add("draw_execute", draw=draw, frame=7, submit=1, depth_clear=True)
    add("draw_after", draw=draw, frame=7, submit=1)
    add("command_return", command=command, outcome="prepared")
    if indirect:
        command, draw = start_draw(indirect_draw=True)
        ids["indirect_draw"] = draw
        ids["indirect_source"] = add("indirect_source", command=command, args_guest=0x3000,
            bda=0x5000, max_count=1, stride=20, index_limit=3, indexed=True, words=[3, 0, 0, 0, 0])
        ids["indirect_fixed"] = add("indirect_fixed", command=command, args_guest=0x3000,
            source_bda=0x5000, count_bda=0, max_count=1, stride=20, index_limit=3, words=[3, 0, 0, 0, 0])
        inputs(draw)
        add("draw_execute", draw=draw, frame=7, submit=1, depth_clear=False)
        add("command_return", command=command, outcome="prepared")
    command, draw = start_draw(material=True)
    ids["loss_draw"] = draw
    ids["material_inputs"] = inputs(draw)
    add("draw_before", draw=draw, frame=7, submit=1, image=image, layer=0, mip=0,
        full={"width": 2, "height": 2, "rgb_nonzero": 4, "hash_words": 42})
    add("draw_execute", draw=draw, frame=7, submit=1, depth_clear=False)
    ids["loss"] = add("draw_after", draw=draw, frame=7, submit=1, image=image,
        full={"width": 2, "height": 2, "rgb_nonzero": 0, "hash_words": 43},
        witness_first_material_loss=True)
    add("command_return", command=command, outcome="prepared")
    add("consumer", frame=7, submit=1, queue=0, image=image,
        view={"mip": 0, "layer": 0}, full={"width": 2, "height": 2, "rgb_nonzero": 0, "hash_words": 43})
    return events, ids


class GateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.capture = self.root / "capture"
        self.queue = self.capture / "ds-lighting-capture/queue-state"
        self.queue.mkdir(parents=True)

    def write(self, events, status=None):
        (self.queue / "ds_prelight_events_0.jsonl").write_text(
            "".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
        state = {"first_retained_seq": events[0]["seq"], "last_seq": events[-1]["seq"],
                 "prehistory_evicted": events[0]["seq"] - 1, "dropped_events": 0,
                 "sample_dropped": 0, "full_dropped": 0}
        state.update(status or {})
        (self.queue / "ds_prelight_status.json").write_text(json.dumps(state), encoding="utf-8")

    def codes(self, report):
        return set(report["gap_counts"])

    def assert_blocked(self, report):
        decision = report["decision"]
        self.assertEqual(decision["status"], "BLOCKED")
        self.assertEqual(decision["G0"], "BLOCKED")
        self.assertEqual(decision["P8"], "BLOCKED")
        self.assertEqual(decision["interval_after"], [0, 16])
        self.assertEqual(decision["pruned_classes"], [])
        self.assertFalse(decision["ready_for_diagnostic_apk"])

    def test_actual_clear_selects_cycle_despite_false_prepared_flag(self):
        events, ids = fixture()
        self.write(events)
        report = gate.audit_capture(self.capture)
        self.assertEqual(report["witness"]["first_draw"], ids["first_draw"])
        self.assertEqual(report["witness"]["executed_clear_seq"], ids["clear_execute"])
        self.assertEqual(report["witness"]["loss_draw"], ids["loss_draw"])
        self.assertNotIn("draw_input_identity_mismatch", self.codes(report))
        self.assert_blocked(report)

    def test_prepared_clear_alone_is_never_a_boundary(self):
        events, ids = fixture()
        for event in events:
            if event["seq"] == ids["clear_inputs"]:
                event["depth_load_clear"] = True
            if event["seq"] == ids["clear_execute"]:
                event["depth_clear"] = False
        self.write(events)
        report = gate.audit_capture(self.capture)
        self.assertIn("executed_clear_missing", self.codes(report))
        self.assertNotIn("witness", report)
        self.assert_blocked(report)

    def test_old_capture_without_explicit_loss_flag_uses_same_pair(self):
        events, ids = fixture()
        for event in events:
            event.pop("witness_first_material_loss", None)
        self.write(events)
        report = gate.audit_capture(self.capture)
        self.assertEqual(report["witness"]["selection_basis"], "same_draw_observed_drop")
        self.assertEqual(report["witness"]["loss_draw"], ids["loss_draw"])
        self.assertFalse(report["witness"]["selection_is_oracle"])
        self.assert_blocked(report)

    def test_prehistory_gap_outside_cycle_does_not_invalidate_active_sequence(self):
        events, _ = fixture()
        events.insert(0, {"seq": 98, "kind": "context"})  # 99 is outside the active cycle.
        self.write(events)
        report = gate.audit_capture(self.capture)
        self.assertEqual(report["journal"]["active_sequence_gaps"], [])
        self.assertTrue(report["journal"]["prehistory_eviction_outside_active_interval"])
        self.assertNotIn("active_sequence_gap", self.codes(report))
        self.assert_blocked(report)

    def test_missing_inputs_execute_or_command_never_becomes_a_decision(self):
        original, ids = fixture()
        for remove in (ids["material_inputs"], ids["loss_draw"] - 1, ids["loss"] - 1):
            with self.subTest(remove=remove):
                self.write([event for event in original if event["seq"] != remove])
                report = gate.audit_capture(self.capture)
                self.assertIn("active_sequence_gap", self.codes(report))
                self.assert_blocked(report)

    def test_claimed_complete_buffer_is_checked_against_actual_words(self):
        events, ids = fixture()
        payload = next(event for event in events if event["seq"] == ids["material_inputs"])
        payload["vs_bound"] = [{"slot": 0, "address": 0x2000, "bytes": 16,
                                "gpu_words": [1], "capture_status": "complete"}]
        self.write(events)
        report = gate.audit_capture(self.capture)
        self.assertEqual(report["buffers"]["missing_bytes_nonzero_draws"], 12)
        self.assertIn("bound_buffer_bytes_missing", self.codes(report))
        self.assert_blocked(report)

    def test_empty_descriptor_is_not_reported_as_rendering_failure(self):
        events, ids = fixture()
        payload = next(event for event in events if event["seq"] == ids["material_inputs"])
        payload["vs_bound"] = [{"slot": 0, "address": 0, "bytes": 0, "capture_status": "empty_or_invalid_guest_range"}]
        self.write(events)
        report = gate.audit_capture(self.capture)
        self.assertNotIn("bound_buffer_bytes_missing", self.codes(report))
        self.assertEqual(report["buffers"]["required_bytes_nonzero_draws"], 0)
        self.assert_blocked(report)

    def test_zero_instance_indirect_skips_unconsumed_vertex_resources_only(self):
        events, ids = fixture(indirect=True)
        self.write(events)
        report = gate.audit_capture(self.capture)
        self.assertEqual(report["operations"]["zero_invocation_indirect_draws"], [ids["indirect_draw"]])
        row = next(row for row in report["operations"]["draws"] if row["draw"] == ids["indirect_draw"])
        self.assertEqual(row["indirect"]["status"], "LOCAL_FIXUP_MATCH")
        self.assertFalse(any(item["scope"] == f"draw:{ids['indirect_draw']}" and item["code"] in
            ("exact_dynamic_state_missing", "indirect_vertex_stream_unsupported", "bound_buffer_bytes_missing")
            for item in report["gaps"]))
        self.assertIn("resource_version_dag_unavailable", self.codes(report))
        self.assert_blocked(report)

    def test_indirect_mismatch_or_missing_source_cannot_claim_zero_work(self):
        events, ids = fixture(indirect=True)
        for mutation in ("missing", "mismatch", "empty", "count_binding_missing"):
            with self.subTest(mutation=mutation):
                changed = copy.deepcopy(events)
                if mutation == "missing":
                    changed = [event for event in changed if event["seq"] != ids["indirect_source"]]
                else:
                    fixed = next(event for event in changed if event["seq"] == ids["indirect_fixed"])
                    if mutation == "count_binding_missing":
                        fixed.pop("count_bda")
                    else:
                        fixed["words"] = [3, 1, 0, 0, 0] if mutation == "mismatch" else []
                self.write(changed)
                report = gate.audit_capture(self.capture)
                self.assertEqual(report["operations"]["zero_invocation_indirect_draws"], [])
                self.assertIn("indirect_arguments_incomplete_or_mismatched", self.codes(report))
                self.assert_blocked(report)

    def test_self_asserted_oracle_and_ready_flags_are_ignored(self):
        events, _ = fixture()
        self.write(events, {"root_decision": "PASS", "oracle_complete": True, "ready": True})
        fake = {"same_cycle_certified": True, "history_certified": True, "all_prefixes_pass": True,
                "expected_output": [0, 0, 0, 0], "backend": "/do/not/execute/this"}
        (self.queue / "ds_prelight_oracle.json").write_text(json.dumps(fake))
        report = gate.audit_capture(self.capture)
        self.assertIsNone(report["oracle"]["backend"])
        self.assertNotIn("queue-state/ds_prelight_oracle.json", report["inspected_artifacts"])
        self.assert_blocked(report)

    def test_manifest_saved_flag_cannot_hide_a_missing_file(self):
        events, ids = fixture()
        self.write(events)
        manifest = {"active": True, "first_draw": ids["first_draw"], "loss_draw": ids["loss_draw"],
                    "inputs": [{"draw": ids["first_draw"], "file": "missing.json", "saved": True}]}
        (self.queue / "ds_prelight_witness_foreground_manifest.json").write_text(json.dumps(manifest))
        report = gate.audit_capture(self.capture)
        self.assertIn("required_artifact_missing", self.codes(report))
        self.assertIn("foreground_file_coverage_incomplete", self.codes(report))
        self.assert_blocked(report)

    def write_depth_witness(self, loss_draw):
        metadata = {"draw": loss_draw, "same_pair_serial": True, "pre_width": 2,
                    "pre_height": 2, "material_width": 2, "material_height": 2,
                    "pre_before_saved": True, "pre_after_saved": True,
                    "material_before_saved": True, "material_before_equals_pre_after": True}
        (self.queue / "ds_prelight_witness_depth.json").write_text(json.dumps(metadata))
        for suffix in ("pre_before", "pre_after"):
            (self.queue / f"ds_prelight_witness_depth_{suffix}.bin").write_bytes(bytes(16))

    def test_explicit_equal_depth_alias_does_not_require_duplicate_payload(self):
        events, ids = fixture()
        self.write(events)
        self.write_depth_witness(ids["loss_draw"])
        report = gate.audit_capture(self.capture)
        alias = report["depth_witness"]["payloads"]["material_before"]
        self.assertTrue(alias["valid"])
        self.assertTrue(alias["aliases_pre_after"])
        self.assertTrue(alias["file"].endswith("pre_after.bin"))
        self.assert_blocked(report)

    def test_depth_equality_claim_does_not_hide_conflicting_payload(self):
        events, ids = fixture()
        self.write(events)
        self.write_depth_witness(ids["loss_draw"])
        (self.queue / "ds_prelight_witness_depth_material_before.bin").write_bytes(bytes([1]) * 16)
        report = gate.audit_capture(self.capture)
        self.assertIn("depth_equality_claim_contradicted", self.codes(report))
        self.assert_blocked(report)

    def test_journal_status_cannot_claim_a_different_retained_sequence(self):
        events, _ = fixture()
        self.write(events, {"first_retained_seq": 999})
        report = gate.audit_capture(self.capture)
        self.assertIn("journal_status_sequence_mismatch", self.codes(report))
        self.assert_blocked(report)

    def test_hash_and_spirv_structure_detect_tampered_bound_module(self):
        if gate.xxh3(b"test") is None:
            self.skipTest("libxxhash unavailable; production gate marks integrity unverified")
        events, ids = fixture()
        original = struct.pack("<5I", 0x07230203, 0x00010000, 0, 1, 0)
        fingerprint = gate.xxh3(original)
        filename = f"spv/vs_0000000000000001_{fingerprint}.spv"
        payload = next(event for event in events if event["seq"] == ids["material_inputs"])
        payload["vs_file"] = filename
        target = self.capture / "ds-lighting-capture" / filename
        target.parent.mkdir(parents=True)
        target.write_bytes(original + b"\x00\x00\x00\x00")
        self.write(events)
        report = gate.audit_capture(self.capture)
        self.assertIn("bound_module_integrity_unverified", self.codes(report))
        self.assert_blocked(report)

    def test_input_digest_changes_when_consumed_evidence_changes(self):
        events, ids = fixture()
        self.write(events)
        first = gate.audit_capture(self.capture)
        next(event for event in events if event["seq"] == ids["material_inputs"])["index_words"][0] = 17
        self.write(events)
        second = gate.audit_capture(self.capture)
        self.assertNotEqual(first["input_manifest_sha256"], second["input_manifest_sha256"])
        self.assert_blocked(second)

    def test_archive_is_read_without_extraction_and_matches_directory(self):
        events, _ = fixture()
        self.write(events)
        archive = self.root / "input.zip"
        with zipfile.ZipFile(archive, "w") as output:
            for file in self.capture.rglob("*"):
                if file.is_file():
                    output.write(file, file.relative_to(self.capture).as_posix())
        a = gate.audit_capture(self.capture)
        b = gate.audit_capture(archive)
        self.assertEqual(a["input_manifest_sha256"], b["input_manifest_sha256"])
        self.assertEqual(a["witness"], b["witness"])
        self.assert_blocked(b)

    def test_unsafe_archive_paths_duplicate_names_and_symlinks_are_rejected(self):
        for kind in ("traversal", "absolute", "symlink", "duplicate"):
            with self.subTest(kind=kind):
                archive = self.root / f"{kind}.zip"
                with zipfile.ZipFile(archive, "w") as output:
                    if kind == "traversal":
                        output.writestr("../escape", "x")
                    elif kind == "absolute":
                        output.writestr("/escape", "x")
                    elif kind == "symlink":
                        info = zipfile.ZipInfo("link")
                        info.external_attr = (stat.S_IFLNK | 0o777) << 16
                        output.writestr(info, "/tmp")
                    else:
                        output.writestr("same", "x")
                        output.writestr("./same", "y")
                with self.assertRaises(gate.CaptureError):
                    gate.audit_capture(archive)
        self.assertFalse((self.root / "escape").exists())

    def test_directory_symlink_is_rejected(self):
        events, _ = fixture()
        self.write(events)
        (self.queue / "outside").symlink_to(self.root)
        with self.assertRaises(gate.CaptureError):
            gate.audit_capture(self.capture)

    def test_cli_returns_nonzero_when_oracle_is_unavailable(self):
        events, _ = fixture()
        self.write(events)
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.capture)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 2)
        self.assert_blocked(json.loads(result.stdout))

    def test_no_cli_switch_accepts_an_asserted_oracle(self):
        events, _ = fixture()
        self.write(events)
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.capture), "--oracle", "claimed.json"],
                                capture_output=True, text=True, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unrecognized arguments", result.stderr)

    def test_empty_journal_returns_documented_input_error(self):
        (self.queue / "ds_prelight_events_0.jsonl").write_text("")
        (self.queue / "ds_prelight_status.json").write_text("{}")
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.capture)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 3)
        report = json.loads(result.stderr)
        self.assertEqual(report["decision"], "BLOCKED")
        self.assertFalse(report["ready_for_diagnostic_apk"])
        self.assertIn("empty prelight journal", report["input_error"])


class RealCaptureTests(unittest.TestCase):
    def test_real_741_and_744_census_and_refusal(self):
        if not REAL_CAPTURES:
            self.skipTest("supply --real-capture for retained #741/#744 fixtures")
        expected = {40091: (39624, [39898, 40079]), 40979: (40367, [40790, 40968])}
        for path in REAL_CAPTURES:
            with self.subTest(path=path):
                report = gate.audit_capture(path)
                witness = report["witness"]
                self.assertIn(witness["loss_draw"], expected)
                first, indirect = expected[witness["loss_draw"]]
                self.assertEqual(witness["first_draw"], first)
                self.assertEqual(report["operations"]["draw_count"], 50)
                self.assertEqual(report["operations"]["zero_invocation_indirect_draws"], indirect)
                self.assertEqual(report["buffers"]["missing_bytes_all_draws"], 3304608)
                self.assertEqual(report["buffers"]["missing_bytes_nonzero_draws"], 3214628)
                self.assertEqual(witness["before"]["rgb_nonzero"], 1439989)
                self.assertEqual(witness["after"]["rgb_nonzero"], 658)
                self.assertEqual(report["journal"]["active_sequence_gaps"], [])
                self.assertTrue(report["journal"]["prehistory_eviction_outside_active_interval"])
                self.assertTrue(report["anchor"]["after_matches_loss"])
                self.assertTrue(all(row["valid"] for row in report["anchor"]["payloads"].values()))
                self.assertNotIn("bound_module_integrity_unverified", report["gap_counts"])
                self.assertNotIn("guest_shader_integrity_unverified", report["gap_counts"])
                self.assertEqual(report["decision"]["interval_after"], [0, 16])
                self.assertFalse(report["decision"]["ready_for_diagnostic_apk"])
                self.assertEqual(report["decision"]["status"], "BLOCKED")
                self.assertEqual(report["decision"]["pruned_classes"], [])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--real-capture", action="append", type=Path, default=[])
    args, unittest_args = parser.parse_known_args()
    REAL_CAPTURES.extend(args.real_capture)
    unittest.main(argv=[sys.argv[0], *unittest_args])
