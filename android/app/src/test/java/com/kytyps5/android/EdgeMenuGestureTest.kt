package com.kytyps5.android
import org.junit.Assert.*
import org.junit.Test
class EdgeMenuGestureTest {
    @Test fun rightEdgeSwipeOpensOnceAndVerticalMotionDoesNot() {
        val swipe = EdgeMenuGesture(2f)
        swipe.begin(1990f, 250f, 2000)
        assertFalse(swipe.move(1930f, 250f))
        assertTrue(swipe.move(1830f, 250f))
        assertTrue(swipe.consumed)
        assertFalse(swipe.move(1600f, 250f))
        swipe.cancel(); assertFalse(swipe.consumed)
        swipe.begin(1000f, 250f, 2000); assertFalse(swipe.move(800f, 250f))
        swipe.begin(1990f, 250f, 2000); assertFalse(swipe.move(1980f, 400f))
        assertFalse(swipe.move(1700f, 400f))
    }
}
