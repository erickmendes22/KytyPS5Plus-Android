package com.kytyps5.android

import android.os.Bundle
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import com.kytyps5.android.presets.GamePresets
import com.kytyps5.android.hud.HudMode
import com.kytyps5.android.presets.PresetLaunchArgs
import org.libsdl.app.SDLActivity

/**
 * Fullscreen, landscape host for the emulator. SDL3 owns the surface, input and the
 * native main thread; this class only supplies the library name and command-line arguments.
 */
class GameActivity : SDLActivity() {

    private var overlays: GameOverlays? = null

    /** Launch options of the title's presets that are on (set in onCreate, before SDL asks for the arguments). */
    private var presetLaunch = PresetLaunchArgs.Plan(emptyList(), emptyList())

    /** SDL loads these in order in onCreate: shared SDL3 first, then the emulator. */
    override fun getLibraries(): Array<String> = arrayOf("SDL3", "main")

    /**
     * Command line for the emulator, placed on the launching Intent by MainActivity. The app's
     * private files directory is passed first as --data-dir so logs, shader dumps and caches land
     * in app storage instead of the (unwritable) process directory. The launch options of the
     * game's presets that are on come last (PresetLaunchArgs).
     */
    override fun getArguments(): Array<String> {
        val launchArgs = intent?.getStringArrayExtra(LaunchArgs.EXTRA_GAME_ARGS) ?: emptyArray()
        return PresetLaunchArgs.apply(arrayOf(LaunchArgs.FLAG_DATA_DIR, filesDir.absolutePath) + launchArgs, presetLaunch)
    }

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
        // Persist the last launch even if SDL or the native runtime aborts before opening its log.
        Diagnostics.saveLaunch(
            this,
            intent?.getStringExtra(LaunchArgs.EXTRA_GAME_PATH) ?: "(unknown)",
            intent?.getStringArrayExtra(LaunchArgs.EXTRA_GAME_ARGS) ?: emptyArray(),
        )
        // Before super.onCreate: SDL loads the emulator (and Mesa reads TU_DEBUG etc.) from there on.
        GameEnvironment.apply(intent?.getStringArrayExtra(LaunchArgs.EXTRA_ENV))
        // Also before super.onCreate: the game's config presets must be in place when it boots.
        // The path comes from the launch args too, so adb launches (tools/kyrun.sh) get them as well.
        presetLaunch = GamePresets(this, LauncherSettings(this)).prepareLaunch(
            intent?.getStringExtra(LaunchArgs.EXTRA_GAME_PATH)
                ?: intent?.getStringArrayExtra(LaunchArgs.EXTRA_GAME_ARGS)?.let(LaunchArgs::gamePath),
        )
        super.onCreate(savedInstanceState)
        enterImmersiveMode()
        if (!mBrokenLibraries) installOverlays()
    }

    private fun installOverlays() {
        val launch = intent ?: return
        val created = GameOverlays(
            activity = this,
            gamePath = launch.getStringExtra(LaunchArgs.EXTRA_GAME_PATH),
            touchControls = launch.getBooleanExtra(LaunchArgs.EXTRA_TOUCH_CONTROLS, true),
            hud = launch.getBooleanExtra(LaunchArgs.EXTRA_HUD, false),
            hudMode = HudMode.fromStorage(launch.getStringExtra(LaunchArgs.EXTRA_HUD_MODE)),
        )
        addContentView(
            created.root,
            ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT),
        )
        overlays = created
    }

    override fun onResume() {
        super.onResume()
        overlays?.onResume()
    }

    override fun onPause() {
        overlays?.onPause()
        super.onPause()
    }

    /** SDL_SetWindowTitle lands here; the emulator puts its frame counter and FPS in the title. */
    override fun onTitleChanged(title: CharSequence?, color: Int) {
        super.onTitleChanged(title, color)
        overlays?.onTitle(title)
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
