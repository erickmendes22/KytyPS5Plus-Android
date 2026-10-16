package com.kytyps5.android.library

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ParamSfoTest {

    @Test
    fun readsStringsAndIntegers() {
        val parsed = ParamSfo.parse(SfoFixture.build("APP_VER" to "01.019", "PARENTAL_LEVEL" to 5, "TITLE" to "ASTRO BOT", "TITLE_ID" to "PPSA21567"))!!
        assertEquals("ASTRO BOT", parsed["TITLE"])
        assertEquals("PPSA21567", parsed["TITLE_ID"])
        assertEquals(5, parsed["PARENTAL_LEVEL"])
    }

    @Test
    fun rejectsGarbageAndTruncatedFiles() {
        assertNull(ParamSfo.parse(ByteArray(4)))
        assertNull(ParamSfo.parse("not a psf file at all....".toByteArray()))
        val good = SfoFixture.build("TITLE" to "X")
        assertNull(ParamSfo.parse(good.copyOf(30)))
    }
}
