package com.kytyps5.android.library

import java.io.File
import java.nio.file.Files
import org.junit.Assert.*
import org.junit.Test

class DemonsSoulsFogFlowTest {
    @Test fun reversibleFogSettingCoexistsWithOtherGameArguments() {
        val root = Files.createTempDirectory("ds-fog").toFile()
        try {
            val args = File(root, "PackageCmdLineArgs.txt")
            val original = "+cp11_gameflowtype=FullGame\n+userSetting=1\n"
            args.writeText(original)
            assertTrue(DemonsSoulsFogFlow.configure(root, true).ok)
            assertTrue(args.readText().contains("+fog_enable=false"))
            assertTrue(args.readText().contains("+userSetting=1"))
            assertFalse(DemonsSoulsFogFlow.configure(root, true).changed)
            assertTrue(DemonsSoulsFogFlow.configure(root, false).ok)
            assertEquals(original, args.readText())
        } finally { root.deleteRecursively() }
    }

    @Test fun refusesToOverwriteUserModifiedManagedBlock() {
        val root = Files.createTempDirectory("ds-fog-tamper").toFile()
        try {
            val file = File(root, "PackageCmdLineArgs.txt")
            assertTrue(DemonsSoulsFogFlow.configure(root, true).ok)
            file.writeText(file.readText().replace("+fog_enable=false", "+fog_enable=true"))
            assertFalse(DemonsSoulsFogFlow.configure(root, false).ok)
            assertTrue(file.readText().contains("+fog_enable=true"))
        } finally { root.deleteRecursively() }
    }
}
