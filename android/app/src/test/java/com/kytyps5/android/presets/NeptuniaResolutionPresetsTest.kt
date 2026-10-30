package com.kytyps5.android.presets

import java.io.File
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class NeptuniaResolutionPresetsTest {
    @Test fun shippedPPSA02721PatchesMatchKnownEbootByteLayout() {
        val catalog = PresetCatalog(DirectoryPresetSource(File("src/main/assets/presets")))
        val expected = linkedMapOf(
            "res1080" to (1920 to 1080),
            "res720" to (1280 to 720),
            "res540" to (960 to 540),
            "res360" to (640 to 360),
        )
        val original = linkedMapOf(
            "0x27d7e0" to "b8000f0000",
            "0x27d7f0" to "b870080000",
            "0x3f20e8" to "b9000f000041b870080000",
            "0x4294cf" to "ba000f0000b970080000",
            "0x429532" to "be000f0000ba70080000",
            "0x3f3775" to "49be8007000038040000",
            "0x3f3916" to "48bb8007000038040000",
            "0x3f39d9" to "49bf8007000038040000",
        )
        val modes = catalog.forTitle("PPSA02721")
        assertEquals(expected.keys.toSet(), modes.map { it.id }.toSet())
        for (mode in modes) {
            assertEquals(emptyList<String>(), mode.files)
            assertEquals(null, mode.launch.renderScale)
            val fileName = requireNotNull(mode.launch.gamePatch)
            val json = JSONObject(String(catalog.launchFileBytes(mode, fileName)))
            assertEquals("PPSA02721", json.getString("id"))
            assertEquals("01.000.000", json.getString("version"))
            assertEquals("eboot.bin", json.getString("process"))
            val entries = json.getJSONArray("mods").getJSONObject(0).getJSONArray("memory")
            assertEquals(8, entries.length())
            val addresses = mutableSetOf<String>()
            val (width, height) = requireNotNull(expected[mode.id])
            fun le32(value: Int) = (0..3).joinToString("") { i -> "%02x".format((value ushr (i * 8)) and 0xff) }
            val w = le32(width)
            val h = le32(height)
            val newBytes = mapOf(
                "0x27d7e0" to "b8$w", "0x27d7f0" to "b8$h",
                "0x3f20e8" to "b9${w}41b8$h",
                "0x4294cf" to "ba${w}b9$h",
                "0x429532" to "be${w}ba$h",
                "0x3f3775" to "49be$w$h",
                "0x3f3916" to "48bb$w$h",
                "0x3f39d9" to "49bf$w$h",
            )
            for (i in 0 until entries.length()) {
                val entry = entries.getJSONObject(i)
                val address = entry.getString("offset")
                assertTrue(addresses.add(address))
                assertEquals(original[address], entry.getString("off"))
                assertEquals(newBytes[address], entry.getString("on"))
                assertEquals(entry.getString("off").length, entry.getString("on").length)
            }
            assertEquals(original.keys, addresses)
        }
        // Also ensure the Android package advertises game classification.
        val manifest = File("src/main/AndroidManifest.xml").readText()
        assertTrue(manifest.contains("android:appCategory=\"game\""))
        assertTrue(manifest.contains("android:isGame=\"true\""))
        assertTrue(manifest.contains("android.intent.category.GAME"))
        assertFalse(manifest.contains("android:appCategory=\"productivity\""))
    }
}
