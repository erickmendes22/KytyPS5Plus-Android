package com.kytyps5.android

import com.kytyps5.android.library.EnvVar
import com.kytyps5.android.library.GameSettings

/** Output resolution the emulator renders at. Values are passed as --screen-width / --screen-height. */
enum class Resolution(
    val label: String,
    val width: Int,
    val height: Int,
    val storageValue: String,
) {
    HD_720(label = "720p", width = 1280, height = 720, storageValue = "720"),
    FULL_HD_1080(label = "1080p", width = 1920, height = 1080, storageValue = "1080");

    companion object {
        fun fromStorage(value: String?): Resolution =
            entries.firstOrNull { it.storageValue == value } ?: HD_720
    }
}

/**
 * Builds the emulator command line for one game. The emulator (src/main.cpp) reads
 * these flags starting at argv[1]; SDL prepends argv[0] itself.
 */
object LaunchArgs {
    /** Intent extra carrying the Array<String> handed to the emulator by GameActivity.getArguments(). */
    const val EXTRA_GAME_ARGS = "com.kytyps5.android.extra.GAME_ARGS"

    /** Emulator working directory for logs, shader dumps and caches; GameActivity supplies it. */
    const val FLAG_DATA_DIR = "--data-dir"

    private const val FLAG_GAME = "--game"
    private const val FLAG_SCREEN_WIDTH = "--screen-width"
    private const val FLAG_SCREEN_HEIGHT = "--screen-height"

    /** Launcher-only extras read by GameActivity (not emulator flags). */
    const val EXTRA_GAME_PATH = "com.kytyps5.android.extra.GAME_PATH"
    const val EXTRA_ENV = "com.kytyps5.android.extra.ENV"
    const val EXTRA_HUD = "com.kytyps5.android.extra.HUD"
    const val EXTRA_TOUCH_CONTROLS = "com.kytyps5.android.extra.TOUCH_CONTROLS"

    /** Directory of an imported Vulkan driver package; omitted for the system driver. */
    const val FLAG_GPU_DRIVER = "--gpu-driver"

    fun forGame(gamePath: String, resolution: Resolution, gpuDriverDir: String? = null): Array<String> =
        arrayOf(
            FLAG_GAME, gamePath,
            FLAG_SCREEN_WIDTH, resolution.width.toString(),
            FLAG_SCREEN_HEIGHT, resolution.height.toString(),
        ) + (gpuDriverDir?.let { arrayOf(FLAG_GPU_DRIVER, it) } ?: emptyArray())
}

/** Which GPU driver a launch uses: a per-game override wins over the global choice. */
object GpuDriverChoice {
    /**
     * Effective driver id: [gameOverride] when set, else [globalId]. "" (or an id that is no longer
     * imported, per [isImported]) means the system driver and resolves to null.
     */
    fun resolve(globalId: String, gameOverride: String?, isImported: (String) -> Boolean): String? {
        val id = gameOverride ?: globalId
        return id.takeIf { it != LauncherSettings.SYSTEM_DRIVER && isImported(it) }
    }
}

/** Settings a launch actually uses after per-game overrides are applied to the global ones. */
data class LaunchOptions(
    val resolution: Resolution,
    val hud: Boolean,
    val touchControls: Boolean,
    val env: List<EnvVar>,
) {
    companion object {
        fun resolve(globalResolution: Resolution, globalHud: Boolean, globalTouch: Boolean, game: GameSettings) =
            LaunchOptions(
                resolution = game.resolution ?: globalResolution,
                hud = game.hud ?: globalHud,
                touchControls = game.touchControls ?: globalTouch,
                env = game.envVars,
            )
    }
}
