package com.kytyps5.android.presets

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class PresetLaunchArgsTest {

    private fun resolved(id: String, launch: PresetLaunch, patch: String? = null) = PresetLaunchArgs.Resolved(id, launch, patch)

    private val perf = resolved(
        "perf720",
        PresetLaunch(env = mapOf("KYTY_RT_HALF" to "1"), renderScale = 0.75f, gamePatch = "p.json"),
        "/data/files/_GamePresets/PPSA21567/perf720.launch/p.json",
    )

    @Test
    fun planListsEnvThenScaleThenPatch() {
        val plan = PresetLaunchArgs.plan(listOf(perf))
        assertEquals(
            listOf(
                "--env", "KYTY_RT_HALF=1",
                "--render-scale", "0.75",
                "--game-patch", "/data/files/_GamePresets/PPSA21567/perf720.launch/p.json",
            ),
            plan.args,
        )
        assertEquals(emptyList<String>(), plan.conflicts)
    }

    @Test
    fun noPresetsMeansNoArgumentsAndTheCommandLineIsUnchanged() {
        val plan = PresetLaunchArgs.plan(emptyList())
        assertTrue(plan.isEmpty)
        val base = arrayOf("--data-dir", "/d", "--game", "/g", "--render-scale", "0.5")
        assertSame(base, PresetLaunchArgs.apply(base, plan))
    }

    @Test
    fun aPatchIsOnlyPassedWhenItWasPlaced() {
        val plan = PresetLaunchArgs.plan(listOf(resolved("x", PresetLaunch(gamePatch = "p.json"), patch = null)))
        assertTrue(plan.isEmpty)
    }

    @Test
    fun theFirstPresetWinsAndConflictsAreReported() {
        val other = resolved(
            "other",
            PresetLaunch(env = mapOf("KYTY_RT_HALF" to "2", "KYTY_X" to "1"), renderScale = 0.5f, gamePatch = "q.json"),
            "/q.json",
        )
        val same = resolved("same", PresetLaunch(env = mapOf("KYTY_RT_HALF" to "1"), renderScale = 0.75f))

        val plan = PresetLaunchArgs.plan(listOf(perf, other, same))

        assertEquals(
            listOf(
                "--env", "KYTY_RT_HALF=1", "--env", "KYTY_X=1",
                "--render-scale", "0.75",
                "--game-patch", "/data/files/_GamePresets/PPSA21567/perf720.launch/p.json",
            ),
            plan.args,
        )
        assertEquals(
            listOf(
                "other: KYTY_RT_HALF already set by perf720",
                "other: render scale already set by perf720",
                "other: game patch already set by perf720",
            ),
            plan.conflicts,
        )
    }

    @Test
    fun applyReplacesScaleAndPatchAndAppendsEnvLast() {
        val base = arrayOf(
            "--data-dir", "/d", "--game", "/g", "--render-scale", "0.5", "--env", "KYTY_RT_HALF=2",
            "--game-patch", "/old.json", "--perf-stats",
        )

        val merged = PresetLaunchArgs.apply(base, PresetLaunchArgs.plan(listOf(perf)))

        assertArrayEquals(
            arrayOf(
                "--data-dir", "/d", "--game", "/g", "--env", "KYTY_RT_HALF=2", "--perf-stats",
                "--env", "KYTY_RT_HALF=1",
                "--render-scale", "0.75",
                "--game-patch", "/data/files/_GamePresets/PPSA21567/perf720.launch/p.json",
            ),
            merged,
        )
    }

    @Test
    fun envOnlyPresetsKeepTheUsersScale() {
        val plan = PresetLaunchArgs.plan(listOf(resolved("rt", PresetLaunch(env = mapOf("KYTY_RT_HALF" to "1")))))
        val merged = PresetLaunchArgs.apply(arrayOf("--game", "/g", "--render-scale", "0.5"), plan)
        assertArrayEquals(arrayOf("--game", "/g", "--render-scale", "0.5", "--env", "KYTY_RT_HALF=1"), merged)
    }
}
