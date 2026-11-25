package com.kytyps5.android

import android.app.ActivityManager
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Process
import android.util.Log
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * Crash diagnostics from the app's own private files and Android's own process exit history.
 * No root, READ_LOGS or broad logcat access is needed.
 */
object Diagnostics {
    private const val TAG = "KytyDiagnostics"
    private const val MAX_LOG_BYTES = 16L * 1024L * 1024L
    private const val MAX_TOMBSTONE_BYTES = 3L * 1024L * 1024L
    private const val MAX_EXITS = 8

    fun saveLaunch(context: Context, path: String, args: Array<String>) {
        CrashDiagnostics.preserveNativeBeforeLaunch(context)
        CrashDiagnostics.start(context, path)
        try {
            val dir = File(context.filesDir, "logs")
            if (!dir.isDirectory && !dir.mkdirs()) throw IOException("Cannot create ${dir.path}")
            File(dir, "last-launch.txt").writeText(buildString {
                appendLine("KytyPS5 Plus launch diagnostics")
                appendLine("Package: ${context.packageName}")
                appendLine("Version: ${context.packageManager.getPackageInfo(context.packageName, 0).versionName}")
                appendLine("Date: ${Date()}")
                appendLine("PID: ${Process.myPid()}")
                appendLine("Game: $path")
                appendLine("Arguments:")
                for (argument in args) appendLine(argument)
                appendLine("Device: ${Build.MANUFACTURER} ${Build.MODEL} (API ${Build.VERSION.SDK_INT})")
                appendLine("ABI: ${Build.SUPPORTED_ABIS.joinToString()}")
            })
        } catch (e: Exception) {
            Log.e(TAG, "Failed to save launch metadata", e)
        }
    }

