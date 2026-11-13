package com.kytyps5.android.library

import java.io.File
import java.io.FileOutputStream
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/**
 * Legacy rollback for the experimental fog override, which was NOT shown to
 * cause successful 3D rendering on PC. The Android launcher only calls this
 * with enabled=false, to restore PackageCmdLineArgs.txt after previous builds.
 * Keep the managed block syntax unchanged so prior installations can migrate.
 */
object DemonsSoulsFogFlow {
    private const val BEGIN = "// >>> kytyps5-android-demons-fog >>>"
    private const val END = "// <<< kytyps5-android-demons-fog <<<"
    private const val CONFIG = "PackageCmdLineArgs.txt"
    private val block = "\n" + BEGIN + "\n+fog_enable=false\n" + END + "\n"

    data class Result(val ok: Boolean, val changed: Boolean = false, val message: String = "")

    fun configure(gameDir: File, enabled: Boolean): Result {
        if (!gameDir.isDirectory) return Result(false, message = "Game directory missing")
        val config = File(gameDir, CONFIG)
        if (!enabled && !config.exists()) return Result(true)
        if (config.exists() && !config.isFile) return Result(false, message = "Invalid game args path")
        return try {
            val before = if (config.exists()) config.readText(Charsets.UTF_8) else ""
            val begin = before.indexOf(BEGIN)
            val end = before.indexOf(END)
            if ((begin >= 0) != (end >= 0) ||
                (begin >= 0 && (!before.contains(block) ||
                    before.indexOf(BEGIN, begin + BEGIN.length) >= 0))) {
                return Result(false, message = "Fog override was externally modified")
            }
            val original = if (begin >= 0) before.replace(block, "") else before
            val after = original + if (enabled) block else ""
            if (after == before) return Result(true)
            val temp = File.createTempFile(".ds-fog-", ".tmp", gameDir)
            try {
                FileOutputStream(temp).use { out ->
                    out.write(after.toByteArray(Charsets.UTF_8))
                    out.fd.sync()
                }
                try {
                    Files.move(temp.toPath(), config.toPath(), StandardCopyOption.ATOMIC_MOVE,
                        StandardCopyOption.REPLACE_EXISTING)
                } catch (_: AtomicMoveNotSupportedException) {
                    Files.move(temp.toPath(), config.toPath(), StandardCopyOption.REPLACE_EXISTING)
                }
            } finally {
                temp.delete()
            }
            Result(true, changed = true)
        } catch (e: Exception) {
            Result(false, message = "Fog override: " +
                (e.message ?: e.javaClass.simpleName))
        }
    }
}
