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

    /**
     * Use host render scale instead of executable/UE configuration resolution patches.
     * Keep the old manifests in assets for provenance, but never install, expose or launch
     * them. The installer will also restore older preset-managed config files on next boot.
     * Astro's half-rate lighting is a separate optimization and remains available without
     * its executable resolution rewrite.
     */
    fun available(titleId: String): List<GamePreset> =
        activePerformancePresets(titleId, catalog.forTitle(titleId))

    companion object {
        internal fun activePerformancePresets(titleId: String, shipped: List<GamePreset>): List<GamePreset> =
            shipped.asSequence()
                .filterNot { it.id.startsWith("res") ||
                    (titleId.equals("PPSA24156", ignoreCase = true) && it.id == "native720") }
                .map { preset ->
                    if (titleId.equals("PPSA21567", ignoreCase = true) && preset.id == "perf720") {
                        preset.copy(
                            name = "Half-rate ray-traced lighting (no resolution patch)",
                            description = "Reduces ray-traced lighting work without modifying the game executable. " +
                                "Set the desired internal resolution using Render scale above.",
                            launch = preset.launch.copy(gamePatch = null, renderScale = null),
                        )
                    } else preset
                }.toList()
    }

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
        val available = available(titleId)
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

    private fun reconcile(titleId: String, available: List<GamePreset> = available(titleId)): PresetInstaller.Report? =
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
