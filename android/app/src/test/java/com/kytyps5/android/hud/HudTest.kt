package com.kytyps5.android.hud

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class HudTest {

    private class MapFiles(private val files: Map<String, String>) : SensorFiles {
        override fun read(path: String): String? = files[path]
        override fun list(dir: String): List<String> =
            files.keys.filter { it.startsWith("$dir/") }.map { it.removePrefix("$dir/").substringBefore('/') }.distinct().sorted()
    }

    @Test
    fun parsesFpsFromTheEmulatorTitle() {
        val stats = FrameStats.fromTitle("[0.3 | Release] ASTRO BOT, PPSA21567, 01.019 [Adreno (TM) 830] [Oryon], frame: 4521, fps: 30")!!
        assertEquals(4521L, stats.frame)
        assertEquals(30.0, stats.fps, 0.0)
        assertEquals(33.33, stats.frameTimeMs!!, 0.01)
        assertNull(FrameStats.fromTitle("KytyPS5"))
        assertNull(FrameStats(1, 0.0).frameTimeMs)
    }

    @Test
    fun gpuLoadPrefersPercentageNodeThenBusyCounters() {
        val pct = SysfsSensors(MapFiles(mapOf("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage" to "47 %")))
        assertEquals(47, pct.gpuLoadPercent())
        val busy = SysfsSensors(MapFiles(mapOf("/sys/class/kgsl/kgsl-3d0/gpubusy" to "  250   1000")))
        assertEquals(25, busy.gpuLoadPercent())
        assertNull(SysfsSensors(MapFiles(emptyMap())).gpuLoadPercent())
        assertNull(SysfsSensors.parseGpuBusy("5 0"))
    }

    @Test
    fun temperaturesComeFromKgslOrMatchingThermalZones() {
        val files = MapFiles(
            mapOf(
                "/sys/class/thermal/thermal_zone0/type" to "cpu-0-0-0",
                "/sys/class/thermal/thermal_zone0/temp" to "51000",
                "/sys/class/thermal/thermal_zone1/type" to "cpu-1-0-0",
                "/sys/class/thermal/thermal_zone1/temp" to "63500",
                "/sys/class/thermal/thermal_zone2/type" to "gpuss-0",
                "/sys/class/thermal/thermal_zone2/temp" to "58000",
                "/sys/class/thermal/thermal_zone3/type" to "battery",
                "/sys/class/thermal/thermal_zone3/temp" to "31000",
            ),
        )
        val sensors = SysfsSensors(files)
        assertEquals(63.5f, sensors.cpuTempC()!!, 1e-3f)
        assertEquals(58f, sensors.gpuTempC()!!, 1e-3f)
        val kgsl = SysfsSensors(MapFiles(mapOf("/sys/class/kgsl/kgsl-3d0/temp" to "61000")))
        assertEquals(61f, kgsl.gpuTempC()!!, 1e-3f)
        assertNull(SysfsSensors(MapFiles(emptyMap())).cpuTempC())
    }

    @Test
    fun temperatureUnitsAreNormalisedAndImplausibleValuesDropped() {
        assertEquals(45f, SysfsSensors.parseTemperature("45000")!!, 1e-3f)
        assertEquals(45f, SysfsSensors.parseTemperature("450")!!, 1e-3f)
        assertEquals(45f, SysfsSensors.parseTemperature("45")!!, 1e-3f)
        assertNull(SysfsSensors.parseTemperature("999999"))
        assertNull(SysfsSensors.parseTemperature("n/a"))
    }

    /** Shape of the OnePlus Pad 3's /proc/meminfo: 16 GB RAM plus a large zram swap. */
    private val padMeminfo = """
        MemTotal:       15833884 kB
        MemFree:          402116 kB
        MemAvailable:    6123456 kB
        Buffers:            2048 kB
        Cached:          5700000 kB
        SwapCached:       120000 kB
        SwapTotal:      12582908 kB
        SwapFree:        4194304 kB
    """.trimIndent()

    @Test
    fun meminfoUsedIsTotalMinusAvailableAndSwapIsSeparate() {
        val mem = MemInfo.parse(padMeminfo)!!
        assertEquals(15833884L - 6123456L, mem.usedKb)
        assertEquals(12582908L - 4194304L, mem.swapUsedKb)
        val lines = HudFormatter.lines(HudSample(memory = mem))
        assertEquals(listOf("FPS --", "RAM 9.3/15.1 GB", "SWAP 8.0/12.0 GB"), lines)
    }

    @Test
    fun usedRamNeverExceedsTotal() {
        val mem = MemInfo.parse("MemTotal: 1000 kB\nMemAvailable: 5000 kB\nSwapTotal: 0 kB")!!
        assertEquals(0L, mem.usedKb)
        val bogus = MemInfo(totalKb = 1000, availableKb = -50, swapTotalKb = 10, swapFreeKb = 99)
        assertEquals(1000L, bogus.usedKb)
        assertEquals(0L, bogus.swapUsedKb)
        assertEquals(listOf("FPS --", "RAM 0.0/0.0 GB"), HudFormatter.lines(HudSample(memory = mem)))
    }

    @Test
    fun meminfoWithoutMemAvailableFallsBackAndGarbageIsNull() {
        val old = MemInfo.parse("MemTotal: 4000 kB\nMemFree: 1000 kB\nBuffers: 500 kB\nCached: 500 kB")!!
        assertEquals(2000L, old.usedKb)
        assertNull(MemInfo.parse("nothing here"))
    }

    @Test
    fun parsesVmRss() {
        assertEquals(2048, SysfsSensors.parseVmRssMiB("Name:\tmain\nVmRSS:\t 2097152 kB\nThreads: 9"))
        assertNull(SysfsSensors.parseVmRssMiB("Name: x"))
    }

    @Test
    fun formatterOmitsUnknownLines() {
        assertEquals(listOf("FPS --"), HudFormatter.lines(HudSample()))
        val full = HudFormatter.lines(
            HudSample(
                frames = FrameStats(10, 60.0),
                cpuTempC = 70.4f,
                gpuTempC = 61f,
                gpuLoadPercent = 88,
                memory = MemInfo(totalKb = 16L shl 20, availableKb = 8L shl 20, swapTotalKb = 8L shl 20, swapFreeKb = 6L shl 20),
                appRssMiB = 3000,
                batteryPercent = 77,
                batteryTempC = 36f,
                batteryCharging = true,
            ),
        )
        assertEquals(
            listOf("FPS 60 16.7 ms", "CPU 70°C", "GPU 88% 61°C", "RAM 8.0/16.0 GB · app 3000 MB", "SWAP 2.0/8.0 GB", "BAT 77%⚡ 36°C"),
            full,
        )
    }
}
