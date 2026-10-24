package com.kytyps5.android

/** Single-finger swipe from the right edge; vertical/multitouch control gestures stay in-game. */
class EdgeMenuGesture(private val density: Float) {
    private var startX = 0f
    private var startY = 0f
    private var armed = false
    var consumed = false
        private set
    fun begin(x: Float, y: Float, width: Int) {
        startX = x; startY = y
        armed = x >= width - 24 * density
        consumed = false
    }
    /** True only on the event that opens the menu. */
    fun move(x: Float, y: Float): Boolean {
        if (!armed || consumed) return false
        val dx = x - startX
        val dy = kotlin.math.abs(y - startY)
        if (dy > 40 * density && dy > kotlin.math.abs(dx)) { armed = false; return false }
        if (dx < -60 * density && dy < 48 * density) { consumed = true; return true }
        return false
    }
    fun cancel() { armed = false; consumed = false }
}
