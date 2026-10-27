package com.kytyps5.android.presets

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GranTurismoMemoryPresetsTest {
    @Test
    fun nativeResolutionPresetIsScopedOptInAndDoesNotOverrideCollectorOrOutputSize() {
        val warnings = mutableListOf<String>()
        val catalog = PresetCatalog(DirectoryPresetSource(File("src/main/assets/presets"))) {
            warnings += it
        }
        val gt = catalog.forTitle("PPSA24156")
        assertTrue(warnings.isEmpty())
        assertEquals(listOf("mem50", "mem75", "native1080"), gt.map { it.id })
        val preset = gt.single { it.id == "native1080" }
        assertEquals(mapOf("KYTY_GT_NATIVE_1080P" to "1"), preset.launch.env)
        assertTrue(preset.files.isEmpty())
        assertEquals(null, preset.launch.renderScale)
        assertEquals(null, preset.launch.gamePatch)
        assertTrue(catalog.forTitle("PPSA10112").none { it.id == preset.id })

        val plan = PresetLaunchArgs.plan(
            listOf(PresetLaunchArgs.Resolved(preset.id, preset.launch, null)),
        )
        assertTrue(plan.conflicts.isEmpty())
        assertEquals(listOf("--env", "KYTY_GT_NATIVE_1080P=1"), plan.args)
        val merged = PresetLaunchArgs.apply(
            arrayOf("--game", "/gt", "--screen-width", "1280", "--screen-height", "720",
                "--gpu-evict-written", "1", "--vma-block-size", "32", "--tessellation"),
            plan,
        ).toList()
        assertTrue("KYTY_GT_NATIVE_1080P=1" in merged)
        assertTrue("--gpu-evict-written" in merged)
        assertTrue("--vma-block-size" in merged)
        assertTrue("--tessellation" in merged)
        assertTrue("--screen-width" in merged)
        assertEquals("1280", merged[merged.indexOf("--screen-width") + 1])
        assertFalse("--game-patch" in merged)
    }
}
