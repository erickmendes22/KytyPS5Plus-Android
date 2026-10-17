package com.kytyps5.android

import com.kytyps5.android.library.EnvVar
import com.kytyps5.android.library.GameSettings
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

class LaunchArgsTest {

    @Test
    fun forGameBuildsEmulatorFlagsInOrder() {
        val args = LaunchArgs.forGame("/sdcard/KytyPS5/games/CUSA1", Resolution.FULL_HD_1080)
        assertArrayEquals(
            arrayOf("--game", "/sdcard/KytyPS5/games/CUSA1", "--screen-width", "1920", "--screen-height", "1080"),
            args,
        )
    }

    @Test
    fun renderScaleIsPassedOnlyWhenNotNative() {
        val native = LaunchArgs.forGame("/g", Resolution.HD_720, null, RenderScale.NATIVE)
        assertArrayEquals(arrayOf("--game", "/g", "--screen-width", "1280", "--screen-height", "720"), native)

        val half = LaunchArgs.forGame("/g", Resolution.HD_720, "/drv", RenderScale.HALF)
        assertArrayEquals(
            arrayOf("--game", "/g", "--screen-width", "1280", "--screen-height", "720", "--gpu-driver", "/drv", "--render-scale", "0.5"),
            half,
        )
        assertEquals("0.75", RenderScale.THREE_QUARTER.factor)
    }

    @Test
    fun renderScaleRoundTripsThroughStorageAndGameSettingsJson() {
        assertEquals(RenderScale.NATIVE, RenderScale.fromStorage(null))
        assertEquals(RenderScale.NATIVE, RenderScale.fromStorage("bogus"))
        assertEquals(RenderScale.HALF, RenderScale.fromStorage(RenderScale.HALF.storageValue))

        val settings = GameSettings(renderScale = RenderScale.THREE_QUARTER)
        assertEquals(settings, GameSettings.fromJson(settings.toJson()))
        assertEquals(null, GameSettings.fromJson(GameSettings.DEFAULT.toJson()).renderScale)
    }

    @Test
    fun renderScaleFollowsGlobalUnlessTheGameOverrides() {
        val global = LaunchOptions.resolve(
            Resolution.HD_720, globalHud = false, globalTouch = true, game = GameSettings.DEFAULT,
            globalRenderScale = RenderScale.HALF,
        )
        assertEquals(RenderScale.HALF, global.renderScale)

        val game = GameSettings(renderScale = RenderScale.THREE_QUARTER)
        val overridden = LaunchOptions.resolve(
            Resolution.HD_720, globalHud = false, globalTouch = true, game = game,
            globalRenderScale = RenderScale.HALF,
        )
        assertEquals(RenderScale.THREE_QUARTER, overridden.renderScale)
    }

    @Test
    fun resolutionRoundTripsThroughStorageAndDefaultsTo720p() {
        assertEquals(Resolution.HD_720, Resolution.fromStorage(null))
        assertEquals(Resolution.HD_720, Resolution.fromStorage("bogus"))
        assertEquals(Resolution.FULL_HD_1080, Resolution.fromStorage(Resolution.FULL_HD_1080.storageValue))
    }

    @Test
    fun launchOptionsFollowGlobalUnlessTheGameOverrides() {
        val global = LaunchOptions.resolve(Resolution.HD_720, globalHud = false, globalTouch = true, game = GameSettings.DEFAULT)
        assertEquals(LaunchOptions(Resolution.HD_720, hud = false, touchControls = true, env = emptyList()), global)

        val game = GameSettings(Resolution.FULL_HD_1080, listOf(EnvVar("TU_DEBUG", "sysmem")), hud = true, touchControls = false)
        assertEquals(
            LaunchOptions(Resolution.FULL_HD_1080, hud = true, touchControls = false, env = listOf(EnvVar("TU_DEBUG", "sysmem"))),
            LaunchOptions.resolve(Resolution.HD_720, globalHud = false, globalTouch = true, game = game),
        )
    }
}
