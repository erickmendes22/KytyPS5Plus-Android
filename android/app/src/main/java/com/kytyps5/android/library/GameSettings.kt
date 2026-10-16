package com.kytyps5.android.library

import com.kytyps5.android.Resolution
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject

/** One environment variable set for the emulator process before its libraries load. */
data class EnvVar(val key: String, val value: String) {
    override fun toString(): String = "$key=$value"
}

/**
 * Per-game launcher overrides. Null means "follow the global setting". Env vars cover Mesa/Turnip
 * debugging knobs such as TU_DEBUG=sysmem or MESA_VK_WSI_PRESENT_MODE=mailbox (Eden-style editor).
 */
data class GameSettings(
    val resolution: Resolution? = null,
    val envVars: List<EnvVar> = emptyList(),
    val hud: Boolean? = null,
    val touchControls: Boolean? = null,
) {
    fun toJson(): String = JSONObject()
        .put("resolution", resolution?.storageValue ?: JSONObject.NULL)
        .put("env", JSONArray(envVars.map { it.toString() }))
        .put("hud", hud ?: JSONObject.NULL)
        .put("touchControls", touchControls ?: JSONObject.NULL)
        .toString()

    companion object {
        val DEFAULT = GameSettings()

        fun fromJson(json: String?): GameSettings {
            if (json.isNullOrBlank()) return DEFAULT
            return try {
                val obj = JSONObject(json)
                val env = obj.optJSONArray("env") ?: JSONArray()
                GameSettings(
                    resolution = if (obj.isNull("resolution")) null else Resolution.entries.firstOrNull { it.storageValue == obj.optString("resolution") },
                    envVars = EnvVars.parse((0 until env.length()).joinToString("\n") { env.optString(it) }).vars,
                    hud = if (obj.isNull("hud") || !obj.has("hud")) null else obj.optBoolean("hud"),
                    touchControls = if (obj.isNull("touchControls") || !obj.has("touchControls")) null else obj.optBoolean("touchControls"),
                )
            } catch (e: JSONException) {
                DEFAULT
            }
        }
    }
}

object EnvVars {
    private val KEY = Regex("^[A-Za-z_][A-Za-z0-9_]*$")

    /** Variables that would let a typo hijack the dynamic linker or the shell environment. */
    private val BLOCKED = setOf("LD_PRELOAD", "LD_LIBRARY_PATH", "LD_DEBUG", "PATH", "HOME", "ANDROID_ROOT", "ANDROID_DATA", "CLASSPATH")

    data class Parsed(val vars: List<EnvVar>, val errors: List<String>)

    /** One KEY=VALUE per line; blank lines and #comments are skipped; later duplicates win. */
    fun parse(text: String): Parsed {
        val vars = LinkedHashMap<String, EnvVar>()
        val errors = mutableListOf<String>()
        text.lines().forEachIndexed { index, raw ->
            val line = raw.trim()
            if (line.isEmpty() || line.startsWith("#")) return@forEachIndexed
            val eq = line.indexOf('=')
            val key = if (eq > 0) line.substring(0, eq).trim() else line
            val value = if (eq > 0) line.substring(eq + 1).trim() else ""
            when {
                eq <= 0 -> errors += "Line ${index + 1}: expected KEY=VALUE"
                !KEY.matches(key) -> errors += "Line ${index + 1}: invalid name \"$key\""
                key.uppercase() in BLOCKED -> errors += "Line ${index + 1}: $key cannot be changed"
                else -> {
                    vars.remove(key)
                    vars[key] = EnvVar(key, value)
                }
            }
        }
        return Parsed(vars.values.toList(), errors)
    }

    fun format(vars: List<EnvVar>): String = vars.joinToString("\n")

    /**
     * What to change in the (process-wide, long-lived) environment for the next launch: unset keys a
     * previous game set that this one does not, then set this game's values.
     */
    fun plan(previousKeys: Set<String>, next: List<EnvVar>): Pair<Set<String>, List<EnvVar>> =
        (previousKeys - next.map { it.key }.toSet()) to next
}
