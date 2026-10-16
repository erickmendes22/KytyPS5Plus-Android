package com.kytyps5.android.input

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class PadStateTest {

    @Test
    fun neutralTriggersRestAtMinusOne() {
        assertEquals(-1f, PadState.NEUTRAL.axis(PadAxis.L2))
        assertEquals(0f, PadState.NEUTRAL.axis(PadAxis.LEFT_X))
    }

    @Test
    fun updatesReturnNewStatesWithoutMutating() {
        val base = PadState.NEUTRAL
        val pressed = base.withButton(PadButton.CROSS, true)
        assertTrue(base.buttons.isEmpty())
        assertEquals(setOf(PadButton.CROSS), pressed.buttons)
        assertEquals(1f, pressed.withAxis(PadAxis.LEFT_X, 3f).axis(PadAxis.LEFT_X))
    }

    @Test
    fun diffReleasesBeforePressesThenAxes() {
        val old = PadState(buttons = setOf(PadButton.CROSS, PadButton.UP))
        val new = PadState(buttons = setOf(PadButton.UP, PadButton.CIRCLE))
            .withTrigger(PadAxis.R2, true)
            .withAxis(PadAxis.LEFT_Y, -0.5f)
        assertEquals(
            listOf(
                PadEvent.Button(PadButton.CROSS, false),
                PadEvent.Button(PadButton.CIRCLE, true),
                PadEvent.Axis(PadAxis.LEFT_Y, -0.5f),
                PadEvent.Axis(PadAxis.R2, 1f),
            ),
            PadDiff.diff(old, new),
        )
    }

    @Test
    fun diffOfEqualStatesIsEmpty() {
        val state = PadState.NEUTRAL.withAxis(PadAxis.L2, -1f)
        assertTrue(PadDiff.diff(PadState.NEUTRAL, state).isEmpty())
    }

    @Test
    fun releasingATriggerReturnsItToRest() {
        val pressed = PadState.NEUTRAL.withTrigger(PadAxis.L2, true)
        assertEquals(listOf(PadEvent.Axis(PadAxis.L2, -1f)), PadDiff.diff(pressed, pressed.withTrigger(PadAxis.L2, false)))
    }
}
