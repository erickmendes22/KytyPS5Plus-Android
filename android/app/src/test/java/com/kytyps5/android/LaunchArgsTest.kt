package com.kytyps5.android

import com.kytyps5.android.library.EnvVar
import com.kytyps5.android.library.GameSettings
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

class LaunchArgsTest {

    @Test
    fun writtenImageCleanupIsScopedToConfirmedTitleMetadata() {
        val silentHill = LaunchArgs.forGame("/games/arbitrary", Resolution.HD_720, titleId = "PPSA10112")
        val index = silentHill.indexOf(LaunchArgs.FLAG_GPU_EVICT_WRITTEN)
        assertEquals("1", silentHill[index + 1])
        val astro = LaunchArgs.forGame("/games/astro", Resolution.HD_720, titleId = "PPSA21564")
        assertEquals(-1, astro.indexOf(LaunchArgs.FLAG_GPU_EVICT_WRITTEN))
        val unknown = LaunchArgs.forGame("/games/PPSA10112", Resolution.HD_720)
        assertEquals(-1, unknown.indexOf(LaunchArgs.FLAG_GPU_EVICT_WRITTEN))
    }

    @Test
    fun smallerAllocatorBlocksAreScopedToSilentHillAndGranTurismoMetadata() {
        for (title in listOf("PPSA10112", "ppsa10112", "PPSA24156", "ppsa24156")) {
            val args = LaunchArgs.forGame("/games/arbitrary", Resolution.HD_720, titleId = title)
            assertEquals(1, args.count { it == LaunchArgs.FLAG_VMA_BLOCK_SIZE })
            assertEquals("32", args[args.indexOf(LaunchArgs.FLAG_VMA_BLOCK_SIZE) + 1])
            assertEquals(1, args.count { it == LaunchArgs.FLAG_GPU_EVICT_WRITTEN })
            assertEquals("1", args[args.indexOf(LaunchArgs.FLAG_GPU_EVICT_WRITTEN) + 1])
        }
        for (title in listOf("PPSA21564", "PPSA01317", null)) {
            val args = LaunchArgs.forGame("/games/PPSA10112", Resolution.HD_720, titleId = title)
            assertEquals(-1, args.indexOf(LaunchArgs.FLAG_VMA_BLOCK_SIZE))
        }
    }

    @Test
    fun hudStyleResolvesGlobalAndPerGame() {
        val global = LaunchOptions.resolve(Resolution.HD_720, globalHud = true,
            globalTouch = true, game = GameSettings.DEFAULT, globalHudMode = com.kytyps5.android.hud.HudMode.MINIMAL)
        assertEquals(com.kytyps5.android.hud.HudMode.MINIMAL, global.hudMode)
        val own = LaunchOptions.resolve(Resolution.HD_720, globalHud = true,
            globalTouch = true, game = GameSettings(hudMode = com.kytyps5.android.hud.HudMode.FULL),
            globalHudMode = com.kytyps5.android.hud.HudMode.COMPACT)
        assertEquals(com.kytyps5.android.hud.HudMode.FULL, own.hudMode)
    }

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
    fun capyScaleOptionsUseNativeRendererAndRemainPerGame() {
        assertEquals("0.25", RenderScale.QUARTER.factor)
        assertEquals("0.33333334", RenderScale.THIRD.factor)
        for (scale in listOf(RenderScale.QUARTER, RenderScale.THIRD)) {
            assertEquals(scale, RenderScale.fromStorage(scale.storageValue))
            val args = LaunchArgs.forGame("/g", Resolution.HD_720, renderScale = scale)
            assertEquals(scale.factor, args[args.indexOf("--render-scale") + 1])
            assertEquals(-1, args.indexOf("--game-patch"))
            assertEquals(GameSettings(renderScale = scale),
                GameSettings.fromJson(GameSettings(renderScale = scale).toJson()))
        }
    }

