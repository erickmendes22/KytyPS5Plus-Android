package com.kytyps5.android.input

import java.io.File
import java.nio.file.Files
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class OverlayLayoutTest {

    private lateinit var dir: File

    @Before
    fun setUp() {
        dir = Files.createTempDirectory("layouts").toFile()
    }

    @After
    fun tearDown() {
        dir.deleteRecursively()
    }

    @Test
    fun defaultLayoutUsesControlDefaults() {
        val p = OverlayLayout.DEFAULT.placement(OverlayControl.CROSS)
        assertEquals(OverlayControl.CROSS.defaultX, p.x)
        assertTrue(p.visible)
    }

    @Test
    fun everyFaceButtonAndShoulderMapsToInput() {
        listOf(OverlayControl.CROSS, OverlayControl.CIRCLE, OverlayControl.SQUARE, OverlayControl.TRIANGLE,
            OverlayControl.L1, OverlayControl.R1, OverlayControl.OPTIONS).forEach { assertTrue(it.button != null) }
        assertEquals(PadAxis.L2, OverlayControl.L2.trigger)
        assertEquals(PadAxis.R2, OverlayControl.R2.trigger)
    }

    @Test
    fun withClampsAndDoesNotMutate() {
        val base = OverlayLayout.DEFAULT
        val moved = base.with(OverlayControl.CROSS, ControlPlacement(1.5f, -2f, 9f, false))
        assertTrue(base.placements.isEmpty())
        assertEquals(ControlPlacement(1f, 0f, ControlPlacement.MAX_SCALE, false), moved.placement(OverlayControl.CROSS))
    }

    @Test
    fun jsonRoundTrip() {
        val layout = OverlayLayout.DEFAULT
            .with(OverlayControl.DPAD, ControlPlacement(0.2f, 0.3f, 1.4f, true))
            .with(OverlayControl.L3, ControlPlacement(0.4f, 0.9f, 1f, false))
            .withOpacity(0.35f)
        val parsed = OverlayLayout.fromJson(layout.toJson())!!
        assertEquals(0.35f, parsed.opacity, 1e-6f)
        assertEquals(0.2f, parsed.placement(OverlayControl.DPAD).x, 1e-6f)
        assertEquals(1.4f, parsed.placement(OverlayControl.DPAD).scale, 1e-6f)
        assertFalse(parsed.placement(OverlayControl.L3).visible)
    }

    @Test
    fun badJsonIsNullAndUnknownControlsAreIgnored() {
        assertNull(OverlayLayout.fromJson("nope"))
        val parsed = OverlayLayout.fromJson("""{"controls":{"BOGUS":{"x":0.1},"CROSS":{"x":"NaN"}}}""")!!
        assertEquals(setOf(OverlayControl.CROSS), parsed.placements.keys)
    }

    @Test
    fun storeFallsBackFromGameToGlobalToDefault() {
        val store = OverlayProfileStore(dir)
        val game = "/sdcard/games/PPSA21567"
        assertEquals(LayoutScope.DEFAULT, store.load(game).scope)

        val global = OverlayLayout.DEFAULT.withOpacity(0.4f)
        assertTrue(store.save(global, gamePath = null))
        assertEquals(LayoutScope.GLOBAL, store.load(game).scope)
        assertEquals(0.4f, store.load(null).layout.opacity, 1e-6f)

        val perGame = OverlayLayout.DEFAULT.withOpacity(0.9f)
        assertTrue(store.save(perGame, gamePath = game))
        assertTrue(store.hasGameLayout(game))
        assertEquals(LayoutScope.GAME, store.load(game).scope)
        assertEquals(0.9f, store.load(game).layout.opacity, 1e-6f)
        assertEquals(LayoutScope.GLOBAL, store.load("/other").scope)

        assertTrue(store.deleteGameLayout(game))
        assertEquals(LayoutScope.GLOBAL, store.load(game).scope)
    }

    @Test
    fun corruptGameFileFallsBackToGlobal() {
        val store = OverlayProfileStore(dir)
        store.save(OverlayLayout.DEFAULT, null)
        File(dir, "game-${OverlayProfileStore.keyFor("/g")}.json").writeText("{{")
        assertEquals(LayoutScope.GLOBAL, store.load("/g").scope)
    }

    @Test
    fun keysAreStableAndFileSafe() {
        val key = OverlayProfileStore.keyFor("/storage/emulated/0/Download/PPSA21567 ASTRO BOT (01.019.000)")
        assertEquals(key, OverlayProfileStore.keyFor("/storage/emulated/0/Download/PPSA21567 ASTRO BOT (01.019.000)"))
        assertTrue(Regex("^[0-9a-f]{16}$").matches(key))
    }
}
