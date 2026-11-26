package com.kytyps5.android;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.stream.Stream;

/** Exact production exporter tests, also runnable without Android or JUnit.
 * Compile alongside DsLightingCaptureExport and org.json:json:20180813.
 * main accepts zero or more extracted ds-lighting-capture directories; their
 * output is a transport inventory, never a G0/P8 certificate.
 */
public final class DsLightingCaptureExportFixtures {
    private static final long MIB = 1024L * 1024;
    private static int assertions;
    private DsLightingCaptureExportFixtures() { }

    private static void check(boolean value, String message) {
        assertions++;
        if (!value) throw new AssertionError(message);
    }
    private static boolean problem(DsLightingCaptureExport.Selection selection, String code, String path) {
        for (DsLightingCaptureExport.Problem issue : selection.problems)
            if (issue.code.equals(code) && (path == null || issue.path.equals(path))) return true;
        return false;
    }
    private static Set<String> paths(DsLightingCaptureExport.Selection selection) {
        Set<String> values = new LinkedHashSet<>();
        for (DsLightingCaptureExport.Entry entry : selection.entries) values.add(entry.path);
        return values;
    }
    private static final class Fixture implements AutoCloseable {
        final File root;
        final JSONObject manifest;
        Fixture() throws Exception {
            root = Files.createTempDirectory("ds-export-test-").toFile();
            manifest = new JSONObject().put("active", true).put("first_draw", 100)
                    .put("loss_draw", 110).put("required_files", new JSONArray());
            write("queue-state/ds_prelight_status.json", "{}");
            write("queue-state/ds_prelight_shader_status.json", "{\"failure_count\":0}");
            write("queue-state/ds_prelight_events_0.jsonl",
                    "{\"seq\":100,\"kind\":\"draw_prepared\",\"depth_target\":true,\"vs\":0,\"ps\":0}\n" +
                    "{\"seq\":101,\"kind\":\"draw_execute\",\"draw\":100,\"depth_clear\":true}\n" +
                    "{\"seq\":110,\"kind\":\"draw_prepared\",\"vs\":0,\"ps\":0}\n" +
                    "{\"seq\":111,\"kind\":\"draw_after\",\"draw\":110,\"witness_first_material_loss\":true}\n");
            saveManifest();
        }
        File write(String name, String value) throws IOException {
            File file = new File(root, name);
            if (!file.getParentFile().isDirectory() && !file.getParentFile().mkdirs()) throw new IOException("mkdir " + name);
            Files.write(file.toPath(), value.getBytes(StandardCharsets.UTF_8));
            return file;
        }
        File sized(String name, long bytes) throws IOException {
            File file = write(name, "x");
            try (RandomAccessFile output = new RandomAccessFile(file, "rw")) { output.setLength(bytes); }
            return file;
        }
        void required(String name, long bytes) throws Exception {
            manifest.getJSONArray("required_files").put(new JSONObject().put("file", name).put("bytes", bytes));
        }
        void saveManifest() throws IOException { write("queue-state/ds_prelight_witness_foreground_manifest.json", manifest.toString()); }
        DsLightingCaptureExport.Plan plan() throws Exception { saveManifest(); return DsLightingCaptureExport.discover(root, 0); }
        DsLightingCaptureExport.Selection select() throws Exception {
            return DsLightingCaptureExport.select(plan(), Collections.emptyList());
        }
        @Override public void close() throws IOException {
            try (Stream<Path> paths = Files.walk(root.toPath())) {
                for (Path path : (Iterable<Path>) paths.sorted(Comparator.reverseOrder())::iterator) Files.delete(path);
            }
        }
    }

