package com.kytyps5.android

import android.app.Activity
import android.app.AlertDialog
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.MotionEvent
import android.widget.ScrollView
import android.view.InputDevice
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import com.kytyps5.android.hud.PerformanceHud
import com.kytyps5.android.hud.HudMode
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
    hudMode: HudMode = HudMode.COMPACT,
) {
    private val density = activity.resources.displayMetrics.density
    private val Int.dp: Int get() = (this * density).toInt()
    private val bridge = GamepadBridge(SdlPadSink)
    private val store = OverlayProfileStore(File(activity.filesDir, OverlayProfileStore.DIRECTORY_NAME))
    private val controller = TouchControllerView(activity) { bridge.update(it) }
    private val editor = ControllerEditor(activity, controller, store, gamePath) { menuButton.visibility = View.VISIBLE }
    private val performanceHud = PerformanceHud(activity)
    private var hudWanted = hud
    private var mode = hudMode
    private val menuGesture = EdgeMenuGesture(density)
    private var menuDialog: AlertDialog? = null
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
        // Top-left corner just below L2/L1: game titles and HUDs are usually centred at the top.
        addView(performanceHud.view, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.START).apply {
            val metrics = activity.resources.displayMetrics
            topMargin = (minOf(metrics.widthPixels, metrics.heightPixels) * HUD_TOP_FRACTION).toInt()
            leftMargin = 12.dp
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
        performanceHud.setMode(mode)
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

    /** Right-side in-game panel matching the landscape launcher's aesthetic. */
    fun consumeMenuSwipe(event: MotionEvent): Boolean {
        if (event.pointerCount > 1) { menuGesture.cancel(); return false }
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> menuGesture.begin(event.x, event.y, root.width)
            MotionEvent.ACTION_MOVE -> if (menuGesture.move(event.x, event.y)) {
                controller.releaseAll()
                showMenu(menuButton)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                val consumed = menuGesture.consumed
                menuGesture.cancel()
                return consumed
            }
        }
        return menuGesture.consumed
    }

    private fun showMenu(anchor: View) {
        if (menuDialog?.isShowing == true) return
        controller.releaseAll()
        val sheet = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(18.dp, 20.dp, 18.dp, 18.dp)
            background = GradientDrawable().apply {
                setColor(Color.rgb(17, 30, 49))
                cornerRadius = 16.dp.toFloat()
                setStroke(1.dp, Color.rgb(53, 91, 126))
            }
        }
        fun heading(text: String, size: Float, color: Int = Color.WHITE) = TextView(activity).apply {
            this.text = text
            textSize = size
            setTextColor(color)
            setPadding(0, 0, 0, 12.dp)
            if (size >= 15f) setTypeface(typeface, android.graphics.Typeface.BOLD)
        }
        sheet.addView(heading("KYTYPS5 PLUS  /  IN-GAME", 11f, Color.rgb(107, 207, 255)))
        sheet.addView(heading("Game menu", 21f))
        sheet.addView(heading("Change the overlay or exit the game.", 12f, Color.rgb(164, 187, 210)))
        lateinit var dialog: AlertDialog
        fun action(title: String, danger: Boolean = false, onClick: () -> Unit) {
            val button = TextView(activity).apply {
                text = title
                textSize = 14f
                gravity = Gravity.CENTER_VERTICAL
                setTextColor(if (danger) Color.rgb(255, 165, 170) else Color.WHITE)
                setPadding(14.dp, 0, 12.dp, 0)
                background = GradientDrawable().apply {
                    cornerRadius = 12.dp.toFloat()
                    setColor(if (danger) Color.rgb(70, 30, 41) else Color.rgb(26, 47, 73))
                    setStroke(1.dp, if (danger) Color.rgb(132, 59, 69) else Color.rgb(47, 79, 112))
                }
                setOnClickListener { onClick() }
            }
            sheet.addView(button, LinearLayout.LayoutParams(-1, 44.dp).apply { bottomMargin = 10.dp })
        }
        action("Retomar jogo") { dialog.dismiss() }
        action(if (controller.visibility == View.VISIBLE) "Hide touch controls" else "Show touch controls") {
            setControllerVisible(controller.visibility != View.VISIBLE)
            dialog.dismiss()
        }
        action("Edit touch controls") {
            setControllerVisible(true)
            menuButton.visibility = View.GONE
            editor.open()
            dialog.dismiss()
        }
        action(if (hudWanted) "Hide performance HUD" else "Show performance HUD") {
            setHudVisible(!hudWanted)
            dialog.dismiss()
        }
        action("HUD style: " + mode.label) {
            dialog.dismiss()
            AlertDialog.Builder(activity)
                .setTitle("Performance HUD")
                .setSingleChoiceItems(HudMode.entries.map { it.label }.toTypedArray(),
                    HudMode.entries.indexOf(mode)) { chooser, which ->
                    mode = HudMode.entries[which]
                    performanceHud.setMode(mode)
                    LauncherSettings(activity).hudMode = mode
                    chooser.dismiss()
                }
                .setNegativeButton("Cancel", null)
                .show()
        }
        action("Exit game", danger = true) {
            dialog.dismiss()
            AlertDialog.Builder(activity)
                .setTitle("Exit game?")
                .setMessage("Close the game and return to the library? Unsaved progress may be lost.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Exit game") { _, _ ->
                    controller.releaseAll()
                    performanceHud.stop()
                    activity.finish()
                }
                .show()
        }
        val scroll = ScrollView(activity).apply {
            isFillViewport = true
            addView(sheet, ViewGroup.LayoutParams(-1, -2))
        }
        dialog = AlertDialog.Builder(activity).setView(scroll, 0, 0, 0, 0).create()
        menuDialog = dialog
        dialog.setOnDismissListener { menuDialog = null; controller.releaseAll() }
        dialog.show()
        dialog.window?.apply {
            setBackgroundDrawableResource(android.R.color.transparent)
            setGravity(Gravity.END or Gravity.TOP)
            setLayout(minOf(340.dp, (activity.resources.displayMetrics.widthPixels * 0.74f).toInt()),
                ViewGroup.LayoutParams.MATCH_PARENT)
        }
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
        const val MENU_EXIT = 4
        const val HUD_TOP_FRACTION = 0.13f
    }
}
