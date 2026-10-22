package com.kytyps5.android.presets

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class TitleIdsTest {
    private val astroFolder = File("/storage/emulated/0/Download/PPSA21567 ASTRO BOT (01.019.000)")

    @Test
    fun validatesTitleIds() {
        assertTrue(TitleIds.isValid("PPSA14708"))
        assertTrue(TitleIds.isValid("CUSA00001"))
        assertFalse(TitleIds.isValid("ppsa14708"))
        assertFalse(TitleIds.isValid("PPSA1470"))
        assertFalse(TitleIds.isValid("PPSA147081"))
        assertFalse(TitleIds.isValid("../PPSA1"))
    }

    @Test
    fun findsTitleIdInFileAndFolderNames() {
        assertEquals("PPSA14708", TitleIds.fromName("PPSA14708.exfat"))
        assertEquals("PPSA21567", TitleIds.fromName("PPSA21567 ASTRO BOT (01.019.000)"))
        assertEquals("PPSA28855", TitleIds.fromName("[PPSA28855] Delivery.zar"))
        assertNull(TitleIds.fromName("Commandos.exfat"))
        assertNull(TitleIds.fromName("XPPSA14708.exfat"))
        assertNull(TitleIds.fromName("PPSA147089.exfat"))
    }

    @Test
    fun paramTitleIdWinsOverTheName() {
        assertEquals("PPSA21703", TitleIds.resolve("PPSA21703", File(astroFolder, "PPSA99999")))
        assertEquals("PPSA21703", TitleIds.resolve(" ppsa21703 ", File(astroFolder, "x")))
    }

    @Test
    fun imagesFallBackToTheirOwnNameOnly() {
        assertEquals("PPSA14708", TitleIds.resolve(null, File(astroFolder, "PPSA14708.exfat")))
        assertEquals("PPSA14708", TitleIds.resolve("garbage", File(astroFolder, "PPSA14708.exfat")))
        // An unnamed image inside Astro's folder must not be taken for Astro.
        assertNull(TitleIds.resolve(null, File(astroFolder, "game.exfat")))
    }
}
