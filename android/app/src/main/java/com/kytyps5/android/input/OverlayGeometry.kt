package com.kytyps5.android.input

import kotlin.math.min

/** Axis-aligned control bounds in view pixels (Android-free so the input logic is unit-testable). */
data class ControlBox(val centerX: Float, val centerY: Float, val width: Float, val height: Float) {
    val left: Float get() = centerX - width / 2f
    val top: Float get() = centerY - height / 2f
    val right: Float get() = centerX + width / 2f
    val bottom: Float get() = centerY + height / 2f

    /** Hit test with [slop] extra margin as a fraction of the box size (fingers are imprecise). */
    fun contains(x: Float, y: Float, slop: Float = 0f): Boolean {
        val mx = width * slop
        val my = height * slop
        return x >= left - mx && x <= right + mx && y >= top - my && y <= bottom + my
    }
}

/** A finger currently owned by [control], at ([x], [y]). */
data class Touch(val control: OverlayControl, val x: Float, val y: Float)

object OverlayGeometry {
    const val HIT_SLOP = 0.15f

    /** Bounds of every visible control for a [width] x [height] view. */
    fun boxes(layout: OverlayLayout, width: Float, height: Float, includeHidden: Boolean = false): Map<OverlayControl, ControlBox> {
        val unit = min(width, height)
        return OverlayControl.entries
            .filter { includeHidden || layout.placement(it).visible }
            .associateWith { control ->
                val p = layout.placement(control)
                val size = control.defaultSize * p.scale * unit
                val (w, h) = shapeOf(control, size)
                ControlBox(p.x * width, p.y * height, w, h)
            }
    }

    private fun shapeOf(control: OverlayControl, size: Float): Pair<Float, Float> = when (control.kind) {
        ControlKind.BUTTON, ControlKind.DPAD, ControlKind.STICK -> size to size
        ControlKind.SHOULDER, ControlKind.TRIGGER -> size * 1.6f to size * 0.85f
        ControlKind.PILL -> if (control == OverlayControl.TOUCHPAD) size * 3.5f to size * 0.8f else size * 1.8f to size * 0.75f
    }

    /**
     * Topmost control under ([x], [y]): exact hits win over slop hits, and among slop hits the
     * nearest centre wins, so neighbouring face buttons stay distinguishable.
     */
    fun hit(boxes: Map<OverlayControl, ControlBox>, x: Float, y: Float): OverlayControl? {
        boxes.entries.lastOrNull { it.value.contains(x, y) }?.let { return it.key }
        return boxes.entries
            .filter { it.value.contains(x, y, HIT_SLOP) }
            .minByOrNull { (_, b) -> (b.centerX - x) * (b.centerX - x) + (b.centerY - y) * (b.centerY - y) }
            ?.key
    }

    /** Pad state produced by the fingers in [touches] (several fingers may share a control). */
    fun padState(boxes: Map<OverlayControl, ControlBox>, touches: Collection<Touch>): PadState {
        var state = PadState.NEUTRAL
        touches.forEach { touch ->
            val box = boxes[touch.control] ?: return@forEach
            val control = touch.control
            control.button?.let { state = state.withButton(it, true) }
            control.trigger?.let { state = state.withTrigger(it, true) }
            when (control.kind) {
                ControlKind.DPAD -> state = state.withButtons(
                    remove = emptySet(),
                    add = PadGeometry.dpadDirections(touch.x, touch.y, box.left, box.top, box.width, box.height),
                )
                ControlKind.STICK -> {
                    val (sx, sy) = PadGeometry.stickDeflection(touch.x - box.centerX, touch.y - box.centerY, box.width / 2f)
                    val (ax, ay) = if (control == OverlayControl.LEFT_STICK) PadAxis.LEFT_X to PadAxis.LEFT_Y else PadAxis.RIGHT_X to PadAxis.RIGHT_Y
                    state = state.withAxis(ax, sx).withAxis(ay, sy)
                }
                else -> Unit
            }
        }
        return state
    }
}
