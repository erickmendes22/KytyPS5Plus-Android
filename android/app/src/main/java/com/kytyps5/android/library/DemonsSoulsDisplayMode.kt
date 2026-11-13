package com.kytyps5.android.library

import java.io.File
import java.io.FileOutputStream
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/**
 * Bluepoint selects 4K from app0/misc/renderconfig_ps5_*.txt after startup.
 * --screen-width/height only changes the host window. The original game's
 * WantsPerf enum requests 1080p on PPSA01341 (AnyPS5 discussion #1283).
 * Preserve each original file for reversible restoration without editing eboot.
 */
object DemonsSoulsDisplayMode {
    private val fileName = Regex("^renderconfig_ps5_.*\\.txt$", RegexOption.IGNORE_CASE)
    private val wants4k = Regex(
        "(?m)^([ \\t]*\\+?r_Wants4K[ \\t]*=[ \\t]*)Wants(?:Off|Base|Perf|High|SuperHigh)(?=[ \\t]*(?:[#;].*)?$)"
    )
    private const val BACKUP_SUFFIX = ".kytyps5-original"
    data class Result(val ok: Boolean, val changed: Boolean = false, val message: String = "")

    private fun performanceConfig(original: String): String {
        val replacement = wants4k.replace(original) { match -> match.groupValues[1] + "WantsPerf" }
        if (replacement != original || wants4k.containsMatchIn(original)) return replacement
        val newline = if (original.contains("\r\n")) "\r\n" else "\n"
        return original + (if (original.endsWith("\n") || original.isEmpty()) "" else newline) +
            "+r_Wants4K = WantsPerf" + newline
    }

    private fun writeAtomic(target: File, value: String) {
        val temp = File.createTempFile(".ds-mode-", ".tmp", target.parentFile)
        try {
            FileOutputStream(temp).use { out ->
                out.write(value.toByteArray(Charsets.UTF_8))
                out.fd.sync()
            }
            try {
                Files.move(temp.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE,
                    StandardCopyOption.REPLACE_EXISTING)
            } catch (_: AtomicMoveNotSupportedException) {
                Files.move(temp.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING)
            }
        } finally {
            temp.delete()
        }
    }

    fun configure(gameDir: File, enabled: Boolean): Result {
        val misc = File(gameDir, "misc")
        val configs = misc.listFiles()
            ?.filter { it.isFile && fileName.matches(it.name) }
            ?.sortedBy { it.name.lowercase() }.orEmpty()
        if (configs.isEmpty()) return Result(true, message = "No PS5 render config files found")
        data class Update(val file: File, val text: String)
        val backups = mutableListOf<Update>()
        val edits = mutableListOf<Update>()
        try {
            // Verify all files before modifying any; do not overwrite user edits.
            for (config in configs) {
                val backup = File(config.parentFile, config.name + BACKUP_SUFFIX)
                if (backup.exists() && !backup.isFile) {
                    return Result(false, message = "Invalid resolution backup: " + backup.name)
                }
                val before = config.readText(Charsets.UTF_8)
                val original = if (backup.exists()) backup.readText(Charsets.UTF_8) else before
                val expected = performanceConfig(original)
                if (backup.exists() && before != expected && before != original) {
                    return Result(false, message = "Render config changed externally: " + config.name)
                }
                if (enabled && !backup.exists()) backups += Update(backup, original)
                val wanted = if (enabled) expected else original
                if (before != wanted) edits += Update(config, wanted)
            }
            backups.forEach { writeAtomic(it.file, it.text) }
            edits.forEach { writeAtomic(it.file, it.text) }
            return Result(true, changed = edits.isNotEmpty(),
                message = if (enabled) "PS5 internal resolution: WantsPerf (1080p)"
                else "PS5 original render configs restored")
        } catch (error: Exception) {
            return Result(false, message = "Resolution config: " +
                (error.message ?: error.javaClass.simpleName))
        }
    }
}