    public static void runFixtureTests() throws Exception {
        assertions = 0;
        completeMinimalCapture();
        requiredFilesIgnoreOldCountCaps();
        requiredBlobsUseNewBudgetBeforeOptionalFiles();
        missingRequiredIsNeverComplete();
        recursiveReferencesAndSlices();
        originalAndBoundModulesRemainDistinct();
        exactUInt64AndExecutedClear();
        staleAndUnsafePathsAreRejected();
        missingAndMalformedJournalAreExplicit();
        budgetAndIntegrityErrorsRemainIncomplete();
        pipelineVariantsHaveNoCountCap();
        shaderFailuresPreserveExactMissingPath();
        System.out.println("DsLightingCaptureExport fixtures: " + assertions + " assertions passed");
    }
    private static void completeMinimalCapture() throws Exception {
        try (Fixture f = new Fixture()) {
            DsLightingCaptureExport.Plan plan = f.plan();
            DsLightingCaptureExport.Selection result = DsLightingCaptureExport.select(plan, Collections.emptyList());
            check(result.getRequiredComplete(), "minimal transport closure");
            check(plan.firstDraw == 100 && plan.lossDraw == 110, "executed clear to fixed loss");
            check(result.entries.size() == 4, "manifest, both statuses and journal retained");
            check(!DsLightingCaptureExport.summary(plan, result).has("p8"), "transport is not an oracle");
        }
    }
    private static void requiredFilesIgnoreOldCountCaps() throws Exception {
        try (Fixture f = new Fixture()) {
            for (int index = 1; index <= 110; index++) {
                String hash = String.format("%016x", index);
                String guest = "guest-shaders/vs_" + hash + "_probe.bin";
                f.write(guest, "guest");
                f.required(guest, 5);
                if (index <= 60) {
                    String module = "spv/vs_" + hash + "_0000000000000001.spv";
                    f.write(module, "native");
                    f.required(module, 6);
                }
            }
            DsLightingCaptureExport.Selection result = f.select();
            check(result.getRequiredComplete(), "over-48 and over-96 required modules are complete");
            check(paths(result).stream().filter(p -> p.startsWith("spv/")).count() == 60, "60 required SPV kept");
            check(paths(result).stream().filter(p -> p.startsWith("guest-shaders/")).count() == 110, "110 required guest kept");
        }
    }
    private static void requiredBlobsUseNewBudgetBeforeOptionalFiles() throws Exception {
        try (Fixture f = new Fixture()) {
            String snapshot = "queue-state/ds_prelight_witness_snapshot.bin";
            String operands = "queue-state/ds_prelight_witness_data_110.bin";
            // The historical #744 queue already occupies ~40 MB. Leave room
            // for it, a further 25 MiB snapshot and 32 MiB of operands/vertices.
            String previous = "queue-state/ds_prelight_witness_existing_anchors.bin";
            f.sized(snapshot, 25 * MIB);
            f.sized(operands, 32 * MIB);
            f.sized(previous, 40 * MIB);
            f.required(snapshot, 25 * MIB);
            f.required(operands, 32 * MIB);
            f.required(previous, 40 * MIB);
            List<File> optional = new ArrayList<>();
            for (int index = 0; index < 40; index++) optional.add(f.sized("buffers/optional_" + index + ".bin", 8 * MIB));
            DsLightingCaptureExport.Selection result = DsLightingCaptureExport.select(f.plan(), optional);
            check(result.getRequiredComplete(), "97 MiB mandatory data fits despite optional exhaustion");
            check(paths(result).contains(snapshot) && paths(result).contains(operands), "large mandatory files are included");
            check(result.totalBytes <= DsLightingCaptureExport.TOTAL_BUDGET, "global 160 MiB bound");
            check(result.queueBytes <= DsLightingCaptureExport.QUEUE_BUDGET, "queue 128 MiB bound");
            check(problem(result, "OPTIONAL_BUDGET_EXCEEDED", null), "optional overflow is reported");
            boolean optionalSeen = false;
            for (DsLightingCaptureExport.Entry entry : result.entries) {
                optionalSeen |= !entry.required;
                check(!optionalSeen || !entry.required, "required entry preceded optional entries");
            }
        }
    }
    private static void missingRequiredIsNeverComplete() throws Exception {
        try (Fixture f = new Fixture()) {
            String missing = "guest-shaders/vs_557d65da5b786842_probe.bin";
            f.required(missing, 16);
            DsLightingCaptureExport.Selection result = f.select();
            check(!result.getRequiredComplete(), "missing source never complete");
            check(problem(result, "REQUIRED_FILE_MISSING", missing), "exact missing guest reported");
        }
    }
    private static void recursiveReferencesAndSlices() throws Exception {
        try (Fixture f = new Fixture()) {
            String metadata = "queue-state/ds_prelight_witness_input_110.json";
            String blob = "queue-state/ds_prelight_witness_data_110.bin";
            f.sized(blob, 4096);
            f.write(metadata, "{\"vs_bound\":[{\"file\":\"ds_prelight_witness_data_110.bin\",\"offset_bytes\":64,\"bytes\":128}],\"extra_file\":\"buffers/index.bin\"}");
            f.write("buffers/index.bin", "idx");
            f.required(metadata, new File(f.root, metadata).length());
            f.required(blob, 4096);
            DsLightingCaptureExport.Selection result = f.select();
            check(result.getRequiredComplete(), "slice length is not mistaken for whole-file length");
            check(paths(result).contains(blob) && paths(result).contains("buffers/index.bin"), "relative and root references closed");
            check(!problem(result, "CONFLICTING_REQUIRED_LENGTH", blob), "slice does not conflict with manifest total");
        }
    }
    private static void exactUInt64AndExecutedClear() throws Exception {
        try (Fixture f = new Fixture()) {
            File journal = new File(f.root, "queue-state/ds_prelight_events_0.jsonl");
            String lines = new String(Files.readAllBytes(journal.toPath()), StandardCharsets.UTF_8)
                    .replace("\"vs\":0,\"ps\":0", "\"vs\":18446744073709551615,\"ps\":0");
            // A later planned clear that never executed must not replace draw100.
            lines += "{\"seq\":109,\"kind\":\"draw_prepared\",\"depth_target\":true,\"vs\":0,\"ps\":0}\n";
            f.write("queue-state/ds_prelight_events_0.jsonl", lines);
            f.write("guest-shaders/vs_ffffffffffffffff_probe.bin", "guest");
            DsLightingCaptureExport.Plan plan = f.plan();
            DsLightingCaptureExport.Selection result = DsLightingCaptureExport.select(plan, Collections.emptyList());
            check(plan.firstDraw == 100, "planned clear is not executed clear");
            check(paths(result).contains("guest-shaders/vs_ffffffffffffffff_probe.bin"), "UInt64 hash parsed exactly");
            check(!paths(result).contains("guest-shaders/vs_0000000000000000_probe.bin"), "zero hash is not a missing stage");
        }
    }
    private static void originalAndBoundModulesRemainDistinct() throws Exception {
        try (Fixture f = new Fixture()) {
            String original = "spv/vs_aff9c7ef505c6253_03110de384d15f6b.spv";
            String bound = "spv/vs_aff9c7ef505c6253_07509b385c7a9adc.spv";
            f.write(original, "original");
            f.write(bound, "instrumented");
            f.write("guest-shaders/vs_aff9c7ef505c6253_probe.bin", "guest");
            f.write("queue-state/ds_prelight_witness_vertex_110.json", "{\"original_vs_file\":\"" + original + "\",\"native_vs_file\":\"" + bound + "\"}");
            DsLightingCaptureExport.Selection result = f.select();
            check(result.getRequiredComplete(), "original and instrumented transport complete");
            check(paths(result).contains(original) && paths(result).contains(bound), "exact original and bound variants both retained");
        }
    }
    private static void staleAndUnsafePathsAreRejected() throws Exception {
        try (Fixture f = new Fixture()) {
            String current = "queue-state/ds_prelight_witness_old.bin";
            File old = f.write(current, "old");
            f.required(current, 3);
            File cached = f.write("guest-shaders/vs_0000000000000001_probe.bin", "guest");
            f.required("guest-shaders/vs_0000000000000001_probe.bin", 5);
            f.saveManifest();
            long launch = System.currentTimeMillis() - 1000;
            check(old.setLastModified(launch - 1000) && cached.setLastModified(launch - 1000), "mtime fixture setup");
            DsLightingCaptureExport.Selection result = DsLightingCaptureExport.select(DsLightingCaptureExport.discover(f.root, launch), Collections.emptyList());
            check(problem(result, "REQUIRED_FILE_STALE", current), "stale queue operand rejected");
            check(paths(result).contains("guest-shaders/vs_0000000000000001_probe.bin"), "older cached shader is valid transport");
            check(!result.getRequiredComplete(), "stale required file incomplete");
            check(!DsLightingCaptureExport.safeRelative("queue-state/../outside"), "traversal rejected");
            check(!DsLightingCaptureExport.safeRelative("/etc/passwd"), "absolute path rejected");
            check(!DsLightingCaptureExport.safeRelative("queue-state\\outside"), "backslash path rejected");
            File outside = Files.createTempFile("ds-export-outside-", ".bin").toFile();
            try {
                Files.write(outside.toPath(), new byte[]{1});
                Files.createDirectories(new File(f.root, "buffers").toPath());
                Files.createSymbolicLink(new File(f.root, "buffers/symlink.bin").toPath(), outside.toPath());
                f.required("buffers/symlink.bin", 1);
                result = f.select();
                check(problem(result, "REQUIRED_FILE_MISSING", "buffers/symlink.bin"), "symlink outside dump rejected");
            } finally { Files.deleteIfExists(outside.toPath()); }
        }
        try (Fixture f = new Fixture()) {
            File manifest = new File(f.root, "queue-state/ds_prelight_witness_foreground_manifest.json");
            long launch = System.currentTimeMillis();
            for (File file : new File(f.root, "queue-state").listFiles()) check(file.setLastModified(launch - 1000), "stale setup");
            DsLightingCaptureExport.Selection result = DsLightingCaptureExport.select(DsLightingCaptureExport.discover(f.root, launch), Collections.emptyList());
            check(problem(result, "REQUIRED_FILE_STALE", "queue-state/" + manifest.getName()), "stale active manifest cannot claim complete");
            check(!result.getRequiredComplete(), "old whole capture incomplete");
        }
    }
    private static void missingAndMalformedJournalAreExplicit() throws Exception {
        try (Fixture f = new Fixture()) {
            Files.delete(new File(f.root, "queue-state/ds_prelight_events_0.jsonl").toPath());
            DsLightingCaptureExport.Selection result = f.select();
            check(problem(result, "JOURNAL_MISSING", "queue-state"), "missing journal explicit");
            check(!result.getRequiredComplete(), "declared interval alone not complete");
            f.write("queue-state/ds_prelight_events_0.jsonl", "{\n");
            result = f.select();
            check(problem(result, "JOURNAL_PARSE_ERROR", "queue-state/ds_prelight_events_0.jsonl"), "bad journal explicit");
            check(!result.getRequiredComplete(), "malformed journal incomplete");
        }
    }
    private static void budgetAndIntegrityErrorsRemainIncomplete() throws Exception {
        try (Fixture f = new Fixture()) {
            String blob = "queue-state/ds_prelight_witness_data_110.bin";
            f.sized(blob, 2048);
            f.required(blob, 2048);
            DsLightingCaptureExport.Selection result = DsLightingCaptureExport.select(f.plan(), Collections.emptyList(), 1024, 1024);
            check(problem(result, "REQUIRED_BUDGET_EXCEEDED", blob), "required budget failure explicit");
            check(!result.getRequiredComplete(), "budget failure never complete");
            f.manifest.getJSONArray("required_files").getJSONObject(0).put("bytes", 2049).put("sha256", String.join("", Collections.nCopies(64, "0")));
            result = f.select();
            check(problem(result, "REQUIRED_LENGTH_MISMATCH", blob), "wrong manifest size explicit");
            check(problem(result, "REQUIRED_DIGEST_MISMATCH", blob), "wrong manifest hash explicit");
            check(paths(result).contains(blob), "mismatched evidence still retained");
            check(!result.getRequiredComplete(), "integrity mismatch never complete");
        }
    }
    private static void pipelineVariantsHaveNoCountCap() throws Exception {
        try (Fixture f = new Fixture()) {
            String module = "spv/vs_0000000000000001_0000000000000002.spv";
            f.write(module, "native");
            f.write("guest-shaders/vs_0000000000000001_probe.bin", "guest");
            f.required(module, 6);
            for (int index = 0; index < 30; index++) f.write("pipelines/gfx_" + index + ".json", "{\"stages\":[{\"spv\":\"" + module + "\"}]}");
            DsLightingCaptureExport.Selection result = f.select();
            check(paths(result).stream().filter(p -> p.startsWith("pipelines/")).count() == 30, "all30 matching pipeline variants kept");
            check(result.getRequiredComplete(), "pipeline closure complete");
        }
    }
    private static void shaderFailuresPreserveExactMissingPath() throws Exception {
        try (Fixture f = new Fixture()) {
            String module = "spv/vs_557d65da5b786842_0000000000000001.spv";
            f.write("queue-state/ds_prelight_shader_status.json", "{\"failure_count\":1,\"failures\":[{\"file\":\"" + module + "\",\"bytes\":65536,\"reason\":\"graphics_spv_budget_96MiB\"}]}");
            DsLightingCaptureExport.Selection result = f.select();
            check(problem(result, "REQUIRED_FILE_MISSING", module), "native write/budget failure remains explicit");
            check(!result.getRequiredComplete(), "missing failed shader incomplete");
        }
    }

