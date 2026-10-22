package com.kytyps5.android

import android.content.Context
import com.kytyps5.android.library.GameSettings

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

    /** Internal render scale unless a game overrides it (see [RenderScale]). */
    var renderScale: RenderScale
        get() = RenderScale.fromStorage(prefs.getString(KEY_RENDER_SCALE, null))
        set(value) {
            prefs.edit().putString(KEY_RENDER_SCALE, value.storageValue).apply()
        }

    /** Global GPU driver: an imported driver id from [GpuDriverStore], or "" for the system driver. */
    var gpuDriverId: String
        get() = prefs.getString(KEY_GPU_DRIVER, SYSTEM_DRIVER) ?: SYSTEM_DRIVER
        set(value) {
            prefs.edit().putString(KEY_GPU_DRIVER, value).apply()
        }

    /** Version of the bundled driver whose one-time default setup already ran (0 = never). */
    var bundledDriverVersion: Int
        get() = prefs.getInt(KEY_BUNDLED_DRIVER_VERSION, 0)
        set(value) {
            prefs.edit().putInt(KEY_BUNDLED_DRIVER_VERSION, value).apply()
        }

    /** Show the performance HUD in game unless a game overrides it. */
    var hudEnabled: Boolean
        get() = prefs.getBoolean(KEY_HUD, false)
        set(value) {
            prefs.edit().putBoolean(KEY_HUD, value).apply()
        }

    /** Show the on-screen controller in game unless a game overrides it (hidden while a gamepad is attached). */
    var touchControlsEnabled: Boolean
        get() = prefs.getBoolean(KEY_TOUCH_CONTROLS, true)
        set(value) {
            prefs.edit().putBoolean(KEY_TOUCH_CONTROLS, value).apply()
        }

    /** Max GPU clock (--gpu-turbo) unless a game overrides it: ~1.85x faster GPU work, runs hotter. */
    var gpuTurbo: Boolean
        get() = prefs.getBoolean(KEY_GPU_TURBO, DEFAULT_GPU_TURBO)
        set(value) {
            prefs.edit().putBoolean(KEY_GPU_TURBO, value).apply()
        }

    fun gameSettings(gamePath: String): GameSettings =
        GameSettings.fromJson(prefs.getString(KEY_GAME_SETTINGS_PREFIX + gamePath, null))

    fun setGameSettings(gamePath: String, settings: GameSettings) {
        val key = KEY_GAME_SETTINGS_PREFIX + gamePath
        prefs.edit().apply { if (settings == GameSettings.DEFAULT) remove(key) else putString(key, settings.toJson()) }.apply()
    }

    /** Per-game GPU driver override (keyed by game path): null follows [gpuDriverId]. */
    fun gameGpuDriverOverride(gamePath: String): String? =
        prefs.getString(KEY_GPU_DRIVER_GAME_PREFIX + gamePath, null)

    fun setGameGpuDriverOverride(gamePath: String, driverId: String?) {
        val key = KEY_GPU_DRIVER_GAME_PREFIX + gamePath
        prefs.edit().apply { if (driverId == null) remove(key) else putString(key, driverId) }.apply()
    }

    /** Ids of the shipped config presets (GamePresets) turned on for a title; none by default (opt-in). */
    fun enabledPresets(titleId: String): Set<String> =
        prefs.getStringSet(KEY_GAME_PRESETS_PREFIX + titleId, null)?.toSet().orEmpty()

    fun setEnabledPresets(titleId: String, presetIds: Set<String>) {
        val key = KEY_GAME_PRESETS_PREFIX + titleId
        prefs.edit().apply { if (presetIds.isEmpty()) remove(key) else putStringSet(key, presetIds.toSet()) }.apply()
    }

    companion object {
        /** Stored driver id meaning "the device's own Vulkan driver". */
        const val SYSTEM_DRIVER = ""
        const val DEFAULT_GAMES_ROOT = "/sdcard/KytyPS5/games"
        const val DEFAULT_GPU_TURBO = true
        private const val PREFS_NAME = "kytyps5_launcher"
        private const val KEY_GAMES_ROOT = "games_root"
        private const val KEY_RESOLUTION = "resolution"
        private const val KEY_RENDER_SCALE = "render_scale"
        private const val KEY_GPU_DRIVER = "gpu_driver"
        private const val KEY_GPU_DRIVER_GAME_PREFIX = "gpu_driver_game:"
        private const val KEY_BUNDLED_DRIVER_VERSION = "bundled_driver_version"
        private const val KEY_HUD = "hud_enabled"
        private const val KEY_TOUCH_CONTROLS = "touch_controls_enabled"
        private const val KEY_GPU_TURBO = "gpu_turbo"
        private const val KEY_GAME_SETTINGS_PREFIX = "game_settings:"
        private const val KEY_GAME_PRESETS_PREFIX = "game_presets:"
    }
}
