package com.kytyps5.android.presets

import com.kytyps5.android.stringAt
import com.kytyps5.android.stringOr
import java.io.File
import org.json.JSONException
import org.json.JSONObject

/**
 * Where a preset's files go, relative to the app's files directory (the emulator's --data-dir).
 * Each target is a per-title folder the emulator mounts for the guest; only writable layers the
 * game reads anyway belong here. A read-only /app0 overlay would be a new target later.
 */
enum class PresetTarget(val storageValue: String, private val dataDirName: String) {
    /** files/_DownloadData/<TITLE_ID>, mounted as /download0 (src/emulator.cpp MountSandboxDirs). */
    DOWNLOAD_DATA(storageValue = "download_data", dataDirName = "_DownloadData");

    fun directory(filesDir: File, titleId: String): File = File(File(filesDir, dataDirName), titleId)

    companion object {
        fun fromStorage(value: String): PresetTarget? = entries.firstOrNull { it.storageValue == value }
    }
}

/**
 * A known-good configuration overlay for one title, shipped in the APK under
 * assets/presets/<TITLE_ID>/<preset id>/: preset.json plus files/<path>, where each <path> is also
 * the file's location under the target folder.
 */
data class GamePreset(
    val titleId: String,
    val id: String,
    val name: String,
    val description: String,
    val target: PresetTarget,
    val files: List<String>,
)

/** preset.json; see [GamePreset]. */
object PresetManifest {
    const val FILE_NAME = "preset.json"
    const val FILES_DIR = "files"
    const val SCHEMA_VERSION = 1

    private val ID = Regex("^[a-z0-9][a-z0-9_-]{0,31}$")

    /** Parsed manifest, or the reason it was rejected. */
    sealed interface Result {
        data class Ok(val preset: GamePreset) : Result
        data class Invalid(val reason: String) : Result
    }

    /**
     * Parses the manifest of the preset in folder <[titleId]>/<[folderName]>. The manifest's id must
     * equal the folder name, every file path must be a plain relative path (see [isSafeRelativePath]),
     * and the schema version must be one this app understands.
     */
    fun parse(titleId: String, folderName: String, json: String): Result {
        val obj = try {
            JSONObject(json)
        } catch (e: JSONException) {
            return Result.Invalid("not JSON: ${e.message}")
        }
        val schema = obj.optInt("schemaVersion", -1)
        if (schema != SCHEMA_VERSION) return Result.Invalid("unsupported schemaVersion $schema")
        val id = obj.stringOr("id").trim()
        if (!ID.matches(id)) return Result.Invalid("invalid id \"$id\"")
        if (id != folderName) return Result.Invalid("id \"$id\" does not match folder \"$folderName\"")
        val name = obj.stringOr("name").trim()
        if (name.isEmpty()) return Result.Invalid("missing name")
        val targetValue = obj.stringOr("target")
        val target = PresetTarget.fromStorage(targetValue) ?: return Result.Invalid("unknown target \"$targetValue\"")
        val array = obj.optJSONArray("files") ?: return Result.Invalid("missing files")
        val files = (0 until array.length()).map { array.stringAt(it) }
        if (files.isEmpty()) return Result.Invalid("no files")
        files.firstOrNull { !isSafeRelativePath(it) }?.let { return Result.Invalid("unsafe path \"$it\"") }
        if (files.toSet().size != files.size) return Result.Invalid("duplicate file")
        return Result.Ok(
            GamePreset(
                titleId = titleId,
                id = id,
                name = name,
                description = obj.stringOr("description").trim(),
                target = target,
                files = files,
            ),
        )
    }

    /** A forward-slash path below the target folder: no leading slash, no ".", "..", or empty segments. */
    fun isSafeRelativePath(path: String): Boolean {
        if (path.isEmpty() || path.startsWith("/") || '\\' in path || '\u0000' in path) return false
        return path.split('/').all { it.isNotEmpty() && it != "." && it != ".." }
    }
}

/** PlayStation title ids (PPSA14708, CUSA00001): four capital letters and five digits. */
object TitleIds {
    private val EXACT = Regex("^[A-Z]{4}[0-9]{5}$")
    private val EMBEDDED = Regex("(?<![A-Za-z0-9])([A-Z]{4}[0-9]{5})(?![0-9])")

    fun isValid(titleId: String): Boolean = EXACT.matches(titleId)

    /** The first title id inside a file or folder name, e.g. "PPSA14708.exfat" or "PPSA21567 ASTRO BOT (01.019.000)". */
    fun fromName(name: String): String? = EMBEDDED.find(name)?.groupValues?.get(1)

    /**
     * Title id of a game: the one read from its param.json / param.sfo when known, else one in the
     * name of the image file or game folder. The enclosing folder is not consulted: images are often
     * kept inside another game's dump folder, whose id would be wrong.
     */
    fun resolve(paramTitleId: String?, game: File): String? {
        paramTitleId?.trim()?.uppercase()?.takeIf(::isValid)?.let { return it }
        return fromName(game.name)
    }
}
