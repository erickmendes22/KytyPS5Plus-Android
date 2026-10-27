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

    /** The sole GT 720p preset combines the verified executable patch with
     * reduced Vulkan image backing; other experiments have been removed. */
    @Test
    fun shippedGranTurismoMemoryPresetsAreTitleScopedAndConflictSafely() {
        val shipped = PresetCatalog(DirectoryPresetSource(File("src/main/assets/presets"))) { warnings += it }
        val gt = shipped.forTitle("PPSA24156")
        assertEquals(listOf("native720"), gt.map { it.id })
        val mode = gt.single()
        assertTrue(mode.files.isEmpty())
        assertTrue(mode.launch.env.isEmpty())
        assertEquals("gt_native720.json", mode.launch.gamePatch)
        assertEquals(0.33333334f, mode.launch.renderScale)
        val patch = org.json.JSONObject(String(shipped.launchFileBytes(mode, requireNotNull(mode.launch.gamePatch))))
        assertEquals("PPSA24156", patch.getString("id"))
        assertEquals("01.010.000", patch.getString("version"))
        assertEquals("eboot.bin", patch.getString("process"))
        val writes = patch.getJSONArray("mods").getJSONObject(0).getJSONArray("memory")
        assertEquals(8, writes.length())
        for (i in 0 until writes.length()) {
            val write = writes.getJSONObject(i)
            assertEquals(write.getString("off").length, write.getString("on").length)
            assertTrue(write.getString("offset").startsWith("0x"))
        }
        assertEquals(emptyList<GamePreset>(), shipped.forTitle("PPSA01317"))
        val plan = PresetLaunchArgs.plan(listOf(
            PresetLaunchArgs.Resolved(mode.id, mode.launch, "/tmp/gt720.json")
        ))
        assertEquals(listOf("--render-scale", "0.33333334", "--game-patch", "/tmp/gt720.json"), plan.args)
        assertEquals(emptyList<String>(), plan.conflicts)
        assertEquals(emptyList<String>(), warnings)
    }

    /** Astro's performance mode: launch options only, with a game patch for the shipped eboot version. */
    @Test
    fun shippedAstroPerformanceModeIsValid() {
        val shipped = PresetCatalog(DirectoryPresetSource(File("src/main/assets/${PresetCatalog.ASSET_ROOT}"))) { warnings += it }
        val perf = shipped.forTitle("PPSA21567").single()
        assertEquals("perf720", perf.id)
        assertEquals("Performance mode (720p, faster lighting)", perf.name)
        assertEquals(emptyList<String>(), perf.files)
        assertEquals("1", perf.launch.env["KYTY_RT_HALF"])
        val patchName = requireNotNull(perf.launch.gamePatch)
        val patch = org.json.JSONObject(String(shipped.launchFileBytes(perf, patchName)))
        assertEquals("PPSA21567", patch.getString("id"))
        assertEquals("01.019.000", patch.getString("version"))
        assertEquals("eboot.bin", patch.getString("process"))
        val mods = patch.getJSONArray("mods")
        assertTrue(mods.length() > 0)
        for (m in 0 until mods.length()) {
            val memory = mods.getJSONObject(m).getJSONArray("memory")
            for (w in 0 until memory.length()) {
                val write = memory.getJSONObject(w)
                assertEquals(write.toString(), write.getString("off").length, write.getString("on").length)
                assertTrue(write.toString(), write.getString("offset").startsWith("0x"))
            }
        }
        assertEquals(emptyList<String>(), warnings)
    }
}
