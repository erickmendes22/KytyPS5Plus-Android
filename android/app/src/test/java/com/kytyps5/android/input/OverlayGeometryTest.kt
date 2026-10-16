package com.kytyps5.android.input

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class OverlayGeometryTest {

    private val width = 2000f
    private val height = 1000f
    private val boxes = OverlayGeometry.boxes(OverlayLayout.DEFAULT, width, height)

    private fun box(c: OverlayControl) = boxes.getValue(c)

    @Test
    fun boxesFollowNormalisedPlacementAndShorterSide() {
        val cross = box(OverlayControl.CROSS)
        assertEquals(OverlayControl.CROSS.defaultX * width, cross.centerX, 1e-3f)
        assertEquals(OverlayControl.CROSS.defaultSize * height, cross.width, 1e-3f)
        assertTrue(box(OverlayControl.TOUCHPAD).width > box(OverlayControl.OPTIONS).width)
    }

    @Test
    fun hiddenControlsHaveNoBoxUnlessRequested() {
        val layout = OverlayLayout.DEFAULT.with(OverlayControl.L3, OverlayControl.L3.defaultPlacement.copy(visible = false))
        assertFalse(OverlayControl.L3 in OverlayGeometry.boxes(layout, width, height))
        assertTrue(OverlayControl.L3 in OverlayGeometry.boxes(layout, width, height, includeHidden = true))
    }

    /** Phones (2.2:1), 16:9 and 16:10 screens, and tablets such as the OnePlus Pad 3 (3392x2400, 1.41:1) and 4:3. */
    @Test
    fun defaultControlsDoNotOverlapOnCommonAspectRatios() {
        listOf(2.2f, 2f, 16f / 9f, 1.6f, 3392f / 2400f, 4f / 3f).forEach { aspect ->
            assertNoOverlap(OverlayGeometry.boxes(OverlayLayout.DEFAULT, 1000f * aspect, 1000f), aspect)
        }
    }

    @Test
    fun defaultControlsStayOnScreen() {
        listOf(2.2f, 4f / 3f).forEach { aspect ->
            val w = 1000f * aspect
            OverlayGeometry.boxes(OverlayLayout.DEFAULT, w, 1000f).forEach { (c, b) ->
                assertTrue("$c off screen at $aspect", b.left >= 0f && b.top >= 0f && b.right <= w && b.bottom <= 1000f)
            }
        }
    }

    private fun assertNoOverlap(boxes: Map<OverlayControl, ControlBox>, aspect: Float) {
        val list = boxes.entries.toList()
        for (i in list.indices) for (j in i + 1 until list.size) {
            val a = list[i].value
            val b = list[j].value
            val overlap = a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom
            assertFalse("${list[i].key} overlaps ${list[j].key} at $aspect:1", overlap)
        }
    }

    @Test
    fun hitFindsExactThenNearestWithinSlop() {
        val cross = box(OverlayControl.CROSS)
        assertEquals(OverlayControl.CROSS, OverlayGeometry.hit(boxes, cross.centerX, cross.centerY))
        assertEquals(OverlayControl.CROSS, OverlayGeometry.hit(boxes, cross.centerX, cross.bottom + cross.height * 0.1f))
        assertNull(OverlayGeometry.hit(boxes, width * 0.5f, height * 0.5f))
    }

    @Test
    fun padStateCombinesFingers() {
        val cross = box(OverlayControl.CROSS)
        val stick = box(OverlayControl.LEFT_STICK)
        val dpad = box(OverlayControl.DPAD)
        val state = OverlayGeometry.padState(
            boxes,
            listOf(
                Touch(OverlayControl.CROSS, cross.centerX, cross.centerY),
                Touch(OverlayControl.R2, 0f, 0f),
                Touch(OverlayControl.LEFT_STICK, stick.right + 100f, stick.centerY),
                Touch(OverlayControl.DPAD, dpad.left + 1f, dpad.top + 1f),
            ),
        )
        assertEquals(setOf(PadButton.CROSS, PadButton.UP, PadButton.LEFT), state.buttons)
        assertEquals(1f, state.axis(PadAxis.R2))
        assertEquals(-1f, state.axis(PadAxis.L2))
        assertEquals(1f, state.axis(PadAxis.LEFT_X), 1e-4f)
        assertEquals(0f, state.axis(PadAxis.LEFT_Y), 1e-4f)
    }

    @Test
    fun noTouchesIsNeutral() {
        assertEquals(PadState.NEUTRAL, OverlayGeometry.padState(boxes, emptyList()))
    }
}
