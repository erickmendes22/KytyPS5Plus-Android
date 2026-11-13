package com.kytyps5.android.library

import java.io.File
import java.nio.file.Files
import org.junit.Assert.*
import org.junit.Test

class DemonsSoulsDisplayModeTest {
    @Test fun replacesAllKnownModesAndRestoresOriginals() {
        val root = Files.createTempDirectory("ds-1080").toFile()
        try {
            val misc = File(root, "misc").apply { mkdirs() }
            val configs = listOf(
                "renderconfig_ps5_high.txt" to "+r_Wants4K = WantsSuperHigh\r\n+keep=1\r\n",
                "renderconfig_ps5_perf.txt" to "+r_Wants4K=WantsHigh\n+keep=2\n",
                "renderconfig_ps5_charactercreator.txt" to "+r_Wants4K = WantsSuperHigh\n"
            )
            configs.forEach { (name, content) -> File(misc, name).writeText(content) }
            val enabled = DemonsSoulsDisplayMode.configure(root, true)
            assertTrue(enabled.message, enabled.ok)
            assertTrue(enabled.changed)
            configs.forEach { (name, content) ->
                val config = File(misc, name)
                assertTrue(config.readText().contains("WantsPerf"))
                assertFalse(config.readText().contains("WantsSuperHigh"))
                assertEquals(content, File(misc, name + ".kytyps5-original").readText())
            }
            assertFalse(DemonsSoulsDisplayMode.configure(root, true).changed)
            val disabled = DemonsSoulsDisplayMode.configure(root, false)
            assertTrue(disabled.message, disabled.ok)
            configs.forEach { (name, content) -> assertEquals(content, File(misc, name).readText()) }
            assertFalse(DemonsSoulsDisplayMode.configure(root, false).changed)
            assertTrue(DemonsSoulsDisplayMode.configure(root, true).ok)
        } finally { root.deleteRecursively() }
    }

    @Test fun neverOverwritesExternallyChangedFile() {
        val root = Files.createTempDirectory("ds-display-safety").toFile()
        try {
            val misc = File(root, "misc").apply { mkdirs() }
            val config = File(misc, "renderconfig_ps5_high.txt").apply {
                writeText("+r_Wants4K=WantsHigh\n")
            }
            assertTrue(DemonsSoulsDisplayMode.configure(root, true).ok)
            config.writeText("+r_Wants4K = WantsOff\n+userEdited=true")
            val refusal = DemonsSoulsDisplayMode.configure(root, false)
            assertFalse(refusal.ok)
            assertTrue(config.readText().contains("userEdited=true"))
            assertEquals("+r_Wants4K=WantsHigh\n",
                File(misc, config.name + ".kytyps5-original").readText())
        } finally { root.deleteRecursively() }
    }

    @Test fun missingAndUnrelatedFilesAreLeftUntouched() {
        val root = Files.createTempDirectory("ds-display-other").toFile()
        try {
            assertTrue(DemonsSoulsDisplayMode.configure(root, true).ok)
            val misc = File(root, "misc").apply { mkdirs() }
            val other = File(misc, "renderconfig_ps4_base.txt").apply {
                writeText("+r_Wants4K = WantsSuperHigh\n")
            }
            assertTrue(DemonsSoulsDisplayMode.configure(root, true).ok)
            assertEquals("+r_Wants4K = WantsSuperHigh\n", other.readText())
        } finally { root.deleteRecursively() }
    }
}
