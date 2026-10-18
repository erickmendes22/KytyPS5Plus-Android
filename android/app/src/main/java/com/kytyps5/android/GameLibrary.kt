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

sealed interface ScanResult {
    data class Found(val games: List<GameEntry>) : ScanResult
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

    /** Subtitle for a dump image in the grid; empty for anything else. */
    fun imageLabel(file: File): String = when (file.extension.lowercase()) {
        ZARCHIVE_EXTENSION -> "ZArchive"
        EXFAT_EXTENSION -> "exFAT image"
        else -> ""
    }

    /** How deep below the chosen folder to look; dumps are often nested one or two levels. */
    const val MAX_SCAN_DEPTH = 3

    fun scan(root: File): ScanResult {
        if (!root.exists()) return ScanResult.Failure("Folder not found: ${root.path}")
        if (!root.isDirectory) return ScanResult.Failure("Not a folder: ${root.path}")
        if (root.listFiles() == null) {
            return ScanResult.Failure("Cannot read ${root.path}. Check all-files access.")
        }
        val games = mutableListOf<GameEntry>()
        collect(root, depth = 0, into = games)
        return ScanResult.Found(games.sortedBy { it.title.lowercase() })
    }

    /** The chosen folder may itself be a game; otherwise look below it, without entering games. */
    private fun collect(folder: File, depth: Int, into: MutableList<GameEntry>) {
        if (isGameFolder(folder)) {
            into += readEntry(folder)
            return
        }
        val children = folder.listFiles() ?: return
        children
            .filter { it.isFile && it.extension.lowercase() in IMAGE_EXTENSIONS }
            .mapTo(into) { GameEntry(folder = it, title = it.nameWithoutExtension) }
        if (depth >= MAX_SCAN_DEPTH) return
        children.filter { it.isDirectory && !it.name.startsWith(".") }
            .forEach { collect(it, depth + 1, into) }
    }

    private fun isGameFolder(folder: File): Boolean = File(folder, PARAM_JSON).isFile

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
        if (file.length() > MAX_PARAM_JSON_BYTES) throw IOException("${file.path} is too large")
        JSONObject(file.readText())
    } catch (e: JSONException) {
        Log.w(TAG, "Malformed $PARAM_JSON in ${folder.path}", e)
        null
    } catch (e: IOException) {
        null
    }

    private fun readParamSfo(folder: File): Map<String, Any>? {
        val file = File(folder, PARAM_SFO)
        if (!file.isFile || file.length() > MAX_SFO_BYTES) return null
        return try {
            ParamSfo.parse(file.readBytes())
        } catch (e: IOException) {
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
