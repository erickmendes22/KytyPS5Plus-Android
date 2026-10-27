package com.kytyps5.android.presets

import java.io.File
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class GranTurismoMemoryPresetsTest {
    @Test
    fun onlyCorrected720ModeShipsAndUsesMemoryCollector() {
        val warnings = mutableListOf<String>()
        val catalog = PresetCatalog(DirectoryPresetSource(File("src/main/assets/presets"))) {
            warnings += it
        }
        val gt = catalog.forTitle("PPSA24156")
        assertTrue(warnings.isEmpty())
        assertEquals(listOf("native720"), gt.map { it.id })
        val preset = gt.single()
        assertTrue(preset.files.isEmpty())
        assertTrue(preset.launch.env.isEmpty())
        assertEquals("gt_native720.json", preset.launch.gamePatch)
        val scale = requireNotNull(preset.launch.renderScale)
        assertTrue(scale > 0.3333f && scale < 0.3334f)
        val patch = JSONObject(String(catalog.launchFileBytes(preset, "gt_native720.json")))
        assertEquals("PPSA24156", patch.getString("id"))
        assertEquals("01.010.000", patch.getString("version"))
        assertEquals("eboot.bin", patch.getString("process"))
        assertEquals(8, patch.getJSONArray("mods").getJSONObject(0).getJSONArray("memory").length())

        val path = "/data/gt_native720.json"
        val plan = PresetLaunchArgs.plan(listOf(
            PresetLaunchArgs.Resolved(preset.id, preset.launch, path),
        ))
        assertTrue(plan.conflicts.isEmpty())
        val merged = PresetLaunchArgs.apply(
            arrayOf("--game", "/gt", "--screen-width", "1280", "--screen-height", "720",
                "--gpu-evict-written", "1", "--vma-block-size", "32", "--tessellation"),
            plan,
        ).toList()
        assertEquals("0.33333334", merged[merged.indexOf("--render-scale") + 1])
        assertEquals(path, merged[merged.indexOf("--game-patch") + 1])
        assertEquals("1", merged[merged.indexOf("--gpu-evict-written") + 1])
        assertEquals("32", merged[merged.indexOf("--vma-block-size") + 1])
        assertEquals(1, merged.count { it == "--tessellation" })
        assertTrue(catalog.forTitle("PPSA10112").none { it.id == preset.id })
    }
}
