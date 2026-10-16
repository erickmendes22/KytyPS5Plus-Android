package com.kytyps5.android.hud

import java.util.Locale

/** One HUD refresh; null fields are unknown on this device and are left out. */
data class HudSample(
    val frames: FrameStats? = null,
    val cpuTempC: Float? = null,
    val gpuTempC: Float? = null,
    val gpuLoadPercent: Int? = null,
    val memory: MemInfo? = null,
    val appRssMiB: Int? = null,
    val batteryPercent: Int? = null,
    val batteryTempC: Float? = null,
    val batteryCharging: Boolean? = null,
)

/** Formats a [HudSample] into compact lines (GameNative/Winlator-style HUD, own implementation). */
object HudFormatter {

    fun lines(sample: HudSample): List<String> = listOfNotNull(
        sample.frames?.let { f ->
            val ms = f.frameTimeMs?.let { String.format(Locale.US, " %.1f ms", it) } ?: ""
            String.format(Locale.US, "FPS %.0f%s", f.fps, ms)
        } ?: "FPS --",
        temps(sample),
        sample.gpuLoadPercent?.let { "GPU $it%" + (sample.gpuTempC?.let { t -> " ${deg(t)}" } ?: "") }
            ?: sample.gpuTempC?.let { "GPU ${deg(it)}" },
        ram(sample),
        swap(sample.memory),
        battery(sample),
    )

    private fun temps(sample: HudSample): String? = sample.cpuTempC?.let { "CPU ${deg(it)}" }

    private fun ram(sample: HudSample): String? {
        val system = sample.memory?.let { "RAM ${gib(it.usedKb)}/${gib(it.totalKb)} GB" }
        val app = sample.appRssMiB?.let { "app ${it} MB" }
        return listOfNotNull(system, app).joinToString(" · ").ifEmpty { null }
    }

    private fun swap(memory: MemInfo?): String? =
        memory?.takeIf { it.swapTotalKb > 0 }?.let { "SWAP ${gib(it.swapUsedKb)}/${gib(it.swapTotalKb)} GB" }

    /** kB -> GiB with one decimal; RAM and swap use the same unit as their totals. */
    private fun gib(kb: Long): String = String.format(Locale.US, "%.1f", kb / 1_048_576.0)

    private fun battery(sample: HudSample): String? {
        val percent = sample.batteryPercent ?: return null
        val charging = if (sample.batteryCharging == true) "⚡" else ""
        val temp = sample.batteryTempC?.let { " ${deg(it)}" } ?: ""
        return "BAT $percent%$charging$temp"
    }

    private fun deg(celsius: Float): String = String.format(Locale.US, "%.0f°C", celsius)
}
