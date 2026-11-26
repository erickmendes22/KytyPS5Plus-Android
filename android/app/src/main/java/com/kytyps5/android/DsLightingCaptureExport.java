package com.kytyps5.android;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Required-file closure for the fixed F capture. No graphics/oracle verdict is inferred. */
public final class DsLightingCaptureExport {
    public static final long TOTAL_BUDGET = 160L * 1024 * 1024;
    public static final long QUEUE_BUDGET = 128L * 1024 * 1024;
    public static final long REQUIRED_FILE_LIMIT = 64L * 1024 * 1024;
    private static final long JSON_SCAN_LIMIT = 8L * 1024 * 1024;
    private static final Set<String> DIRECTORIES = new LinkedHashSet<>(Arrays.asList(
            "queue-state", "guest-shaders", "spv", "pipelines", "buffers"));
    private static final Pattern MODULE = Pattern.compile("spv/([a-z]+)_([0-9a-f]{16})_[0-9a-f]{16}\\.spv");
    private static final Pattern JOURNAL = Pattern.compile("ds_prelight_events_([0-9]+)\\.jsonl");
    private static final Pattern STAGE_HASH = Pattern.compile("\"(vs|ps)\"\\s*:\\s*(?:\"([0-9a-f]{16})\"|([0-9]+))");

    private DsLightingCaptureExport() { }

    public static final class Requirement {
        public final String path;
        public final Set<String> sources = new LinkedHashSet<>();
        public Long expectedBytes;
        public String expectedSha256;
        private Requirement(String path) { this.path = path; }
    }
    public static final class Problem {
        public final String code, path, detail;
        public Problem(String code, String path, String detail) {
            this.code = code; this.path = path; this.detail = detail;
        }
    }
    public static final class Plan {
        public final File root;
        public final long launchTime;
        public final List<Requirement> required;
        public final List<Problem> problems;
        public final Long firstDraw, lossDraw;
        private Plan(Discovery discovery) {
            root = discovery.root; launchTime = discovery.launchTime;
            required = new ArrayList<>(discovery.required.values());
            problems = new ArrayList<>(discovery.problems);
            firstDraw = discovery.firstDraw; lossDraw = discovery.lossDraw;
        }
    }
    public static final class Entry {
        public final String path;
        public final File file;
        public final boolean required;
        public final long bytes;
        public final String expectedSha256;
        private Entry(String path, File file, boolean required, long bytes, String expectedSha256) {
            this.path = path; this.file = file; this.required = required; this.bytes = bytes;
            this.expectedSha256 = expectedSha256;
        }
    }
    public static final class Selection {
        public final List<Entry> entries;
        public final List<Problem> problems;
        public final Set<String> requiredPaths;
        public final long totalBytes, queueBytes;
        private Selection(List<Entry> entries, List<Problem> problems, Set<String> requiredPaths,
                          long totalBytes, long queueBytes) {
            this.entries = entries; this.problems = problems; this.requiredPaths = requiredPaths;
            this.totalBytes = totalBytes; this.queueBytes = queueBytes;
        }
        public boolean getRequiredComplete() {
            for (Problem problem : problems) if (!problem.code.startsWith("OPTIONAL_")) return false;
            return true;
        }
        public Selection withProblem(Problem problem) {
            List<Problem> updated = new ArrayList<>(problems);
            updated.add(problem);
            return new Selection(entries, updated, requiredPaths, totalBytes, queueBytes);
        }
        public Selection withSkippedEntry(Entry skipped, Problem problem) {
            List<Entry> retained = new ArrayList<>(entries);
            retained.remove(skipped);
            List<Problem> updated = new ArrayList<>(problems);
            updated.add(problem);
            return new Selection(retained, updated, requiredPaths, totalBytes - skipped.bytes,
                    queueBytes - (skipped.path.startsWith("queue-state/") ? skipped.bytes : 0));
        }
    }
    private static final class Draw {
        final long id;
        final boolean depth;
        final Map<String, String> stages;
        Draw(long id, boolean depth, Map<String, String> stages) {
            this.id = id; this.depth = depth; this.stages = stages;
        }
    }
    private static final class Population {
        final long area, colored;
        final String identity;
        Population(JSONObject event) {
            JSONObject full = event.optJSONObject("full");
            area = full == null ? 0 : full.optLong("width") * full.optLong("height");
            colored = full == null ? -1 : full.optLong("rgb_nonzero", -1);
            JSONObject image = event.optJSONObject("image");
            identity = image == null ? "" : image.optString("owner") + ":" + image.optString("native")
                    + ":" + image.optString("format") + ":" + image.optString("width") + ":" + image.optString("height");
        }
    }

