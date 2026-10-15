package com.kytyps5.android

import java.io.File
import java.nio.file.Files
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class GameLibraryTest {

    private lateinit var root: File

    @Before
    fun setUp() {
        root = Files.createTempDirectory("kytyps5-games").toFile()
    }

    @After
    fun tearDown() {
        root.deleteRecursively()
    }

    @Test
    fun titlePrefersDefaultLanguageLocalizedTitle() {
        val json = JSONObject(
            """{"titleName":"Top","localizedParameters":{"defaultLanguage":"ja","ja":{"titleName":"JP"},"en":{"titleName":"EN"}}}""",
        )
        assertEquals("JP", GameLibrary.titleFromParamJson(json, "folder"))
    }

    @Test
    fun titleFallsBackToEnglishThenOtherLocalesThenTopLevel() {
        val english = JSONObject("""{"localizedParameters":{"defaultLanguage":"xx","en":{"titleName":"EN"}}}""")
        assertEquals("EN", GameLibrary.titleFromParamJson(english, "folder"))

        val other = JSONObject("""{"localizedParameters":{"defaultLanguage":"xx","fr":{"titleName":"FR"}}}""")
        assertEquals("FR", GameLibrary.titleFromParamJson(other, "folder"))

        val topLevel = JSONObject("""{"titleName":"  Top  "}""")
        assertEquals("Top", GameLibrary.titleFromParamJson(topLevel, "folder"))
    }

    @Test
    fun titleFallsBackToFolderNameWhenNoTitleIsPresent() {
        assertEquals("folder", GameLibrary.titleFromParamJson(JSONObject("{}"), "folder"))
        assertEquals("folder", GameLibrary.titleFromParamJson(JSONObject("""{"titleName":"   "}"""), "folder"))
    }

    @Test
    fun scanReturnsOnlyFoldersWithParamJsonSortedByTitle() {
        makeGame("CUSA00001", """{"titleName":"Zeta"}""")
        makeGame("CUSA00002", """{"localizedParameters":{"defaultLanguage":"en","en":{"titleName":"alpha"}}}""")
        makeGame("CUSA00003", null) // sce_sys/ without param.json is not a game
        File(root, "not-a-game").mkdirs()
        File(root, "loose-file.txt").writeText("x")

        val result = GameLibrary.scan(root) as ScanResult.Found

        assertEquals(listOf("alpha", "Zeta"), result.games.map { it.title })
        assertEquals(File(root, "CUSA00002").absolutePath, result.games.first().path)
    }

    @Test
    fun scanListsZArchiveDumpsAlongsideFoldersByFileName() {
        makeGame("CUSA00001", """{"titleName":"Middle"}""")
        File(root, "Beta Game.zar").writeText("zar")
        File(root, "ALPHA.ZAR").writeText("zar")
        File(root, "notes.zip").writeText("x")

        val result = GameLibrary.scan(root) as ScanResult.Found

        assertEquals(listOf("ALPHA", "Beta Game", "Middle"), result.games.map { it.title })
        assertEquals(File(root, "Beta Game.zar").absolutePath, result.games[1].path)
    }

    @Test
    fun scanReportsMissingAndNonDirectoryRoots() {
        val missing = GameLibrary.scan(File(root, "nope"))
        assertTrue(missing is ScanResult.Failure)

        val file = File(root, "file.txt").apply { writeText("x") }
        assertTrue(GameLibrary.scan(file) is ScanResult.Failure)
    }

    @Test
    fun malformedParamJsonFallsBackToFolderName() {
        makeGame("BROKEN", "{not json")
        val result = GameLibrary.scan(root) as ScanResult.Found
        assertEquals("BROKEN", result.games.single().title)
    }

    private fun makeGame(folderName: String, paramJson: String?) {
        val folder = File(root, folderName)
        File(folder, "sce_sys").mkdirs()
        paramJson?.let { File(folder, GameLibrary.PARAM_JSON).writeText(it) }
    }
}
