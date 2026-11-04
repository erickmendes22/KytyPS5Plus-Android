package com.kytyps5.android.library

import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.RandomAccessFile
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/**
 * Opt-in, memory-only first-boot flow patches for Demon's Souls PPSA01341 01.007.000.
 * Ported from Supermedo/KytyPS5-SuperMedo/tools/local/demons-souls-patch.py.
 *
 * Never edits eboot.bin, saves, or game assets. The checked signatures must
 * match *all three* PC reference sites, or no cheat JSON is generated.
 * The native GamePatch loader independently revalidates exact original bytes,
 * TITLE_ID, APP_VER and process before patching guest memory at boot.
 */
object DemonsSoulsFirstBootPatch {
    private const val VERSION = "01.007.000"
    private const val TITLE = "PPSA01341"
    private const val OUT = "demons-souls-first-boot-1.07.json"

    private data class Site(val label: String, val va: Long, val length: Int, val signature: String,
                            val replacementOffset: Int, val replacement: ByteArray)

    private val sites = listOf(
        Site("skip character-creation transition", 0x7c0a4fL, 32,
             "498b3fb8011f0afa488d3532e16d02488d55d0", 0x17, ByteArray(5) { 0x90.toByte() }),
        Site("skip new-profile language/brightness/settings screens", 0x7c9254L, 16,
             "c7430c2d000000498b07483b45d8", 3, byteArrayOf(0x1b, 0, 0, 0)),
        Site("suppress fallback language transition", 0x7c930cL, 32,
             "488d35e520b402b8011f0afa488d55cc488945cce8ab946100", 0x14,
             ByteArray(5) { 0x90.toByte() }),
    )

    data class Result(val path: File? = null, val error: String? = null, val verifiedMapping: String? = null)

    fun prepare(gameDir: File, outputDir: File): Result {
        val candidates = listOf(File(gameDir, "decrypted/eboot.bin"), File(gameDir, "eboot.bin"))
        val errors = mutableListOf<String>()
        for (eboot in candidates) {
            if (!eboot.isFile || !eboot.canRead()) continue
            try {
                val patches = JSONArray()
                var verifiedMapping = ""
                RandomAccessFile(eboot, "r").use { raf ->
                    val mapping = Ps5ExecutableMapping.locate(
                        raf, sites.map { Ps5ExecutableMapping.Probe(it.va, hexBytes(it.signature)) }
                    )
                    verifiedMapping = mapping.format + ", executable file base 0x" +
                        mapping.fileBase.toString(16) + " (three verified signatures)"
                    for (site in sites) {
                        val offset = mapping.fileOffset(site.va)
                        if (offset + site.length > raf.length()) {
                            throw IllegalArgumentException("short eboot at 0x" + offset.toString(16))
                        }
                        val original = ByteArray(site.length)
                        raf.seek(offset)
                        raf.readFully(original)
                        val expected = hexBytes(site.signature)
                        if (!original.copyOfRange(0, expected.size).contentEquals(expected)) {
                            throw IllegalArgumentException(site.label + ": eboot signature mismatch at VA 0x" +
                                site.va.toString(16))
                        }
                        val modified = original.clone()
                        site.replacement.copyInto(modified, destinationOffset = site.replacementOffset)
                        if (original.contentEquals(modified)) throw IllegalArgumentException("patch has no effect")
                        val write = JSONObject().put("offset", site.va.toString(16))
                            .put("off", toHex(original)).put("on", toHex(modified))
                        patches.put(JSONObject().put("name", site.label).put("enabled", true)
                            .put("memory", JSONArray().put(write)))
                    }
                }
                outputDir.mkdirs()
                if (!outputDir.isDirectory) return Result(error = "Cannot create private patch directory")
                val output = File(outputDir, OUT)
                val patch = JSONObject().put("id", TITLE).put("version", VERSION)
                    .put("process", "eboot.bin").put("mods", patches).toString(2) + "\n"
                val temp = File.createTempFile(".ds-boot-", ".json", outputDir)
                try {
                    FileOutputStream(temp).use {
                        it.write(patch.toByteArray(Charsets.UTF_8))
                        it.fd.sync()
                    }
                    Files.move(temp.toPath(), output.toPath(), StandardCopyOption.REPLACE_EXISTING)
                } finally { temp.delete() }
                return Result(path = output, verifiedMapping = verifiedMapping)
            } catch (error: Exception) {
                errors.add(eboot.name + ": " + (error.message ?: error.javaClass.simpleName))
            }
        }
        return Result(error = if (errors.isEmpty())
            "No readable decrypted eboot.bin found. This patch needs the PS5 1.07 executable."
            else errors.joinToString("; "))
    }

    private fun hexBytes(s: String): ByteArray {
        require(s.length % 2 == 0)
        return ByteArray(s.length / 2) { i -> s.substring(i * 2, i * 2 + 2).toInt(16).toByte() }
    }

    private fun toHex(bytes: ByteArray): String {
        val hex = "0123456789abcdef"
        return buildString(bytes.size * 2) {
            for (b in bytes) {
                val n = b.toInt() and 255
                append(hex[n ushr 4])
                append(hex[n and 15])
            }
        }
    }
}
