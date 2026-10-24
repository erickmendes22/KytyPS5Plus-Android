package com.kytyps5.android.hud

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
import android.widget.LinearLayout
import android.view.Gravity
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
    private var mode: HudMode = HudMode.COMPACT
    private var previousCpu: Pair<Long, Long>? = null

    @Volatile
    private var frames: FrameStats? = null

    @Volatile
    private var framesAtMs = 0L

    private val title = TextView(context).apply {
        text = "●  KYTY  /  LIVE"
        textSize = 9f
        letterSpacing = 0.13f
        setTextColor(Color.rgb(113, 219, 255))
        setTypeface(typeface, Typeface.BOLD)
    }
    private val measurements = TextView(context).apply {
        typeface = Typeface.MONOSPACE
        textSize = 11f
        setTextColor(Color.WHITE)
        includeFontPadding = false
        setLineSpacing(1f * resources.displayMetrics.density, 1f)
        text = HudFormatter.lines(HudSample(), mode).joinToString("\n")
    }
    val view: LinearLayout = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        gravity = Gravity.START
        val d = resources.displayMetrics.density
        val horizontal = (10 * d).toInt()
        val vertical = (7 * d).toInt()
        setPadding(horizontal, vertical, horizontal, vertical)
        background = GradientDrawable().apply {
            setColor(Color.argb(220, 12, 27, 48))
            cornerRadius = 11f * d
            setStroke((1 * d).coerceAtLeast(1f).toInt(), Color.argb(205, 54, 160, 224))
        }
        addView(title)
        addView(measurements)
    }

    /** Changes the visual density without restarting the GPU or the emulator. */
    fun setMode(next: HudMode) {
        mode = next
        main.post {
            title.visibility = if (next == HudMode.MINIMAL) android.view.View.GONE else android.view.View.VISIBLE
            measurements.textSize = if (next == HudMode.MINIMAL) 17f else 11f
        }
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
                    val activeMode = mode
                    val lines = HudFormatter.lines(sample(activeMode), activeMode).joinToString("\n")
                    main.post { if (mode == activeMode) measurements.text = lines }
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

    private fun sample(activeMode: HudMode): HudSample {
        val frame = frames.takeIf { SystemClock.elapsedRealtime() - framesAtMs < STALE_FPS_MS }
        if (activeMode == HudMode.MINIMAL) return HudSample(frames = frame)
        val battery = if (activeMode == HudMode.FULL)
            appContext.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED)) else null
        val level = battery?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = battery?.getIntExtra(BatteryManager.EXTRA_SCALE, 100) ?: 100
        val temp = battery?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE) ?: Int.MIN_VALUE
        val status = battery?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
        return HudSample(
            // The title stops updating while the emulator is paused or stalled: show "FPS --" then.
            frames = frame,
            cpuTempC = sensors.cpuTempC(),
            gpuTempC = sensors.gpuTempC(),
            gpuLoadPercent = sensors.gpuLoadPercent(),
            cpuLoadPercent = cpuLoadPercent(),
            memory = RealSensorFiles.read(PROC_MEMINFO)?.let(MemInfo::parse),
            appRssMiB = RealSensorFiles.read(PROC_STATUS)?.let(SysfsSensors::parseVmRssMiB),
            batteryPercent = if (level >= 0 && scale > 0) level * 100 / scale else null,
            batteryTempC = if (temp != Int.MIN_VALUE) temp / 10f else null,
            batteryCharging = status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL,
        )
    }

    /** Percentage of non-idle CPU time across all cores since the previous refresh. */
    private fun cpuLoadPercent(): Int? {
        val line = RealSensorFiles.read("/proc/stat")?.lineSequence()?.firstOrNull() ?: return null
        val values = line.split(Regex("\\s+")).drop(1).mapNotNull(String::toLongOrNull)
        if (values.size < 4) return null
        val total = values.sum()
        val idle = values[3] + (values.getOrNull(4) ?: 0L)
        val previous = previousCpu
        previousCpu = total to idle
        if (previous == null) return null
        val delta = total - previous.first
        return if (delta > 0L) (((delta - (idle - previous.second)) * 100) / delta).toInt().coerceIn(0, 100)
            else null
    }

    private companion object {
        const val TAG = "KytyHud"
        const val INTERVAL_MS = 1000L
        const val STALE_FPS_MS = 3000L
        val PROC_STATUS = File("/proc/self/status").path
        val PROC_MEMINFO = File("/proc/meminfo").path
    }
}
