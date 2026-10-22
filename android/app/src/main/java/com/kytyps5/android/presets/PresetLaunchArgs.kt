package com.kytyps5.android.presets

/**
 * Turns the launch options of the presets that are on ([PresetLaunch]) into emulator arguments and
 * merges them into a command line. Pure, so the merge rules are unit tested:
 * - presets are taken in the order given (GamePresets passes them sorted by id); for a single-value
 *   option (an env variable, --render-scale, --game-patch) the first preset that sets it wins and the
 *   others are reported as conflicts;
 * - a preset's --render-scale / --game-patch replace the ones already on the command line (the
 *   emulator rejects a second --game-patch, and the preset is the user's explicit choice);
 * - env variables become --env NAME=VALUE after everything else, so they win over the same variable
 *   set earlier on the command line (src/main.cpp applies --env in order).
 */
object PresetLaunchArgs {
    const val FLAG_ENV = "--env"
    const val FLAG_RENDER_SCALE = "--render-scale"
    const val FLAG_GAME_PATCH = "--game-patch"

    /** One applied preset's options, with its game patch already placed at [gamePatchPath]. */
    data class Resolved(
        val presetId: String,
        val launch: PresetLaunch,
        val gamePatchPath: String?,
    )

    /** Arguments to append, plus what was dropped because an earlier preset set it already. */
    data class Plan(val args: List<String>, val conflicts: List<String>) {
        val isEmpty: Boolean get() = args.isEmpty()
    }

    fun plan(presets: List<Resolved>): Plan {
        val env = linkedMapOf<String, String>()
        var renderScale: Pair<String, Float>? = null
        var gamePatch: Pair<String, String>? = null
        val conflicts = mutableListOf<String>()
        val owners = mutableMapOf<String, String>()
        presets.forEach { preset ->
            preset.launch.env.forEach { (key, value) ->
                val owner = owners[key]
                if (owner == null) {
                    env[key] = value
                    owners[key] = preset.presetId
                } else if (env[key] != value) {
                    conflicts += "${preset.presetId}: $key already set by $owner"
                }
            }
            preset.launch.renderScale?.let { scale ->
                val current = renderScale
                if (current == null) renderScale = preset.presetId to scale
                else if (current.second != scale) conflicts += "${preset.presetId}: render scale already set by ${current.first}"
            }
            preset.gamePatchPath?.let { path ->
                val current = gamePatch
                if (current == null) gamePatch = preset.presetId to path
                else conflicts += "${preset.presetId}: game patch already set by ${current.first}"
            }
        }
        val args = env.flatMap { (key, value) -> listOf(FLAG_ENV, "$key=$value") } +
            renderScale?.let { listOf(FLAG_RENDER_SCALE, it.second.toString()) }.orEmpty() +
            gamePatch?.let { listOf(FLAG_GAME_PATCH, it.second) }.orEmpty()
        return Plan(args, conflicts)
    }

    /** [base] with [plan]'s arguments appended and the options they replace removed. */
    fun apply(base: Array<String>, plan: Plan): Array<String> {
        if (plan.isEmpty) return base
        val replaced = setOf(FLAG_RENDER_SCALE, FLAG_GAME_PATCH).filter { it in plan.args }.toSet()
        val kept = ArrayList<String>(base.size)
        var index = 0
        while (index < base.size) {
            if (base[index] in replaced) {
                index += 2 // the flag and its value
                continue
            }
            kept += base[index]
            index++
        }
        return (kept + plan.args).toTypedArray()
    }
}
