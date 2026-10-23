package com.kytyps5.android

import android.util.Log
import com.kytyps5.android.library.ParamSfo
import java.io.File
import java.io.IOException
import org.json.JSONException
import org.json.JSONObject

/**
 * A game the emulator can load: an extracted dump folder (contains sce_sys/param.json) or a
 * dump image the emulator mounts read-only without extracting: a ZArchive file (*.zar) or an
 * exFAT disk image (*.exfat).
 * [titleId] and [icon] (sce_sys/icon0.png) are known for extracted folders only.
 */
data class GameEntry(
    val folder: File,
    val title: String,
    val titleId: String? = null,
    val icon: File? = null,
) {
    val path: String get() = folder.absolutePath
}

/** Details of what was actually searched, so an empty grid is not a silent failure. */
data class ScanDiagnostics(
    val visitedDirectories: Int = 0,
    val unreadableDirectories: List<String> = emptyList(),
    val depthLimitedDirectories: Int = 0,
    val hitDirectoryLimit: Boolean = false,
)

sealed interface ScanResult {
    data class Found(
        val games: List<GameEntry>,
        val diagnostics: ScanDiagnostics = ScanDiagnostics(),
    ) : ScanResult
    data class Failure(val message: String) : ScanResult
}

object GameLibrary {
    private const val TAG = "GameLibrary"
    const val PARAM_JSON = "sce_sys/param.json"
    const val PARAM_SFO = "sce_sys/param.sfo"
    const val ICON = "sce_sys/icon0.png"
    private const val MAX_SFO_BYTES = 64 * 1024
    private const val MAX_PARAM_JSON_BYTES = 1024 * 1024
    const val ZARCHIVE_EXTENSION = "zar"
    const val EXFAT_EXTENSION = "exfat"
    private val IMAGE_EXTENSIONS = setOf(ZARCHIVE_EXTENSION, EXFAT_EXTENSION)
    private val EXECUTABLE_EXTENSIONS = setOf("elf", "self")
    const val EBOOT = "eboot.bin"
    private const val MAX_SCANNED_DIRECTORIES = 4096

    /** Subtitle for a dump image in the grid; empty for anything else. */
    fun imageLabel(file: File): String = when (file.extension.lowercase()) {
        ZARCHIVE_EXTENSION -> "ZArchive"
        EXFAT_EXTENSION -> "exFAT image"
        "elf" -> "ELF executable"
        "self" -> "SELF executable"
        else -> ""
    }

    /** Also handle installers with two or more wrapper folders around the game. */
    const val MAX_SCAN_DEPTH = 8

    private class ScanState {
        val games = mutableListOf<GameEntry>()
        val visited = mutableSetOf<String>()
        val unreadable = mutableListOf<String>()
        var depthLimited = 0
        var hitDirectoryLimit = false

        fun diagnostics() = ScanDiagnostics(
            visitedDirectories = visited.size,
            unreadableDirectories = unreadable.toList(),
            depthLimitedDirectories = depthLimited,
            hitDirectoryLimit = hitDirectoryLimit,
        )
    }

    fun scan(root: File): ScanResult {
        if (!root.exists()) return ScanResult.Failure("Folder not found: ${root.path}")
        if (!root.isDirectory) return ScanResult.Failure("Not a folder: ${root.path}")
        if (safeListFiles(root) == null) {
            return ScanResult.Failure("Cannot read ${root.path}. Check all-files access and the selected games folder.")
        }
        val state = ScanState()
        collect(root, depth = 0, state)
        val games = state.games.distinctBy { it.path }.sortedBy { it.title.lowercase() }
        Log.i(TAG, "Scan ${root.path}: ${games.size} games; ${state.visited.size} folders; " +
            "${state.unreadable.size} unreadable; depth-limited=${state.depthLimited}; " +
            "directory-limit=${state.hitDirectoryLimit}")
        return ScanResult.Found(games, state.diagnostics())
    }

