package com.kytyps5.android.input

import java.io.File
import java.io.IOException
import java.security.MessageDigest

/** Where a loaded layout came from, so the editor can say what "Save" will overwrite. */
enum class LayoutScope { GAME, GLOBAL, DEFAULT }

data class LoadedLayout(val layout: OverlayLayout, val scope: LayoutScope)

/**
 * Touch-controller layouts on disk: one global layout plus optional per-game overrides keyed by the
 * game's path. Lookup order is game -> global -> built-in default.
 */
class OverlayProfileStore(private val dir: File) {

    fun load(gamePath: String?): LoadedLayout {
        gamePath?.let { read(gameFile(it)) }?.let { return LoadedLayout(it, LayoutScope.GAME) }
        read(globalFile())?.let { return LoadedLayout(it, LayoutScope.GLOBAL) }
        return LoadedLayout(OverlayLayout.DEFAULT, LayoutScope.DEFAULT)
    }

    fun hasGameLayout(gamePath: String): Boolean = gameFile(gamePath).isFile

    /** Saves for one game when [gamePath] is set, else as the global layout. */
    fun save(layout: OverlayLayout, gamePath: String?): Boolean =
        write(if (gamePath != null) gameFile(gamePath) else globalFile(), layout.toJson())

    fun deleteGameLayout(gamePath: String): Boolean = gameFile(gamePath).let { !it.exists() || it.delete() }

    private fun globalFile() = File(dir, GLOBAL_FILE)

    private fun gameFile(gamePath: String) = File(dir, "game-${keyFor(gamePath)}.json")

    private fun read(file: File): OverlayLayout? = try {
        if (file.isFile) OverlayLayout.fromJson(file.readText()) else null
    } catch (e: IOException) {
        null
    }

    private fun write(file: File, text: String): Boolean {
        if (!dir.isDirectory && !dir.mkdirs()) return false
        val partial = File(dir, file.name + ".part")
        return try {
            partial.writeText(text)
            partial.renameTo(file) || run { partial.delete(); false }
        } catch (e: IOException) {
            partial.delete()
            false
        }
    }

    companion object {
        const val DIRECTORY_NAME = "controller_layouts"
        private const val GLOBAL_FILE = "global.json"

        /** File-name-safe stable key for a game path. */
        fun keyFor(gamePath: String): String =
            MessageDigest.getInstance("SHA-1").digest(gamePath.toByteArray())
                .take(KEY_BYTES).joinToString("") { "%02x".format(it) }

        private const val KEY_BYTES = 8
    }
}
