package com.kytyps5.android.presets

import android.content.Context
import android.util.Log
import com.kytyps5.android.GameEntry
import com.kytyps5.android.GameLibrary
import com.kytyps5.android.LauncherSettings
import java.io.File

/**
 * Per-game config presets shipped in the APK (assets/presets/<TITLE_ID>/...), opt-in per title in
 * [LauncherSettings]. Every launch reconciles the title's folders with the stored choice before the
 * emulator starts, so the files are in place when enabled and gone when disabled.
 */
class GamePresets(context: Context, private val settings: LauncherSettings) {
    private val catalog = PresetCatalog(AssetPresetSource(context.assets))
    private val installer = PresetInstaller(context.filesDir)

    fun titleIdFor(game: GameEntry): String? = TitleIds.resolve(game.titleId, game.folder)

    fun available(titleId: String): List<GamePreset> = catalog.forTitle(titleId)

    fun isEnabled(preset: GamePreset): Boolean = preset.id in settings.enabledPresets(preset.titleId)

    /** Stores the choice for [titleId] and brings its folders in line right away. */
    fun setEnabled(titleId: String, enabledIds: Set<String>) {
        settings.setEnabledPresets(titleId, enabledIds)
        reconcile(titleId)
    }

    /** Called by GameActivity before the emulator loads: [gamePath] is the --game argument. */
    fun prepareLaunch(gamePath: String?) {
        if (gamePath.isNullOrEmpty()) return
        val game = File(gamePath)
        val paramTitleId = if (game.isDirectory) GameLibrary.readEntry(game).titleId else null
        val titleId = TitleIds.resolve(paramTitleId, game) ?: return
        reconcile(titleId)
    }

    private fun reconcile(titleId: String) {
        try {
            val report = installer.reconcile(titleId, catalog.forTitle(titleId), settings.enabledPresets(titleId)) { preset, path ->
                catalog.fileBytes(preset, path)
            }
            if (report.applied.isNotEmpty()) Log.i(TAG, "$titleId: applied ${report.applied}")
            if (report.removed.isNotEmpty()) Log.i(TAG, "$titleId: removed ${report.removed}")
            report.errors.forEach { Log.e(TAG, "$titleId: $it") }
        } catch (e: RuntimeException) {
            // A broken preset must never keep the game from starting.
            Log.e(TAG, "$titleId: presets not reconciled", e)
        }
    }

    private companion object {
        const val TAG = "KytyPresets"
    }
}
