package com.kytyps5.android.input

/** Where pad events go: SDL's virtual joystick in the app ([SdlPadSink]), a recorder in tests. */
interface PadSink {
    val isConnected: Boolean
    fun button(keyCode: Int, down: Boolean)
    fun axis(index: Int, value: Float)
}

/**
 * Turns successive [PadState]s from the touch overlay into SDL joystick events. Until SDL has
 * registered the pad nothing is sent; the first state after it connects is sent in full, so a
 * button held while connecting is not lost and triggers start at rest.
 */
class GamepadBridge(private val sink: PadSink) {
    private var sent: PadState? = null

    fun update(state: PadState) {
        if (!sink.isConnected) {
            sent = null
            return
        }
        val events = sent?.let { PadDiff.diff(it, state) } ?: fullState(state)
        events.forEach { event ->
            when (event) {
                is PadEvent.Button -> sink.button(event.button.keyCode, event.down)
                is PadEvent.Axis -> sink.axis(event.axis.index, event.value)
            }
        }
        sent = state
    }

    fun releaseAll() = update(PadState.NEUTRAL)

    private fun fullState(state: PadState): List<PadEvent> =
        state.buttons.sortedBy { it.ordinal }.map { PadEvent.Button(it, true) } +
            PadAxis.entries.map { PadEvent.Axis(it, state.axis(it)) }
}

/** The real sink: SDL's joystick JNI through [org.libsdl.app.KytyVirtualPad]. */
object SdlPadSink : PadSink {
    override val isConnected: Boolean get() = org.libsdl.app.KytyVirtualPad.isConnected()
    override fun button(keyCode: Int, down: Boolean) = org.libsdl.app.KytyVirtualPad.button(keyCode, down)
    override fun axis(index: Int, value: Float) = org.libsdl.app.KytyVirtualPad.axis(index, value)
}
