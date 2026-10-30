package com.kytyps5.android.presets

import java.io.File
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

class NeptuniaEffectsPatchTest {
    private val assets = File("src/main/assets/presets")
    private val catalog = PresetCatalog(DirectoryPresetSource(assets))
    private val title = "PPSA02721"

    private fun patch(id: String): File {
        val preset = catalog.forTitle(title).first { it.id == id }
        return File(assets, "$title/$id/launch/${preset.launch.gamePatch}")
    }

    @Test fun verifiedShadowAndDofModsAreAvailableAndDoNotOverrideResolution() {
        val exposed = GamePresets.activePerformancePresets(title, catalog.forTitle(title))
        assertEquals(setOf("nodof", "shadow25", "shadow50"), exposed.map { it.id }.toSet())
        assertTrue(exposed.none { it.launch.renderScale != null })
        val expected = mapOf(
            "shadow50" to Triple("0x3e9fb2", "49be000c0000000c0000", "49be0006000000060000"),
            "shadow25" to Triple("0x3e9fb2", "49be000c0000000c0000", "49be0003000000030000"),
            "nodof" to Triple("0x41da50", "554889e5535089fbe85359fdff",
                "554889e5535031dbe85359fdff"),
        )
        expected.forEach { (id, bytes) ->
            val plan = JSONObject(patch(id).readText())
            assertEquals(title, plan.getString("id"))
            assertEquals("01.000.000", plan.getString("version"))
            assertEquals("eboot.bin", plan.getString("process"))
            val write = plan.getJSONArray("mods").getJSONObject(0)
                .getJSONArray("memory").getJSONObject(0)
            assertEquals(bytes.first, write.getString("offset"))
            assertEquals(bytes.second, write.getString("off"))
            assertEquals(bytes.third, write.getString("on"))
        }
    }

    @Test fun onlyOneShadowScaleIsEnabledButDofIsIndependent() {
        assertEquals(setOf("shadow50", "nodof"),
            GamePresets.normalizedShadowChoice(title,
                setOf("shadow25", "shadow50", "nodof")))
        assertEquals(setOf("shadow25", "nodof"),
            GamePresets.normalizedShadowChoice(title,
                setOf("shadow25", "shadow50", "nodof"),
                setOf("shadow50", "nodof")))
        assertEquals(setOf("shadow25", "nodof"),
            GamePresets.normalizedShadowChoice(title,
                setOf("shadow25", "nodof"),
                setOf("shadow50", "nodof")))
        assertEquals(setOf("shadow25", "shadow50"),
            GamePresets.normalizedShadowChoice("PPSA99999", setOf("shadow25", "shadow50")))
    }

    @Test fun combineDofAndShadowIntoOneValidatedGoldhenFile() {
        val temp = File.createTempFile("neptunia", "presets")
        temp.delete()
        temp.mkdirs()
        try {
            val combined = PresetGamePatchCombiner(temp).combine(title,
                listOf("shadow50" to patch("shadow50"), "nodof" to patch("nodof")))
            val payload = JSONObject(combined.readText())
            assertEquals(title, payload.getString("id"))
            assertEquals("01.000.000", payload.getString("version"))
            assertEquals("eboot.bin", payload.getString("process"))
            assertEquals(2, payload.getJSONArray("mods").length())
            val paths = listOf(
                PresetLaunchArgs.Resolved("shadow50", PresetLaunch(gamePatch = "s.json"), combined.path),
                PresetLaunchArgs.Resolved("nodof", PresetLaunch(gamePatch = "d.json"), null),
            )
            val plan = PresetLaunchArgs.plan(paths)
            assertTrue(plan.conflicts.isEmpty())
            assertEquals(1, plan.args.count { it == PresetLaunchArgs.FLAG_GAME_PATCH })
            assertFalse(PresetLaunchArgs.FLAG_RENDER_SCALE in plan.args)
        } finally {
            temp.deleteRecursively()
        }
    }

    @Test fun conflictingShadowWritesAreRejectedInsteadOfSilentlyOverriding() {
        val temp = File.createTempFile("neptunia", "overlap")
        temp.delete()
        temp.mkdirs()
        try {
            try {
                PresetGamePatchCombiner(temp).combine(title,
                    listOf("shadow50" to patch("shadow50"), "shadow25" to patch("shadow25")))
                fail("overlapping shadow writes must not be merged")
            } catch (e: IllegalArgumentException) {
                assertTrue(e.message.orEmpty().contains("overlapping"))
            }
        } finally {
            temp.deleteRecursively()
        }
    }
}
