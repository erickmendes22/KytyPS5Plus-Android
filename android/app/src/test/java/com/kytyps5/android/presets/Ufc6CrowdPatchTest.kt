package com.kytyps5.android.presets

import java.io.File
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class Ufc6CrowdPatchTest {
    private val root = File("src/main/assets/presets")
    private val catalog = PresetCatalog(DirectoryPresetSource(root))

    @Test fun crowdPatchIsOptInAndHasFourCompleteVersionGuardedInstructions() {
        val presets = GamePresets.activePerformancePresets("PPSA23566",
            catalog.forTitle("PPSA23566"))
        val preset = presets.single { it.id == "nocrowd" }
        assertTrue(preset.launch.gamePatch == "ufc6_crowd_off.json")
        assertTrue(preset.launch.env.isEmpty())
        assertNull(preset.launch.renderScale)

        val json = JSONObject(String(catalog.launchFileBytes(preset, requireNotNull(
            preset.launch.gamePatch)), Charsets.UTF_8))
        assertEquals("PPSA23566", json.getString("id"))
        assertEquals("eboot.bin", json.getString("process"))
        // The special version token is accepted only by the native loader
        // when all four 13-byte signatures have been matched in one image.
        assertEquals("@ufc6-crowd-signatures-v1", json.getString("version"))
        val mods = json.getJSONArray("mods")
        assertEquals(1, mods.length())
        val mod = mods.getJSONObject(0)
        assertTrue(mod.getBoolean("enabled"))
        val memory = mod.getJSONArray("memory")
        assertEquals(4, memory.length())
        val expected = listOf(
            "0x04a634ce" to "47030000",
            "0x04a63e94" to "53010000",
            "0x04a642a5" to "ab010000",
            "0x04a65da5" to "a0010000",
        )
        expected.forEachIndexed { index, (address, displacement) ->
            val patch = memory.getJSONObject(index)
            val off = patch.getString("off").lowercase()
            val on = patch.getString("on").lowercase()
            assertEquals(address, patch.getString("offset"))
            assertEquals("83bf180e0000000f84${displacement}", off)
            assertEquals("83bf180e00000090e9${displacement}", on)
            assertEquals(26, off.length)
            assertEquals(off.length, on.length)
        }
        // No uncontrolled gameplay-time, fighter or animation modifications.
        assertFalse(memory.toString().contains("04a1e9a0", ignoreCase = true))
    }
}
