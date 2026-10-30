package com.kytyps5.android.presets

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** No game eboot or Unreal config resolution patch should override the host render scale. */
class GlobalRenderScalePresetsTest {
    private val catalog = PresetCatalog(DirectoryPresetSource(File("src/main/assets/presets")))

    @Test fun retiredResolutionPresetsAreExcludedFromRuntime() {
        for (title in listOf("PPSA02721", "PPSA10112", "PPSA14708", "PPSA24156")) {
            val shipped = catalog.forTitle(title)
            val runtime = GamePresets.activePerformancePresets(title, shipped)
            assertFalse(runtime.any { it.id.startsWith("res") || it.id == "native720" })
            assertTrue(runtime.all { it.launch.renderScale == null })
        }
    }

    @Test fun independentCompatibilityAndEffectPresetsRemainAvailable() {
        val sh = GamePresets.activePerformancePresets("PPSA10112", catalog.forTitle("PPSA10112"))
        assertTrue(sh.any { it.id == "nort" })
        assertTrue(sh.any { it.id == "nossr" })
        val gt = GamePresets.activePerformancePresets("PPSA24156", catalog.forTitle("PPSA24156"))
        assertEquals(setOf("depthhalf", "hdrhalf", "turnipvisual"), gt.map { it.id }.toSet())
    }

    @Test fun astroHalfRateLightingRetainsEnvironmentWithoutGamePatch() {
        val astro = GamePresets.activePerformancePresets("PPSA21567", catalog.forTitle("PPSA21567")).single()
        assertEquals("perf720", astro.id)
        assertEquals("1", astro.launch.env["KYTY_RT_HALF"])
        assertNull(astro.launch.gamePatch)
        assertNull(astro.launch.renderScale)
        val plan = PresetLaunchArgs.plan(listOf(PresetLaunchArgs.Resolved(astro.id, astro.launch, null)))
        assertFalse("--game-patch" in plan.args)
        assertTrue("KYTY_RT_HALF=1" in plan.args)
    }
}
