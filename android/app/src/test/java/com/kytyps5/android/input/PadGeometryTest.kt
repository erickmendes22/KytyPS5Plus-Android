package com.kytyps5.android.input

import org.junit.Assert.assertEquals
import org.junit.Test

class PadGeometryTest {

    private fun dpad(x: Float, y: Float) = PadGeometry.dpadDirections(x, y, 0f, 0f, 90f, 90f)

    @Test
    fun dpadCentreIsNeutralAndEdgesPressOneDirection() {
        assertEquals(emptySet<PadButton>(), dpad(45f, 45f))
        assertEquals(setOf(PadButton.UP), dpad(45f, 10f))
        assertEquals(setOf(PadButton.DOWN), dpad(45f, 80f))
        assertEquals(setOf(PadButton.LEFT), dpad(10f, 45f))
        assertEquals(setOf(PadButton.RIGHT), dpad(80f, 45f))
    }

    @Test
    fun dpadCornersPressDiagonals() {
        assertEquals(setOf(PadButton.UP, PadButton.RIGHT), dpad(85f, 5f))
        assertEquals(setOf(PadButton.DOWN, PadButton.LEFT), dpad(5f, 85f))
    }

    @Test
    fun stickInsideDeadZoneReadsZero() {
        assertEquals(0f to 0f, PadGeometry.stickDeflection(2f, 2f, radius = 100f))
    }

    @Test
    fun stickIsClampedToUnitRange() {
        val (x, y) = PadGeometry.stickDeflection(500f, 0f, radius = 100f)
        assertEquals(1f, x, 1e-5f)
        assertEquals(0f, y, 1e-5f)
    }

    @Test
    fun fullDiagonalProjectsToSquareCorner() {
        val (x, y) = PadGeometry.stickDeflection(-300f, 300f, radius = 100f)
        assertEquals(-1f, x, 1e-4f)
        assertEquals(1f, y, 1e-4f)
    }

    @Test
    fun halfDeflectionIsProportional() {
        val (x, y) = PadGeometry.stickDeflection(0f, -50f, radius = 100f)
        assertEquals(0f, x, 1e-5f)
        assertEquals(-0.5f, y, 1e-5f)
    }

    @Test
    fun zeroRadiusIsSafe() {
        assertEquals(0f to 0f, PadGeometry.stickDeflection(5f, 5f, radius = 0f))
    }

    @Test
    fun knobStaysOnTheRim() {
        val (x, y) = PadGeometry.knobOffset(30f, 40f, radius = 10f)
        assertEquals(6f, x, 1e-5f)
        assertEquals(8f, y, 1e-5f)
        assertEquals(3f to 4f, PadGeometry.knobOffset(3f, 4f, radius = 10f))
    }
}
