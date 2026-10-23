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
                appendLine("KytyPS5+ launch diagnostics")
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
        "KytyPS5-diagnostics-" +
            SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date()) + ".zip"

    /** May run on a worker thread. Writes directly to a user-selected SAF destination. */
    fun export(context: Context, destination: Uri): String {
        val stream = context.contentResolver.openOutputStream(destination, "w")
            ?: throw IOException("The selected destination could not be opened")
        var count = 0
        ZipOutputStream(stream.buffered()).use { zip ->
            val logs = File(context.filesDir, "logs")
            val files = logs.listFiles()
                ?.filter { it.isFile && (it.extension == "log" || it.extension == "txt") }
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
