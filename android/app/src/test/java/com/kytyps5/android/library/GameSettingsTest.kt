package com.kytyps5.android.library

import com.kytyps5.android.Resolution
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class GameSettingsTest {

    @Test
    fun parsesEnvLinesSkippingCommentsAndKeepingLastDuplicate() {
        val parsed = EnvVars.parse(
            """
            # Turnip debug
            TU_DEBUG=sysmem,noubwc
            MESA_VK_WSI_PRESENT_MODE = mailbox

            TU_DEBUG=sysmem
            EMPTY=
            """.trimIndent(),
        )
        assertTrue(parsed.errors.isEmpty())
        assertEquals(
            listOf(EnvVar("MESA_VK_WSI_PRESENT_MODE", "mailbox"), EnvVar("TU_DEBUG", "sysmem"), EnvVar("EMPTY", "")),
            parsed.vars,
        )
    }

    @Test
    fun reportsBadLinesAndBlocksLinkerVariables() {
        val parsed = EnvVars.parse("JUSTAKEY\n1BAD=x\nLD_PRELOAD=/sdcard/x.so\nOK=1")
        assertEquals(listOf(EnvVar("OK", "1")), parsed.vars)
        assertEquals(3, parsed.errors.size)
        assertTrue(parsed.errors[2].contains("LD_PRELOAD"))
    }

    @Test
    fun valuesMayContainEquals() {
        assertEquals(EnvVar("A", "b=c"), EnvVars.parse("A=b=c").vars.single())
    }

    @Test
    fun planUnsetsKeysFromThePreviousGame() {
        val (unset, set) = EnvVars.plan(setOf("TU_DEBUG", "FD_MESA_DEBUG"), listOf(EnvVar("TU_DEBUG", "gmem")))
        assertEquals(setOf("FD_MESA_DEBUG"), unset)
        assertEquals(listOf(EnvVar("TU_DEBUG", "gmem")), set)
    }

    @Test
    fun settingsRoundTripThroughJson() {
        val settings = GameSettings(
            resolution = Resolution.FULL_HD_1080,
            envVars = listOf(EnvVar("TU_DEBUG", "sysmem")),
            hud = true,
            touchControls = false,
        )
        assertEquals(settings, GameSettings.fromJson(settings.toJson()))
        assertEquals(GameSettings.DEFAULT, GameSettings.fromJson(GameSettings.DEFAULT.toJson()))
    }

    @Test
    fun badJsonFallsBackToDefaults() {
        assertEquals(GameSettings.DEFAULT, GameSettings.fromJson("{{"))
        assertEquals(GameSettings.DEFAULT, GameSettings.fromJson(null))
        assertNull(GameSettings.fromJson("""{"resolution":"4k"}""").resolution)
    }
}
