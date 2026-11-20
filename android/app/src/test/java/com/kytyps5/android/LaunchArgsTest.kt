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
        val match = LaunchArgs.forGame("/g", Resolution.HD_720, shadowScale = ShadowScale.MATCH_RENDER)
        assertEquals("KYTY_SHADOW_SCALE=match", match[match.indexOf("--env") + 1])
        assertEquals(ShadowScale.MATCH_RENDER, ShadowScale.fromStorage("match"))
        assertEquals(ShadowScale.MATCH_RENDER, ShadowScale.fromStorage("0.5"))
        assertEquals(ShadowScale.MATCH_RENDER, GameSettings.fromJson("""{"shadowScale":"0.5"}""").shadowScale)
        assertEquals(GameSettings(shadowScale = ShadowScale.MATCH_RENDER),
            GameSettings.fromJson(GameSettings(shadowScale = ShadowScale.MATCH_RENDER).toJson()))
        val game = LaunchOptions.resolve(
            Resolution.HD_720, globalHud = false, globalTouch = true,
            game = GameSettings(shadowScale = ShadowScale.NATIVE),
            globalShadowScale = ShadowScale.MATCH_RENDER,
        )
        assertEquals(ShadowScale.NATIVE, game.shadowScale)
    }

    @Test
    fun ufc6CanCacheRenderTargetDescriptionsWithoutChangingOtherTitles() {
        for (title in listOf("PPSA23566", "ppsa23566")) {
            val args = LaunchArgs.forGame("/games/ufc", Resolution.HD_720, titleId = title)
            assertEquals(1, args.count { it == "KYTY_BIND_MEMO=11" })
            assertEquals("0", args[args.indexOf("--async-shaders") + 1])
        }
        for (title in listOf("PPSA01341", "PPSA24156", null)) {
            val args = LaunchArgs.forGame("/games/other", Resolution.HD_720, titleId = title)
            assertEquals(-1, args.indexOf("KYTY_BIND_MEMO=11"))
        }
    }

    @Test
    fun ufc6NeverDropsUncompiledGraphicsDraws() {
        // A shader that is not compiled yet cannot be silently skipped when
        // later depth/stencil and composite passes consume its output.
        for (title in listOf("PPSA23566", "ppsa23566")) {
            val args = LaunchArgs.forGame("/games/ufc6", Resolution.HD_720, titleId = title)
            assertEquals(1, args.count { it == "--async-shaders" })
            assertEquals("0", args[args.indexOf("--async-shaders") + 1])
        }
        for (title in listOf("PPSA24156", "PPSA21564", null)) {
            val args = LaunchArgs.forGame("/games/ufc6", Resolution.HD_720, titleId = title)
            assertEquals(-1, args.indexOf("--async-shaders"))
        }
    }

    @Test
    fun demonsSoulsReferenceRequiresTessellationAndLinearImageReadback() {
        val demons = LaunchArgs.forGame("/g", Resolution.HD_720, titleId = "PPSA01341")
        assertEquals(1, demons.count { it == LaunchArgs.FLAG_TESSELLATION })
        val readback = demons.indexOf("--readback-linear-images")
        assertEquals("true", demons.getOrNull(readback + 1))
        assertEquals(1, demons.count { it == "KYTY_DS_GPU_ZERO_INDIRECT=1" })
        assertEquals(1, demons.count { it == "KYTY_DS_LDS64_STORAGE=1" })
        val other = LaunchArgs.forGame("/g", Resolution.HD_720, titleId = "PPSA99999")
        assertEquals(-1, other.indexOf(LaunchArgs.FLAG_TESSELLATION))
        assertEquals(-1, other.indexOf("--readback-linear-images"))
        assertEquals(-1, other.indexOf("KYTY_DS_GPU_ZERO_INDIRECT=1"))
        assertEquals(-1, other.indexOf("KYTY_DS_LDS64_STORAGE=1"))
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
    fun demonsSoulsKeepsActualGuestColorTargetsAndShaderWriteGuards() {
        for (title in listOf("PPSA01341", "ppsa01341")) {
            val args = LaunchArgs.forGame("/games/demons", Resolution.HD_720, titleId = title)
            val env = args.indices.filter { args[it] == "--env" }.map { args[it + 1] }
            assertEquals(1, env.count { it == "KYTY_DS_GUEST_COLOR_TARGETS=1" })
            assertEquals(0, env.count { it == "KYTY_DS_GUEST_COLOR_TARGETS=0" })
            assertEquals(1, env.count { it == "KYTY_DS_COLOR_PROBE=0" })
            assertEquals(1, env.count { it == "KYTY_DS_COLOR_PROBE_TARGETED=0" })
            assertEquals(1, env.count { it == "KYTY_DS_FRAME_TRACE=1" })
            assertEquals(1, env.count { it == "KYTY_DS_SHADER_BUFFER_IMAGE_ALIAS=1" })
            assertEquals(1, env.count { it == "KYTY_DS_GPU_LDS_GROUPS=1" })
            assertEquals(1, env.count { it == "KYTY_DS_GPU_DRAW_ARGS=1" })
            assertEquals(1, env.count { it == "KYTY_DS_CB_MASK_PREPARE=1" })
            assertEquals(1, env.count { it == "KYTY_DS_STABLE_IMAGE_BINDINGS=1" })
        }
        for (title in listOf("PPSA03671", "PPSA21564", "PPSA24156", null)) {
            val args = LaunchArgs.forGame("/games/other", Resolution.HD_720, titleId = title)
            val env = args.indices.filter { args[it] == "--env" }.map { args[it + 1] }
            assertEquals(0, env.count { it.startsWith("KYTY_DS_GUEST_COLOR_TARGETS=") })
            assertEquals(0, env.count { it.startsWith("KYTY_DS_SHADER_BUFFER_IMAGE_ALIAS=") })
            assertEquals(0, env.count { it.startsWith("KYTY_DS_GPU_LDS_GROUPS=") })
            assertEquals(0, env.count { it.startsWith("KYTY_DS_GPU_DRAW_ARGS=") })
        }
    }

    @Test
    fun demonsSoulsRequiresExplicitOptInForGpuReadback() {
        val base = LaunchArgs.forGame("/games/demons", Resolution.HD_720, titleId = "PPSA01341")
        val normal = DemonsSoulsDiagnosticArgs.append(base, fullPixelTrace = false, perFaceBias = false)
        val normalEnv = normal.indices.filter { normal[it] == "--env" }.map { normal[it + 1] }
        assertEquals("KYTY_DS_COLOR_PROBE=0", normalEnv.last { it.startsWith("KYTY_DS_COLOR_PROBE=") })
        assertEquals("KYTY_DS_FRAME_TRACE=1", normalEnv.last { it.startsWith("KYTY_DS_FRAME_TRACE=") })
        val traced = DemonsSoulsDiagnosticArgs.append(base, fullPixelTrace = true, perFaceBias = true)
        val env = traced.indices.filter { traced[it] == "--env" }.map { traced[it + 1] }
        assertEquals("KYTY_DS_COLOR_PROBE=1", env.last { it.startsWith("KYTY_DS_COLOR_PROBE=") })
        assertEquals("KYTY_DS_COLOR_PROBE_TARGETED=0",
            env.last { it.startsWith("KYTY_DS_COLOR_PROBE_TARGETED=") })
        assertEquals("KYTY_DS_FRAME_TRACE=1", env.last { it.startsWith("KYTY_DS_FRAME_TRACE=") })
        assertEquals(1, env.count { it == "KYTY_DS_PER_FACE_BIAS=1" })
    }

    @Test
    fun demonsSoulsUsesNativeCounterUnitsOnlyForConfirmedTitle() {
        for (title in listOf("PPSA01341", "ppsa01341")) {
            val args = LaunchArgs.forGame("/games/arbitrary", Resolution.HD_720, titleId = title)
            assertEquals(1, args.count { it == "KYTY_DS_NATIVE_TSC=1" })
            assertEquals("0", args[args.indexOf("--async-shaders") + 1])
            assertEquals(-1, args.indexOf("--gpu-indirect"))
            assertEquals(-1, args.indexOf("--targeted-readback"))
            assertEquals(1, args.count { it == "KYTY_REBIND_AFTER_JOIN=1" })
            // U59's PPSA01341 reference preset uses GPU-written linear image
            // readback for guest coherency; this is not diagnostic tracing.
            assertEquals("true", args.getOrNull(args.indexOf("--readback-linear-images") + 1))
            assertEquals(1, args.count { it == LaunchArgs.FLAG_TESSELLATION })
            assertEquals(1, args.count { it == "KYTY_DS_GPU_LAYER_KEEP=1" })
            // The experimental frame fence stalled queues 17/49 for 2 s in the device log.
            // Normal GPU ordering remains; do not impose this additional gate by default.
            assertEquals(-1, args.indexOf("KYTY_DS_QUEUE_FRAME_FENCE=1"))
            assertEquals(1, args.count { it == "KYTY_PRESENT_COMPACT=1" })
            assertEquals("32", args[args.indexOf("--vma-block-size") + 1])
        }
        for (title in listOf("PPSA03671", "PPSA21564", "PPSA24156", null)) {
            val args = LaunchArgs.forGame("/games/PPSA01341", Resolution.HD_720, titleId = title)
            assertEquals(-1, args.indexOf("KYTY_DS_NATIVE_TSC=1"))
            assertEquals(-1, args.indexOf("--async-shaders"))
            assertEquals(-1, args.indexOf("--gpu-indirect"))
            assertEquals(-1, args.indexOf("--targeted-readback"))
            if (title == "PPSA03671") {
                assertEquals(1, args.count { it == "KYTY_REBIND_AFTER_JOIN=1" })
            } else {
                assertEquals(-1, args.indexOf("KYTY_REBIND_AFTER_JOIN=1"))
            }
            assertEquals(-1, args.indexOf("--readback-linear-images"))
            assertEquals(-1, args.indexOf("KYTY_DS_GPU_LAYER_KEEP=1"))
            assertEquals(-1, args.indexOf("KYTY_DS_QUEUE_FRAME_FENCE=1"))
        }
    }

    @Test
    fun wolverineUsesVulkanBindlessOnlyWhenTitleIdIsConfirmed() {
        for (title in listOf("PPSA03671", "ppsa03671")) {
            val args = LaunchArgs.forGame("/games/arbitrary", Resolution.HD_720, titleId = title)
            val env = args.indices.filter { args[it] == "--env" }.map { args[it + 1] }
            assertEquals(1, env.count { it == "KYTY_BINDLESS_IMAGES=1" })
            assertEquals(1, env.count { it == "KYTY_MONITORWAITX_CHECK=1" })
            assertEquals(1, env.count { it == "KYTY_GUEST_FRAME_TRACE=1" })
            assertEquals(1, env.count { it == "KYTY_TITLE_WORKAROUNDS=1" })
            assertEquals(1, env.count { it == "KYTY_PRESENT_COMPACT=1" })
            assertEquals(1, env.count { it == "KYTY_WOLVERINE_RT_ALIAS_RECLAIM=1" })
            assertEquals(1, env.count { it == "KYTY_STAGING_MIB=32" })
            assertEquals("auto", args[args.indexOf("--gpu-memory-budget") + 1])
            assertEquals("1", args[args.indexOf("--gpu-evict-written") + 1])
            assertEquals("32", args[args.indexOf("--vma-block-size") + 1])
            assertEquals(-1, args.indexOf("KYTY_DS_NATIVE_TSC=1"))
            assertEquals(-1, args.indexOf("--async-shaders"))
            assertEquals(-1, args.indexOf("--gpu-indirect"))
            assertEquals(-1, args.indexOf("--targeted-readback"))
            assertEquals(1, args.count { it == "KYTY_REBIND_AFTER_JOIN=1" })
            assertEquals(-1, args.indexOf("--readback-linear-images"))
            assertEquals(-1, args.indexOf("KYTY_DS_STRICT_FRAME=1"))
        }
        for (title in listOf("PPSA01341", "PPSA21564", "PPSA24156", null)) {
            val args = LaunchArgs.forGame("/games/PPSA03671", Resolution.HD_720, titleId = title)
            assertEquals(-1, args.indexOf("KYTY_BINDLESS_IMAGES=1"))
            assertEquals(-1, args.indexOf("KYTY_GUEST_FRAME_TRACE=1"))
            assertEquals(-1, args.indexOf("KYTY_STAGING_MIB=32"))
            if (title != "PPSA24156") assertEquals(-1, args.indexOf("--gpu-evict-written"))
        }
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
