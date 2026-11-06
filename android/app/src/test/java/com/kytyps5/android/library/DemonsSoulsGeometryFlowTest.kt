package com.kytyps5.android.library

import java.io.File
import java.nio.file.Files
import org.junit.Assert.*
import org.junit.Test

class DemonsSoulsGeometryFlowTest {
    private val supported = "doIndexBufferCulling\u0000doSunShadowIndexBufferCulling\u0000"
    @Test fun geometryPathCoexistsWithBootOptionsAndPreservesUserSettings() {
        val folder = Files.createTempDirectory("ds-geometry").toFile()
        try {
            val elf = File(folder, "eboot.bin").apply { writeText(supported) }
            val originalElf = elf.readBytes()
            val config = File(folder, "PackageCmdLineArgs.txt")
            val original = "+UserSetting=á\r\n+cp11_gameflowtype=FullGame"
            config.writeText(original)
            assertTrue(DemonsSoulsNexusFlow.configure(folder, true).ok)
            val withNexus = config.readText()
            val first = DemonsSoulsGeometryFlow.configure(folder, "ppsa01341")
            assertTrue(first.message, first.ok)
            val after = config.readText()
            assertTrue(after.contains("+doIndexBufferCulling=0"))
            assertTrue(after.contains("+doSunShadowIndexBufferCulling=0"))
            assertTrue(after.contains("+cp11_gameflowtype=FullGame"))
            repeat(3) {
                assertTrue(DemonsSoulsNexusFlow.configure(folder, true).ok)
                assertTrue(DemonsSoulsGeometryFlow.configure(folder, "PPSA01341").ok)
                assertEquals(after, config.readText())
            }
            assertTrue(DemonsSoulsGeometryFlow.configure(folder, "PPSA01341", false).ok)
            assertEquals(withNexus, config.readText())
            assertTrue(DemonsSoulsNexusFlow.configure(folder, false).ok)
            assertEquals(original, config.readText())
            assertArrayEquals(originalElf, elf.readBytes())
        } finally { folder.deleteRecursively() }
    }
    @Test fun unsupportedExecutableAndOtherTitlesDoNotChangeFiles() {
        val folder = Files.createTempDirectory("ds-geometry-guard").toFile()
        try {
            val config = File(folder, "PackageCmdLineArgs.txt").apply { writeText("+User=true") }
            File(folder, "eboot.bin").writeText("doIndexBufferCulling\u0000")
            assertFalse(DemonsSoulsGeometryFlow.configure(folder, "PPSA01341").ok)
            assertEquals("+User=true", config.readText())
            File(folder, "eboot.bin").writeText(supported)
            for (title in listOf("PPSA03671", "PPSA24156", null)) {
                assertTrue(DemonsSoulsGeometryFlow.configure(folder, title).ok)
                assertEquals("+User=true", config.readText())
            }
        } finally { folder.deleteRecursively() }
    }
    @Test fun executableScanHandlesChunkBoundariesAndDecryptedPath() {
        val folder = Files.createTempDirectory("ds-geometry-boundary").toFile()
        try {
            val decrypted = File(folder, "decrypted").apply { mkdir() }
            val elf = File(decrypted, "eboot.bin")
            elf.writeBytes(ByteArray(64 * 1024 - 7) { 0x58 } + supported.toByteArray())
            assertTrue(DemonsSoulsGeometryFlow.supports(elf))
            assertTrue(DemonsSoulsGeometryFlow.configure(folder, "PPSA01341").ok)
        } finally { folder.deleteRecursively() }
    }
    @Test fun modifiedManagedBlockIsNeverOverwritten() {
        val folder = Files.createTempDirectory("ds-geometry-modified").toFile()
        try {
            File(folder, "eboot.bin").writeText(supported)
            val config = File(folder, "PackageCmdLineArgs.txt")
            val altered = DemonsSoulsGeometryFlow.BEGIN + "\n+doIndexBufferCulling=1\n" +
                DemonsSoulsGeometryFlow.END
            config.writeText(altered)
            assertFalse(DemonsSoulsGeometryFlow.configure(folder, "PPSA01341").ok)
            assertFalse(DemonsSoulsGeometryFlow.configure(folder, "PPSA01341", false).ok)
            assertEquals(altered, config.readText())
        } finally { folder.deleteRecursively() }
    }
}
