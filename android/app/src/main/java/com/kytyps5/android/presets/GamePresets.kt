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

    /**
     * Called by GameActivity before the emulator loads: [gamePath] is the --game argument. Brings the
     * title's folders in line and returns the launch options of its presets that are on and applied
     * cleanly (empty when there are none, or on any error: a broken preset never blocks a launch).
     */
    fun prepareLaunch(gamePath: String?): PresetLaunchArgs.Plan {
        val none = PresetLaunchArgs.Plan(emptyList(), emptyList())
        if (gamePath.isNullOrEmpty()) return none
        val game = File(gamePath)
        val paramTitleId = if (game.isDirectory) GameLibrary.readEntry(game).titleId else null
        val titleId = TitleIds.resolve(paramTitleId, game) ?: return none
        val available = catalog.forTitle(titleId)
        val applied = reconcile(titleId, available)?.applied?.toSet() ?: return none
        val plan = PresetLaunchArgs.plan(
            available.filter { it.id in applied && !it.launch.isEmpty }.map { preset ->
                PresetLaunchArgs.Resolved(
                    presetId = preset.id,
                    launch = preset.launch,
                    gamePatchPath = preset.launch.gamePatch?.let { installer.launchFile(titleId, preset.id, it).absolutePath },
                )
            },
        )
        plan.conflicts.forEach { Log.w(TAG, "$titleId: $it") }
        if (!plan.isEmpty) Log.i(TAG, "$titleId: launch options ${plan.args}")
        return plan
    }

    private fun reconcile(titleId: String, available: List<GamePreset> = catalog.forTitle(titleId)): PresetInstaller.Report? =
        try {
            installer.reconcile(
                titleId,
                available,
                settings.enabledPresets(titleId),
                bytes = { preset, path -> catalog.fileBytes(preset, path) },
                launchBytes = { preset, path -> catalog.launchFileBytes(preset, path) },
            ).also { report ->
                if (report.applied.isNotEmpty()) Log.i(TAG, "$titleId: applied ${report.applied}")
                if (report.removed.isNotEmpty()) Log.i(TAG, "$titleId: removed ${report.removed}")
                report.errors.forEach { Log.e(TAG, "$titleId: $it") }
            }
        } catch (e: RuntimeException) {
            // A broken preset must never keep the game from starting.
            Log.e(TAG, "$titleId: presets not reconciled", e)
            null
        }

    private companion object {
        const val TAG = "KytyPresets"
    }
}
