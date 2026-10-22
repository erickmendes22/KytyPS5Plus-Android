package com.kytyps5.android.presets

import android.content.res.AssetManager
import android.util.Log
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException

/** Read-only tree the presets are shipped in (the APK's assets, or a folder in tests). */
interface PresetSource {
    /** Names directly below [path] ("" is the root); empty when [path] is missing or a file. */
    fun list(path: String): List<String>

    /** Contents of the file at [path]. */
    @Throws(IOException::class)
    fun read(path: String): ByteArray
}

class AssetPresetSource(private val assets: AssetManager, private val root: String = PresetCatalog.ASSET_ROOT) : PresetSource {
    override fun list(path: String): List<String> = try {
        assets.list(join(root, path))?.toList().orEmpty()
    } catch (e: IOException) {
        emptyList()
    }

    override fun read(path: String): ByteArray = assets.open(join(root, path)).use { it.readBytes() }
}

class DirectoryPresetSource(private val root: File) : PresetSource {
    override fun list(path: String): List<String> =
        File(root, path).takeIf { it.isDirectory }?.list()?.sorted().orEmpty()

    override fun read(path: String): ByteArray {
        val file = File(root, path)
        if (!file.isFile) throw FileNotFoundException(file.path)
        return file.readBytes()
    }
}

private fun join(a: String, b: String): String = listOf(a, b).filter { it.isNotEmpty() }.joinToString("/")

/** The presets shipped for each title: <TITLE_ID>/<preset id>/preset.json (+ files/...). */
class PresetCatalog(private val source: PresetSource, private val log: (String) -> Unit = { Log.w(TAG, it) }) {

    /** Valid presets for [titleId] (case-insensitive), sorted by id; invalid ones are logged and skipped. */
    fun forTitle(titleId: String): List<GamePreset> {
        val folder = source.list("").firstOrNull { it.equals(titleId, ignoreCase = true) && TitleIds.isValid(it) }
            ?: return emptyList()
        return source.list(folder).sorted().mapNotNull { presetFolder -> load(folder, presetFolder) }
    }

    /** Bytes of one of [preset]'s files ([GamePreset.files] entry). */
    @Throws(IOException::class)
    fun fileBytes(preset: GamePreset, path: String): ByteArray =
        source.read(join(join(preset.titleId, preset.id), "${PresetManifest.FILES_DIR}/$path"))

    private fun load(titleFolder: String, presetFolder: String): GamePreset? {
        val manifestPath = "$titleFolder/$presetFolder/${PresetManifest.FILE_NAME}"
        val json = try {
            String(source.read(manifestPath), Charsets.UTF_8)
        } catch (e: IOException) {
            log("$manifestPath: ${e.message}")
            return null
        }
        return when (val result = PresetManifest.parse(titleFolder, presetFolder, json)) {
            is PresetManifest.Result.Ok -> result.preset
            is PresetManifest.Result.Invalid -> {
                log("$manifestPath: ${result.reason}")
                null
            }
        }
    }

    companion object {
        private const val TAG = "KytyPresets"
        const val ASSET_ROOT = "presets"
    }
}
