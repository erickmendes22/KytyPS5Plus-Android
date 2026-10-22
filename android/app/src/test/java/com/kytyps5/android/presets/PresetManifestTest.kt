package com.kytyps5.android.presets

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PresetManifestTest {

    private fun manifest(
        id: String = "res50",
        target: String = "download_data",
        files: String = """["a/b/user engine.ini", "c.ini"]""",
        schema: Int = 1,
    ) = """{"schemaVersion":$schema,"id":"$id","name":"Render at 50%","description":" Faster. ","target":"$target","files":$files}"""

    private fun invalidReason(json: String, folder: String = "res50"): String =
        (PresetManifest.parse("PPSA14708", folder, json) as PresetManifest.Result.Invalid).reason

    @Test
    fun parsesManifest() {
        val result = PresetManifest.parse("PPSA14708", "res50", manifest())
        val preset = (result as PresetManifest.Result.Ok).preset
        assertEquals(
            GamePreset(
                titleId = "PPSA14708",
                id = "res50",
                name = "Render at 50%",
                description = "Faster.",
                target = PresetTarget.DOWNLOAD_DATA,
                files = listOf("a/b/user engine.ini", "c.ini"),
            ),
            preset,
        )
    }

    @Test
    fun rejectsInvalidManifests() {
        assertTrue(invalidReason("{").startsWith("not JSON"))
        assertEquals("unsupported schemaVersion 2", invalidReason(manifest(schema = 2)))
        assertEquals("id \"res50\" does not match folder \"other\"", invalidReason(manifest(), folder = "other"))
        assertEquals("invalid id \"Res 50\"", invalidReason(manifest(id = "Res 50"), folder = "Res 50"))
        assertEquals("unknown target \"app0\"", invalidReason(manifest(target = "app0")))
        assertEquals("no files", invalidReason(manifest(files = "[]")))
        assertEquals("missing files", invalidReason(manifest().replace(""","files":["a/b/user engine.ini", "c.ini"]""", "")))
        assertEquals("unsafe path \"../x.ini\"", invalidReason(manifest(files = """["ok.ini", "../x.ini"]""")))
        assertEquals("duplicate file", invalidReason(manifest(files = """["a.ini", "a.ini"]""")))
        assertEquals("missing name", invalidReason(manifest().replace("Render at 50%", " ")))
    }

    @Test
    fun onlyPlainRelativePathsAreSafe() {
        listOf("a.ini", "commandos/saved/unreal engine/engine/config/userengine.ini", "x/.hidden").forEach {
            assertTrue(it, PresetManifest.isSafeRelativePath(it))
        }
        listOf("", "/etc/x", "a/../b", "../a", "./a", "a//b", "a/", "a\\b", "a\u0000b").forEach {
            assertFalse(it, PresetManifest.isSafeRelativePath(it))
        }
    }

    @Test
    fun targetIsThePerTitleDataFolder() {
        assertEquals(
            File("/data/files/_DownloadData/PPSA14708"),
            PresetTarget.DOWNLOAD_DATA.directory(File("/data/files"), "PPSA14708"),
        )
        assertEquals(PresetTarget.DOWNLOAD_DATA, PresetTarget.fromStorage("download_data"))
        assertNull(PresetTarget.fromStorage("DOWNLOAD_DATA"))
    }
}