    /** A capture cannot direct the exporter outside its own dump directory. */
    public static boolean safeRelative(String path) {
        if (path == null || path.isEmpty() || path.startsWith("/") || path.indexOf('\\') >= 0 || path.indexOf('\0') >= 0)
            return false;
        String[] parts = path.split("/", -1);
        for (String part : parts)
            if (part.isEmpty() || part.equals(".") || part.equals("..") || part.indexOf(':') >= 0) return false;
        return path.equals("device.json") || (parts.length >= 2 && DIRECTORIES.contains(parts[0]));
    }
    private static File sourceFile(File root, String path) {
        if (!safeRelative(path)) return null;
        try {
            File base = root.getCanonicalFile();
            File file = new File(base, path);
            File resolved = file.getCanonicalFile();
            return resolved.getPath().startsWith(base.getPath() + File.separator)
                    && resolved.getPath().equals(file.getAbsolutePath()) ? file : null;
        } catch (IOException error) { return null; }
    }
    private static List<File> files(File directory) {
        File[] values = directory.listFiles();
        if (values == null) return Collections.emptyList();
        List<File> result = new ArrayList<>(Arrays.asList(values));
        result.sort(Comparator.comparing(File::getName));
        return result;
    }
    private static String text(File file) throws IOException {
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
    }

    public static Plan discover(File root, long launchTime) {
        Discovery discovery = new Discovery(root, launchTime);
        discovery.run();
        return new Plan(discovery);
    }
    private static final class Discovery {
        final File root;
        final long launchTime;
        final Map<String, Requirement> required = new LinkedHashMap<>();
        final List<Problem> problems = new ArrayList<>();
        final ArrayDeque<String> pending = new ArrayDeque<>();
        final Set<String> scanned = new LinkedHashSet<>();
        final Map<Long, Draw> draws = new LinkedHashMap<>();
        final List<long[]> clears = new ArrayList<>();
        final Map<Long, Population> before = new LinkedHashMap<>();
        Long firstDraw, lossDraw, lossSequence;
        Discovery(File root, long launchTime) { this.root = root; this.launchTime = launchTime; }

