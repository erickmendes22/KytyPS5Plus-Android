package com.kytyps5.android

import android.app.Activity
import android.app.AlertDialog
import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import com.kytyps5.android.drivers.DriverBrowserDialog
import java.io.IOException
import java.util.concurrent.ExecutorService

/**
 * Launcher UI for custom Vulkan drivers: shows the global choice, imports driver packages through
 * the system file picker, and offers per-game overrides. The chosen driver's directory reaches the
 * emulator as --gpu-driver (see [LaunchArgs.forGame]).
 */
class GpuDriverPanel(
    private val activity: Activity,
    private val settings: LauncherSettings,
    private val worker: ExecutorService,
    private val post: (Runnable) -> Unit,
    private val showMessage: (String) -> Unit,
    private val label: (String) -> TextView,
) {
    val store = GpuDriverStore(activity.filesDir)
    private val current: TextView = label("")
    private var onDriversChanged: () -> Unit = {}

    fun build(onChanged: () -> Unit): View {
        onDriversChanged = onChanged
        refresh()
        return LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(current, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            addView(button("Change…") { chooseGlobal() })
            addView(button("Download…") { openBrowser() })
            addView(button("Import zip…") { openPicker() })
            addView(button("Remove…") { chooseToRemove() })
        }
    }

    /** Driver directory for [gamePath] (per-game override, else global), or null for the system driver. */
    fun driverDirFor(gamePath: String): String? =
        GpuDriverChoice.resolve(settings.gpuDriverId, settings.gameGpuDriverOverride(gamePath)) {
            store.find(it) != null
        }?.let { store.find(it)?.directory?.absolutePath }

    /** Short label for a game row's driver button. */
    fun gameButtonText(gamePath: String): String {
        val override = settings.gameGpuDriverOverride(gamePath) ?: return "Driver: global"
        return "Driver: " + nameOf(override)
    }

    /** Per-game override dialog: follow global, system driver, or one imported driver. */
    fun chooseForGame(game: GameEntry, onChosen: () -> Unit) {
        val drivers = store.list()
        val labels = listOf("Use global (${nameOf(settings.gpuDriverId)})", SYSTEM_LABEL) + drivers.map { it.label }
        val values = listOf<String?>(null, LauncherSettings.SYSTEM_DRIVER) + drivers.map { it.id }
        val checked = values.indexOf(settings.gameGpuDriverOverride(game.path)).coerceAtLeast(0)
        AlertDialog.Builder(activity)
            .setTitle("GPU driver for ${game.title}")
            .setSingleChoiceItems(labels.toTypedArray(), checked) { dialog, which ->
                settings.setGameGpuDriverOverride(game.path, values[which])
                dialog.dismiss()
                onChosen()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    /** Import a package picked with ACTION_OPEN_DOCUMENT; called from onActivityResult. */
    fun onPicked(uri: Uri) {
        showMessage("Importing driver…")
        worker.execute {
            val result = try {
                activity.contentResolver.openInputStream(uri)?.use(store::import)
                    ?: DriverImportResult.Failure("Could not open the picked file.")
            } catch (e: IOException) {
                DriverImportResult.Failure("Could not open the picked file: ${e.message}")
            } catch (e: SecurityException) {
                DriverImportResult.Failure("No permission to read the picked file.")
            }
            post(Runnable {
                when (result) {
                    is DriverImportResult.Imported -> {
                        settings.gpuDriverId = result.driver.id
                        showMessage("Imported ${result.driver.label}; it is now the global GPU driver.")
                    }
                    is DriverImportResult.Failure -> showMessage(result.message)
                }
                refresh()
                onDriversChanged()
            })
        }
    }

    /** GPU model for driver recommendations, detected once via EGL (call off the main thread). */
    @Volatile
    private var adrenoModel: Int? = null
    @Volatile
    private var adrenoProbed = false

    private fun detectAdreno(): Int? {
        if (!adrenoProbed) {
            adrenoModel = AdrenoGpu.detectModel()
            adrenoProbed = true
        }
        return adrenoModel
    }

    private fun openBrowser() {
        DriverBrowserDialog(activity, store, ::detectAdreno) { driver, useNow ->
            if (useNow) settings.gpuDriverId = driver.id
            showMessage(if (useNow) "Installed ${driver.label}; it is now the global GPU driver." else "Installed ${driver.label}.")
            refresh()
            onDriversChanged()
        }.show()
    }

    private fun chooseGlobal() {
        val drivers = store.list()
        val labels = listOf(SYSTEM_LABEL) + drivers.map { it.label }
        val values = listOf(LauncherSettings.SYSTEM_DRIVER) + drivers.map { it.id }
        val checked = values.indexOf(settings.gpuDriverId).coerceAtLeast(0)
        AlertDialog.Builder(activity)
            .setTitle("GPU driver")
            .setSingleChoiceItems(labels.toTypedArray(), checked) { dialog, which ->
                settings.gpuDriverId = values[which]
                dialog.dismiss()
                refresh()
                onDriversChanged()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun chooseToRemove() {
        val drivers = store.list()
        if (drivers.isEmpty()) {
            showMessage("No imported GPU drivers.")
            return
        }
        AlertDialog.Builder(activity)
            .setTitle("Remove GPU driver")
            .setItems(drivers.map { it.label }.toTypedArray()) { _, which ->
                val driver = drivers[which]
                if (store.delete(driver.id)) {
                    if (settings.gpuDriverId == driver.id) settings.gpuDriverId = LauncherSettings.SYSTEM_DRIVER
                    showMessage("Removed ${driver.label}.")
                } else {
                    showMessage("Could not remove ${driver.label}.")
                }
                refresh()
                onDriversChanged()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun openPicker() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream"))
        }
        try {
            activity.startActivityForResult(intent, REQUEST_PICK_DRIVER)
        } catch (e: ActivityNotFoundException) {
            showMessage("No file picker is available on this device.")
        }
    }

    /** Re-reads the global choice, e.g. after the first-run default driver setup changed it. */
    fun refresh() {
        current.text = "GPU driver: " + nameOf(settings.gpuDriverId)
    }

    private fun nameOf(id: String): String =
        if (id == LauncherSettings.SYSTEM_DRIVER) SYSTEM_LABEL else store.find(id)?.label ?: "$SYSTEM_LABEL (missing: $id)"

    private fun button(text: String, onClick: () -> Unit) = Button(activity).apply {
        this.text = text
        setOnClickListener { onClick() }
    }

    companion object {
        const val REQUEST_PICK_DRIVER = 2
        private const val SYSTEM_LABEL = "System driver"
    }
}
