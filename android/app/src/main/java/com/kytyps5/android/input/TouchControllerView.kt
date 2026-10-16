/*
 * Multi-touch handling (one owner control per pointer, sticks/D-pad tracking their finger until it
 * lifts) follows the Dolphin/Vita3K InputOverlay and RPCSX PadOverlay designs (GPL-2.0-or-later /
 * GPL-2.0); controls are drawn as vectors instead of bitmaps.
 */
package com.kytyps5.android.input

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View

/**
 * PS-style on-screen controller. In play mode every touch is consumed (so SDL's tap-to-Cross
 * fallback never fires) and the combined [PadState] goes to [onPadState]. In edit mode controls
 * are dragged to new positions and the selection is reported for the editor bar.
 */
@SuppressLint("ViewConstructor")
class TouchControllerView(
    context: Context,
    private val onPadState: (PadState) -> Unit,
) : View(context) {

    var layout: OverlayLayout = OverlayLayout.DEFAULT
        set(value) {
            field = value
            boxes = emptyMap()
            invalidate()
        }

    var editMode: Boolean = false
        set(value) {
            field = value
            boxes = emptyMap()
            releaseAll()
            selected = null
            invalidate()
        }

    var selected: OverlayControl? = null
        private set(value) {
            field = value
            onSelectionChanged(value)
        }

    var onSelectionChanged: (OverlayControl?) -> Unit = {}
    var onLayoutEdited: (OverlayLayout) -> Unit = {}

    private var boxes: Map<OverlayControl, ControlBox> = emptyMap()
    private val touches = LinkedHashMap<Int, Touch>()
    private var state = PadState.NEUTRAL
    private var dragDx = 0f
    private var dragDy = 0f

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER; isFakeBoldText = true }
    private val dashed = DashPathEffect(floatArrayOf(12f, 10f), 0f)
    private val rect = RectF()
    private val path = Path()

    private fun currentBoxes(): Map<OverlayControl, ControlBox> {
        if (boxes.isEmpty() && width > 0 && height > 0) {
            boxes = OverlayGeometry.boxes(layout, width.toFloat(), height.toFloat(), includeHidden = editMode)
        }
        return boxes
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        boxes = emptyMap()
    }

    /** Lifts every finger, e.g. when the overlay is hidden or the activity pauses. */
    fun releaseAll() {
        touches.clear()
        publish()
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (editMode) {
            onEditTouch(event)
        } else {
            onPlayTouch(event)
        }
        return true
    }

    private fun onPlayTouch(event: MotionEvent) {
        val all = currentBoxes()
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                // A fresh gesture: drop fingers whose UP was never delivered.
                if (event.actionMasked == MotionEvent.ACTION_DOWN) touches.clear()
                val i = event.actionIndex
                val control = OverlayGeometry.hit(all, event.getX(i), event.getY(i))
                if (control != null) {
                    touches[event.getPointerId(i)] = Touch(control, event.getX(i), event.getY(i))
                    performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                }
            }
            MotionEvent.ACTION_MOVE -> for (i in 0 until event.pointerCount) {
                val id = event.getPointerId(i)
                touches[id]?.let { touches[id] = it.copy(x = event.getX(i), y = event.getY(i)) }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> touches.remove(event.getPointerId(event.actionIndex))
            MotionEvent.ACTION_CANCEL -> touches.clear()
        }
        publish()
    }

    private fun publish() {
        val next = OverlayGeometry.padState(currentBoxes(), touches.values)
        if (next != state) {
            state = next
            onPadState(next)
        }
        invalidate()
    }

    private fun onEditTouch(event: MotionEvent) {
        val all = currentBoxes()
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val control = OverlayGeometry.hit(all, event.x, event.y)
                selected = control
                control?.let { all[it] }?.let { box ->
                    dragDx = event.x - box.centerX
                    dragDy = event.y - box.centerY
                }
                invalidate()
            }
            MotionEvent.ACTION_MOVE -> {
                val control = selected ?: return
                val p = layout.placement(control)
                layout = layout.with(control, p.copy(x = (event.x - dragDx) / width, y = (event.y - dragDy) / height))
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> if (selected != null) onLayoutEdited(layout)
        }
    }

    override fun onDraw(canvas: Canvas) {
        val alpha = (layout.opacity * 255).toInt()
        currentBoxes().forEach { (control, box) ->
            val visible = layout.placement(control).visible
            val pressed = isPressed(control)
            stroke.pathEffect = if (visible) null else dashed
            val a = if (visible) alpha else alpha / 2
            when (control.kind) {
                ControlKind.BUTTON -> drawFaceButton(canvas, control, box, pressed, a)
                ControlKind.SHOULDER, ControlKind.TRIGGER, ControlKind.PILL -> drawPill(canvas, control, box, pressed, a)
                ControlKind.DPAD -> drawDpad(canvas, box, a)
                ControlKind.STICK -> drawStick(canvas, control, box, a)
            }
            if (editMode && control == selected) {
                stroke.pathEffect = null
                stroke.color = SELECTION
                stroke.strokeWidth = 4f
                rect.set(box.left - 6, box.top - 6, box.right + 6, box.bottom + 6)
                canvas.drawRect(rect, stroke)
            }
        }
    }

    private fun isPressed(control: OverlayControl): Boolean = touches.values.any { it.control == control }

    private fun base(canvas: Canvas, box: ControlBox, pressed: Boolean, alpha: Int, round: Boolean) {
        fill.color = if (pressed) PRESSED_FILL else IDLE_FILL
        fill.alpha = alpha * (if (pressed) 3 else 2) / 3
        stroke.color = Color.WHITE
        stroke.alpha = alpha
        stroke.strokeWidth = box.height * 0.04f
        rect.set(box.left, box.top, box.right, box.bottom)
        if (round) {
            canvas.drawOval(rect, fill)
            canvas.drawOval(rect, stroke)
        } else {
            val r = box.height / 2f
            canvas.drawRoundRect(rect, r, r, fill)
            canvas.drawRoundRect(rect, r, r, stroke)
        }
    }

    private fun drawFaceButton(canvas: Canvas, control: OverlayControl, box: ControlBox, pressed: Boolean, alpha: Int) {
        base(canvas, box, pressed, alpha, round = true)
        val r = box.width * 0.26f
        val cx = box.centerX
        val cy = box.centerY
        stroke.strokeWidth = box.width * 0.07f
        stroke.alpha = alpha
        when (control) {
            OverlayControl.CROSS -> {
                stroke.color = CROSS_COLOR
                stroke.alpha = alpha
                canvas.drawLine(cx - r, cy - r, cx + r, cy + r, stroke)
                canvas.drawLine(cx - r, cy + r, cx + r, cy - r, stroke)
            }
            OverlayControl.CIRCLE -> {
                stroke.color = CIRCLE_COLOR
                stroke.alpha = alpha
                canvas.drawCircle(cx, cy, r, stroke)
            }
            OverlayControl.SQUARE -> {
                stroke.color = SQUARE_COLOR
                stroke.alpha = alpha
                canvas.drawRect(cx - r * 0.85f, cy - r * 0.85f, cx + r * 0.85f, cy + r * 0.85f, stroke)
            }
            OverlayControl.TRIANGLE -> {
                stroke.color = TRIANGLE_COLOR
                stroke.alpha = alpha
                path.reset()
                path.moveTo(cx, cy - r)
                path.lineTo(cx + r * 0.95f, cy + r * 0.7f)
                path.lineTo(cx - r * 0.95f, cy + r * 0.7f)
                path.close()
                canvas.drawPath(path, stroke)
            }
            else -> label(canvas, control.label, box, alpha)
        }
    }

    private fun drawPill(canvas: Canvas, control: OverlayControl, box: ControlBox, pressed: Boolean, alpha: Int) {
        base(canvas, box, pressed, alpha, round = false)
        label(canvas, control.label, box, alpha)
    }

    private fun label(canvas: Canvas, value: String, box: ControlBox, alpha: Int) {
        text.color = Color.WHITE
        text.alpha = alpha
        text.textSize = minOf(box.height * 0.42f, box.width * 0.9f / value.length.coerceAtLeast(2) * 1.7f)
        canvas.drawText(value, box.centerX, box.centerY - (text.ascent() + text.descent()) / 2f, text)
    }

    private fun drawDpad(canvas: Canvas, box: ControlBox, alpha: Int) {
        val third = box.width / 3f
        val arms = listOf(
            PadButton.UP to RectF(box.left + third, box.top, box.right - third, box.top + third * 1.1f),
            PadButton.DOWN to RectF(box.left + third, box.bottom - third * 1.1f, box.right - third, box.bottom),
            PadButton.LEFT to RectF(box.left, box.top + third, box.left + third * 1.1f, box.bottom - third),
            PadButton.RIGHT to RectF(box.right - third * 1.1f, box.top + third, box.right, box.bottom - third),
        )
        stroke.color = Color.WHITE
        stroke.alpha = alpha
        stroke.strokeWidth = box.height * 0.015f
        arms.forEach { (direction, arm) ->
            val pressed = direction in state.buttons
            fill.color = if (pressed) PRESSED_FILL else IDLE_FILL
            fill.alpha = alpha * (if (pressed) 3 else 2) / 3
            canvas.drawRoundRect(arm, third * 0.15f, third * 0.15f, fill)
            canvas.drawRoundRect(arm, third * 0.15f, third * 0.15f, stroke)
        }
    }

    private fun drawStick(canvas: Canvas, control: OverlayControl, box: ControlBox, alpha: Int) {
        val radius = box.width / 2f
        fill.color = IDLE_FILL
        fill.alpha = alpha / 2
        stroke.color = Color.WHITE
        stroke.alpha = alpha
        stroke.strokeWidth = box.width * 0.02f
        canvas.drawCircle(box.centerX, box.centerY, radius, fill)
        canvas.drawCircle(box.centerX, box.centerY, radius, stroke)
        val touch = touches.values.lastOrNull { it.control == control }
        val (kx, ky) = touch?.let { PadGeometry.knobOffset(it.x - box.centerX, it.y - box.centerY, radius * 0.6f) } ?: (0f to 0f)
        fill.color = if (touch != null) PRESSED_FILL else KNOB_FILL
        fill.alpha = alpha
        canvas.drawCircle(box.centerX + kx, box.centerY + ky, radius * 0.4f, fill)
        canvas.drawCircle(box.centerX + kx, box.centerY + ky, radius * 0.4f, stroke)
    }

    private companion object {
        val IDLE_FILL = Color.rgb(20, 20, 24)
        val PRESSED_FILL = Color.rgb(230, 230, 235)
        val KNOB_FILL = Color.rgb(60, 60, 68)
        val SELECTION = Color.rgb(255, 196, 0)
        val CROSS_COLOR = Color.rgb(124, 178, 232)
        val CIRCLE_COLOR = Color.rgb(255, 102, 102)
        val SQUARE_COLOR = Color.rgb(232, 149, 200)
        val TRIANGLE_COLOR = Color.rgb(64, 224, 176)
    }
}
