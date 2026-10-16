package com.kytyps5.android.input

import android.view.KeyEvent

/**
 * Digital pad buttons and the Android key codes SDL's Android joystick driver maps them from
 * (SDL_sysjoystick.c keycode_to_SDL). The emulator then maps SDL gamepad buttons to PS buttons
 * (window.cpp ControllerButtonToPadButton); BACK becomes a centred touch-pad click.
 */
enum class PadButton(val keyCode: Int) {
    CROSS(KeyEvent.KEYCODE_BUTTON_A),
    CIRCLE(KeyEvent.KEYCODE_BUTTON_B),
    SQUARE(KeyEvent.KEYCODE_BUTTON_X),
    TRIANGLE(KeyEvent.KEYCODE_BUTTON_Y),
    L1(KeyEvent.KEYCODE_BUTTON_L1),
    R1(KeyEvent.KEYCODE_BUTTON_R1),
    L3(KeyEvent.KEYCODE_BUTTON_THUMBL),
    R3(KeyEvent.KEYCODE_BUTTON_THUMBR),
    OPTIONS(KeyEvent.KEYCODE_BUTTON_START),
    TOUCHPAD(KeyEvent.KEYCODE_BUTTON_SELECT),
    UP(KeyEvent.KEYCODE_DPAD_UP),
    DOWN(KeyEvent.KEYCODE_DPAD_DOWN),
    LEFT(KeyEvent.KEYCODE_DPAD_LEFT),
    RIGHT(KeyEvent.KEYCODE_DPAD_RIGHT),
}

/**
 * Joystick axes in the order SDL's generated Android mapping expects (leftx:a0 … righttrigger:a5).
 * Triggers rest at -1: the mapping scales the full -1..1 input range onto 0..max trigger travel.
 */
enum class PadAxis(val index: Int, val rest: Float) {
    LEFT_X(0, 0f),
    LEFT_Y(1, 0f),
    RIGHT_X(2, 0f),
    RIGHT_Y(3, 0f),
    L2(4, -1f),
    R2(5, -1f),
}

/** Immutable snapshot of the virtual pad; transitions are turned into events by [PadDiff]. */
data class PadState(
    val buttons: Set<PadButton> = emptySet(),
    val axes: Map<PadAxis, Float> = emptyMap(),
) {
    fun axis(axis: PadAxis): Float = axes[axis] ?: axis.rest

    fun withButton(button: PadButton, down: Boolean): PadState =
        copy(buttons = if (down) buttons + button else buttons - button)

    fun withButtons(remove: Set<PadButton>, add: Set<PadButton>): PadState =
        copy(buttons = buttons - remove + add)

    fun withAxis(axis: PadAxis, value: Float): PadState =
        copy(axes = axes + (axis to value.coerceIn(-1f, 1f)))

    /** Digital trigger: fully pressed or back at rest. */
    fun withTrigger(axis: PadAxis, down: Boolean): PadState = withAxis(axis, if (down) 1f else axis.rest)

    companion object {
        val NEUTRAL = PadState()
    }
}

sealed interface PadEvent {
    data class Button(val button: PadButton, val down: Boolean) : PadEvent
    data class Axis(val axis: PadAxis, val value: Float) : PadEvent
}

object PadDiff {
    /** Events that move a pad from [old] to [new]: releases first, then presses, then axes. */
    fun diff(old: PadState, new: PadState): List<PadEvent> {
        val released = (old.buttons - new.buttons).sortedBy { it.ordinal }.map { PadEvent.Button(it, false) }
        val pressed = (new.buttons - old.buttons).sortedBy { it.ordinal }.map { PadEvent.Button(it, true) }
        val axes = PadAxis.entries
            .filter { old.axis(it) != new.axis(it) }
            .map { PadEvent.Axis(it, new.axis(it)) }
        return released + pressed + axes
    }
}
