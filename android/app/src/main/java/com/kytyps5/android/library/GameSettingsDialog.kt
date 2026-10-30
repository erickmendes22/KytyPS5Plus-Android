package com.kytyps5.android.library

import android.app.Activity
import android.app.AlertDialog
import android.graphics.Typeface
import android.text.InputType
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.Switch
import android.widget.TextView
import com.kytyps5.android.GameEntry
import com.kytyps5.android.LauncherSettings
import com.kytyps5.android.hud.HudMode
import com.kytyps5.android.RenderScale
import com.kytyps5.android.Resolution
import com.kytyps5.android.input.OverlayProfileStore
import com.kytyps5.android.presets.GamePreset
import com.kytyps5.android.presets.GamePresets

/**
 * Per-game settings: resolution, GPU driver, HUD and touch-controller overrides, the title's
 * shipped config presets (opt-in), and environment variables for Mesa/Turnip (the per-game driver
 * + env editor of Eden, reimplemented; Eden is GPL-3.0 and only its design is reused).
 */
class GameSettingsDialog(
    private val activity: Activity,
    private val settings: LauncherSettings,
    private val layouts: OverlayProfileStore,
    private val presets: GamePresets,
    private val driverLabel: (GameEntry) -> String,
    private val chooseDriver: (GameEntry, () -> Unit) -> Unit,
) {
    private val density = activity.resources.displayMetrics.density
    private val Int.dp: Int get() = (this * density).toInt()

    fun show(game: GameEntry) {
        val current = settings.gameSettings(game.path)
        val resolution = spinner(listOf("Global (${settings.resolution.label})") + Resolution.entries.map { it.label })
        resolution.setSelection(current.resolution?.let { Resolution.entries.indexOf(it) + 1 } ?: 0)
        val renderScale = spinner(listOf("Global (${settings.renderScale.label})") + RenderScale.entries.map { it.label })
        renderScale.setSelection(current.renderScale?.let { RenderScale.entries.indexOf(it) + 1 } ?: 0)
        val hud = triState("Performance HUD", current.hud, settings.hudEnabled)
        val hudMode = spinner(listOf("Global (${settings.hudMode.label})") + HudMode.entries.map { it.label })
        hudMode.setSelection(current.hudMode?.let { HudMode.entries.indexOf(it) + 1 } ?: 0)
        val touch = triState("Touch controls", current.touchControls, settings.touchControlsEnabled)
        val gpuTurbo = triState(GPU_TURBO_LABEL, current.gpuTurbo, settings.gpuTurbo)
        val env = EditText(activity).apply {
            setText(EnvVars.format(current.envVars))
            hint = "TU_DEBUG=sysmem"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            minLines = 3
            typeface = Typeface.MONOSPACE
        }
        val errors = label("", secondary = true)
        val titleId = presets.titleIdFor(game)
        val presetSwitches = titleId?.let { presets.available(it) }.orEmpty().map { it to presetSwitch(it) }
        // Each title has only one active eboot rewrite. Avoid silently selecting
        // the alphabetically first resolution when several switches were enabled.
        if (titleId == "PPSA02721") {
            presetSwitches.forEach { (preset, toggle) ->
                if (preset.id.startsWith("res")) {
                    toggle.setOnCheckedChangeListener { _, selected ->
                        if (selected) {
                            presetSwitches.forEach { (other, otherToggle) ->
                                if (other.id.startsWith("res") && otherToggle !== toggle) {
                                    otherToggle.isChecked = false
                                }
                            }
                        }
                    }
                }
            }
        }
        val driverButton = Button(activity).apply {
            text = driverLabel(game)
            isAllCaps = false
            setOnClickListener { chooseDriver(game) { text = driverLabel(game) } }
        }
        val content = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(20.dp, 8.dp, 20.dp, 8.dp)
            addView(label(listOfNotNull(game.titleId, game.path).joinToString("\n"), secondary = true))
            addView(label("Resolution", bold = true))
            addView(resolution)
            addView(label("Render scale (internal resolution of frame-sized render targets)", bold = true))
            addView(renderScale)
            addView(label("GPU driver", bold = true))
            addView(driverButton)
            addView(hud.first)
            addView(label("HUD style", bold = true))
            addView(hudMode)
            addView(touch.first)
            addView(gpuTurbo.first)
            if (presetSwitches.isNotEmpty()) {
                addView(label("Game presets", bold = true))
                addView(label("Known-good config files for this game, written into its data folder before launch. Off removes them again.", secondary = true))
                presetSwitches.forEach { (preset, switch) ->
                    addView(switch)
                    if (preset.description.isNotEmpty()) addView(label(preset.description, secondary = true))
                }
            }
            addView(label("Environment variables (one KEY=VALUE per line)", bold = true))
            addView(env)
            addView(presetsRow(env))
            addView(errors)
            if (layouts.hasGameLayout(game.path)) {
                addView(Button(activity).apply {
                    text = "Reset this game's controller layout"
                    isAllCaps = false
                    setOnClickListener {
                        layouts.deleteGameLayout(game.path)
                        visibility = View.GONE
                    }
                })
            }
        }
        val dialog = AlertDialog.Builder(activity)
            .setTitle(game.title)
            .setView(ScrollView(activity).apply { addView(content) })
            .setPositiveButton("Save", null)
            .setNeutralButton("Reset all", null)
            .setNegativeButton("Cancel", null)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val parsed = EnvVars.parse(env.text.toString())
                if (parsed.errors.isNotEmpty()) {
                    errors.text = parsed.errors.joinToString("\n")
                    return@setOnClickListener
                }
                settings.setGameSettings(
                    game.path,
                    GameSettings(
                        resolution = resolution.selectedItemPosition.takeIf { it > 0 }?.let { Resolution.entries[it - 1] },
                        envVars = parsed.vars,
                        hud = hud.second(),
                        hudMode = hudMode.selectedItemPosition.takeIf { it > 0 }?.let { HudMode.entries[it - 1] },
                        touchControls = touch.second(),
                        gpuTurbo = gpuTurbo.second(),
                        renderScale = renderScale.selectedItemPosition.takeIf { it > 0 }?.let { RenderScale.entries[it - 1] },
                    ),
                )
                if (titleId != null && presetSwitches.isNotEmpty()) {
                    presets.setEnabled(titleId, presetSwitches.filter { it.second.isChecked }.map { it.first.id }.toSet())
                }
                dialog.dismiss()
            }
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener {
                settings.setGameSettings(game.path, GameSettings.DEFAULT)
                settings.setGameGpuDriverOverride(game.path, null)
                if (titleId != null && presetSwitches.isNotEmpty()) presets.setEnabled(titleId, emptySet())
                dialog.dismiss()
            }
        }
        dialog.show()
    }

    /** Global / On / Off selector; the second value reads the choice back (null = global). */
    private fun triState(title: String, value: Boolean?, global: Boolean): Pair<View, () -> Boolean?> {
        val spinner = spinner(listOf("Global (${if (global) "on" else "off"})", "On", "Off"))
        spinner.setSelection(when (value) { null -> 0; true -> 1; false -> 2 })
        val row = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(label(title, bold = true), LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            addView(spinner)
        }
        return row to { when (spinner.selectedItemPosition) { 1 -> true; 2 -> false; else -> null } }
    }

    private fun presetSwitch(preset: GamePreset) = Switch(activity).apply {
        text = preset.name
        textSize = 15f
        isChecked = presets.isEnabled(preset)
        setPadding(0, 8.dp, 0, 0)
    }

    private fun presetsRow(env: EditText): View = Button(activity).apply {
        text = "Add preset…"
        isAllCaps = false
        setOnClickListener {
            AlertDialog.Builder(activity)
                .setTitle("Common Turnip / Mesa variables")
                .setItems(ENV_PRESETS.map { "${it.first}\n  ${it.second}" }.toTypedArray()) { _, which ->
                    val line = ENV_PRESETS[which].first
                    val text = env.text.toString().trimEnd()
                    env.setText(if (text.isEmpty()) line else "$text\n$line")
                }
                .show()
        }
    }

    private fun spinner(items: List<String>) = Spinner(activity).apply {
        adapter = ArrayAdapter(activity, android.R.layout.simple_spinner_dropdown_item, items)
    }

    private fun label(text: String, bold: Boolean = false, secondary: Boolean = false) = TextView(activity).apply {
        this.text = text
        textSize = if (bold) 15f else 13f
        if (bold) setTypeface(typeface, Typeface.BOLD)
        if (secondary) alpha = 0.7f
        setPadding(0, 8.dp, 0, 2.dp)
    }

    companion object {
        const val GPU_TURBO_LABEL = "Max GPU clock (faster, runs hotter)"

        private val ENV_PRESETS = listOf(
            "TU_DEBUG=sysmem" to "Force sysmem rendering (fixes GMEM corruption / rainbow textures)",
            "TU_DEBUG=gmem" to "Force GMEM (tile) rendering",
            "TU_DEBUG=noubwc" to "Disable UBWC compression",
            "TU_DEBUG=nolrz" to "Disable low-resolution Z",
            "TU_DEBUG=startup" to "Log Turnip startup info to logcat",
            "MESA_VK_WSI_PRESENT_MODE=mailbox" to "Present without vsync waits",
            "MESA_SHADER_CACHE_DISABLE=true" to "Disable Mesa's on-disk shader cache",
        )
    }
}
