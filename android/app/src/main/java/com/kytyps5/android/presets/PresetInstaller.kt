package com.kytyps5.android.presets

import com.kytyps5.android.stringOr
import java.io.File
import java.io.IOException
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject

/**
 * Places and removes preset files in a title's data folders, touching only files it placed itself.
 *
 * What it placed is recorded per preset in [stateRoot]/<TITLE_ID>/<preset id>.json (outside the
 * folders the game sees). A file that already existed where a preset file goes (not ours) is moved
 * to <preset id>.backup/ first and put back on removal. Everything else in the game's folders (its
 * own generated config, saves) is never read or written.
 *
 * Applying is idempotent: files whose contents already match are left alone, and a file the game
 * rewrote is overwritten with the preset's version again. Removing deletes our files even if the
 * game rewrote them (the content was derived from ours).
 */
class PresetInstaller(
    private val filesDir: File,
    private val stateRoot: File = File(filesDir, STATE_DIR_NAME),
) {
    /** What one reconcile did, for the log. */
    data class Report(val applied: List<String>, val removed: List<String>, val errors: List<String>)

    /**
     * Makes [titleId]'s folders match the choice: each of [available] in [enabledIds] applied, the
     * rest removed, and anything recorded for a preset no longer shipped (an app update) removed too.
     */
    fun reconcile(
        titleId: String,
        available: List<GamePreset>,
        enabledIds: Set<String>,
        bytes: (GamePreset, String) -> ByteArray,
    ): Report {
        val applied = mutableListOf<String>()
        val removed = mutableListOf<String>()
        val errors = mutableListOf<String>()
        available.forEach { preset ->
            try {
                if (preset.id in enabledIds) {
                    apply(preset) { path -> bytes(preset, path) }
                    applied += preset.id
                } else if (remove(titleId, preset.id)) {
                    removed += preset.id
                }
            } catch (e: IOException) {
                errors += "${preset.id}: ${e.message}"
            }
        }
        val shipped = available.map { it.id }.toSet()
        appliedPresetIds(titleId).filter { it !in shipped }.forEach { orphan ->
            try {
                if (remove(titleId, orphan)) removed += orphan
            } catch (e: IOException) {
                errors += "$orphan: ${e.message}"
            }
        }
        return Report(applied, removed, errors)
    }

    /** Places [preset]'s files (contents from [bytes] by preset path) and records them. */
    @Throws(IOException::class)
    fun apply(preset: GamePreset, bytes: (String) -> ByteArray) {
        // Read everything first: a missing preset file must not leave a half-applied preset.
        val contents = preset.files.associateWith(bytes)
        val targetDir = preset.target.directory(filesDir, preset.titleId)
        val previous = readState(preset.titleId, preset.id)
        val ours = previous?.files?.associateBy { it.path }.orEmpty()
        // A file an older version of the preset placed but this one no longer ships.
        previous?.files?.filter { it.path !in preset.files }?.forEach { removeFile(previous, it) }
        val records = preset.files.map { path ->
            val earlier = ours[path]
            val dest = File(targetDir, path)
            val backedUp = earlier?.backedUp ?: (dest.exists() && backup(preset.titleId, preset.id, path, dest))
            AppliedFile(path, backedUp)
        }
        // Record before writing, so a crash part-way leaves files a later removal still knows about.
        writeState(AppliedState(preset.titleId, preset.id, preset.target, records))
        contents.forEach { (path, data) -> writeIfChanged(File(targetDir, path), data) }
    }

    /** Removes what [presetId] placed for [titleId] and restores backups; false when nothing was applied. */
    @Throws(IOException::class)
    fun remove(titleId: String, presetId: String): Boolean {
        val state = readState(titleId, presetId) ?: return false
        state.files.forEach { removeFile(state, it) }
        backupDir(titleId, presetId).deleteRecursively()
        if (!stateFile(titleId, presetId).delete()) throw IOException("cannot delete state for $presetId")
        return true
    }

    /** Ids of presets with files placed for [titleId]. */
    fun appliedPresetIds(titleId: String): List<String> =
        File(stateRoot, titleId).listFiles { f -> f.isFile && f.name.endsWith(STATE_SUFFIX) }
            ?.map { it.name.removeSuffix(STATE_SUFFIX) }?.sorted().orEmpty()

    private fun removeFile(state: AppliedState, file: AppliedFile) {
        val dest = File(state.target.directory(filesDir, state.titleId), file.path)
        if (dest.isFile && !dest.delete()) throw IOException("cannot delete ${dest.path}")
        if (file.backedUp) {
            val saved = File(backupDir(state.titleId, state.presetId), file.path)
            if (saved.isFile) {
                dest.parentFile?.mkdirs()
                if (!saved.renameTo(dest)) saved.copyTo(dest, overwrite = true)
            }
        }
    }

    private fun backup(titleId: String, presetId: String, path: String, dest: File): Boolean {
        if (!dest.isFile) throw IOException("${dest.path} exists and is not a file")
        val saved = File(backupDir(titleId, presetId), path)
        saved.parentFile?.mkdirs()
        dest.copyTo(saved, overwrite = true)
        return true
    }

    private fun writeIfChanged(dest: File, data: ByteArray) {
        if (dest.isFile && dest.length() == data.size.toLong() && dest.readBytes().contentEquals(data)) return
        val parent = dest.parentFile ?: throw IOException("no parent for ${dest.path}")
        if (!parent.isDirectory && !parent.mkdirs()) throw IOException("cannot create ${parent.path}")
        val temp = File(parent, ".${dest.name}.preset-tmp")
        temp.writeBytes(data)
        if (!temp.renameTo(dest)) {
            temp.delete()
            throw IOException("cannot write ${dest.path}")
        }
    }

    private fun stateFile(titleId: String, presetId: String) = File(File(stateRoot, titleId), presetId + STATE_SUFFIX)

    private fun backupDir(titleId: String, presetId: String) = File(File(stateRoot, titleId), presetId + BACKUP_SUFFIX)

    private fun readState(titleId: String, presetId: String): AppliedState? {
        val file = stateFile(titleId, presetId)
        if (!file.isFile) return null
        return AppliedState.fromJson(file.readText()) ?: throw IOException("corrupt state ${file.path}")
    }

    private fun writeState(state: AppliedState) {
        val file = stateFile(state.titleId, state.presetId)
        file.parentFile?.mkdirs()
        val temp = File(file.parentFile, file.name + ".tmp")
        temp.writeText(state.toJson())
        if (!temp.renameTo(file)) throw IOException("cannot write ${file.path}")
    }

    private data class AppliedFile(val path: String, val backedUp: Boolean)

    private data class AppliedState(
        val titleId: String,
        val presetId: String,
        val target: PresetTarget,
        val files: List<AppliedFile>,
    ) {
        fun toJson(): String = JSONObject()
            .put("titleId", titleId)
            .put("preset", presetId)
            .put("target", target.storageValue)
            .put("files", JSONArray(files.map { JSONObject().put("path", it.path).put("backedUp", it.backedUp) }))
            .toString(2)

        companion object {
            fun fromJson(json: String): AppliedState? = try {
                val obj = JSONObject(json)
                val array = obj.getJSONArray("files")
                val files = (0 until array.length()).map { i ->
                    val entry = array.getJSONObject(i)
                    AppliedFile(entry.stringOr("path"), entry.optBoolean("backedUp", false))
                }
                val target = PresetTarget.fromStorage(obj.stringOr("target"))
                val titleId = obj.stringOr("titleId")
                if (target == null || !TitleIds.isValid(titleId) || files.any { !PresetManifest.isSafeRelativePath(it.path) }) {
                    null
                } else {
                    AppliedState(titleId, obj.stringOr("preset"), target, files)
                }
            } catch (e: JSONException) {
                null
            }
        }
    }

    companion object {
        /** Under the app's files directory, beside (not inside) the title's data folders. */
        const val STATE_DIR_NAME = "_GamePresets"
        private const val STATE_SUFFIX = ".json"
        private const val BACKUP_SUFFIX = ".backup"
    }
}

