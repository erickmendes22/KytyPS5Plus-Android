package com.kytyps5.android.hud

import java.io.File
import java.io.IOException

/** Frame counter and FPS the emulator puts in its window title once per second (window.cpp UpdateTitle). */
data class FrameStats(val frame: Long, val fps: Double) {
    val frameTimeMs: Double? get() = if (fps > 0.0) 1000.0 / fps else null

    companion object {
        private val PATTERN = Regex("""frame:\s*(\d+),\s*fps:\s*([0-9]+(?:\.[0-9]+)?)""")

        /** Parses "... frame: 1234, fps: 59" from the SDL window title (Activity.setTitle on Android). */
        fun fromTitle(title: CharSequence?): FrameStats? {
            val match = title?.let { PATTERN.find(it) } ?: return null
            return FrameStats(match.groupValues[1].toLong(), match.groupValues[2].toDouble())
        }
    }
}

/**
 * Physical RAM and swap from /proc/meminfo, all in kB (the file's own unit). Used RAM is
 * MemTotal - MemAvailable, so it can never exceed the total; zram/swap is reported separately
 * instead of being mixed into RAM.
 */
data class MemInfo(val totalKb: Long, val availableKb: Long, val swapTotalKb: Long, val swapFreeKb: Long) {
    val usedKb: Long get() = (totalKb - availableKb).coerceIn(0L, totalKb)
    val swapUsedKb: Long get() = (swapTotalKb - swapFreeKb).coerceIn(0L, swapTotalKb)

    companion object {
        private val LINE = Regex("""^(\w+):\s+(\d+)\s*kB""", RegexOption.MULTILINE)

        /** Null when MemTotal is missing; MemAvailable falls back to MemFree + Buffers + Cached (pre-3.14 kernels). */
        fun parse(text: String): MemInfo? {
            val values = LINE.findAll(text).associate { it.groupValues[1] to it.groupValues[2].toLong() }
            val total = values["MemTotal"]?.takeIf { it > 0 } ?: return null
            val available = values["MemAvailable"]
                ?: ((values["MemFree"] ?: 0L) + (values["Buffers"] ?: 0L) + (values["Cached"] ?: 0L))
            return MemInfo(
                totalKb = total,
                availableKb = available.coerceIn(0L, total),
                swapTotalKb = values["SwapTotal"] ?: 0L,
                swapFreeKb = values["SwapFree"] ?: 0L,
            )
        }
    }
}

/** Filesystem access for [SysfsSensors]; the real one reads files, tests use a map. */
interface SensorFiles {
    fun read(path: String): String?
    fun list(dir: String): List<String>
}

object RealSensorFiles : SensorFiles {
    override fun read(path: String): String? = try {
        File(path).takeIf { it.canRead() }?.readText()?.trim()
    } catch (e: IOException) {
        null
    } catch (e: SecurityException) {
        null
    }

    override fun list(dir: String): List<String> = File(dir).list()?.sorted() ?: emptyList()
}

/**
 * GPU load and CPU/GPU temperatures from kgsl and thermal-zone sysfs nodes (research/frontends.md).
 * Every source is optional: Android's SELinux policy hides most of these from apps on many devices,
 * so a missing or unreadable node yields null and the HUD omits that line.
 */
class SysfsSensors(private val files: SensorFiles = RealSensorFiles) {

    private val zoneTypes: Map<String, String> by lazy {
        files.list(THERMAL_DIR)
            .filter { it.startsWith("thermal_zone") }
            .mapNotNull { zone -> files.read("$THERMAL_DIR/$zone/type")?.let { zone to it.lowercase() } }
            .toMap()
    }

    /** GPU busy percentage 0..100, or null when no readable source exists. */
    fun gpuLoadPercent(): Int? {
        files.read("$KGSL/gpu_busy_percentage")?.let { parseLeadingInt(it) }?.let { return it.coerceIn(0, 100) }
        files.read("$KGSL/gpubusy")?.let(::parseGpuBusy)?.let { return it }
        return null
    }

    fun gpuTempC(): Float? =
        files.read("$KGSL/temp")?.let(::parseTemperature)
            ?: maxZoneTemp { it.startsWith("gpu") || "kgsl" in it }

    fun cpuTempC(): Float? = maxZoneTemp { it.startsWith("cpu") || it.startsWith("cpuss") }

    /** Which sources answered, for logging on the device (readability differs per ROM). */
    fun describe(): String = buildString {
        append("gpuLoad=").append(gpuLoadPercent())
        append(" gpuTemp=").append(gpuTempC())
        append(" cpuTemp=").append(cpuTempC())
        append(" zones=").append(zoneTypes.size)
    }

    private fun maxZoneTemp(match: (String) -> Boolean): Float? =
        zoneTypes.filterValues(match).keys
            .mapNotNull { zone -> files.read("$THERMAL_DIR/$zone/temp")?.let(::parseTemperature) }
            .maxOrNull()

    companion object {
        private const val KGSL = "/sys/class/kgsl/kgsl-3d0"
        private const val THERMAL_DIR = "/sys/class/thermal"
        private const val MIN_PLAUSIBLE_C = -40f
        private const val MAX_PLAUSIBLE_C = 150f

        fun parseLeadingInt(text: String): Int? = Regex("""^\s*(\d+)""").find(text)?.groupValues?.get(1)?.toIntOrNull()

        /** kgsl "gpubusy": "<busy> <total>" cycle counts for the last sample window. */
        fun parseGpuBusy(text: String): Int? {
            val parts = text.trim().split(Regex("\\s+")).mapNotNull { it.toLongOrNull() }
            if (parts.size < 2 || parts[1] <= 0L) return null
            return (parts[0] * 100 / parts[1]).toInt().coerceIn(0, 100)
        }

        /** Thermal nodes report millidegrees, decidegrees or degrees depending on the driver. */
        fun parseTemperature(text: String): Float? {
            val raw = text.trim().toFloatOrNull() ?: return null
            val celsius = when {
                raw >= 1000f || raw <= -1000f -> raw / 1000f
                raw >= 200f -> raw / 10f
                else -> raw
            }
            return celsius.takeIf { it in MIN_PLAUSIBLE_C..MAX_PLAUSIBLE_C }
        }

        /** Resident set size in MiB from /proc/self/status ("VmRSS:  123456 kB"). */
        fun parseVmRssMiB(status: String): Int? =
            Regex("""VmRSS:\s+(\d+)\s+kB""").find(status)?.groupValues?.get(1)?.toLongOrNull()?.let { (it / 1024).toInt() }
    }
}
