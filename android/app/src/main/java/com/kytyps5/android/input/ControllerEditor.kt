package com.kytyps5.android.input

import android.app.Activity
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView

/**
 * In-game layout editor bar (the overlay-edit flow of Vita3K's OverlayLayoutEditorDialog and RPCSX's
 * OverlayEditActivity, as a toolbar over the running game): drag controls on the overlay, resize,
 * hide/show, change opacity, then save for this game or as the default for all games.
 */
class ControllerEditor(
    private val activity: Activity,
    private val overlay: TouchControllerView,
    private val store: OverlayProfileStore,
    private val gamePath: String?,
    private val onClosed: () -> Unit,
) {
    private val density = activity.resources.displayMetrics.density
    private val Int.dp: Int get() = (this * density).toInt()
    private var original: OverlayLayout = overlay.layout
    private val status = TextView(activity).apply {
        setTextColor(Color.WHITE)
        textSize = 13f
        setPadding(8.dp, 0, 12.dp, 0)
    }

    val view: View = HorizontalScrollView(activity).apply {
        visibility = View.GONE
        background = GradientDrawable().apply {
            setColor(Color.argb(235, 17, 30, 49))
            cornerRadius = 12f * density
        }
        addView(
            LinearLayout(activity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(8.dp, 4.dp, 8.dp, 4.dp)
                addView(status)
                addView(button("−") { resize(-SCALE_STEP) })
                addView(button("+") { resize(SCALE_STEP) })
                addView(button("Hide/Show") { toggleVisible() })
                addView(button("Opacity −") { opacity(-OPACITY_STEP) })
                addView(button("Opacity +") { opacity(OPACITY_STEP) })
                addView(button("Reset") { reset() })
                if (gamePath != null) addView(button("Save for game") { save(gamePath) })
                addView(button("Save default") { save(null) })
                addView(button("Cancel") { cancel() })
            },
        )
    }

    init {
        overlay.onSelectionChanged = { updateStatus() }
        overlay.onLayoutEdited = { updateStatus() }
    }

    fun open() {
        original = overlay.layout
        overlay.visibility = View.VISIBLE
        overlay.editMode = true
        view.visibility = View.VISIBLE
        updateStatus()
    }

    val isOpen: Boolean get() = view.visibility == View.VISIBLE

    private fun resize(step: Float) = editSelected { it.copy(scale = it.scale + step) }

    private fun toggleVisible() = editSelected { it.copy(visible = !it.visible) }

    private fun editSelected(change: (ControlPlacement) -> ControlPlacement) {
        val control = overlay.selected ?: return updateStatus("Tap a control first.")
        overlay.layout = overlay.layout.with(control, change(overlay.layout.placement(control)))
        updateStatus()
    }

    private fun opacity(step: Float) {
        overlay.layout = overlay.layout.withOpacity(overlay.layout.opacity + step)
        updateStatus()
    }

    private fun reset() {
        overlay.layout = OverlayLayout.DEFAULT.withOpacity(overlay.layout.opacity)
        updateStatus("Reset to the default layout (not saved yet).")
    }

    private fun save(forGame: String?) {
        val ok = store.save(overlay.layout, forGame)
        close()
        if (!ok) android.widget.Toast.makeText(activity, "Could not save the controller layout.", android.widget.Toast.LENGTH_LONG).show()
    }

    private fun cancel() {
        overlay.layout = original
        close()
    }

    private fun close() {
        overlay.editMode = false
        view.visibility = View.GONE
        onClosed()
    }

    private fun updateStatus(message: String? = null) {
        val control = overlay.selected
        val selection = control?.let {
            val p = overlay.layout.placement(it)
            "${it.label} ${(p.scale * 100).toInt()}%${if (p.visible) "" else " (hidden)"}"
        } ?: "Drag a control"
        status.text = message ?: "$selection · opacity ${(overlay.layout.opacity * 100).toInt()}%"
    }

    private fun button(label: String, onClick: () -> Unit) = Button(activity).apply {
        text = label
        isAllCaps = false
        minWidth = 0
        minimumWidth = 0
        setOnClickListener { onClick() }
    }

    private companion object {
        const val SCALE_STEP = 0.1f
        const val OPACITY_STEP = 0.1f
    }
}