        void problem(String code, String path, String detail) { problems.add(new Problem(code, path, detail)); }
        void require(String path, String source) { require(path, source, null, null); }
        void require(String path, String source, Long bytes, String sha256) {
            if (!safeRelative(path)) { problem("INVALID_REQUIRED_PATH", path, source); return; }
            Requirement item = required.get(path);
            if (item == null) {
                item = new Requirement(path); required.put(path, item); pending.addLast(path);
            }
            item.sources.add(source);
            if (bytes != null && bytes >= 0) {
                if (item.expectedBytes != null && !item.expectedBytes.equals(bytes))
                    problem("CONFLICTING_REQUIRED_LENGTH", path, source + " bytes=" + bytes + " previous=" + item.expectedBytes);
                item.expectedBytes = bytes;
            }
            if (sha256 != null && !sha256.isEmpty()) {
                if (!sha256.matches("[0-9a-fA-F]{64}")) problem("INVALID_REQUIRED_DIGEST", path, source);
                else {
                    String normalized = sha256.toLowerCase(java.util.Locale.ROOT);
                    if (item.expectedSha256 != null && !item.expectedSha256.equals(normalized))
                        problem("CONFLICTING_REQUIRED_DIGEST", path, source);
                    item.expectedSha256 = normalized;
                }
            }
            Matcher module = MODULE.matcher(path);
            if (module.matches())
                require("guest-shaders/" + module.group(1) + "_" + module.group(2) + "_probe.bin", path);
        }
        void references(Object value, String source, String key) {
            if (value instanceof JSONObject) {
                JSONObject object = (JSONObject) value;
                String file = object.optString("file");
                if (!file.isEmpty()) {
                    String path = file.contains("/") ? file : source.substring(0, source.lastIndexOf('/') + 1) + file;
                    Long bytes = object.has("offset_bytes") || !object.has("bytes") ? null : object.optLong("bytes", -1);
                    require(path, source, bytes, object.optString("sha256"));
                }
                Iterator<String> keys = object.keys();
                while (keys.hasNext()) {
                    String child = keys.next();
                    if (!child.equals("file")) references(object.opt(child), source, child);
                }
            } else if (value instanceof JSONArray) {
                JSONArray array = (JSONArray) value;
                for (int index = 0; index < array.length(); index++) references(array.opt(index), source, key);
            } else if (value instanceof String && !((String) value).isEmpty()) {
                String string = (String) value;
                if (key.endsWith("_file") || key.equals("required_files") || key.equals("spv") || MODULE.matcher(string).matches()) {
                    String path = string.contains("/") ? string : source.substring(0, source.lastIndexOf('/') + 1) + string;
                    require(path, source);
                }
            }
        }
        void drainReferences() {
            while (!pending.isEmpty()) {
                String path = pending.removeFirst();
                if (!scanned.add(path) || !path.endsWith(".json")) continue;
                File file = sourceFile(root, path);
                if (file == null || !file.isFile()) continue;
                if (file.length() > JSON_SCAN_LIMIT) {
                    problem("REQUIRED_JSON_SCAN_LIMIT", path, "bytes=" + file.length()); continue;
                }
                try { references(new JSONObject(text(file)), path, ""); }
                catch (Exception error) { problem("REQUIRED_JSON_PARSE_ERROR", path, error.getClass().getSimpleName()); }
            }
        }
        Map<String, String> stageHashes(String line) {
            Map<String, String> result = new LinkedHashMap<>();
            Matcher match = STAGE_HASH.matcher(line);
            while (match.find()) {
                String value = match.group(2);
                if (value == null) {
                    BigInteger number = new BigInteger(match.group(3), 10);
                    if (number.bitLength() > 64) throw new IllegalArgumentException("shader hash exceeds UInt64");
                    value = number.toString(16);
                    value = "0000000000000000".substring(value.length()) + value;
                }
                if (!value.equals("0000000000000000")) result.put(match.group(1), value);
            }
            return result;
        }
        void inspectJournalEvent(JSONObject event, String line) {
            String kind = event.optString("kind");
            long sequence = event.optLong("seq", -1), draw = event.optLong("draw", -1);
            if (kind.equals("draw_prepared"))
                draws.put(sequence, new Draw(sequence, event.optBoolean("depth_target"), stageHashes(line)));
            else if (kind.equals("draw_execute") && event.optBoolean("depth_clear")) clears.add(new long[]{sequence, draw});
            else if (kind.equals("draw_before")) before.put(draw, new Population(event));
            else if (kind.equals("draw_after") && lossDraw == null) {
                Population current = new Population(event), previous = before.get(draw);
                if (event.optBoolean("witness_first_material_loss") ||
                        (previous != null && current.area > 0 && current.area == previous.area &&
                                !current.identity.isEmpty() && current.identity.equals(previous.identity) &&
                                previous.colored >= current.area / 2 && current.colored >= 0 && current.colored <= current.area / 10)) {
                    lossDraw = draw; lossSequence = sequence;
                }
            }
        }
        void readJournal(File file, boolean collectReferences, Set<Long> selected) {
            String path = "queue-state/" + file.getName();
            try (BufferedReader reader = Files.newBufferedReader(file.toPath(), StandardCharsets.UTF_8)) {
                String line;
                int number = 0;
                while ((line = reader.readLine()) != null) {
                    number++;
                    try {
                        JSONObject event = new JSONObject(line);
                        if (!collectReferences) inspectJournalEvent(event, line);
                        else if (selected.contains(event.optLong("draw", -1))) {
                            references(event, path, "");
                            Draw draw = draws.get(event.optLong("draw"));
                            if (event.optString("kind").equals("draw_inputs") && draw != null) {
                                for (Map.Entry<String, String> stage : draw.stages.entrySet())
                                    if (event.optString(stage.getKey() + "_file").isEmpty())
                                        problem("BOUND_MODULE_IDENTITY_MISSING", "draw=" + draw.id, stage.getKey() + "=" + stage.getValue());
                            }
                        }
                    } catch (Exception error) {
                        if (!collectReferences) problem("JOURNAL_PARSE_ERROR", path, "line=" + number + " " + error.getClass().getSimpleName());
                    }
                }
            } catch (IOException error) { problem("JOURNAL_READ_ERROR", path, error.getClass().getSimpleName()); }
        }
        // P8 observations are exported separately, but their guest-code
        // dependencies belong in this mandatory closure, not in the 96-file
        // optional shader selection for the early material-loss witness.
        void discoverP8GuestCode() {
            String path = "p8-reference/observations.jsonl";
            File file;
            try {
                File base = root.getCanonicalFile();
                file = new File(base, path);
                if (!file.exists()) return; // captures predating P8
                if (!file.getCanonicalFile().equals(file.getAbsoluteFile()) || !file.isFile()) {
                    problem("P8_OBSERVATIONS_UNSAFE", path, "not a regular capture file"); return;
                }
                if (launchTime != 0 && file.lastModified() < launchTime) return;
            } catch (IOException error) {
                problem("P8_OBSERVATIONS_READ_ERROR", path, error.getClass().getSimpleName()); return;
            }
            Pattern guest = Pattern.compile("guest-shaders/(cs|vs|ps|ms|ls|hs|ds)_[0-9a-f]{16}_probe\\.bin");
            int uses = 0, lineNumber = 0;
            try (BufferedReader reader = Files.newBufferedReader(file.toPath(), StandardCharsets.UTF_8)) {
                String line;
                while ((line = reader.readLine()) != null) {
                    ++lineNumber;
                    JSONObject event;
                    try { event = new JSONObject(line); }
                    catch (Exception error) {
                        problem("P8_OBSERVATIONS_PARSE_ERROR", path, "line=" + lineNumber); continue;
                    }
                    if (!"cut_use".equals(event.optString("kind"))) continue;
                    ++uses;
                    String source = path + ":" + lineNumber + " " + event.optString("marker");
                    JSONArray stages = event.optJSONArray("stages");
                    if (stages == null || stages.length() == 0) {
                        problem("P8_GUEST_STAGE_MISSING", path, source); continue;
                    }
                    for (int i = 0; i < stages.length(); ++i) {
                        JSONObject stage = stages.optJSONObject(i);
                        String code = stage == null ? "" : stage.optString("guest_file");
                        if (stage == null || !stage.optBoolean("valid") || !guest.matcher(code).matches()) {
                            problem("P8_GUEST_STAGE_MISSING", path, source + " stage=" + i);
                            continue;
                        }
                        require(code, source);
                    }
                }
            } catch (IOException error) {
                problem("P8_OBSERVATIONS_READ_ERROR", path, error.getClass().getSimpleName());
            }
            if (uses == 0) problem("P8_USE_CENSUS_MISSING", path, "no cut_use records");
        }
        void run() {
            discoverP8GuestCode();
            List<File> journals = new ArrayList<>();
            boolean indirectWitnessPresent = false;
            for (File file : files(new File(root, "queue-state"))) {
                if (!file.isFile() || (launchTime != 0 && file.lastModified() < launchTime)) continue;
                if (file.getName().startsWith("ds_prelight_")) require("queue-state/" + file.getName(), "fixed witness capture");
                indirectWitnessPresent |= file.getName().startsWith("ds_prelight_cp_args_");
                if (JOURNAL.matcher(file.getName()).matches()) journals.add(file);
            }
            // Producer generations can precede the retained 50-draw window.
            // Their own manifest closes every pinned blob/module dependency;
            // an event file alone must never claim a complete acquisition.
            if (indirectWitnessPresent)
                require("queue-state/ds_prelight_cp_args_manifest.json", "indirect producer/consumer closure");
            if (new File(root, "device.json").isFile()) require("device.json", "capture device");
            String manifestPath = "queue-state/ds_prelight_witness_foreground_manifest.json";
            require(manifestPath, "fixed witness manifest");
            require("queue-state/ds_prelight_status.json", "journal retention and loss status");
            require("queue-state/ds_prelight_shader_status.json", "bounded shader capture status");
            File manifestFile = sourceFile(root, manifestPath);
            JSONObject manifest = null;
            if (manifestFile != null && manifestFile.isFile() && manifestFile.length() <= JSON_SCAN_LIMIT) {
                try { manifest = new JSONObject(text(manifestFile)); }
                catch (Exception error) { problem("MANIFEST_PARSE_ERROR", manifestPath, error.getClass().getSimpleName()); }
            }
            if (manifest == null) problem("WITNESS_MANIFEST_MISSING", manifestPath, "journal fallback retained");
            else if (!manifest.optBoolean("active")) problem("WITNESS_MANIFEST_INACTIVE", manifestPath, "journal fallback retained");
            if (journals.isEmpty()) problem("JOURNAL_MISSING", "queue-state", "no current-session prelight event chunks");
            journals.sort(Comparator.comparing(file -> {
                Matcher match = JOURNAL.matcher(file.getName()); match.matches(); return new BigInteger(match.group(1));
            }));
            for (File file : journals) readJournal(file, false, Collections.emptySet());
            for (long[] clear : clears)
                if (lossSequence != null && clear[0] <= lossSequence && draws.containsKey(clear[1]) && draws.get(clear[1]).depth)
                    firstDraw = clear[1];
            Long declaredFirst = manifest == null || manifest.optLong("first_draw") <= 0 ? null : manifest.optLong("first_draw");
            Long declaredLoss = manifest == null || manifest.optLong("loss_draw") <= 0 ? null : manifest.optLong("loss_draw");
            if (declaredFirst != null && firstDraw != null && !declaredFirst.equals(firstDraw))
                problem("WITNESS_CYCLE_MISMATCH", manifestPath, "declared=" + declaredFirst + " executed=" + firstDraw);
            if (declaredLoss != null && lossDraw != null && !declaredLoss.equals(lossDraw))
                problem("WITNESS_LOSS_MISMATCH", manifestPath, "declared=" + declaredLoss + " observed=" + lossDraw);
            if (firstDraw == null) firstDraw = declaredFirst;
            if (lossDraw == null) lossDraw = declaredLoss;
            if (firstDraw == null || lossDraw == null || firstDraw > lossDraw)
                problem("WITNESS_CYCLE_UNRESOLVED", manifestPath, "first=" + firstDraw + " loss=" + lossDraw);
            Set<Long> selected = new LinkedHashSet<>();
            for (Draw draw : draws.values()) {
                if (firstDraw == null || lossDraw == null || draw.id < firstDraw || draw.id > lossDraw) continue;
                selected.add(draw.id);
                for (Map.Entry<String, String> stage : draw.stages.entrySet())
                    require("guest-shaders/" + stage.getKey() + "_" + stage.getValue() + "_probe.bin", "draw=" + draw.id);
            }
            for (File file : journals) readJournal(file, true, selected);
            drainReferences();
            Set<String> modules = new LinkedHashSet<>();
            for (String path : required.keySet()) if (MODULE.matcher(path).matches()) modules.add(path);
            // A bound module can have multiple recorded pipeline-state variants.
            // Preserve all matching records; they do not pass through take(24).
            for (File file : files(new File(root, "pipelines"))) {
                if (!file.isFile() || !file.getName().endsWith(".json") || file.length() > JSON_SCAN_LIMIT) continue;
                try {
                    Matcher matcher = MODULE.matcher(text(file));
                    while (matcher.find()) if (modules.contains(matcher.group())) {
                        require("pipelines/" + file.getName(), "pipeline for required bound module"); break;
                    }
                } catch (IOException error) { problem("PIPELINE_SCAN_ERROR", "pipelines/" + file.getName(), error.getClass().getSimpleName()); }
            }
            drainReferences();
        }
    }

