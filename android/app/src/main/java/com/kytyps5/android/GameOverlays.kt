package com.kytyps5.android

import android.app.Activity
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.InputDevice
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.PopupMenu
import android.widget.TextView
import com.kytyps5.android.hud.PerformanceHud
import com.kytyps5.android.input.ControllerEditor
import com.kytyps5.android.input.GamepadBridge
import com.kytyps5.android.input.OverlayProfileStore
import com.kytyps5.android.input.SdlPadSink
import com.kytyps5.android.input.TouchControllerView
import java.io.File
import org.libsdl.app.KytyVirtualPad

/**
 * Everything GameActivity draws over the SDL surface: the touch controller, its layout editor, the
 * performance HUD and a small menu button to toggle them. The container itself never consumes
 * touches, so with the controller hidden SDL receives them as before (tap = Cross).
 */
class GameOverlays(
    private val activity: Activity,
    private val gamePath: String?,
    touchControls: Boolean,
    hud: Boolean,
) {
    private val density = activity.resources.displayMetrics.density
    private val Int.dp: Int get() = (this * density).toInt()
    private val bridge = GamepadBridge(SdlPadSink)
    private val store = OverlayProfileStore(File(activity.filesDir, OverlayProfileStore.DIRECTORY_NAME))
    private val controller = TouchControllerView(activity) { bridge.update(it) }
    private val editor = ControllerEditor(activity, controller, store, gamePath) { menuButton.visibility = View.VISIBLE }
    private val performanceHud = PerformanceHud(activity)
    private var hudWanted = hud
    private val menuButton = TextView(activity).apply {
        text = "⋯"
        textSize = 22f
        gravity = Gravity.CENTER
        setTextColor(Color.WHITE)
        alpha = 0.7f
        background = GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            setColor(Color.argb(120, 0, 0, 0))
        }
        setOnClickListener { showMenu(it) }
    }

    val root: View = FrameLayout(activity).apply {
        addView(controller, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        addView(performanceHud.view, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.START).apply {
            setMargins(12.dp, 12.dp, 0, 0)
        })
        addView(editor.view, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.CENTER_HORIZONTAL).apply {
            topMargin = 8.dp
        })
        addView(menuButton, FrameLayout.LayoutParams(40.dp, 40.dp, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply {
            bottomMargin = 6.dp
        })
    }

    init {
        KytyVirtualPad.install()
        controller.layout = store.load(gamePath).layout
        setControllerVisible(touchControls && !hasPhysicalGamepad())
        performanceHud.view.visibility = if (hudWanted) View.VISIBLE else View.GONE
    }

    fun onResume() {
        if (hudWanted) performanceHud.start()
    }

    fun onPause() {
        controller.releaseAll()
        performanceHud.stop()
    }

    fun onTitle(title: CharSequence?) = performanceHud.onTitle(title)

    private fun setControllerVisible(visible: Boolean) {
        controller.releaseAll()
        controller.visibility = if (visible) View.VISIBLE else View.GONE
        // Once SDL knows the virtual pad it stays connected; hiding only stops input.
        if (visible) KytyVirtualPad.setWanted(true)
    }

    private fun setHudVisible(visible: Boolean) {
        hudWanted = visible
        performanceHud.view.visibility = if (visible) View.VISIBLE else View.GONE
        if (visible) performanceHud.start() else performanceHud.stop()
    }

    private fun showMenu(anchor: View) {
        val controlsShown = controller.visibility == View.VISIBLE
        PopupMenu(activity, anchor).apply {
            menu.add(0, MENU_CONTROLS, 0, if (controlsShown) "Hide touch controls" else "Show touch controls")
            menu.add(0, MENU_EDIT, 1, "Edit controls layout…")
            menu.add(0, MENU_HUD, 2, if (hudWanted) "Hide performance HUD" else "Show performance HUD")
            setOnMenuItemClickListener { item ->
                when (item.itemId) {
                    MENU_CONTROLS -> setControllerVisible(!controlsShown)
                    MENU_EDIT -> {
                        setControllerVisible(true)
                        menuButton.visibility = View.GONE
                        editor.open()
                    }
                    MENU_HUD -> setHudVisible(!hudWanted)
                }
                true
            }
        }.show()
    }

    /** A real controller makes the touch overlay redundant; it can still be shown from the menu. */
    private fun hasPhysicalGamepad(): Boolean = InputDevice.getDeviceIds().any { id ->
        val device = InputDevice.getDevice(id) ?: return@any false
        !device.isVirtual && device.supportsSource(InputDevice.SOURCE_GAMEPAD)
    }

    private companion object {
        const val MENU_CONTROLS = 1
        const val MENU_EDIT = 2
        const val MENU_HUD = 3
    }
}
