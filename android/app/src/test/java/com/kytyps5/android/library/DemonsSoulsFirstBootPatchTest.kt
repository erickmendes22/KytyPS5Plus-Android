package com.kytyps5.android.library

import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.file.Files
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DemonsSoulsFirstBootPatchTest {
    private val sites = listOf(
        0x7c0a4fL to "498b3fb8011f0afa488d3532e16d02488d55d0",
        0x7c9254L to "c7430c2d000000498b07483b45d8",
        0x7c930cL to "488d35e520b402b8011f0afa488d55cc488945cce8ab946100",
    )
    private val codeLength = 0x7cb000L

    private fun le64(number: Long): ByteArray =
        ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(number).array()
    private fun le16(number: Int): ByteArray =
        ByteBuffer.allocate(2).order(ByteOrder.LITTLE_ENDIAN).putShort(number.toShort()).array()
    private fun le32(number: Int): ByteArray =
        ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(number).array()

    /**
     * Synthetic, sparse PS5 SELF with the same segment geometry as the supplied
     * 43.6 MiB file: embedded ELF at 0x60 (two SELF entries), ELF p_offset=0x4000,
     * actual executable bytes at SELF physical offset 0xfe50.
     */
    private fun createExecutable(file: File, self: Boolean, valid: Boolean = true) {
        val elfStart = if (self) 0x60L else 0L
        val dataStart = if (self) 0xfe50L else 0x4000L
        RandomAccessFile(file, "rw").use { raf ->
            raf.setLength(dataStart + codeLength)
            if (self) {
                raf.seek(0)
                raf.write(byteArrayOf(0x4f, 0x15, 0x3d, 0x1d))
                raf.seek(0x18)
                raf.write(le16(2))
                raf.seek(0x40 + 8)
                raf.write(le64(dataStart))
                raf.write(le64(codeLength))
            }
            raf.seek(elfStart)
            raf.write(byteArrayOf(0x7f, 0x45, 0x4c, 0x46, 2, 1))
            raf.seek(elfStart + 0x20)
            raf.write(le64(0x40L)) // e_phoff
            raf.seek(elfStart + 0x36)
            raf.write(le16(56))
            raf.write(le16(1))
            raf.seek(elfStart + 0x40)
            raf.write(le32(1)) // PT_LOAD
            raf.write(le32(1)) // PF_X
            raf.write(le64(0x4000L)) // ELF p_offset is NOT physical SELF offset
            raf.write(le64(0)) // p_vaddr
            raf.write(le64(0)) // p_paddr
            raf.write(le64(codeLength)) // p_filesz
            raf.write(le64(codeLength)) // p_memsz
            raf.write(le64(0x4000))
            if (valid) {
                for ((va, signature) in sites) {
                    raf.seek(dataStart + va)
                    raf.write(signature.chunked(2).map { it.toInt(16).toByte() }.toByteArray())
                }
            }
        }
    }

    private fun verifyPatch(file: File, out: File, expectedFormat: String, expectedBase: String) {
        val result = DemonsSoulsFirstBootPatch.prepare(file.parentFile!!, out)
        assertNull(result.error)
        assertTrue(result.verifiedMapping!!.contains(expectedFormat))
        assertTrue(result.verifiedMapping!!.contains(expectedBase))
        val json = JSONObject(requireNotNull(result.path).readText())
        assertEquals("PPSA01341", json.getString("id"))
        assertEquals("01.007.000", json.getString("version"))
        assertEquals("eboot.bin", json.getString("process"))
        val patches = json.getJSONArray("mods")
        assertEquals(3, patches.length())
        assertEquals("9090909090",
            patches.getJSONObject(0).getJSONArray("memory").getJSONObject(0).getString("on").substring(46,56))
        assertTrue(patches.getJSONObject(1).getJSONArray("memory").getJSONObject(0)
            .getString("on").startsWith("c7430c1b000000"))
        assertEquals("9090909090",
            patches.getJSONObject(2).getJSONArray("memory").getJSONObject(0).getString("on").substring(40,50))
    }

    @Test
    fun ps5SelfUsesPhysicalOffsetNotElfPOffset() {
        val folder = Files.createTempDirectory("kyty-self-patch").toFile()
        try {
            val eboot = File(folder, "eboot.bin")
            createExecutable(eboot, self = true)
            verifyPatch(eboot, File(folder, "patches"), "PS5 SELF", "0xfe50")
        } finally { folder.deleteRecursively() }
    }

    @Test
    fun decryptedElfUsesProgramHeaderPOffset() {
        val folder = Files.createTempDirectory("kyty-elf-patch").toFile()
        try {
            val eboot = File(folder, "eboot.bin")
            createExecutable(eboot, self = false)
            verifyPatch(eboot, File(folder, "patches"), "decrypted ELF", "0x4000")
        } finally { folder.deleteRecursively() }
    }

    @Test
    fun refusesMismatchedExecutableWithoutWritingPatch() {
        val folder = Files.createTempDirectory("kyty-ds-mismatch").toFile()
        try {
            val eboot = File(folder, "eboot.bin")
            createExecutable(eboot, self = true, valid = false)
            val out = File(folder, "patches")
            val result = DemonsSoulsFirstBootPatch.prepare(folder, out)
            assertNotNull(result.error)
            assertNull(result.path)
            assertFalse(File(out, "demons-souls-first-boot-1.07.json").exists())
        } finally { folder.deleteRecursively() }
    }
}
