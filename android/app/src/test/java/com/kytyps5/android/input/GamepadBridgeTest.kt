package com.kytyps5.android.input

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class GamepadBridgeTest {

    private class Recorder : PadSink {
        override var isConnected = false
        val log = mutableListOf<String>()
        override fun button(keyCode: Int, down: Boolean) {
            log += "b$keyCode=${if (down) 1 else 0}"
        }

        override fun axis(index: Int, value: Float) {
            log += "a$index=$value"
        }
    }

    @Test
    fun nothingIsSentBeforeSdlConnects() {
        val sink = Recorder()
        GamepadBridge(sink).update(PadState.NEUTRAL.withButton(PadButton.CROSS, true))
        assertTrue(sink.log.isEmpty())
    }

    @Test
    fun firstStateAfterConnectIsSentInFull() {
        val sink = Recorder()
        val bridge = GamepadBridge(sink)
        val held = PadState.NEUTRAL.withButton(PadButton.CROSS, true)
        bridge.update(held)
        sink.isConnected = true
        bridge.update(held)
        assertEquals(
            listOf("b${PadButton.CROSS.keyCode}=1", "a0=0.0", "a1=0.0", "a2=0.0", "a3=0.0", "a4=-1.0", "a5=-1.0"),
            sink.log,
        )
    }

    @Test
    fun laterUpdatesSendOnlyChanges() {
        val sink = Recorder().apply { isConnected = true }
        val bridge = GamepadBridge(sink)
        bridge.update(PadState.NEUTRAL)
        sink.log.clear()
        bridge.update(PadState.NEUTRAL.withTrigger(PadAxis.R2, true))
        bridge.update(PadState.NEUTRAL.withTrigger(PadAxis.R2, true))
        assertEquals(listOf("a5=1.0"), sink.log)
    }

    @Test
    fun releaseAllReturnsToNeutral() {
        val sink = Recorder().apply { isConnected = true }
        val bridge = GamepadBridge(sink)
        bridge.update(PadState.NEUTRAL.withButton(PadButton.UP, true).withAxis(PadAxis.LEFT_X, 0.7f))
        sink.log.clear()
        bridge.releaseAll()
        assertEquals(listOf("b${PadButton.UP.keyCode}=0", "a0=0.0"), sink.log)
    }

    @Test
    fun reconnectResendsFullState() {
        val sink = Recorder().apply { isConnected = true }
        val bridge = GamepadBridge(sink)
        bridge.update(PadState.NEUTRAL)
        sink.isConnected = false
        bridge.update(PadState.NEUTRAL)
        sink.isConnected = true
        sink.log.clear()
        bridge.update(PadState.NEUTRAL)
        assertEquals(6, sink.log.size)
    }
}