    private static void auditRealCapture(File root) throws Exception {
        DsLightingCaptureExport.Plan plan = DsLightingCaptureExport.discover(root, 0);
        DsLightingCaptureExport.Selection result = DsLightingCaptureExport.select(plan, Collections.emptyList());
        check(!result.getRequiredComplete(), "known incomplete capture must remain incomplete");
        check(plan.firstDraw != null && plan.lossDraw != null, "historical cycle recovered");
        if (plan.firstDraw == 39624) {
            check(plan.lossDraw == 40091, "#744 fixed witness end");
            Set<String> missing = new LinkedHashSet<>();
            for (DsLightingCaptureExport.Problem issue : result.problems)
                if (issue.code.equals("REQUIRED_FILE_MISSING") && issue.path.startsWith("spv/")) missing.add(issue.path);
            Set<String> expected = new LinkedHashSet<>(Arrays.asList(
                    "spv/ps_afd9334e8581cda2_43f07dcc4471d2d4.spv", "spv/ps_c92deb23fc611b10_fb81a661bdb99988.spv",
                    "spv/ps_cfc1d9193e7f9e53_e24330d6dde61230.spv", "spv/ps_e31a5473e9ee7503_3618ac14cc2ada27.spv",
                    "spv/vs_2337192397a30339_264e0ec02c0e9232.spv", "spv/vs_a7208481a1f2b760_a338dff0ce2abb1a.spv"));
            check(missing.equals(expected), "exact six missing #744 native modules");
            check(problem(result, "REQUIRED_FILE_MISSING", "guest-shaders/vs_557d65da5b786842_probe.bin"), "#744 main foreground VS source missing");
        } else if (plan.firstDraw == 40367) check(plan.lossDraw == 40979, "#741 fixed witness end");
        else throw new AssertionError("unexpected historical cycle " + plan.firstDraw + " in " + root);
        System.out.println(DsLightingCaptureExport.summary(plan, result).toString(2));
    }
    public static void main(String[] args) throws Exception {
        runFixtureTests();
        for (String path : args) auditRealCapture(new File(path));
    }
}
