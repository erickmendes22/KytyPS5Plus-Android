package com.kytyps5.android.library

import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DemonsSoulsDemoFlowTest {
    @Test
    fun enablesSkipsAndRestoresOriginalBytes() {
        val folder = Files.createTempDirectory("kyty-ds-flow-").toFile()
        try {
            val cmd = File(folder, "PackageCmdLineArgs.txt")
            val original = "// user arg\r\n+foo=true"
            cmd.writeText(original)
            assertTrue(DemonsSoulsDemoFlow.configure(folder, true).ok)
            val modified = cmd.readText()
            assertTrue(modified.contains(DemonsSoulsDemoFlow.BEGIN))
            assertTrue(modified.contains("+cp11_gameflowtype=DemoLoopA"))
            assertTrue(modified.contains("+cp11_demoModeFile=\$/misc/demo_tutorial.txt"))
            assertTrue(DemonsSoulsDemoFlow.configure(folder, true).ok)
            assertEquals(modified, cmd.readText())
            assertTrue(DemonsSoulsDemoFlow.configure(folder, false).ok)
            assertEquals(original, cmd.readText())
        } finally { folder.deleteRecursively() }
    }

    @Test
    fun rejectsBrokenMarkersWithoutModifyingGameFile() {
        val folder = Files.createTempDirectory("kyty-ds-flow-").toFile()
        try {
            val cmd = File(folder, "PackageCmdLineArgs.txt")
            val original = DemonsSoulsDemoFlow.BEGIN + "\nuser entry"
            cmd.writeText(original)
            assertFalse(DemonsSoulsDemoFlow.configure(folder, true).ok)
            assertEquals(original, cmd.readText())
        } finally { folder.deleteRecursively() }
    }

    @Test
    fun disabledModeDoesNotCreateMissingFile() {
        val folder = Files.createTempDirectory("kyty-ds-flow-").toFile()
        try {
            assertTrue(DemonsSoulsDemoFlow.configure(folder, false).ok)
            assertFalse(File(folder, "PackageCmdLineArgs.txt").exists())
        } finally { folder.deleteRecursively() }
    }
}
