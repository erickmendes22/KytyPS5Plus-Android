package com.kytyps5.android.library

import java.io.File
import java.io.RandomAccessFile
import java.nio.file.Files
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DemonsSoulsFirstBootPatchTest {
    private val sites = listOf(
        0x7c0a4fL to "498b3fb8011f0afa488d3532e16d02488d55d0",
        0x7c9254L to "c7430c2d000000498b07483b45d8",
        0x7c930cL to "488d35e520b402b8011f0afa488d55cc488945cce8ab946100",
    )

    @Test
    fun checksReferenceSitesAndGeneratesMemoryOnlyGoldHenJson() {
        val folder = Files.createTempDirectory("kyty-ds-patch").toFile()
        try {
            val decrypted = File(folder, "decrypted").apply { mkdirs() }
            val eboot = File(decrypted, "eboot.bin")
            RandomAccessFile(eboot, "rw").use { raf ->
                raf.setLength(0x7c930cL + 0x4000 + 32)
                for ((va, prefix) in sites) {
                    val bytes = prefix.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
                    raf.seek(va + 0x4000)
                    raf.write(bytes)
                }
            }
            val outDir = File(folder, "patches")
            val outcome = DemonsSoulsFirstBootPatch.prepare(folder, outDir)
            assertNull(outcome.error)
            val output = requireNotNull(outcome.path)
            val json = JSONObject(output.readText())
            assertEquals("PPSA01341", json.getString("id"))
            assertEquals("01.007.000", json.getString("version"))
            assertEquals("eboot.bin", json.getString("process"))
            val patches = json.getJSONArray("mods")
            assertEquals(3, patches.length())
            assertEquals("90".repeat(5),
                patches.getJSONObject(0).getJSONArray("memory").getJSONObject(0).getString("on").substring(46, 56))
            val changedState = patches.getJSONObject(1).getJSONArray("memory").getJSONObject(0).getString("on")
            assertTrue(changedState.startsWith("c7430c1b000000"))
            val changedFallback = patches.getJSONObject(2).getJSONArray("memory").getJSONObject(0).getString("on")
            assertTrue(changedFallback.substring(40, 50) == "9090909090")
        } finally { folder.deleteRecursively() }
    }

    @Test
    fun refusesMismatchedExecutableWithoutWritingPatch() {
        val folder = Files.createTempDirectory("kyty-ds-mismatch").toFile()
        try {
            val eboot = File(folder, "eboot.bin")
            RandomAccessFile(eboot, "rw").use { it.setLength(0x7c930c + 0x4000 + 32L) }
            val out = File(folder, "patches")
            val result = DemonsSoulsFirstBootPatch.prepare(folder, out)
            assertNotNull(result.error)
            assertNull(result.path)
            assertFalse(File(out, "demons-souls-first-boot-1.07.json").exists())
        } finally { folder.deleteRecursively() }
    }
}
