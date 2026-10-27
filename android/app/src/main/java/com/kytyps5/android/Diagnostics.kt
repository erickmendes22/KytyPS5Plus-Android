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
                        (it.extension == "log" || it.extension == "txt") &&
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
                appendLine("Exit reports returned: ${exits.size}")
                for ((index, info) in exits.withIndex()) {
                    appendLine("---- Exit $index ----")
                    appendLine("Time: ${Date(info.timestamp)}")
                    appendLine("Process: ${info.processName}")
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

            count += exportGtTessellationCapture(context, zip)

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
     * Capture the last ENTERed PPSA24156 tessellation pipeline. Queued
     * pipelines are excluded: only an ENTERed call reaches the Vulkan driver.
     * Include its exact SPIR-V modules and fixed-function pipeline JSON.
     */
    private fun exportGtTessellationCapture(context: Context, zip: ZipOutputStream): Int {
        val logFile = File(context.filesDir, "logs/session.log")
        val dump = File(context.filesDir, "gt-pipeline-dumps")
        if (!logFile.isFile || !dump.isDirectory) return 0
        return try {
            val records = logFile.readLines()
            val entered = records.lastOrNull { it.startsWith("GT tess pipeline ENTER #") }
                ?: return 0
            val serial = Regex("""ENTER #(\d+)""").find(entered)
                ?.groupValues?.getOrNull(1) ?: return 0
            val begin = records.lastOrNull { it.startsWith("GT pipeline BEGIN #$serial ") }
                ?: return 0
            val hashRegion = begin.substringAfter(" hashes=", "").substringBefore(" topology=", "")
            val hashes = Regex("""0x([a-fA-F0-9]{16})""").findAll(hashRegion)
                .map { it.groupValues[1].lowercase(Locale.ROOT) }.toList()
            if (hashes.size < 3) return 0

            val files = mutableListOf<File>()
            val prefix = "gfx_" + hashes.joinToString("_") + "_"
            File(dump, "pipelines").listFiles()
                ?.filter { it.isFile && it.extension == "json" && it.name.startsWith(prefix) }
                ?.sortedByDescending { it.lastModified() }?.take(3)?.let(files::addAll)
            val spvs = File(dump, "spv").listFiles().orEmpty()
            for (hash in hashes) {
                spvs.filter { it.isFile && it.extension == "spv" && it.name.contains("_${hash}_") }
                    .sortedByDescending { it.lastModified() }.take(3).let(files::addAll)
            }
            File(dump, "device.json").takeIf { it.isFile }?.let(files::add)

            var budget = 16L * 1024L * 1024L
            var exported = 0
            val note = buildString {
                appendLine("PPSA24156 last entered tessellation pipeline #$serial")
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
