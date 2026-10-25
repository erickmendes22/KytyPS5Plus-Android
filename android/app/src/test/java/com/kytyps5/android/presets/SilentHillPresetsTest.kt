package com.kytyps5.android.presets

import com.kytyps5.android.GameEnvironment
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SilentHillPresetsTest {
    private fun presets(): List<GamePreset> {
        val warnings = mutableListOf<String>()
        val root = File("src/main/assets/presets")
        assertTrue(root.isDirectory)
        val catalog = PresetCatalog(DirectoryPresetSource(root)) { warnings += it }
        val presets = catalog.forTitle("PPSA10112")
        assertTrue(warnings.isEmpty())
        assertEquals(11, presets.size)
        assertEquals(GameEnvironment.silentHillPresetKeys, presets.flatMap { it.launch.env.keys }.toSet())
        return presets
    }

    @Test
    fun effectsCombineWithOneNativeResolutionWithoutFilePatches() {
        val selected = presets().filter { !it.id.startsWith("res") || it.id == "res50" }
        assertTrue(selected.all { it.files.isEmpty() && it.launch.gamePatch == null })
        val plan = PresetLaunchArgs.plan(selected.map { PresetLaunchArgs.Resolved(it.id, it.launch, null) })
        assertTrue(plan.conflicts.isEmpty())
        val args = PresetLaunchArgs.apply(arrayOf("--game", "/g", "--render-scale", "0.5"), plan).toList()
        assertEquals(1, args.count { it == "--render-scale" })
        assertEquals("1.0", args[args.indexOf("--render-scale") + 1])
        assertTrue("KYTY_SH_SCREEN_PERCENTAGE=50" in args)
        for (key in listOf("SHADOWS", "FOG", "SSR", "AO", "BLOOM", "DOF", "MOTION_BLUR", "RT")) {
            assertTrue("KYTY_SH_NO_$key=1" in args)
        }
        assertFalse("--game-patch" in args)
        assertFalse("--screen-width" in plan.args)
        assertFalse("--screen-height" in plan.args)
    }

    @Test
    fun resolutionConflictsAreReportedAndEffectsKeepHostScalingChoice() {
        val all = presets()
        val resolution = PresetLaunchArgs.plan(all.filter { it.id.startsWith("res") }
            .map { PresetLaunchArgs.Resolved(it.id, it.launch, null) })
        assertEquals(2, resolution.conflicts.size)
        assertEquals(1, resolution.args.count { it.startsWith("KYTY_SH_SCREEN_PERCENTAGE=") })
        val effects = PresetLaunchArgs.plan(all.filter { !it.id.startsWith("res") }
            .map { PresetLaunchArgs.Resolved(it.id, it.launch, null) })
        assertTrue(effects.conflicts.isEmpty())
        assertFalse("--render-scale" in effects.args)
    }
}