    fun suggestedFileName(): String =
        "KytyPS5-Plus-diagnostics-" +
            SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date()) + ".zip"

    /** May run on a worker thread. Writes directly to a user-selected SAF destination. */
    fun export(context: Context, destination: Uri): String {
        val stream = context.contentResolver.openOutputStream(destination, "w")
            ?: throw IOException("The selected destination could not be opened")
        var count = 0
        ZipOutputStream(stream.buffered()).use { zip ->
            val logs = File(context.filesDir, "logs")
            val files = logs.listFiles()
                ?.filter {
                    it.isFile && it.length() > 0 &&
                        (it.extension == "log" || it.extension == "txt" || it.name == "signal-origin.txt.stack") &&
                        it.name != "host-faults.log" && it.name != "fatal-signals.log"
                }
                ?.sortedBy { it.name }
                .orEmpty()
            for (file in files) {
                if (file.length() <= MAX_LOG_BYTES) {
                    zip.putNextEntry(ZipEntry("logs/${file.name}"))
                    file.inputStream().use { it.copyTo(zip) }
                    zip.closeEntry()
                    count++
                }
            }

            val activityManager = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val exits = try {
                activityManager.getHistoricalProcessExitReasons(context.packageName, 0, MAX_EXITS)
            } catch (e: SecurityException) {
                Log.w(TAG, "Cannot query process exits", e)
                emptyList()
            }
            zip.putNextEntry(ZipEntry("process-exits.txt"))
            val exitReport = buildString {
                appendLine("Package: ${context.packageName}")
                appendLine("Generated: ${Date()}")
                appendLine("Low-memory kill reporting supported: ${ActivityManager.isLowMemoryKillReportSupported()}")
                appendLine("Exit reports returned: ${exits.size}")
                for ((index, info) in exits.withIndex()) {
                    appendLine("---- Exit $index ----")
                    appendLine("Time: ${Date(info.timestamp)}")
                    appendLine("Process: ${info.processName}")
                    appendLine("PID: ${info.pid} UID: ${info.realUid} packageUID: ${info.packageUid}")
                    appendLine("Importance: ${info.importance}")
                    appendLine("Process state summary: ${info.processStateSummary?.toString(Charsets.UTF_8)}")
                    appendLine("Reason code: ${info.reason}")
                    appendLine("Status/signal: ${info.status}")
                    appendLine("Description: ${info.description}")
                    appendLine("PSS/RSS KB: ${info.pss}/${info.rss}")
                }
            }
            zip.write(exitReport.toByteArray(Charsets.UTF_8))
            zip.closeEntry()
            count++

            for ((index, info) in exits.withIndex()) {
                try {
                    val trace = info.traceInputStream ?: continue
                    trace.use {
                        val name = "process-exit-trace-$index.txt"
                        zip.putNextEntry(ZipEntry(name))
                        copyAtMost(it, zip, MAX_TOMBSTONE_BYTES)
                        zip.closeEntry()
                        count++
                    }
                } catch (e: Exception) {
                    Log.w(TAG, "Could not export exit trace $index", e)
                }
            }

            count += CrashDiagnostics.export(context, zip)
            count += exportGtTessellationCapture(context, zip)
            count += exportDsLightingCapture(context, zip)

            if (files.isEmpty()) {
                zip.putNextEntry(ZipEntry("HOW_TO_COLLECT.txt"))
                zip.write(
                    ("No native session log was found. Reproduce the crash using a build " +
                        "with native file logging enabled, then export again. Android process " +
                        "exit metadata is available in process-exits.txt.\n").toByteArray(),
                )
                zip.closeEntry()
                count++
            }
        }
        return "Exported $count diagnostic file(s)"
    }


    /**
     * Prefer the graphics pipeline that returned an error. Fall back to the last
     * ENTERed tessellation call when a driver crash prevents a result record.
     * Export the exact SPIR-V filenames referenced by its pipeline JSON.
     */
    private fun exportGtTessellationCapture(context: Context, zip: ZipOutputStream): Int {
        val logFile = File(context.filesDir, "logs/session.log")
        val dump = File(context.filesDir, "gt-pipeline-dumps")
        if (!logFile.isFile || !dump.isDirectory) return 0
        return try {
            val records = logFile.readLines()
            val entered = records.lastOrNull {
                it.startsWith("GT pipeline END #") &&
                    !it.substringAfter(" result=", "").substringBefore(' ').let { result ->
                        result.isEmpty() || result == "Success"
                    }
            } ?: records.lastOrNull { it.startsWith("GT tess pipeline ENTER #") } ?: return 0
            val serial = Regex("""#(\d+)""").find(entered)
                ?.groupValues?.getOrNull(1) ?: return 0
            val begin = records.lastOrNull { it.startsWith("GT pipeline BEGIN #$serial ") }
                ?: return 0
            val hashRegion = begin.substringAfter(" hashes=", "").substringBefore(" topology=", "")
            val hashes = Regex("""0x([a-fA-F0-9]{16})""").findAll(hashRegion)
                .map { it.groupValues[1].lowercase(Locale.ROOT) }.toList()
            if (hashes.isEmpty()) return 0

            val files = mutableListOf<File>()
            val prefix = "gfx_" + hashes.joinToString("_") + "_"
            val exactName = records.lastOrNull { it.contains("dump record: $prefix") }
                ?.substringAfter("dump record: ")?.trim()
                ?.takeIf { it.startsWith(prefix) && it.endsWith(".json") && '/' !in it && '\\' !in it }
            val exact = exactName?.let { File(dump, "pipelines/$it") }?.takeIf { it.isFile }
            val pipelineFiles = if (exact != null) listOf(exact) else
                File(dump, "pipelines").listFiles().orEmpty()
                    .filter { it.isFile && it.extension == "json" && it.name.startsWith(prefix) }
                    .sortedByDescending { it.lastModified() }.take(3)
            files.addAll(pipelineFiles)
            for (pipelineFile in pipelineFiles) {
                val stages = org.json.JSONObject(pipelineFile.readText()).optJSONArray("stages")
                    ?: continue
                for (i in 0 until stages.length()) {
                    val relative = stages.optJSONObject(i)?.optString("spv").orEmpty()
                    if (!relative.startsWith("spv/") || relative.split('/').contains("..") ||
                        '\\' in relative) continue
                    File(dump, relative).takeIf { it.isFile }?.let(files::add)
                }
            }
            File(dump, "device.json").takeIf { it.isFile }?.let(files::add)

            var budget = 16L * 1024L * 1024L
            var exported = 0
            val note = buildString {
                appendLine("PPSA24156 selected graphics pipeline #$serial")
                appendLine(begin)
                appendLine(entered)
                appendLine("Shaders: ${hashes.joinToString(" ")}")
                appendLine("Includes original shader SPIR-V and Vulkan pipeline JSON, if present.")
            }
            zip.putNextEntry(ZipEntry("gt-pipeline-capture/README.txt"))
            zip.write(note.toByteArray(Charsets.UTF_8))
            zip.closeEntry()
            exported++
            for (file in files.distinctBy { it.absolutePath }.take(18)) {
                if (file.length() <= 0 || file.length() > 4L * 1024L * 1024L ||
                    file.length() > budget) continue
                val folder = if (file.parentFile?.name == "spv") "spv"
                    else if (file.parentFile?.name == "pipelines") "pipelines" else ""
                val relative = if (folder.isEmpty()) file.name else "$folder/${file.name}"
                zip.putNextEntry(ZipEntry("gt-pipeline-capture/$relative"))
                file.inputStream().use { it.copyTo(zip) }
                zip.closeEntry()
                budget -= file.length()
                exported++
            }
            exported
        } catch (e: Exception) {
            Log.w(TAG, "Could not export Gran Turismo shader capture", e)
            0
        }
    }

    /**
     * Demon’s Souls GPU light capture. Export the queue shaders, pipeline layouts,
     * resolved inputs and GPU light lists alongside the frame-correlated session log.
     */
    private fun exportDsLightingCapture(context: Context, zip: ZipOutputStream): Int {
        val dump = File(context.filesDir, "ds-lighting-dumps")
        if (!dump.isDirectory) return 0
        val queueShaders = setOf("f0c482b0b355e9de", "e3daeb1e7426761b", "32c7da582db1795a")
        val producerShaders = setOf("afd9334e8581cda2", "e31a5473e9ee7503", "b631777eb4aee68c",
            "cfc1d9193e7f9e53", "c92deb23fc611b10", "684e797dce67bb8e")
        // Vulkan already records this 360-layer light-atlas storage writer, but
        // older Android exports silently excluded its SPIR-V and pipeline JSON.
        val atlasWriterShaders = setOf("c03afccb96a13c5d")
        val coefficientWriterShaders = setOf("2f493aadc660cf03", "567d21ad45386c8d",
            "bcbcc6ba1e68c974", "acce347eef557bd2")
        val postShaders = setOf("3265b01ed00a5c01", "17defa333d9d1e22", "d1768d02fcf96a58",
            "844e5697cf62ecd7", "a5fc1658592e3cba", "41a57a218d07a778")
        val countProducers = setOf("cb3b9c1ea64fe876", "c387ee3c610ed66b", "b89a7139ba15ee52", "3b7e54e11ee65120", "fb18d110fab8234e", "2454ae3e3a6cbdab",
            "b16650e4c9acfddd", "d45e1b87d6741f63")
        val dominantShaders = countProducers + setOf("f0c140ea8df8f532", "2337192397a30339", "651f53f5932d8794", "aff9c7ef505c6253",
            "c96f3fcde8141266", "552d823dcfcf2dee",
            "34c51585c926a664", "a7208481a1f2b760", "3dce2bd78d58db62", "895c9fe037e704f5",
            "f825a988127faeb4", "74b55dea9562eda1", "cfc1d9193e7f9e53", "c92deb23fc611b10")
        val watched = dominantShaders + queueShaders + producerShaders + atlasWriterShaders + coefficientWriterShaders + postShaders + setOf(
            "3dfe402653925fbf", "6cbfe18ea2005263", "1ce1a858f62b5a11",
            "c2e01fbe98f5ebab", "23056085c1120461", "fabe0bcb275a3005",
            "e75589aa0bb3ae52", "93b0be7f1a0293d1"
        )
        return try {
            // Queue data belongs to one launch; cached shaders and pipelines
            // can legitimately predate it. Do not silently mix old ray rows
            // with the current session's identical frame numbers.
            val launchTime = File(context.filesDir, "logs/last-launch.txt").lastModified()
            val candidates = listOf("queue-state", "guest-shaders", "spv", "pipelines", "buffers").flatMap { subdir ->
                val files = File(dump, subdir).listFiles().orEmpty()
                    .filter { file ->
                        file.isFile && file.length() > 0L &&
                            ((subdir != "queue-state" && subdir != "buffers") ||
                                launchTime == 0L || file.lastModified() >= launchTime) &&
                            (subdir == "guest-shaders" ||
                                (subdir == "queue-state" &&
                                    (file.name.matches(Regex("ds_ray_producer_f[0-9]+_s[0-9]+(_t[0-9]+)?(_target)?\\.json")) ||
                                     file.name.matches(Regex("ds_relight_trace_f[0-9]+_s[0-9]+_p[0-9]+\\.(json|bin)")) ||
                                     file.name.matches(Regex("ds_vertex_position_[0-9a-f]{16}_f[0-9]+_s[0-9]+_p[0-9]+\\.json")) ||
                                     file.name.matches(Regex("ds_geometry_coverage_f[0-9]+\\.json")) ||
                                     file.name.matches(Regex("ds_geometry_writer_[0-9a-f]{16}_f[0-9]+_s[0-9]+_p[0-9]+\\.json")) ||
                                     file.name.matches(Regex("ds_dominant_material_f[0-9]+_s[0-9]+\\.json")) ||
                                     file.name.matches(Regex("ds_depth_prepass_f[0-9]+_s[0-9]+_p[0-9]+\\.json")) ||
                                     file.name.matches(Regex("ds_scene_writer_f[0-9]+_p[0-9]+\\.json")) ||
                                     file.name.matches(Regex("ds_light_bridge_(bright|onset|paired_dark|post_transition)_f[0-9]+\\.json")) ||
                                     file.name.matches(Regex("ds_gbuffer_material_(bright|onset|dark|post_transition)_f[0-9]+_s[0-9]+(_ps[0-9a-f]{16})?\\.json")))) ||
                                watched.any { hash -> file.name.contains("_${hash}_") }) &&
                            (when (subdir) {
                                "spv" -> file.extension == "spv"
                                "pipelines" -> file.extension == "json"
                                "buffers" -> file.extension == "bin"
                                "queue-state" -> file.extension == "json" || file.extension == "bin"
                                "guest-shaders" -> file.name.matches(Regex("(cs|vs|ps|ms|ls|hs|ds)_[0-9a-f]{16}_probe\\.bin"))
                                else -> false
                            })
                    }
                    .sortedWith(compareByDescending<File> { file ->
                        countProducers.any { hash -> file.name.contains("_${hash}_") }
                    }.thenByDescending { file ->
                        dominantShaders.any { hash -> file.name.contains("_${hash}_") } || file.name.startsWith("ds_dominant_material_f")
                    }.thenByDescending { file ->
                        file.name.startsWith("ds_gbuffer_material_") ||
                            file.name.startsWith("ds_light_bridge_") ||
                            file.name.startsWith("ds_scene_writer_f") ||
                            file.name.startsWith("ds_ray_producer_f")
                    }.thenByDescending { file ->
                        coefficientWriterShaders.any { hash -> file.name.contains("_${hash}_") }
                    }.thenByDescending { file ->
                        (queueShaders + producerShaders + atlasWriterShaders).any { hash -> file.name.contains("_${hash}_") }
                    }.thenByDescending { file ->
                        postShaders.any { hash -> file.name.contains("_${hash}_") }
                    }.thenByDescending { it.lastModified() })
                val selected = if (subdir == "queue-state") {
                    // A busy scene can produce more than 96 small JSON
                    // files. Give the producer captures their own slots so
                    // the ZIP actually contains the data cited by the log.
                    val newest = files.sortedByDescending { it.lastModified() }
                    val geometry = newest.filter { it.name.startsWith("ds_geometry_writer_") }
                    val countTraces = newest.filter { it.name.startsWith("ds_count_producer_") }
                    val upstreamCounts = countTraces.filter { it.name.contains("cb3b9c1ea64fe876") }.take(20)
                    val rayTargets = newest.filter { it.name.startsWith("ds_ray_producer_f") && it.name.endsWith("_target.json") }
                    // Retain both the first and last matched updates of the actually consumed probes.
                    val prioritizedRays = (rayTargets.takeLast(10) + rayTargets.take(20)).distinct()
                    val relightTraces = newest.filter { it.name.startsWith("ds_relight_trace_") }.take(18)
                    val producer = relightTraces + prioritizedRays +
                        newest.filter { it.name.startsWith("ds_vertex_position_") }.take(24) +
                        countTraces.filterNot { it.name.contains("cb3b9c1ea64fe876") }
                        .sortedByDescending { it.name.endsWith("_p3.json") }.take(40) + upstreamCounts +
                        geometry.filter { it.name.contains("f0c140ea8df8f532") }.take(4) +
                        geometry.filterNot { it.name.contains("f0c140ea8df8f532") }.take(20) +
                        newest.filter { it.name.startsWith("ds_dominant_material_f") }.take(2) +
                        newest.filter { it.name.startsWith("ds_geometry_coverage_f") }.take(1) +
                        newest.filter { it.name.startsWith("ds_depth_prepass_f") }.take(4) +
                        newest.filter { it.name.startsWith("ds_ray_producer_f") && !it.name.endsWith("_target.json") }.take(16) +
                        newest.filter { it.name.startsWith("cs_c03afccb96a13c5d_atlas_inputs_") }
                        .take(18) +
                        newest.filter { it.name.startsWith("cs_bcbcc6ba1e68c974_ray_hits_") }.take(18) +
                        newest.filter { it.name.startsWith("cs_567d21ad45386c8d_radiance_") }.take(16)
                    producer + (files - producer.toSet()).take((96 - producer.size).coerceAtLeast(0))
                } else {
                    files.take(when (subdir) {
                        "guest-shaders" -> 96
                        "spv" -> 48
                        else -> 24
                    })
                }
                selected.map { subdir to it }
            }
            var count = 0
            var budget = 40L * 1024L * 1024L
            val info = "PPSA01341 lighting shaders and GPU buffer capture. " +
                "The session.log entries [DS_GPU_BUFFER_CAPTURE] contain native Vulkan " +
                "buffer summaries; buffers/*.bin contain the actual bytes. " +
                "Join DS_QUEUE_TILES, DS_QUEUE_CAPTURE, DS_QUEUE_IMAGE, DS_QUEUE_EXECUTED and " +
                "DS_QUEUE_DRAW by frame and queue. queue-state contains the most recent " +
                "binding snapshots and GPU tile lists. Join DS_GBUFFER_DRAW, DS_GBUFFER_PIXELS, " +
                "DS_GBUFFER_PIXEL to DS_MATERIAL_LIGHT and DS_MATERIAL_HANDOFF by draw_frame and material shader. " +
                "The material lighting image markers identify the immediately following pixel probe. " +
                "DS_LIGHT_MATH_VARIANT identifies instrumented shader binaries. DS_LIGHT_MATH records the exact " +
                "shader values for the queue 33 pixels. Join DS_LIGHT_TERM by frame, submit and sample. Terms " +
                "report the last active sample and total contribution before/after shadow weighting at each site. " +
                "DS_LIGHT_ATLAS_TEXELS reports the raw RGB16 footprint and DS_LIGHT_ATLAS_WRITER the recent " +
                "overlapping output bindings. DS_LIGHT_ATLAS_INPUT identifies the queue-state JSON with the " +
                "atlas writer's live SRT words, workgroup Z indices, selected coefficient words and raw " +
                "atlas texels copied immediately after that dispatch. " +
                "Use the in-bounds fields to distinguish zero values from reads outside the bound slots. " +
                "The coefficient_writer object compares the same records immediately before and after the " +
                "coefficient dispatch and at the atlas. Its matched_source fields contain slot 3 coefficients " +
                "and slot 2 weight bits for the same probe IDs. DS_COEFFICIENT_PAIRED confirms a matching " +
                "writer and atlas in the same frame. inputs_before.source_writer gives the upstream binding " +
                "provenance. When available, source_dispatch and source_words_after_writer describe the " +
                "dispatch producing the slot 3 values. For 567d21ad45386c8d, probe_indices maps " +
                "workgroup Z to the output record. radiance_file names the bound image in queue-state " +
                "as tightly packed little-endian RGBA16F rows. guest-shaders contains their guest compute code. " +
                "Compare radiance_digest with sampled_compute_writer.radiance_digest and join " +
                "DS_RADIANCE_IMAGE_WRITER, DS_RADIANCE_BUFFER_WRITER and DS_RADIANCE_CACHE by tick and owner. " +
                "The relight_inputs record lists the exact probe indices, ray-hit summaries and light masks " +
                "read by bcbcc6ba1e68c974, with bound-buffer writer candidates. " +
                "selected_lights stores the records used by the two-level light mask; mask_writer stores " +
                "the upstream producer's live bindings, inputs and GPU mask words from the same dispatch. " +
                "probe_positions and probe_bounds are indexed by probe_indices. Full paired snapshots " +
                "and low-radiance snapshots have separate storage from periodic atlas samples. " +
                "A DS_QUEUE_TILES record with radiance_paired=true shares its frame with an atlas or radiance capture. " +
                "Join DS_POST_PASS, DS_POST_BINDING and DS_POST_TARGET by light_frame and pass. " +
                "ds_gbuffer_material_* captures the same 8x8 fixed R32_UINT grid before and " +
                "after a verified material draw in bright/onset/dark/post-transition candidate phases, with color masks, " +
                "depth/stencil, exact D32 depth samples and load state; shader hashes are part of " +
                "the filename so materials sharing a submit cannot overwrite one another. " +
                "ds_light_bridge_* captures the same 8x8 normalized grid of R32_UINT " +
                "G-buffer albedo and R16G16B16A16 HDR immediately after queue 33, " +
                "in observed bright, onset, paired-dark and post-transition candidate phases. " +
                "ds_scene_writer_f*_p*.json captures the 0x17defa... scene writer at " +
                "observed bright, first dark onset and paired dark phases, with fixed " +
                "64-point pre/post grids and shader buffer bindings.\n"
            zip.putNextEntry(ZipEntry("ds-lighting-capture/README.txt"))
            zip.write(info.toByteArray(Charsets.UTF_8))
            zip.closeEntry()
            count++
            File(dump, "device.json").takeIf { it.isFile && it.length() in 1L..65536L }?.let { file ->
                zip.putNextEntry(ZipEntry("ds-lighting-capture/device.json"))
                file.inputStream().use { it.copyTo(zip) }
                zip.closeEntry()
                count++
            }
            for ((subdir, file) in candidates) {
                if (file.length() > 8L * 1024L * 1024L || file.length() > budget) continue
                zip.putNextEntry(ZipEntry("ds-lighting-capture/${subdir}/${file.name}"))
                file.inputStream().use { it.copyTo(zip) }
                zip.closeEntry()
                budget -= file.length()
                count++
            }
            count
        } catch (e: Exception) {
            Log.w(TAG, "Could not export Demon's Souls native lighting capture", e)
            0
        }
    }

    private fun copyAtMost(input: InputStream, output: ZipOutputStream, maxBytes: Long) {
        val buffer = ByteArray(8192)
        var remaining = maxBytes
        while (remaining > 0) {
            val read = input.read(buffer, 0, minOf(buffer.size.toLong(), remaining).toInt())
            if (read <= 0) break
            output.write(buffer, 0, read)
            remaining -= read
        }
    }
}
