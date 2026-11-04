package com.kytyps5.android.library

import java.io.File
import java.io.FileOutputStream
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/**
 * Reproduces the actually reported working PC route for PPSA01341:
 * skip the "Journey to the Nexus" tutorial, then enter the Nexus.
 *
 * The PC compatibility report #1461 documents that going through the tutorial
 * fog freezes, while choosing to skip the Journey loads the Nexus.
 * We don't assert the guest follows this route on Android until verified.
 *
 * This mode restores FullGame, asks the CP11 engine to show the Skip Tutorial
 * choice and avoids the DemoLoopA forced Tutorial corridor.
 * No EBOOT modifications and no save modification.
 */
object DemonsSoulsNexusFlow {
    const val BEGIN = "// >>> kytyps5-android-nexus-route >>>"
    const val END = "// <<< kytyps5-android-nexus-route <<<"
    private const val CONFIG_NAME = "PackageCmdLineArgs.txt"
    private val cvars = listOf(
        "+cp11_debug_skipSplashScreens=true",
        "+cp11_gameflowtype=FullGame",
        "+cp11_debug_forceOptionToSkipTutorial=true",
    )
    private val block = "\n" + BEGIN + "\n" + cvars.joinToString("\n") + "\n" + END + "\n"

    data class Result(val ok: Boolean, val changed: Boolean = false, val message: String = "")

    fun configure(gameDir: File, enabled: Boolean): Result {
        return try {
            if (!gameDir.isDirectory) return Result(false, message = "Game directory not found: " + gameDir.path)
            val file = File(gameDir, CONFIG_NAME)
            if (!enabled && !file.exists()) return Result(true)
            if (file.exists() && !file.isFile) return Result(false, message = "Not a file: " + file.path)
            val before = if (file.exists()) file.readText(Charsets.UTF_8) else ""
            val starts = before.indexOf(BEGIN)
            val ends = before.indexOf(END)
            if ((starts >= 0) != (ends >= 0) ||
                (starts >= 0 && (before.indexOf(BEGIN, starts + 1) >= 0 ||
                    !before.contains(block)))) {
                return Result(false, message = "Nexus managed block has been modified. Nothing changed.")
            }
            val original = if (starts >= 0) before.replace(block, "") else before
            val after = original + if (enabled) block else ""
            if (after == before) return Result(true)
            val temp = File.createTempFile(".ds-nexus-", ".tmp", gameDir)
            try {
                FileOutputStream(temp).use {
                    it.write(after.toByteArray(Charsets.UTF_8))
                    it.fd.sync()
                }
                try {
                    Files.move(temp.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE,
                        StandardCopyOption.REPLACE_EXISTING)
                } catch (_: AtomicMoveNotSupportedException) {
                    Files.move(temp.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING)
                }
            } finally {
                temp.delete()
            }
            Result(true, changed = true)
        } catch (e: Exception) {
            Result(false, message = "Nexus route error: " + (e.message ?: e.javaClass.simpleName))
        }
    }
}
