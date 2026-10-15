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

    companion object {
        const val DEFAULT_GAMES_ROOT = "/sdcard/KytyPS5/games"
        private const val PREFS_NAME = "kytyps5_launcher"
        private const val KEY_GAMES_ROOT = "games_root"
        private const val KEY_RESOLUTION = "resolution"
    }
}
