package com.kytyps5.android.presets

import java.io.File
import org.json.JSONArray
import org.json.JSONObject

/**
 * The native loader accepts one --game-patch file per game. Combine separately
 * selected (and byte-checked) independent GoldHEN-style mods before launch,
 * keeping the game/APP_VER/process guards and rejecting conflicting writes.
 */
internal class PresetGamePatchCombiner(private val filesDir: File) {
    fun combine(titleId: String, sources: List<Pair<String, File>>): File {
        require(sources.size >= 2) { "At least two independent patches are required" }
        val merged = JSONArray()
        val ranges = mutableListOf<Pair<Long, Long>>()
        var version: String? = null
        var process: String? = null
        sources.forEach { (presetId, sourceFile) ->
            val source = JSONObject(sourceFile.readText())
            val id = source.getString("id")
            require(id.equals(titleId, ignoreCase = true)) {
                "$presetId: patch title ID does not match $titleId"
            }
            val sourceVersion = source.getString("version")
            val sourceProcess = source.getString("process")
            require(sourceVersion.isNotBlank() && sourceProcess.isNotBlank()) {
                "$presetId: missing version or executable"
            }
            if (version == null) {
                version = sourceVersion
                process = sourceProcess
            } else {
                require(version == sourceVersion && process.equals(sourceProcess, true)) {
                    "$presetId: patch version or executable differs from another preset"
                }
            }
            val mods = source.getJSONArray("mods")
            for (modIndex in 0 until mods.length()) {
                val original = mods.getJSONObject(modIndex)
                val mod = JSONObject(original.toString())
                mod.put("name", "$presetId: ${mod.getString("name")}")
                val enabled = mod.optBoolean("enabled", true)
                if (enabled) {
                    val writes = mod.getJSONArray("memory")
                    for (writeIndex in 0 until writes.length()) {
                        val write = writes.getJSONObject(writeIndex)
                        val originalBytes = write.getString("off")
                        val replacementBytes = write.getString("on")
                        require(validHex(originalBytes) && validHex(replacementBytes) &&
                                originalBytes.length == replacementBytes.length) {
                            "$presetId: invalid original/replacement bytes"
                        }
                        val address = write.getString("offset")
                            .removePrefix("0x").removePrefix("0X").toLong(16)
                        val end = Math.addExact(address, originalBytes.length / 2L)
                        require(address >= 0L && ranges.none { (start, stop) ->
                            address < stop && start < end
                        }) {
                            "$presetId: overlapping executable patch at 0x${address.toString(16)}"
                        }
                        ranges.add(address to end)
                    }
                }
                merged.put(mod)
            }
        }
        val target = File(File(File(filesDir, PresetInstaller.STATE_DIR_NAME), titleId),
            "combined-game-patch.json")
        target.parentFile?.mkdirs()
        val output = JSONObject().put("id", titleId).put("version", version)
            .put("process", process).put("mods", merged).toString(2) + "\n"
        val temporary = File(target.parentFile, target.name + ".tmp")
        temporary.writeText(output)
        if (!temporary.renameTo(target)) {
            try {
                temporary.copyTo(target, overwrite = true)
            } finally {
                temporary.delete()
            }
        }
        return target
    }

    private fun validHex(value: String): Boolean =
        value.isNotEmpty() && value.length % 2 == 0 &&
            value.all { it in '0'..'9' || it in 'a'..'f' || it in 'A'..'F' }
}
