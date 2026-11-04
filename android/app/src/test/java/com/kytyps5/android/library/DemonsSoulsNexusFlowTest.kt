package com.kytyps5.android.library

import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DemonsSoulsNexusFlowTest {
    @Test
    fun fullGameNexusRouteIsReversibleWithoutChangingUserArguments() {
        val folder = Files.createTempDirectory("ds-nexus-flow").toFile()
        try {
            val config = File(folder, "PackageCmdLineArgs.txt")
            val previous = "+UserSetting=true\r\n+anotherCustomFlag=1"
            config.writeText(previous)
            val install = DemonsSoulsNexusFlow.configure(folder, true)
            assertTrue(install.message, install.ok)
            val patched = config.readText()
            assertTrue(patched.contains("+cp11_gameflowtype=FullGame"))
            assertTrue(patched.contains("+cp11_debug_forceOptionToSkipTutorial=true"))
            assertFalse(patched.contains("+cp11_gameflowtype=DemoLoopA"))
            assertTrue(DemonsSoulsNexusFlow.configure(folder, true).ok)
            assertEquals(patched, config.readText())
            val restore = DemonsSoulsNexusFlow.configure(folder, false)
            assertTrue(restore.message, restore.ok)
            assertEquals(previous, config.readText())
        } finally {
            folder.deleteRecursively()
        }
    }
    @Test
    fun rejectsTamperedManagedNexusConfiguration() {
        val folder = Files.createTempDirectory("ds-nexus-guard").toFile()
        try {
            val file = File(folder, "PackageCmdLineArgs.txt")
            val altered = DemonsSoulsNexusFlow.BEGIN + "\n+cp11_gameflowtype=Dummy\n" +
                DemonsSoulsNexusFlow.END
            file.writeText(altered)
            assertFalse(DemonsSoulsNexusFlow.configure(folder, false).ok)
            assertEquals(altered, file.readText())
        } finally {
            folder.deleteRecursively()
        }
    }
    @Test
    fun disabledDoesNotCreateGuestConfigFile() {
        val folder = Files.createTempDirectory("ds-nexus-off").toFile()
        try {
            assertTrue(DemonsSoulsNexusFlow.configure(folder, false).ok)
            assertFalse(File(folder, "PackageCmdLineArgs.txt").exists())
        } finally {
            folder.deleteRecursively()
        }
    }
}