    /** Mandatory files consume the budget first, without per-type count caps. */
    public static Selection select(Plan plan, List<File> optional) {
        return select(plan, optional, TOTAL_BUDGET, QUEUE_BUDGET);
    }
    public static Selection select(Plan plan, List<File> optional, long totalBudget, long queueBudget) {
        Selector selector = new Selector(plan, totalBudget, queueBudget);
        List<Requirement> requirements = new ArrayList<>(plan.required);
        requirements.sort(Comparator.comparingInt((Requirement item) -> {
            if (item.path.endsWith("manifest.json") || item.path.endsWith("status.json")) return 0;
            if (item.path.startsWith("spv/") || item.path.startsWith("guest-shaders/")) return 1;
            return item.path.endsWith(".json") ? 2 : 3;
        }).thenComparing(item -> item.path));
        for (Requirement item : requirements)
            selector.include(item.path, sourceFile(plan.root, item.path), true, item.expectedBytes, item.expectedSha256);
        for (File file : optional) {
            String path = plan.root.getAbsoluteFile().toPath().relativize(file.getAbsoluteFile().toPath()).toString().replace(File.separatorChar, '/');
            if (!selector.requiredPaths.contains(path)) selector.include(path, sourceFile(plan.root, path), false, null, null);
        }
        return new Selection(selector.entries, selector.problems, selector.requiredPaths, selector.total, selector.queue);
    }
    private static final class Selector {
        final Plan plan;
        final long totalBudget, queueBudget;
        final List<Entry> entries = new ArrayList<>();
        final List<Problem> problems;
        final Set<String> requiredPaths = new LinkedHashSet<>(), seen = new LinkedHashSet<>();
        long total, queue;
        Selector(Plan plan, long totalBudget, long queueBudget) {
            this.plan = plan; this.totalBudget = totalBudget; this.queueBudget = queueBudget;
            problems = new ArrayList<>(plan.problems);
            for (Requirement requirement : plan.required) requiredPaths.add(requirement.path);
        }
        void include(String path, File file, boolean required, Long expectedBytes, String expectedSha256) {
            if (!seen.add(path)) return;
            String prefix = required ? "REQUIRED" : "OPTIONAL";
            if (file == null || !file.isFile() || file.length() <= 0) {
                problems.add(new Problem(prefix + "_FILE_MISSING", path, "not a nonempty regular capture file")); return;
            }
            long size = file.length();
            if ((path.startsWith("queue-state/") || path.startsWith("buffers/")) &&
                    plan.launchTime != 0 && file.lastModified() < plan.launchTime) {
                problems.add(new Problem(prefix + "_FILE_STALE", path, "mtime=" + file.lastModified() + " launch=" + plan.launchTime)); return;
            }
            if (expectedBytes != null && expectedBytes != size)
                problems.add(new Problem(prefix + "_LENGTH_MISMATCH", path, "expected=" + expectedBytes + " actual=" + size));
            if (expectedSha256 != null) {
                try {
                    if (!expectedSha256.equals(sha256(file)))
                        problems.add(new Problem(prefix + "_DIGEST_MISMATCH", path, "sha256 differs from manifest"));
                } catch (IOException error) {
                    problems.add(new Problem(prefix + "_READ_FAILED", path, error.getClass().getSimpleName())); return;
                }
            }
            long limit = required ? REQUIRED_FILE_LIMIT : 8L * 1024 * 1024;
            boolean queueFile = path.startsWith("queue-state/");
            if (size > limit || size > totalBudget - total || (queueFile && size > queueBudget - queue)) {
                problems.add(new Problem(prefix + "_BUDGET_EXCEEDED", path,
                        "bytes=" + size + " file_limit=" + limit + " total_remaining=" + (totalBudget - total)
                                + " queue_remaining=" + (queueBudget - queue))); return;
            }
            entries.add(new Entry(path, file, required, size, expectedSha256));
            total += size;
            if (queueFile) queue += size;
        }
    }
    private static String sha256(File file) throws IOException {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            try (InputStream input = Files.newInputStream(file.toPath())) {
                byte[] buffer = new byte[64 * 1024];
                int count;
                while ((count = input.read(buffer)) >= 0) digest.update(buffer, 0, count);
            }
            StringBuilder result = new StringBuilder();
            for (byte value : digest.digest()) result.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
            return result.toString();
        } catch (NoSuchAlgorithmException impossible) { throw new IllegalStateException(impossible); }
    }
    public static JSONObject summary(Plan plan, Selection selection) throws JSONException {
        JSONObject result = new JSONObject();
        result.put("schema", 1).put("scope", "export file closure only; no G0/P8 or gameplay verdict");
        result.put("first_draw", plan.firstDraw).put("loss_draw", plan.lossDraw);
        result.put("required_complete", selection.getRequiredComplete());
        result.put("required_files", new JSONArray(selection.requiredPaths));
        JSONArray included = new JSONArray(), problems = new JSONArray();
        for (Entry entry : selection.entries) if (entry.required) included.put(entry.path);
        for (Problem problem : selection.problems)
            problems.put(new JSONObject().put("code", problem.code).put("file", problem.path).put("detail", problem.detail));
        result.put("included_required", included).put("total_bytes", selection.totalBytes).put("queue_bytes", selection.queueBytes);
        result.put("problems", problems);
        return result;
    }
}
