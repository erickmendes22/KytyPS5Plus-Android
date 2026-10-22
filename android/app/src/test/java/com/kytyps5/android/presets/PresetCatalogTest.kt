package com.kytyps5.android.presets

import java.io.File
import java.nio.file.Files
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class PresetCatalogTest {
    private lateinit var root: File
    private val warnings = mutableListOf<String>()
    private lateinit var catalog: PresetCatalog

    @Before
    fun setUp() {
        root = Files.createTempDirectory("preset-catalog-test").toFile()
        catalog = PresetCatalog(DirectoryPresetSource(root)) { warnings += it }
    }

    @After
    fun tearDown() {
        root.deleteRecursively()
    }

    private fun preset(title: String, id: String, vararg files: String, manifestId: String = id) {
        val dir = File(root, "$title/$id")
        dir.mkdirs()
        val list = files.joinToString(",") { "\"$it\"" }
        File(dir, PresetManifest.FILE_NAME).writeText(
            """{"schemaVersion":1,"id":"$manifestId","name":"$id","target":"download_data","files":[$list]}""",
        )
        files.forEach { File(dir, "files/$it").apply { parentFile.mkdirs() }.writeText("$id:$it") }
    }

    @Test
    fun listsValidPresetsOfOneTitleSortedById() {
        preset("PPSA14708", "res50", "x/user engine.ini")
        preset("PPSA14708", "aa", "a.ini")
        preset("PPSA21703", "other", "b.ini")

        val presets = catalog.forTitle("PPSA14708")

        assertEquals(listOf("aa", "res50"), presets.map { it.id })
        assertTrue(presets.all { it.titleId == "PPSA14708" })
        assertArrayEquals("res50:x/user engine.ini".toByteArray(), catalog.fileBytes(presets[1], "x/user engine.ini"))
    }

    @Test
    fun matchesTitleIdsCaseInsensitively() {
        preset("PPSA14708", "res50", "a.ini")
        assertEquals(listOf("res50"), catalog.forTitle("ppsa14708").map { it.id })
    }

    @Test
    fun unknownTitlesAndInvalidFoldersHaveNoPresets() {
        preset("PPSA14708", "res50", "a.ini")
        preset("notatitle", "res50", "a.ini")
        assertEquals(emptyList<GamePreset>(), catalog.forTitle("PPSA21567"))
        assertEquals(emptyList<GamePreset>(), catalog.forTitle("notatitle"))
        assertEquals(emptyList<GamePreset>(), catalog.forTitle(""))
    }

    @Test
    fun skipsAndReportsBrokenPresets() {
        preset("PPSA14708", "good", "a.ini")
        preset("PPSA14708", "renamed", "a.ini", manifestId = "other")
        File(root, "PPSA14708/nomanifest").mkdirs()

        assertEquals(listOf("good"), catalog.forTitle("PPSA14708").map { it.id })
        assertEquals(2, warnings.size)
    }

    /** The presets shipped in the APK must parse and contain every file they list. */
    @Test
    fun shippedPresetsAreValidAndComplete() {
        val shipped = PresetCatalog(DirectoryPresetSource(File("src/main/assets/${PresetCatalog.ASSET_ROOT}"))) { warnings += it }
        val res50 = shipped.forTitle("PPSA14708").single()
        assertEquals("res50", res50.id)
        assertEquals(PresetTarget.DOWNLOAD_DATA, res50.target)
        assertEquals(
            setOf(
                "commandos/saved/unreal engine/engine/config/userengine.ini",
                "commandos/saved/unreal engine/engine/config/userdeviceprofiles.ini",
                "commandos/saved/ps5/engine.ini",
                "commandos/saved/ps5/deviceprofiles.ini",
            ),
            res50.files.toSet(),
        )
        res50.files.forEach { path ->
            val text = String(shipped.fileBytes(res50, path))
            assertTrue(path, "r.ScreenPercentage=50" in text && "r.SecondaryScreenPercentage.GameViewport=50" in text)
        }
        assertEquals(emptyList<String>(), warnings)
    }
}
