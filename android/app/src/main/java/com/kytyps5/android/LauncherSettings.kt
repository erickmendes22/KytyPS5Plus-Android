package com.kytyps5.android

import android.content.Context

/** Launcher preferences persisted in SharedPreferences. Reads fall back to defaults on any failure. */
class LauncherSettings(context: Context) {
    private val prefs = context.applicationContext
        .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    var gamesRoot: String
        get() = prefs.getString(KEY_GAMES_ROOT, DEFAULT_GAMES_ROOT) ?: DEFAULT_GAMES_ROOT
        set(value) {
            prefs.edit().putString(KEY_GAMES_ROOT, value).apply()
        }

    var resolution: Resolution
        get() = Resolution.fromStorage(prefs.getString(KEY_RESOLUTION, null))
        set(value) {
            prefs.edit().putString(KEY_RESOLUTION, value.storageValue).apply()
        }

    /** Global GPU driver: an imported driver id from [GpuDriverStore], or "" for the system driver. */
    var gpuDriverId: String
        get() = prefs.getString(KEY_GPU_DRIVER, SYSTEM_DRIVER) ?: SYSTEM_DRIVER
        set(value) {
            prefs.edit().putString(KEY_GPU_DRIVER, value).apply()
        }

    /** Per-game GPU driver override (keyed by game path): null follows [gpuDriverId]. */
    fun gameGpuDriverOverride(gamePath: String): String? =
        prefs.getString(KEY_GPU_DRIVER_GAME_PREFIX + gamePath, null)

    fun setGameGpuDriverOverride(gamePath: String, driverId: String?) {
        val key = KEY_GPU_DRIVER_GAME_PREFIX + gamePath
        prefs.edit().apply { if (driverId == null) remove(key) else putString(key, driverId) }.apply()
    }

    companion object {
        /** Stored driver id meaning "the device's own Vulkan driver". */
        const val SYSTEM_DRIVER = ""
        const val DEFAULT_GAMES_ROOT = "/sdcard/KytyPS5/games"
        private const val PREFS_NAME = "kytyps5_launcher"
        private const val KEY_GAMES_ROOT = "games_root"
        private const val KEY_RESOLUTION = "resolution"
        private const val KEY_GPU_DRIVER = "gpu_driver"
        private const val KEY_GPU_DRIVER_GAME_PREFIX = "gpu_driver_game:"
    }
}
