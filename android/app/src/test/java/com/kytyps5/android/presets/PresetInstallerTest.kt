package com.kytyps5.android.presets

import java.io.File
import java.nio.file.Files
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class PresetInstallerTest {
    private lateinit var filesDir: File
    private lateinit var installer: PresetInstaller
    private lateinit var titleDir: File

    private val userEngine = "commandos/saved/unreal engine/engine/config/userengine.ini"
    private val engine = "commandos/saved/ps5/engine.ini"
    private val gameSettings = "commandos/saved/ps5/gameusersettings.ini"
    private val res50 = preset("res50", userEngine, engine)

    private fun preset(id: String, vararg files: String) = GamePreset(
        titleId = "PPSA14708",
        id = id,
        name = id,
        description = "",
        target = PresetTarget.DOWNLOAD_DATA,
        files = files.toList(),
    )

    private fun contentOf(preset: GamePreset, path: String) = "[${preset.id}] $path".toByteArray()

    private fun file(path: String) = File(titleDir, path)

    /** Every file under the app's files directory with its contents, to compare whole states. */
    private fun snapshot(): Map<String, String> = filesDir.walkTopDown().filter { it.isFile }
        .associate { it.relativeTo(filesDir).path to it.readText() }

    private fun reconcile(enabled: Set<String>, vararg available: GamePreset) =
        installer.reconcile("PPSA14708", available.toList(), enabled) { p, path -> contentOf(p, path) }

    @Before
    fun setUp() {
        filesDir = Files.createTempDirectory("preset-installer-test").toFile()
        installer = PresetInstaller(filesDir)
        titleDir = File(filesDir, "_DownloadData/PPSA14708")
        // What the game itself wrote on an earlier boot, plus another title's data and a save.
        file(gameSettings).apply { parentFile.mkdirs() }.writeText("game's own settings")
        File(filesDir, "_DownloadData/PPSA21703/vs.ini").apply { parentFile.mkdirs() }.writeText("vs")
        File(filesDir, "_SaveData/PPSA14708/ue4savegame.dpx.sav").apply { parentFile.mkdirs() }.writeText("save")
    }

    @After
    fun tearDown() {
        filesDir.deleteRecursively()
    }

    @Test
    fun applyPlacesFilesCreatingFoldersAndLeavesTheGamesFilesAlone() {
        val before = snapshot()

        installer.apply(res50) { contentOf(res50, it) }

        assertEquals("[res50] $userEngine", file(userEngine).readText())
        assertEquals("[res50] $engine", file(engine).readText())
        assertEquals(listOf("res50"), installer.appliedPresetIds("PPSA14708"))
        val after = snapshot()
        before.forEach { (path, text) -> assertEquals(path, text, after[path]) }
        // The record lives outside the folder the game sees.
        assertTrue(after.keys.filter { it !in before }.all { it.startsWith("_DownloadData/PPSA14708/commandos/") || it.startsWith("_GamePresets/") })
        assertFalse(titleDir.walkTopDown().any { it.name.contains("preset") })
    }

    @Test
    fun removeDeletesOnlyOurFiles() {
        val before = snapshot()
        installer.apply(res50) { contentOf(res50, it) }
        file("commandos/saved/ps5/scalability.ini").writeText("written by the game while ours were in place")

        assertTrue(installer.remove("PPSA14708", "res50"))

        assertFalse(file(userEngine).exists())
        assertFalse(file(engine).exists())
        assertEquals(before + ("_DownloadData/PPSA14708/commandos/saved/ps5/scalability.ini" to "written by the game while ours were in place"), snapshot())
        assertEquals(emptyList<String>(), installer.appliedPresetIds("PPSA14708"))
        assertFalse("removing twice is a no-op", installer.remove("PPSA14708", "res50"))
    }

    @Test
    fun applyIsIdempotent() {
        installer.apply(res50) { contentOf(res50, it) }
        val once = snapshot()
        val modified = file(engine).lastModified()
        file(engine).setLastModified(modified - 10_000)

        installer.apply(res50) { contentOf(res50, it) }

        assertEquals(once, snapshot())
        assertEquals("unchanged files are not rewritten", modified - 10_000, file(engine).lastModified())
    }

    @Test
    fun reapplyOverwritesAFileTheGameRewroteAndRemoveStillDeletesIt() {
        installer.apply(res50) { contentOf(res50, it) }
        file(engine).writeText("[SystemSettings]\nr.ScreenPercentage=100 ; rewritten by the game")

        installer.apply(res50) { contentOf(res50, it) }
        assertEquals("[res50] $engine", file(engine).readText())

        file(engine).writeText("rewritten again")
        installer.remove("PPSA14708", "res50")
        assertFalse(file(engine).exists())
        assertEquals("game's own settings", file(gameSettings).readText())
    }

    @Test
    fun aFileThatWasAlreadyThereIsBackedUpAndRestored() {
        val clash = preset("clash", gameSettings)

        installer.apply(clash) { contentOf(clash, it) }
        assertEquals("[clash] $gameSettings", file(gameSettings).readText())
        installer.apply(clash) { contentOf(clash, it) }

        installer.remove("PPSA14708", "clash")
        assertEquals("game's own settings", file(gameSettings).readText())
        assertFalse(File(filesDir, "_GamePresets/PPSA14708/clash.backup").exists())
    }

    @Test
    fun reconcileFollowsTheChoice() {
        val initial = snapshot()

        val on = reconcile(setOf("res50"), res50)
        assertEquals(listOf("res50"), on.applied)
        assertTrue(file(userEngine).isFile)

        val off = reconcile(emptySet(), res50)
        assertEquals(listOf("res50"), off.removed)
        assertEquals(initial, snapshot())

        val stillOff = reconcile(emptySet(), res50)
        assertEquals(PresetInstaller.Report(emptyList(), emptyList(), emptyList()), stillOff)
        assertEquals(initial, snapshot())
    }

    @Test
    fun presetsNoLongerShippedAreRemoved() {
        val initial = snapshot()
        reconcile(setOf("res50"), res50)

        val report = reconcile(setOf("res50"))

        assertEquals(listOf("res50"), report.removed)
        assertEquals(initial, snapshot())
    }

    @Test
    fun aFileDroppedFromAnUpdatedPresetIsRemoved() {
        installer.apply(res50) { contentOf(res50, it) }
        val updated = preset("res50", userEngine)

        installer.apply(updated) { contentOf(updated, it) }

        assertTrue(file(userEngine).isFile)
        assertFalse(file(engine).exists())
        installer.remove("PPSA14708", "res50")
        assertFalse(file(userEngine).exists())
    }

    @Test
    fun aMissingPresetFileChangesNothing() {
        val initial = snapshot()

        val report = installer.reconcile("PPSA14708", listOf(res50), setOf("res50")) { _, path ->
            if (path == engine) throw java.io.FileNotFoundException(path) else "x".toByteArray()
        }

        assertEquals(1, report.errors.size)
        assertEquals(initial, snapshot())
    }

    @Test
    fun otherTitlesAreUntouched() {
        reconcile(setOf("res50"), res50)
        val vs = installer.reconcile("PPSA21703", emptyList(), emptySet()) { _, _ -> ByteArray(0) }

        assertEquals(PresetInstaller.Report(emptyList(), emptyList(), emptyList()), vs)
        assertEquals("vs", File(filesDir, "_DownloadData/PPSA21703/vs.ini").readText())
        assertTrue(file(userEngine).isFile)
    }
}
