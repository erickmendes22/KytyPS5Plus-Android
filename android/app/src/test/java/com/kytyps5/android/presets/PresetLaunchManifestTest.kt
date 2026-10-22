package com.kytyps5.android.presets

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** Schema 2: presets that carry emulator launch options. */
class PresetLaunchManifestTest {

    private fun manifest(launch: String?, files: String? = null, schema: Int = 2, target: String? = null): String {
        val parts = mutableListOf(""""schemaVersion":$schema""", """"id":"perf720"""", """"name":"Performance mode"""")
        target?.let { parts += """"target":"$it"""" }
        files?.let { parts += """"files":$it""" }
        launch?.let { parts += """"launch":$it""" }
        return parts.joinToString(",", "{", "}")
    }

    private fun parse(json: String) = PresetManifest.parse("PPSA21567", "perf720", json)

    private fun ok(json: String): GamePreset = (parse(json) as PresetManifest.Result.Ok).preset

    private fun invalid(json: String): String = (parse(json) as PresetManifest.Result.Invalid).reason

    @Test
    fun parsesALaunchOnlyPreset() {
        val preset = ok(manifest("""{"env":{"KYTY_RT_HALF":"1","KYTY_A":"x.y"},"renderScale":0.75,"gamePatch":"astro_res720.json"}"""))

        assertEquals(emptyList<String>(), preset.files)
        assertEquals(PresetTarget.DOWNLOAD_DATA, preset.target)
        assertEquals(
            PresetLaunch(env = mapOf("KYTY_A" to "x.y", "KYTY_RT_HALF" to "1"), renderScale = 0.75f, gamePatch = "astro_res720.json"),
            preset.launch,
        )
        assertEquals("env is kept in a stable (sorted) order", listOf("KYTY_A", "KYTY_RT_HALF"), preset.launch.env.keys.toList())
    }

    @Test
    fun launchAndFilesTogether() {
        val preset = ok(manifest("""{"renderScale":1.5}""", files = """["a.ini"]""", target = "download_data"))
        assertEquals(listOf("a.ini"), preset.files)
        assertEquals(PresetLaunch(renderScale = 1.5f), preset.launch)
    }

    @Test
    fun schemaOnePresetsHaveNoLaunchOptions() {
        val v1 = """{"schemaVersion":1,"id":"perf720","name":"x","target":"download_data","files":["a.ini"]}"""
        assertTrue(ok(v1).launch.isEmpty)
        assertEquals("launch needs schemaVersion 2", invalid(manifest("""{"renderScale":0.5}""", files = """["a.ini"]""", schema = 1, target = "download_data")))
    }

    @Test
    fun rejectsInvalidLaunchOptions() {
        assertEquals("no files", invalid(manifest("{}")))
        assertEquals("no files", invalid(manifest(null)))
        assertEquals("missing files", invalid(manifest("""{"renderScale":0.5}""").replace("}}", "},\"files\":null}")))
        assertEquals("launch is not an object", invalid(manifest("[]")))
        assertEquals("unknown launch option \"args\"", invalid(manifest("""{"args":["--x"]}""")))
        assertEquals("launch env is not an object", invalid(manifest("""{"env":["KYTY_A=1"]}""")))
        listOf("LD_PRELOAD", "TU_DEBUG", "kyty_rt_half", "KYTY_", "KYTY_A B").forEach { key ->
            assertEquals(key, "env name \"$key\" is not a KYTY_ knob", invalid(manifest("""{"env":{"$key":"1"}}""")))
        }
        listOf("\"\"", "1", "\"a b\"", "\"a,b\"", "\"\$(x)\"", "null").forEach { value ->
            assertEquals(value, "invalid value for KYTY_A", invalid(manifest("""{"env":{"KYTY_A":$value}}""")))
        }
        listOf("0.2", "2.5", "\"0.5\"", "null", "true").forEach { value ->
            assertTrue(value, invalid(manifest("""{"renderScale":$value}""")).startsWith("renderScale must be a number"))
        }
        listOf("\"../x.json\"", "\"/data/x.json\"", "\"x.txt\"", "\"\"", "1").forEach { value ->
            assertEquals(value, "gamePatch must be a relative .json path", invalid(manifest("""{"gamePatch":$value}""")))
        }
    }

    @Test
    fun aLaunchOnlyPresetMayNameItsTargetButNotAnUnknownOne() {
        assertEquals(PresetTarget.DOWNLOAD_DATA, ok(manifest("""{"renderScale":0.5}""", target = "download_data")).target)
        assertEquals("unknown target \"app0\"", invalid(manifest("""{"renderScale":0.5}""", target = "app0")))
    }
}
