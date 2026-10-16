package com.kytyps5.android.hud

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.BatteryManager
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.widget.TextView
import java.io.File

/**
 * Toggleable performance overlay. FPS/frame time come from the emulator's window title (pushed by
 * [onTitle]); temperatures, GPU load, RAM and battery are sampled once a second on a background
 * thread and shown only when the device exposes them.
 */
class PerformanceHud(context: Context) {
    private val appContext = context.applicationContext
    private val sensors = SysfsSensors()
    private val main = Handler(Looper.getMainLooper())
    private var thread: HandlerThread? = null
    private var background: Handler? = null

    @Volatile
    private var frames: FrameStats? = null

    @Volatile
    private var framesAtMs = 0L

    val view: TextView = TextView(context).apply {
        typeface = Typeface.MONOSPACE
        textSize = 12f
        setTextColor(Color.WHITE)
        setShadowLayer(3f, 0f, 0f, Color.BLACK)
        val pad = (6 * resources.displayMetrics.density).toInt()
        setPadding(pad, pad / 2, pad, pad / 2)
        background = GradientDrawable().apply {
            setColor(Color.argb(140, 0, 0, 0))
            cornerRadius = 8f * resources.displayMetrics.density
        }
        text = HudFormatter.lines(HudSample()).joinToString("\n")
    }

    val isRunning: Boolean get() = thread != null

    fun onTitle(title: CharSequence?) {
        FrameStats.fromTitle(title)?.let {
            frames = it
            framesAtMs = SystemClock.elapsedRealtime()
        }
    }

    fun start() {
        if (thread != null) return
        val t = HandlerThread("KytyHud").apply { start() }
        thread = t
        background = Handler(t.looper).also { handler ->
            handler.post { Log.i(TAG, "HUD sensors: ${sensors.describe()}") }
            handler.post(object : Runnable {
                override fun run() {
                    val lines = HudFormatter.lines(sample()).joinToString("\n")
                    main.post { view.text = lines }
                    handler.postDelayed(this, INTERVAL_MS)
                }
            })
        }
    }

    fun stop() {
        thread?.quitSafely()
        thread = null
        background = null
    }

    private fun sample(): HudSample {
        val memory = ActivityManager.MemoryInfo().also {
            (appContext.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager).getMemoryInfo(it)
        }
        val battery = appContext.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val level = battery?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = battery?.getIntExtra(BatteryManager.EXTRA_SCALE, 100) ?: 100
        val temp = battery?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE) ?: Int.MIN_VALUE
        val status = battery?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
        return HudSample(
            // The title stops updating while the emulator is paused or stalled: show "FPS --" then.
            frames = frames.takeIf { SystemClock.elapsedRealtime() - framesAtMs < STALE_FPS_MS },
            cpuTempC = sensors.cpuTempC(),
            gpuTempC = sensors.gpuTempC(),
            gpuLoadPercent = sensors.gpuLoadPercent(),
            ramUsedMiB = ((memory.totalMem - memory.availMem) shr 20).toInt(),
            ramTotalMiB = (memory.totalMem shr 20).toInt(),
            appRssMiB = RealSensorFiles.read(PROC_STATUS)?.let(SysfsSensors::parseVmRssMiB),
            batteryPercent = if (level >= 0 && scale > 0) level * 100 / scale else null,
            batteryTempC = if (temp != Int.MIN_VALUE) temp / 10f else null,
            batteryCharging = status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL,
        )
    }

    private companion object {
        const val TAG = "KytyHud"
        const val INTERVAL_MS = 1000L
        const val STALE_FPS_MS = 3000L
        val PROC_STATUS = File("/proc/self/status").path
    }
}