    @Test
    fun shadowAtlasScaleUsesIndependentOptionalEnvironment() {
        val native = LaunchArgs.forGame("/g", Resolution.HD_720)
        assertEquals(-1, native.indexOf("KYTY_SHADOW_SCALE=0.5"))
        val half = LaunchArgs.forGame("/g", Resolution.HD_720, shadowScale = ShadowScale.HALF)
        assertEquals("KYTY_SHADOW_SCALE=0.5", half[half.indexOf("--env") + 1])
        assertEquals(ShadowScale.HALF, ShadowScale.fromStorage("0.5"))
        assertEquals(GameSettings(shadowScale = ShadowScale.HALF),
            GameSettings.fromJson(GameSettings(shadowScale = ShadowScale.HALF).toJson()))
        val game = LaunchOptions.resolve(
            Resolution.HD_720, globalHud = false, globalTouch = true,
            game = GameSettings(shadowScale = ShadowScale.NATIVE),
            globalShadowScale = ShadowScale.HALF,
        )
        assertEquals(ShadowScale.NATIVE, game.shadowScale)
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

    @Test
    fun gpuTurboEmitsTheFlagOnlyWhenOn() {
        val off = LaunchArgs.forGame("/g", Resolution.HD_720, null, RenderScale.NATIVE, gpuTurbo = false)
        assertArrayEquals(arrayOf("--game", "/g", "--screen-width", "1280", "--screen-height", "720"), off)

        val on = LaunchArgs.forGame("/g", Resolution.HD_720, "/drv", RenderScale.HALF, gpuTurbo = true)
        assertArrayEquals(
            arrayOf(
                "--game", "/g", "--screen-width", "1280", "--screen-height", "720",
                "--gpu-driver", "/drv", "--render-scale", "0.5", "--gpu-turbo",
            ),
            on,
        )
    }

    @Test
    fun tessellationIsEnabledOnlyForMyFirstGranTurismo() {
        val granTurismo = LaunchArgs.forGame("/games/gt", Resolution.HD_720, titleId = "PPSA24156")
        assertEquals(1, granTurismo.count { it == LaunchArgs.FLAG_TESSELLATION })
        val silentHill = LaunchArgs.forGame("/games/sh", Resolution.HD_720, titleId = "PPSA10112")
        assertEquals(-1, silentHill.indexOf(LaunchArgs.FLAG_TESSELLATION))
        val unknown = LaunchArgs.forGame("/games/unknown", Resolution.HD_720)
        assertEquals(-1, unknown.indexOf(LaunchArgs.FLAG_TESSELLATION))
    }

    @Test
    fun gpuTurboDefaultsOnAndFollowsGlobalUnlessTheGameOverrides() {
        val byDefault = LaunchOptions.resolve(Resolution.HD_720, globalHud = false, globalTouch = true, game = GameSettings.DEFAULT)
        assertEquals(true, byDefault.gpuTurbo)

        val globalOff = LaunchOptions.resolve(
            Resolution.HD_720, globalHud = false, globalTouch = true, game = GameSettings.DEFAULT, globalGpuTurbo = false,
        )
        assertEquals(false, globalOff.gpuTurbo)

        val gameOn = LaunchOptions.resolve(
            Resolution.HD_720, globalHud = false, globalTouch = true, game = GameSettings(gpuTurbo = true), globalGpuTurbo = false,
        )
        assertEquals(true, gameOn.gpuTurbo)

        val gameOff = LaunchOptions.resolve(
            Resolution.HD_720, globalHud = false, globalTouch = true, game = GameSettings(gpuTurbo = false), globalGpuTurbo = true,
        )
        assertEquals(false, gameOff.gpuTurbo)
    }

    @Test
    fun gpuTurboOverrideRoundTripsThroughGameSettingsJson() {
        for (value in listOf(true, false)) {
            val settings = GameSettings(gpuTurbo = value)
            assertEquals(settings, GameSettings.fromJson(settings.toJson()))
        }
        assertEquals(null, GameSettings.fromJson(GameSettings.DEFAULT.toJson()).gpuTurbo)
        // Settings saved before the option existed follow the global setting.
        assertEquals(null, GameSettings.fromJson("""{"resolution":"1080","hud":true}""").gpuTurbo)
    }

    @Test
    fun gamePathIsTheValueAfterTheGameFlag() {
        val args = LaunchArgs.forGame("/sdcard/g/PPSA14708.exfat", Resolution.HD_720, "/drv")
        assertEquals("/sdcard/g/PPSA14708.exfat", LaunchArgs.gamePath(args))
        assertEquals("/x", LaunchArgs.gamePath(arrayOf("--perf-stats", "--game", "/x", "--printf-direction", "File")))
        assertEquals(null, LaunchArgs.gamePath(arrayOf("--perf-stats")))
        assertEquals(null, LaunchArgs.gamePath(arrayOf("--game")))
        assertEquals(null, LaunchArgs.gamePath(arrayOf("--game", "")))
    }
}
