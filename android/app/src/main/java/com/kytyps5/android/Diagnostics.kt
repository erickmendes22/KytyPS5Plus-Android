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
        val producerShaders = setOf("afd9334e8581cda2", "e31a5473e9ee7503", "b631777eb4aee68c")
        val postShaders = setOf("3265b01ed00a5c01", "17defa333d9d1e22", "d1768d02fcf96a58",
            "844e5697cf62ecd7", "a5fc1658592e3cba", "41a57a218d07a778")
        val watched = queueShaders + producerShaders + postShaders + setOf(
            "3dfe402653925fbf", "6cbfe18ea2005263", "1ce1a858f62b5a11",
            "c2e01fbe98f5ebab", "23056085c1120461", "fabe0bcb275a3005",
            "e75589aa0bb3ae52", "93b0be7f1a0293d1"
        )
        return try {
            val candidates = listOf("queue-state", "spv", "pipelines", "buffers").flatMap { subdir ->
                File(dump, subdir).listFiles().orEmpty()
                    .filter { file ->
                        file.isFile && file.length() > 0L &&
                            watched.any { hash -> file.name.contains("_${hash}_") } &&
                            (when (subdir) {
                                "spv" -> file.extension == "spv"
                                "pipelines" -> file.extension == "json"
                                "buffers" -> file.extension == "bin"
                                "queue-state" -> file.extension == "json" || file.extension == "bin"
                                else -> false
                            })
                    }
                    .sortedWith(compareByDescending<File> { file ->
                        (queueShaders + producerShaders).any { hash -> file.name.contains("_${hash}_") }
                    }.thenByDescending { it.lastModified() })
                    .take(if (subdir == "queue-state") 72 else 24)
                    .map { subdir to it }
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
                "Join DS_POST_PASS, DS_POST_BINDING and DS_POST_TARGET by light_frame and pass.\n"
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
