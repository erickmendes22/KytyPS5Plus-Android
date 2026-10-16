/*
 * D-pad thirds rule and stick circle-to-square projection ported from Dolphin's Android input
 * overlay (InputOverlay / InputOverlayDrawableJoystick), as carried by Vita3K's android/ overlay:
 *   Copyright 2013, 2016 Dolphin Emulator Project
 *   SPDX-License-Identifier: GPL-2.0-or-later
 * Adapted for KytyPS5 (GPL-2.0-only) as pure functions without Android types.
 */
package com.kytyps5.android.input

import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.sqrt

object PadGeometry {

    /**
     * D-pad directions for a touch at ([x], [y]) inside a [width] x [height] box at ([left], [top]):
     * the outer thirds of each axis press that direction, so corners press diagonals.
     */
    fun dpadDirections(x: Float, y: Float, left: Float, top: Float, width: Float, height: Float): Set<PadButton> {
        val result = mutableSetOf<PadButton>()
        if (y < top + height / 3f) result += PadButton.UP
        if (y > top + height - height / 3f) result += PadButton.DOWN
        if (x < left + width / 3f) result += PadButton.LEFT
        if (x > left + width - width / 3f) result += PadButton.RIGHT
        return result
    }

    /**
     * Stick deflection for a touch offset ([dx], [dy]) from the stick centre with travel [radius].
     * The knob is clamped to the circle; the reported value is projected onto the bounding square so
     * a full diagonal reads (±1, ±1) like a real stick. Offsets inside [deadZone] (fraction) read 0.
     */
    fun stickDeflection(dx: Float, dy: Float, radius: Float, deadZone: Float = DEFAULT_DEAD_ZONE): Pair<Float, Float> {
        if (radius <= 0f) return 0f to 0f
        var x = dx / radius
        var y = dy / radius
        val length = hypot(x, y)
        if (length > 1f) {
            x /= length
            y /= length
        }
        if (hypot(x, y) < deadZone) return 0f to 0f
        val major = maxOf(abs(x), abs(y))
        val minor = minOf(abs(x), abs(y))
        if (major < MIN_PROJECTION) return x to y
        val ratio = minor / major
        val scale = sqrt(1f + ratio * ratio)
        return (x * scale).coerceIn(-1f, 1f) to (y * scale).coerceIn(-1f, 1f)
    }

    /** Knob position (offset from centre) for drawing: the touch offset clamped to [radius]. */
    fun knobOffset(dx: Float, dy: Float, radius: Float): Pair<Float, Float> {
        val length = hypot(dx, dy)
        if (length <= radius || length == 0f) return dx to dy
        return dx / length * radius to dy / length * radius
    }

    const val DEFAULT_DEAD_ZONE = 0.08f
    private const val MIN_PROJECTION = 0.01f
}
