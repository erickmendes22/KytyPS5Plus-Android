package com.kytyps5.android.library

import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Translate PS5 guest ELF virtual addresses into file offsets.
 *
 * The PS5 SELF container has its *own* physical segment offsets; they differ
 * from the embedded ELF's p_offset fields. On the provided Demon's Souls SELF
 * the executable PT_LOAD starts at 0xFE50 in the file, not p_offset=0x4000.
 *
 * All three independent code signatures must match the same executable
 * segment before any memory-only patch is generated.
 */
object Ps5ExecutableMapping {
    data class Probe(val virtualAddress: Long, val prefix: ByteArray)
    data class Mapping(val fileBase: Long, val virtualBase: Long, val length: Long, val format: String) {
        fun fileOffset(virtualAddress: Long): Long = fileBase + virtualAddress - virtualBase
    }
    private data class Segment(val fileOffset: Long, val size: Long)

    private fun bytes(file: RandomAccessFile, offset: Long, count: Int): ByteArray {
        require(offset >= 0 && count >= 0 && offset <= file.length() && count <= file.length() - offset) {
            "Executable header/segment lies outside the supplied file"
        }
        return ByteArray(count).also { file.seek(offset); file.readFully(it) }
    }
    private fun u16(b: ByteArray, at: Int): Int = ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN)
        .getShort(at).toInt() and 0xffff
    private fun u32(b: ByteArray, at: Int): Long = ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN)
        .getInt(at).toLong() and 0xffffffffL
    private fun u64(b: ByteArray, at: Int): Long = ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN)
        .getLong(at)

    fun locate(file: RandomAccessFile, probes: List<Probe>): Mapping {
        require(probes.isNotEmpty() && probes.all { it.virtualAddress >= 0 && it.prefix.isNotEmpty() })
        val start = bytes(file, 0, 0x20)
        val self = start[0] == 0x4f.toByte() && start[1] == 0x15.toByte() &&
            start[2] == 0x3d.toByte() && start[3] == 0x1d.toByte()
        val rawElf = start[0] == 0x7f.toByte() && start[1] == 0x45.toByte() &&
            start[2] == 0x4c.toByte() && start[3] == 0x46.toByte()
        require(self || rawElf) { "Expected a PS5 SELF or decrypted ELF executable" }

        val segmentCount = if (self) u16(start, 0x18) else 0
        require(!self || segmentCount in 1..256) { "Invalid SELF segment count: $segmentCount" }
        val elfStart = if (self) 0x20L + 0x20L * segmentCount else 0L
        val header = bytes(file, elfStart, 64)
        require(header[0] == 0x7f.toByte() && header[1] == 0x45.toByte() &&
                header[2] == 0x4c.toByte() && header[3] == 0x46.toByte() &&
                header[4] == 2.toByte() && header[5] == 1.toByte()) {
            "Embedded ELF header not found or not 64-bit little endian"
        }
        val phOffset = u64(header, 0x20)
        val phSize = u16(header, 0x36)
        val phCount = u16(header, 0x38)
        require(phOffset >= 0 && phSize in 56..512 && phCount in 1..256) {
            "Invalid executable ELF program headers"
        }

        val selfSegments = if (self) (0 until segmentCount).map {
            val record = bytes(file, 0x20L + it * 0x20L, 32)
            Segment(fileOffset = u64(record, 8), size = u64(record, 16))
        } else emptyList()

        val candidates = mutableListOf<Mapping>()
        for (i in 0 until phCount) {
            val program = bytes(file, elfStart + phOffset + i * phSize, 56)
            val type = u32(program, 0)
            val flags = u32(program, 4)
            if (type != 1L || (flags and 1L) == 0L) continue
            val elfFileOffset = u64(program, 8)
            val virtualBase = u64(program, 16)
            val fileLength = u64(program, 32)
            if (virtualBase < 0 || fileLength <= 0 || fileLength > file.length()) continue
            val possible = if (self) selfSegments.filter { it.size == fileLength }
                else listOf(Segment(elfFileOffset, fileLength))
            for (segment in possible) {
                if (segment.fileOffset < 0 || segment.fileOffset > file.length() - fileLength) continue
                val mapped = Mapping(segment.fileOffset, virtualBase, fileLength,
                    if (self) "PS5 SELF" else "decrypted ELF")
                val matches = probes.all { probe ->
                    val delta = probe.virtualAddress - virtualBase
                    delta >= 0 && delta <= fileLength &&
                        probe.prefix.size <= fileLength - delta &&
                        bytes(file, segment.fileOffset + delta, probe.prefix.size)
                            .contentEquals(probe.prefix)
                }
                if (matches) candidates.add(mapped)
            }
        }
        require(candidates.size == 1) {
            "Could not uniquely map all three first-boot signatures to one executable segment " +
                "(format=" + if (self) "PS5 SELF" else "ELF" + ", matches=" + candidates.size + ")"
        }
        return candidates.single()
    }
}