    /** Do not follow symlink cycles. A bad nested folder must not make the whole library empty. */
    private fun collect(folder: File, depth: Int, state: ScanState) {
        if (state.visited.size >= MAX_SCANNED_DIRECTORIES) {
            state.hitDirectoryLimit = true
            return
        }
        val canonical = try {
            folder.canonicalPath
        } catch (e: IOException) {
            state.unreadable.add(folder.path)
            Log.w(TAG, "Cannot resolve game folder ${folder.path}", e)
            return
        } catch (e: SecurityException) {
            state.unreadable.add(folder.path)
            Log.w(TAG, "No access to ${folder.path}", e)
            return
        }
        if (!state.visited.add(canonical)) return

        // CapyPS5 recognizes the executable, not just metadata. Keep recognizing metadata-only
        // folders for compatibility with the old launcher, but allow valid eboot-only dumps too.
        if (isGameFolder(folder)) {
            val game = readEntry(folder)
            state.games.add(game)
            Log.i(TAG, "Game: ${game.path} (${game.titleId ?: "no title ID"})")
            return
        }

        val children = safeListFiles(folder)
        if (children == null) {
            state.unreadable.add(folder.path)
            Log.w(TAG, "Cannot list game folder: ${folder.path}")
            return
        }
        for (file in children) {
            if (!file.isFile) continue
            val ext = file.extension.lowercase()
            if (ext in IMAGE_EXTENSIONS || ext in EXECUTABLE_EXTENSIONS) {
                state.games.add(GameEntry(folder = file, title = file.nameWithoutExtension))
            }
        }
        val subdirectories = children.filter { it.isDirectory && !it.name.startsWith(".") }
        if (depth >= MAX_SCAN_DEPTH) {
            state.depthLimited += subdirectories.size
            return
        }
        for (subdirectory in subdirectories) {
            if (state.hitDirectoryLimit) break
            collect(subdirectory, depth + 1, state)
        }
    }

    private fun safeListFiles(folder: File): Array<File>? = try {
        folder.listFiles()
    } catch (e: SecurityException) {
        Log.w(TAG, "Access denied: ${folder.path}", e)
        null
    }

    private fun isGameFolder(folder: File): Boolean =
        File(folder, EBOOT).isFile || File(folder, PARAM_JSON).isFile

    /** Title (param.json, then param.sfo TITLE, then folder name), title id and icon of a game folder. */
    fun readEntry(folder: File): GameEntry {
        val json = readParamJson(folder)
        val sfo = readParamSfo(folder)
        val title = json?.let { titleFromParamJson(it, "") }?.ifEmpty { null }
            ?: (sfo?.get("TITLE") as? String)?.trim()?.ifEmpty { null }
            ?: folder.name
        val titleId = json?.stringOr("titleId")?.trim()?.ifEmpty { null }
            ?: (sfo?.get("TITLE_ID") as? String)?.trim()?.ifEmpty { null }
        return GameEntry(folder, title, titleId, File(folder, ICON).takeIf { it.isFile })
    }

    private fun readParamJson(folder: File): JSONObject? = try {
        val file = File(folder, PARAM_JSON)
        if (!file.isFile) null
        else {
            if (file.length() > MAX_PARAM_JSON_BYTES) throw IOException("${file.path} is too large")
            JSONObject(file.readText())
        }
    } catch (e: JSONException) {
        Log.w(TAG, "Malformed $PARAM_JSON in ${folder.path}", e)
        null
    } catch (e: IOException) {
        Log.w(TAG, "Cannot read $PARAM_JSON in ${folder.path}", e)
        null
    } catch (e: SecurityException) {
        Log.w(TAG, "No access to $PARAM_JSON in ${folder.path}", e)
        null
    }

    private fun readParamSfo(folder: File): Map<String, Any>? {
        val file = File(folder, PARAM_SFO)
        if (!file.isFile || file.length() > MAX_SFO_BYTES) return null
        return try {
            ParamSfo.parse(file.readBytes())
        } catch (e: IOException) {
            Log.w(TAG, "Cannot read $PARAM_SFO in ${folder.path}", e)
            null
        } catch (e: SecurityException) {
            Log.w(TAG, "No access to $PARAM_SFO in ${folder.path}", e)
            null
        }
    }

    /**
     * Picks the display title: default-language localized titleName, then "en", then any other
     * localized titleName, then the top-level titleName. Falls back to [fallback].
     */
    fun titleFromParamJson(json: JSONObject, fallback: String): String {
        val candidates = mutableListOf<JSONObject>()
        json.optJSONObject("localizedParameters")?.let { localized ->
            val defaultLanguage = localized.stringOr("defaultLanguage", "en")
            localized.optJSONObject(defaultLanguage)?.let(candidates::add)
            localized.optJSONObject("en")?.let(candidates::add)
            localized.keys().forEach { key ->
                if (key != "defaultLanguage") {
                    localized.optJSONObject(key)?.let(candidates::add)
                }
            }
        }
        candidates.add(json)
        return candidates
            .asSequence()
            .map { it.stringOr("titleName").trim() }
            .firstOrNull { it.isNotEmpty() }
            ?: fallback
    }
}
