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
