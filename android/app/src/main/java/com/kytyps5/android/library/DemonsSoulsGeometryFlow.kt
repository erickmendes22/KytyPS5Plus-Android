package com.kytyps5.android.library

import java.io.File
import java.io.FileOutputStream
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/**
 * Use BPE's original unculled index path, rather than GPU-produced index-culling
 * results. Verified in the supplied PPSA01341 executable: bool 0xCAA selects
 * XPR_IndexCullObjectsNormal; 0xCAB is doSunShadowIndexBufferCulling.
 * This is a geometry compatibility path, not an Android gameplay verification.
 */
object DemonsSoulsGeometryFlow {
    const val BEGIN = "// >>> kytyps5-android-demons-geometry >>>"
    const val END = "// <<< kytyps5-android-demons-geometry <<<"
    private val names = listOf("doIndexBufferCulling", "doSunShadowIndexBufferCulling")
    private val block = "\n" + BEGIN + "\n" + names.joinToString("\n") { "+$it=0" } + "\n" + END + "\n"
    data class Result(val ok: Boolean, val changed: Boolean = false, val message: String = "")

    // Bound memory use even for large SELF files; names may straddle a read boundary.
    internal fun supports(executable: File): Boolean {
        if (!executable.isFile) return false
        val missing = names.map { it + '\u0000' }.toMutableSet()
        val bytes = ByteArray(64 * 1024)
        var tail = ""
        return executable.inputStream().buffered().use { input ->
            while (missing.isNotEmpty()) {
                val n = input.read(bytes)
                if (n < 0) return@use false
                if (n == 0) continue
                val chunk = tail + String(bytes, 0, n, Charsets.ISO_8859_1)
                missing.removeAll { chunk.contains(it) }
                tail = chunk.takeLast(64)
            }
            true
        }
    }

    fun configure(gameDir: File, titleId: String?, enabled: Boolean = true): Result {
        if (!titleId.equals("PPSA01341", ignoreCase = true)) return Result(true)
        return try {
            if (!gameDir.isDirectory) return Result(false, message = "Game directory not found")
            val file = File(gameDir, "PackageCmdLineArgs.txt")
            if (!enabled && !file.exists()) return Result(true)
            if (file.exists() && !file.isFile) return Result(false, message = "Invalid game arguments file")
            val before = if (file.exists()) file.readText(Charsets.UTF_8) else ""
            val start = before.indexOf(BEGIN)
            val end = before.indexOf(END)
            if ((start >= 0) != (end >= 0) || (start >= 0 &&
                (before.indexOf(BEGIN, start + 1) >= 0 || !before.contains(block))))
                return Result(false, message = "Managed geometry block was modified; game arguments unchanged")
            if (enabled && listOf("decrypted/eboot.bin", "eboot.bin", "DemonsSoul_PROSPERO_Release.elf")
                    .none { supports(File(gameDir, it)) })
                return Result(false, message = "Demon's Souls executable does not expose the verified geometry options")
            val original = if (start >= 0) before.replace(block, "") else before
            val after = original + if (enabled) block else ""
            if (after == before) return Result(true)
            val temp = File.createTempFile(".ds-geometry-", ".tmp", gameDir)
            try {
                FileOutputStream(temp).use { it.write(after.toByteArray(Charsets.UTF_8)); it.fd.sync() }
                try {
                    Files.move(temp.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE,
                        StandardCopyOption.REPLACE_EXISTING)
                } catch (_: AtomicMoveNotSupportedException) {
                    Files.move(temp.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING)
                }
            } finally { temp.delete() }
            Result(true, changed = true)
        } catch (e: Exception) {
            Result(false, message = "Demon's Souls geometry configuration: " + (e.message ?: e.javaClass.simpleName))
        }
    }
}
