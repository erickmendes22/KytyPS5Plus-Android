package com.kytyps5.android

import android.util.Log
import java.io.File
import java.io.IOException
import org.json.JSONException
import org.json.JSONObject

/**
 * A game the emulator can load: an extracted dump folder (contains sce_sys/param.json) or a
 * ZArchive dump file (*.zar), which the emulator mounts read-only without extracting.
 */
data class GameEntry(val folder: File, val title: String) {
    val path: String get() = folder.absolutePath
}

sealed interface ScanResult {
    data class Found(val games: List<GameEntry>) : ScanResult
    data class Failure(val message: String) : ScanResult
}

object GameLibrary {
    private const val TAG = "GameLibrary"
    const val PARAM_JSON = "sce_sys/param.json"
    const val ZARCHIVE_EXTENSION = "zar"

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
            into += GameEntry(folder = folder, title = readTitle(folder))
            return
        }
        val children = folder.listFiles() ?: return
        children
            .filter { it.isFile && it.extension.equals(ZARCHIVE_EXTENSION, ignoreCase = true) }
            .mapTo(into) { GameEntry(folder = it, title = it.nameWithoutExtension) }
        if (depth >= MAX_SCAN_DEPTH) return
        children.filter { it.isDirectory && !it.name.startsWith(".") }
            .forEach { collect(it, depth + 1, into) }
    }

    private fun isGameFolder(folder: File): Boolean = File(folder, PARAM_JSON).isFile

    fun readTitle(folder: File): String {
        val fallback = folder.name
        return try {
            titleFromParamJson(JSONObject(File(folder, PARAM_JSON).readText()), fallback)
        } catch (e: JSONException) {
            Log.w(TAG, "Malformed $PARAM_JSON in ${folder.path}", e)
            fallback
        } catch (e: IOException) {
            Log.w(TAG, "Cannot read $PARAM_JSON in ${folder.path}", e)
            fallback
        }
    }

    /**
     * Picks the display title: default-language localized titleName, then "en", then any other
     * localized titleName, then the top-level titleName. Falls back to [fallback].
     */
    fun titleFromParamJson(json: JSONObject, fallback: String): String {
        val candidates = mutableListOf<JSONObject>()
        json.optJSONObject("localizedParameters")?.let { localized ->
            val defaultLanguage = localized.optString("defaultLanguage", "en")
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
            .map { it.optString("titleName").trim() }
            .firstOrNull { it.isNotEmpty() }
            ?: fallback
    }
}
