package com.kytyps5.android

import android.os.Bundle
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import org.libsdl.app.SDLActivity

/**
 * Fullscreen, landscape host for the emulator. SDL3 owns the surface, input and the
 * native main thread; this class only supplies the library name and command-line arguments.
 */
class GameActivity : SDLActivity() {

    /** SDL loads these in order in onCreate: shared SDL3 first, then the emulator. */
    override fun getLibraries(): Array<String> = arrayOf("SDL3", "main")

    /** Command line for the emulator, placed on the launching Intent by MainActivity. */
    override fun getArguments(): Array<String> =
        intent?.getStringArrayExtra(LaunchArgs.EXTRA_GAME_ARGS) ?: emptyArray()

    /**
     * Runs inside SDL's onCreate before any native call. Throwing here makes SDL show its
     * broken-library dialog instead of crashing, and the message is our own.
     */
    override fun loadLibraries() {
        if (!NativeLibrary.isInstalled(this)) {
            throw UnsatisfiedLinkError(NativeLibrary.MISSING_MESSAGE)
        }
        super.loadLibraries()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enterImmersiveMode()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) enterImmersiveMode()
    }

    private fun enterImmersiveMode() {
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.attributes = window.attributes.apply {
            layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }
        window.insetsController?.let { controller ->
            controller.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            controller.hide(WindowInsets.Type.systemBars())
        }
    }
}
