package com.kytyps5.android

import android.system.ErrnoException
import android.system.Os
import android.util.Log
import com.kytyps5.android.library.EnvVars

/**
 * Applies a game's environment variables to this process before SDL loads the emulator, so Mesa
 * Turnip and the emulator read them at startup (e.g. TU_DEBUG when the Vulkan driver loads).
 * SDLActivity normally restarts the process between games; if the process is reused anyway,
 * variables set for a previous game are unset first.
 */
object GameEnvironment {
    private const val TAG = "KytyEnv"
    private var appliedKeys: Set<String> = emptySet()
    // Native --env presets are applied after this object and are not in appliedKeys.
    // Clear their scoped knobs as well when SDL reuses the process for another launch.
    internal val silentHillPresetKeys = setOf(
        "KYTY_SH_SCREEN_PERCENTAGE", "KYTY_SH_NO_SHADOWS", "KYTY_SH_NO_FOG", "KYTY_SH_NO_SSR",
        "KYTY_SH_NO_AO", "KYTY_SH_NO_BLOOM", "KYTY_SH_NO_DOF", "KYTY_SH_NO_MOTION_BLUR", "KYTY_SH_NO_RT",
    )

    // These two native --env presets must not leak into another game when
    // Android happens to reuse SDL's process after a launch.
    internal val granTurismoPresetKeys = setOf(
        "KYTY_GT_LARGE_DEPTH_HALF", "KYTY_GT_HDR_HALF",
        "KYTY_GT_TURNIP_VISUAL_RESTORE",
    )
    // Native UFC6 --env flags are title-specific. A reused SDL process
    // must not retain CPU, descriptor, fan policy or profiling settings
    // when launching a different game, particularly Demon's Souls.
    internal val ufc6PlacementKeys = setOf(
        "KYTY_CPU_PLACE", "KYTY_UFC6_HD_OUTPUT",
        "KYTY_UFC6_DESCRIPTOR_REUSE", "KYTY_UFC6_PERF",
        "KYTY_UFC6_SUBMIT_BATCH",
        "KYTY_FAN_AUTO_ON_US", "KYTY_FAN_AUTO_OFF_US",
    )

    fun apply(lines: Array<String>?) {
        val vars = EnvVars.parse(lines?.joinToString("\n").orEmpty()).vars
        val (unset, set) = EnvVars.plan(appliedKeys, vars)
        (unset + silentHillPresetKeys + granTurismoPresetKeys + ufc6PlacementKeys).distinct().forEach { key ->
            try {
                Os.unsetenv(key)
            } catch (e: ErrnoException) {
                Log.w(TAG, "unsetenv $key failed", e)
            }
        }
        set.forEach { v ->
            try {
                Os.setenv(v.key, v.value, true)
                Log.i(TAG, "env ${v.key}=${v.value}")
            } catch (e: ErrnoException) {
                Log.w(TAG, "setenv ${v.key} failed", e)
            }
        }
        appliedKeys = set.map { it.key }.toSet()
    }
}
