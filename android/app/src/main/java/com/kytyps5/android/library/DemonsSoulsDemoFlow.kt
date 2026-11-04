package com.kytyps5.android.library

import java.io.File
import java.io.FileOutputStream
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/**
 * Optional Demon's Souls PPSA01341 (01.007.000) engine-side bypass, ported from the
 * verified Supermedo PC tool (tools/local/demons-souls-boot-skip.py).
 *
 * The game reads PackageCmdLineArgs.txt. Adding cp11 console variables changes
 * the guest engine's boot flow without patching eboot.bin or touching save data.
 * The block is strictly managed and fully reversible. This does not assert that
 * the host renderer supports everything in the tutorial.
 */
object DemonsSoulsDemoFlow {
    const val BEGIN = "// >>> kytyps5-android-demons-demo-loop >>>"
    const val END = "// <<< kytyps5-android-demons-demo-loop <<<"
    private const val ARGS_NAME = "PackageCmdLineArgs.txt"
    private val cvars = listOf(
        "+cp11_debug_skipSplashScreens=true",
        "+cp11_gameflowtype=DemoLoopA",
        "+cp11_demoModeFile=\$/misc/demo_tutorial.txt",
        "+cp11_demoCycle=\$/misc/demo_tutorial.txt",
    )
    // Leading newline is deliberate. Removing the block restores previous text exactly.
    private val block = "\n" + BEGIN + "\n" + cvars.joinToString("\n") + "\n" + END + "\n"

    data class Outcome(val ok: Boolean, val message: String = "", val changed: Boolean = false)

    fun configure(gameDir: File, enabled: Boolean): Outcome {
        return try {
        if (!gameDir.isDirectory) return Outcome(false, "Game directory not found: " + gameDir.path)
        val file = File(gameDir, ARGS_NAME)
        if (!enabled && !file.exists()) return Outcome(true)
        if (file.exists() && !file.isFile) return Outcome(false, "Cannot edit PackageCmdLineArgs.txt: not a file")
        val before = if (file.exists()) file.readText(Charsets.UTF_8) else ""
        val hasBegin = before.contains(BEGIN)
        val hasEnd = before.contains(END)
        if (hasBegin != hasEnd || (hasBegin && !before.contains(block))) {
            return Outcome(false, "Modified/incomplete DemoLoopA marker. File left unchanged.")
        }
        if (hasBegin && before.indexOf(BEGIN) != before.lastIndexOf(BEGIN)) {
            return Outcome(false, "Multiple DemoLoopA markers. File left unchanged.")
        }
        val original = if (hasBegin) before.replace(block, "") else before
        val after = original + if (enabled) block else ""
        if (before == after) return Outcome(true)
        val tmp = File.createTempFile(".kyty-ds-", ".tmp", gameDir)
        try {
            FileOutputStream(tmp).use { output ->
                output.write(after.toByteArray(Charsets.UTF_8))
                output.fd.sync()
            }
            try {
                Files.move(tmp.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE,
                           StandardCopyOption.REPLACE_EXISTING)
            } catch (_: AtomicMoveNotSupportedException) {
                Files.move(tmp.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING)
            }
        } finally {
            tmp.delete()
        }
        Outcome(true, changed = true)
    } catch (e: Exception) {
        Outcome(false, "DemoLoopA: " + e.javaClass.simpleName + ": " + (e.message ?: "file error"))
        }
    }
}
