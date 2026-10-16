/*
 * Normalised, immutable overlay layout with global + per-game scopes, adapted from Vita3K's Android
 * overlay (android/.../overlay/OverlayLayout.kt, OverlayStore.kt; Vita3K project, GPL-2.0).
 */
package com.kytyps5.android.input

import org.json.JSONException
import org.json.JSONObject

/** How a control is drawn and how touches on it are interpreted. */
enum class ControlKind { BUTTON, SHOULDER, TRIGGER, DPAD, STICK, PILL }

/**
 * Every on-screen control with its default centre (fractions of view width/height) and size
 * (fraction of the view's shorter side). Defaults target a landscape tablet.
 */
enum class OverlayControl(
    val kind: ControlKind,
    val label: String,
    val defaultX: Float,
    val defaultY: Float,
    val defaultSize: Float,
) {
    DPAD(ControlKind.DPAD, "D-pad", 0.12f, 0.45f, 0.28f),
    LEFT_STICK(ControlKind.STICK, "Left stick", 0.28f, 0.78f, 0.28f),
    RIGHT_STICK(ControlKind.STICK, "Right stick", 0.71f, 0.78f, 0.28f),
    TRIANGLE(ControlKind.BUTTON, "Triangle", 0.89f, 0.31f, 0.115f),
    CROSS(ControlKind.BUTTON, "Cross", 0.89f, 0.59f, 0.115f),
    SQUARE(ControlKind.BUTTON, "Square", 0.83f, 0.45f, 0.115f),
    CIRCLE(ControlKind.BUTTON, "Circle", 0.95f, 0.45f, 0.115f),
    L1(ControlKind.SHOULDER, "L1", 0.17f, 0.08f, 0.10f),
    L2(ControlKind.TRIGGER, "L2", 0.06f, 0.08f, 0.10f),
    R1(ControlKind.SHOULDER, "R1", 0.83f, 0.08f, 0.10f),
    R2(ControlKind.TRIGGER, "R2", 0.94f, 0.08f, 0.10f),
    L3(ControlKind.BUTTON, "L3", 0.43f, 0.93f, 0.08f),
    R3(ControlKind.BUTTON, "R3", 0.57f, 0.93f, 0.08f),
    OPTIONS(ControlKind.PILL, "OPTIONS", 0.66f, 0.07f, 0.08f),
    TOUCHPAD(ControlKind.PILL, "TOUCH PAD", 0.50f, 0.07f, 0.08f),
    ;

    /** The pad button this control presses (sticks/D-pad report through axes and directions). */
    val button: PadButton?
        get() = when (this) {
            TRIANGLE -> PadButton.TRIANGLE
            CROSS -> PadButton.CROSS
            SQUARE -> PadButton.SQUARE
            CIRCLE -> PadButton.CIRCLE
            L1 -> PadButton.L1
            R1 -> PadButton.R1
            L3 -> PadButton.L3
            R3 -> PadButton.R3
            OPTIONS -> PadButton.OPTIONS
            TOUCHPAD -> PadButton.TOUCHPAD
            else -> null
        }

    val trigger: PadAxis?
        get() = when (this) {
            L2 -> PadAxis.L2
            R2 -> PadAxis.R2
            else -> null
        }

    val defaultPlacement: ControlPlacement get() = ControlPlacement(defaultX, defaultY, 1f, true)
}

data class ControlPlacement(val x: Float, val y: Float, val scale: Float, val visible: Boolean) {
    fun normalized(): ControlPlacement = copy(
        x = x.finiteIn(0f, 1f, 0.5f),
        y = y.finiteIn(0f, 1f, 0.5f),
        scale = scale.finiteIn(MIN_SCALE, MAX_SCALE, 1f),
    )

    companion object {
        const val MIN_SCALE = 0.5f
        const val MAX_SCALE = 2.5f
    }
}

data class OverlayLayout(
    val placements: Map<OverlayControl, ControlPlacement> = emptyMap(),
    val opacity: Float = DEFAULT_OPACITY,
) {
    fun placement(control: OverlayControl): ControlPlacement = placements[control] ?: control.defaultPlacement

    fun with(control: OverlayControl, placement: ControlPlacement): OverlayLayout =
        copy(placements = placements + (control to placement.normalized()))

    fun withOpacity(value: Float): OverlayLayout = copy(opacity = value.finiteIn(MIN_OPACITY, 1f, DEFAULT_OPACITY))

    fun toJson(): String {
        val controls = JSONObject()
        placements.forEach { (control, p) ->
            controls.put(
                control.name,
                JSONObject().put("x", p.x.toDouble()).put("y", p.y.toDouble())
                    .put("scale", p.scale.toDouble()).put("visible", p.visible),
            )
        }
        return JSONObject().put("version", VERSION).put("opacity", opacity.toDouble()).put("controls", controls).toString()
    }

    companion object {
        const val VERSION = 1
        const val DEFAULT_OPACITY = 0.5f
        const val MIN_OPACITY = 0.1f
        val DEFAULT = OverlayLayout()

        /** Parses [toJson] output; unknown controls are skipped, bad values clamped, garbage -> null. */
        fun fromJson(json: String): OverlayLayout? = try {
            val root = JSONObject(json)
            val controls = root.optJSONObject("controls") ?: JSONObject()
            val placements = OverlayControl.entries.mapNotNull { control ->
                val obj = controls.optJSONObject(control.name) ?: return@mapNotNull null
                control to ControlPlacement(
                    x = obj.optDouble("x", control.defaultX.toDouble()).toFloat(),
                    y = obj.optDouble("y", control.defaultY.toDouble()).toFloat(),
                    scale = obj.optDouble("scale", 1.0).toFloat(),
                    visible = obj.optBoolean("visible", true),
                ).normalized()
            }.toMap()
            OverlayLayout(placements).withOpacity(root.optDouble("opacity", DEFAULT_OPACITY.toDouble()).toFloat())
        } catch (e: JSONException) {
            null
        }
    }
}

private fun Float.finiteIn(min: Float, max: Float, fallback: Float): Float =
    if (isFinite()) coerceIn(min, max) else fallback
