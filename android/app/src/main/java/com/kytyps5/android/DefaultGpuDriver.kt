package com.kytyps5.android

import android.content.Context
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.GLES20
import android.util.Log
import java.io.IOException

/** A driver package shipped inside the APK (assets/), fetched and checksum-pinned by the Gradle build. */
data class BundledDriver(
    val assetName: String,
    /** Bump when the bundled package changes so the one-time default selection runs again. */
    val version: Int,
    private val supportedModels: IntRange,
) {
    fun supports(adrenoModel: Int?): Boolean = adrenoModel != null && adrenoModel in supportedModels

    companion object {
        /** StevenMXZ Turnip Gen8 V38 Patched (WinNative/Vauzi fixes), Adreno 8xx. */
        val GEN8_V38_PATCHED = BundledDriver(
            assetName = "gpu_drivers/turnip-gen8-v38-patched.zip",
            version = 2,
            supportedModels = 800..899,
        )
        // Keep the public name used by earlier launcher/tests; the package is now V38 Patched.
        val MAINLINE_TURNIP = GEN8_V38_PATCHED
    }
}

/** Identifies the Adreno GPU from the GL renderer string ("Adreno (TM) 830"). */
object AdrenoGpu {
    private val MODEL = Regex("""Adreno\D{0,8}(\d{3})""")

    fun parseModel(renderer: String?): Int? =
        renderer?.let { MODEL.find(it)?.groupValues?.get(1)?.toIntOrNull() }

    /** Reads GL_RENDERER through a throwaway 1x1 pbuffer context; null if EGL is unavailable. */
    fun detectModel(): Int? = parseModel(readGlRenderer())

    private fun readGlRenderer(): String? {
        val display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        if (display == EGL14.EGL_NO_DISPLAY || !EGL14.eglInitialize(display, null, 0, null, 0)) return null
        try {
            val configs = arrayOfNulls<EGLConfig>(1)
            val count = IntArray(1)
            val configAttribs = intArrayOf(
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
                EGL14.EGL_NONE,
            )
            if (!EGL14.eglChooseConfig(display, configAttribs, 0, configs, 0, 1, count, 0) || count[0] == 0) return null
            val config = configs[0] ?: return null
            val context = EGL14.eglCreateContext(
                display, config, EGL14.EGL_NO_CONTEXT, intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0,
            )
            if (context == EGL14.EGL_NO_CONTEXT) return null
            val surface = EGL14.eglCreatePbufferSurface(
                display, config, intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE), 0,
            )
            try {
                if (surface == EGL14.EGL_NO_SURFACE || !EGL14.eglMakeCurrent(display, surface, surface, context)) return null
                return GLES20.glGetString(GLES20.GL_RENDERER)
            } finally {
                EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
                if (surface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, surface)
                EGL14.eglDestroyContext(display, context)
            }
        } finally {
            EGL14.eglTerminate(display)
        }
    }
}

/** Decides whether to install the bundled driver and make it the global default. */
object DefaultDriverPolicy {
    enum class Decision { NOTHING, INSTALL_ONLY, INSTALL_AND_SELECT }

    /** Matches only the previous APK-bundled mainline-v32 package (not user-imported builds). */
    internal fun isPreviousBundledTurnip(id: String): Boolean =
        id.contains("mainline", ignoreCase = true) && id.contains("v32", ignoreCase = true)

    /**
     * On first setup select the patched Gen8 driver for Adreno 8xx. When upgrading from the
     * original built-in v32, replace only that older bundled driver, never an unrelated
     * imported driver or a user's explicit choice of the system driver.
     */
    fun decide(adrenoModel: Int?, bundle: BundledDriver, appliedVersion: Int, currentGlobal: String): Decision = when {
        appliedVersion >= bundle.version || !bundle.supports(adrenoModel) -> Decision.NOTHING
        appliedVersion == 0 && currentGlobal == LauncherSettings.SYSTEM_DRIVER -> Decision.INSTALL_AND_SELECT
        appliedVersion == 1 && isPreviousBundledTurnip(currentGlobal) -> Decision.INSTALL_AND_SELECT
        else -> Decision.INSTALL_ONLY
    }
}

/** First-run setup: installs the bundled Turnip on Adreno 8xx and makes it the default driver. */
class DefaultGpuDriverSetup(
    private val context: Context,
    private val settings: LauncherSettings,
    private val store: GpuDriverStore,
    private val bundle: BundledDriver = BundledDriver.MAINLINE_TURNIP,
) {
    /** Returns a user-facing message when the default driver changed, else null. Safe off the main thread. */
    fun apply(): String? {
        if (settings.bundledDriverVersion >= bundle.version) return null
        val model = AdrenoGpu.detectModel()
        val decision = DefaultDriverPolicy.decide(model, bundle, settings.bundledDriverVersion, settings.gpuDriverId)
        if (decision == DefaultDriverPolicy.Decision.NOTHING) {
            settings.bundledDriverVersion = bundle.version
            return null
        }
        val driver = when (val result = importBundle()) {
            is DriverImportResult.Imported -> result.driver
            is DriverImportResult.Failure -> {
                // Leave the version unapplied so the next launch retries.
                Log.w(TAG, "Bundled driver not installed: ${result.message}")
                return null
            }
        }
        settings.bundledDriverVersion = bundle.version
        if (decision != DefaultDriverPolicy.Decision.INSTALL_AND_SELECT) return null
        settings.gpuDriverId = driver.id
        return "Adreno $model detected: using ${driver.label} by default (change it under GPU driver)."
    }

    private fun importBundle(): DriverImportResult = try {
        context.assets.open(bundle.assetName).use(store::import)
    } catch (e: IOException) {
        DriverImportResult.Failure("${bundle.assetName} is not in this build (${e.message})")
    }

    private companion object {
        const val TAG = "KytyDriver"
    }
}
