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
    val launch: PresetLaunch = PresetLaunch(),
)

/**
 * Emulator launch options a preset adds while it is on (schema 2): environment variables for the
 * emulator's own knobs, --render-scale, and a --game-patch file shipped in the preset under
 * launch/<[gamePatch]>. GameActivity appends them to the command line (see [PresetLaunchArgs]).
 */
data class PresetLaunch(
    val env: Map<String, String> = emptyMap(),
    val renderScale: Float? = null,
    val gamePatch: String? = null,
    val playgoHack: Boolean = false,
) {
    val isEmpty: Boolean get() = env.isEmpty() && renderScale == null && gamePatch == null && !playgoHack

    companion object {
        /** Only the emulator's own knobs: a preset cannot change the loader, Mesa or Android variables. */
        val ENV_KEY = Regex("^KYTY_[A-Z0-9_]{1,60}$")
        val ENV_VALUE = Regex("^[A-Za-z0-9_.:+-]{1,64}$")
        const val MIN_RENDER_SCALE = 0.25f // src/main.cpp Config::MIN/MAX_RENDER_SCALE
        const val MAX_RENDER_SCALE = 2.0f
        const val GAME_PATCH_SUFFIX = ".json"
    }
}

/** preset.json; see [GamePreset]. */
object PresetManifest {
    const val FILE_NAME = "preset.json"
    const val FILES_DIR = "files"
    const val LAUNCH_DIR = "launch"

    /** 1: files only. 2: adds "launch" (emulator options); "files" may then be empty or absent. */
    const val SCHEMA_VERSION = 2
    private val SCHEMAS = 1..SCHEMA_VERSION

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
        if (schema !in SCHEMAS) return Result.Invalid("unsupported schemaVersion $schema")
        val id = obj.stringOr("id").trim()
        if (!ID.matches(id)) return Result.Invalid("invalid id \"$id\"")
        if (id != folderName) return Result.Invalid("id \"$id\" does not match folder \"$folderName\"")
        val name = obj.stringOr("name").trim()
        if (name.isEmpty()) return Result.Invalid("missing name")
        val launch = when {
            !obj.has("launch") -> PresetLaunch()
            schema < 2 -> return Result.Invalid("launch needs schemaVersion 2")
            else -> when (val parsed = parseLaunch(obj.optJSONObject("launch"))) {
                is LaunchResult.Ok -> parsed.launch
                is LaunchResult.Invalid -> return Result.Invalid(parsed.reason)
            }
        }
        val array = obj.optJSONArray("files")
        if (array == null && (schema < 2 || obj.has("files"))) return Result.Invalid("missing files")
        val files = array?.let { a -> (0 until a.length()).map { a.stringAt(it) } }.orEmpty()
        if (files.isEmpty() && launch.isEmpty) return Result.Invalid("no files")
        files.firstOrNull { !isSafeRelativePath(it) }?.let { return Result.Invalid("unsafe path \"$it\"") }
        if (files.toSet().size != files.size) return Result.Invalid("duplicate file")
        // A launch-only preset places nothing; its state is still kept beside download_data's.
        val targetValue = obj.stringOr("target", if (files.isEmpty()) PresetTarget.DOWNLOAD_DATA.storageValue else "")
        val target = PresetTarget.fromStorage(targetValue) ?: return Result.Invalid("unknown target \"$targetValue\"")
        return Result.Ok(
            GamePreset(
                titleId = titleId,
                id = id,
                name = name,
                description = obj.stringOr("description").trim(),
                target = target,
                files = files,
                launch = launch,
            ),
        )
    }

    private sealed interface LaunchResult {
        data class Ok(val launch: PresetLaunch) : LaunchResult
        data class Invalid(val reason: String) : LaunchResult
    }

    /** "launch": {"env": {"KYTY_X": "1"}, "renderScale": 0.75, "gamePatch": "x.json"}; every field optional. */
    private fun parseLaunch(obj: JSONObject?): LaunchResult {
        obj ?: return LaunchResult.Invalid("launch is not an object")
        val known = setOf("env", "renderScale", "gamePatch", "playgoHack")
        obj.keys().asSequence().firstOrNull { it !in known }?.let { return LaunchResult.Invalid("unknown launch option \"$it\"") }
        val env = linkedMapOf<String, String>()
        if (obj.has("env")) {
            val envObj = obj.optJSONObject("env") ?: return LaunchResult.Invalid("launch env is not an object")
            for (key in envObj.keys().asSequence().sorted()) {
                if (!PresetLaunch.ENV_KEY.matches(key)) return LaunchResult.Invalid("env name \"$key\" is not a KYTY_ knob")
                val value = envObj.opt(key)
                if (value !is String || !PresetLaunch.ENV_VALUE.matches(value)) {
                    return LaunchResult.Invalid("invalid value for $key")
                }
                env[key] = value
            }
        }
        val renderScale = if (obj.has("renderScale")) {
            val value = obj.opt("renderScale")
            val scale = (value as? Number)?.toFloat()
            if (scale == null || scale.isNaN() || scale < PresetLaunch.MIN_RENDER_SCALE || scale > PresetLaunch.MAX_RENDER_SCALE) {
                return LaunchResult.Invalid("renderScale must be a number in ${PresetLaunch.MIN_RENDER_SCALE}..${PresetLaunch.MAX_RENDER_SCALE}")
            }
            scale
        } else {
            null
        }
        val gamePatch = if (obj.has("gamePatch")) {
            val path = obj.opt("gamePatch") as? String
            if (path == null || !isSafeRelativePath(path) || !path.endsWith(PresetLaunch.GAME_PATCH_SUFFIX)) {
                return LaunchResult.Invalid("gamePatch must be a relative .json path")
            }
            path
        } else {
            null
        }
        val playgoHack = if (obj.has("playgoHack")) {
            obj.opt("playgoHack") as? Boolean
                ?: return LaunchResult.Invalid("playgoHack must be true or false")
        } else {
            false
        }
        return LaunchResult.Ok(PresetLaunch(env, renderScale, gamePatch, playgoHack))
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
