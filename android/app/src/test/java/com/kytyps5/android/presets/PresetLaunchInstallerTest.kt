package com.kytyps5.android.presets

import java.io.File
import java.io.FileNotFoundException
import java.nio.file.Files
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/** Launch files (a preset's --game-patch) placed and removed by [PresetInstaller]. */
class PresetLaunchInstallerTest {
    private lateinit var filesDir: File
    private lateinit var installer: PresetInstaller

    private fun preset(id: String, patch: String?, vararg files: String) = GamePreset(
        titleId = "PPSA21567",
        id = id,
        name = id,
        description = "",
        target = PresetTarget.DOWNLOAD_DATA,
        files = files.toList(),
        launch = PresetLaunch(env = mapOf("KYTY_RT_HALF" to "1"), gamePatch = patch),
    )

    private val perf = preset("perf720", "astro_res720.json")

    private fun patchBytes(preset: GamePreset, path: String) = "{\"patch\":\"${preset.id}/$path\"}".toByteArray()

    private fun snapshot(): Map<String, String> = filesDir.walkTopDown().filter { it.isFile }
        .associate { it.relativeTo(filesDir).path to it.readText() }

    private fun reconcile(enabled: Set<String>, vararg available: GamePreset) =
        installer.reconcile("PPSA21567", available.toList(), enabled, launchBytes = ::patchBytes) { _, path -> "file $path".toByteArray() }

    @Before
    fun setUp() {
        filesDir = Files.createTempDirectory("preset-launch-test").toFile()
        installer = PresetInstaller(filesDir)
        File(filesDir, "_SaveData/PPSA21567/save.dat").apply { parentFile.mkdirs() }.writeText("save")
    }

    @After
    fun tearDown() {
        filesDir.deleteRecursively()
    }

    @Test
    fun aLaunchOnlyPresetPlacesItsPatchOutsideTheGameFoldersAndRemovesIt() {
        val initial = snapshot()

        val on = reconcile(setOf("perf720"), perf)

        assertEquals(listOf("perf720"), on.applied)
        val patch = installer.launchFile("PPSA21567", "perf720", "astro_res720.json")
        assertEquals(File(filesDir, "_GamePresets/PPSA21567/perf720.launch/astro_res720.json"), patch)
        assertEquals("{\"patch\":\"perf720/astro_res720.json\"}", patch.readText())
        assertFalse("nothing lands in the game's folders", File(filesDir, "_DownloadData").exists())

        val off = reconcile(emptySet(), perf)

        assertEquals(listOf("perf720"), off.removed)
        assertEquals(initial, snapshot())
    }

    @Test
    fun applyingTwiceKeepsOnePatchAndAnUpdatedPresetDropsTheOldOne() {
        reconcile(setOf("perf720"), perf)
        reconcile(setOf("perf720"), perf)
        val renamed = preset("perf720", "astro_res720_v2.json")

        reconcile(setOf("perf720"), renamed)

        val dir = File(filesDir, "_GamePresets/PPSA21567/perf720.launch")
        assertEquals(listOf("astro_res720_v2.json"), dir.list()?.toList())
        val envOnly = preset("perf720", null)
        reconcile(setOf("perf720"), envOnly)
        assertFalse(dir.exists())
    }

    @Test
    fun aMissingPatchLeavesThePresetUnapplied() {
        val initial = snapshot()
        val withFile = preset("perf720", "astro_res720.json", "a.ini")

        val report = installer.reconcile(
            "PPSA21567", listOf(withFile), setOf("perf720"),
            launchBytes = { _, path -> throw FileNotFoundException(path) },
        ) { _, path -> "file $path".toByteArray() }

        assertEquals(emptyList<String>(), report.applied)
        assertEquals(1, report.errors.size)
        assertEquals(initial, snapshot())
    }

    @Test
    fun filesAndPatchTogether() {
        val both = preset("both", "p.json", "cfg/a.ini")

        reconcile(setOf("both"), both)

        assertEquals("file cfg/a.ini", File(filesDir, "_DownloadData/PPSA21567/cfg/a.ini").readText())
        assertTrue(installer.launchFile("PPSA21567", "both", "p.json").isFile)
        assertTrue(installer.remove("PPSA21567", "both"))
        assertFalse(File(filesDir, "_DownloadData/PPSA21567/cfg/a.ini").exists())
        assertFalse(installer.launchFile("PPSA21567", "both", "p.json").exists())
    }
}
