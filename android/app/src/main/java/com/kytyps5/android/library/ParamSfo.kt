package com.kytyps5.android.library

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Reader for PlayStation param.sfo (PSF) files: a little-endian header, an index of
 * (key offset, format, length, max length, data offset) entries, a key table and a data table.
 * Strings are UTF-8 (format 0x0204 / 0x0004), integers are uint32 (0x0404).
 */
object ParamSfo {
    private const val MAGIC = 0x46535000 // "\0PSF"
    private const val HEADER_SIZE = 20
    private const val ENTRY_SIZE = 16
    private const val FORMAT_UTF8_SPECIAL = 0x0004
    private const val FORMAT_UTF8 = 0x0204
    private const val FORMAT_INT32 = 0x0404
    private const val MAX_ENTRIES = 1024

    /** Keys to String/Int values, or null if [bytes] is not a well-formed PSF file. */
    fun parse(bytes: ByteArray): Map<String, Any>? {
        if (bytes.size < HEADER_SIZE) return null
        val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        if (buffer.getInt(0) != MAGIC) return null
        val keyTable = buffer.getInt(8)
        val dataTable = buffer.getInt(12)
        val count = buffer.getInt(16)
        if (count !in 0..MAX_ENTRIES || HEADER_SIZE.toLong() + count.toLong() * ENTRY_SIZE > bytes.size) return null
        if (keyTable !in 0..bytes.size || dataTable !in 0..bytes.size) return null
        val result = LinkedHashMap<String, Any>()
        for (i in 0 until count) {
            val entry = HEADER_SIZE + i * ENTRY_SIZE
            val keyStart = keyTable + (buffer.getShort(entry).toInt() and 0xFFFF)
            val format = buffer.getShort(entry + 2).toInt() and 0xFFFF
            val length = buffer.getInt(entry + 4)
            val dataStart = dataTable + buffer.getInt(entry + 12)
            val key = cString(bytes, keyStart) ?: return null
            if (length < 0 || dataStart < 0 || dataStart.toLong() + length > bytes.size) return null
            when (format) {
                FORMAT_UTF8, FORMAT_UTF8_SPECIAL ->
                    result[key] = String(bytes, dataStart, length, Charsets.UTF_8).trimEnd('\u0000')
                FORMAT_INT32 -> if (length >= 4) result[key] = buffer.getInt(dataStart)
            }
        }
        return result
    }

    private fun cString(bytes: ByteArray, start: Int): String? {
        if (start !in bytes.indices) return null
        var end = start
        while (end < bytes.size && bytes[end] != 0.toByte()) end++
        return String(bytes, start, end - start, Charsets.UTF_8)
    }
}
