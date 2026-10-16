package com.kytyps5.android.library

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

object SfoFixture {
    /** Builds a PSF file from (key, value) pairs; String -> utf8 (0x0204), Int -> int32 (0x0404). */
    fun build(vararg entries: Pair<String, Any>): ByteArray {
        val keys = ByteArrayOutputStream()
        val data = ByteArrayOutputStream()
        val index = ByteBuffer.allocate(entries.size * 16).order(ByteOrder.LITTLE_ENDIAN)
        entries.forEach { (key, value) ->
            val keyOffset = keys.size()
            keys.write(key.toByteArray())
            keys.write(0)
            val dataOffset = data.size()
            val (format, payload, max) = when (value) {
                is String -> Triple(0x0204, value.toByteArray() + 0, 32)
                is Int -> Triple(0x0404, ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(value).array(), 4)
                else -> error("unsupported")
            }
            data.write(payload)
            repeat(max - payload.size) { data.write(0) }
            index.putShort(keyOffset.toShort()).putShort(format.toShort()).putInt(payload.size).putInt(max).putInt(dataOffset)
        }
        while (keys.size() % 4 != 0) keys.write(0)
        val keyTable = 20 + index.capacity()
        val dataTable = keyTable + keys.size()
        val header = ByteBuffer.allocate(20).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(0x46535000).putInt(0x0101).putInt(keyTable).putInt(dataTable).putInt(entries.size)
        return header.array() + index.array() + keys.toByteArray() + data.toByteArray()
    }
}
