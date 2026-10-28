package com.kytyps5.android

/** Evidence only: never infer a killer from low available RAM or an idle guest PC. */
internal object CrashEvidence {
    data class Exit(val pid: Int, val at: Long, val reason: Int, val signal: Int)
    fun matches(exit: Exit, pid: Int, started: Long, nextStart: Long = Long.MAX_VALUE): Boolean =
        exit.pid == pid && exit.at >= started && exit.at < nextStart

    fun conclusion(exit: Exit?, origin: String): String = when {
        exit == null -> "UNRESOLVED: no matching OS exit for this session; current session may still be alive."
        origin.lineSequence().any { it.startsWith("INTERNAL_SIGKILL_ATTEMPT ") } ->
            "INTERNAL_SIGNAL_ATTEMPT: intercepted syscall. Inspect target arguments and native/guest state; exit 190 is intentional diagnostic termination."
        exit.reason == 3 -> "OS_REPORTED_LOW_MEMORY: Android explicitly classified this session as a low-memory kill."
        exit.reason == 6 -> "OS_REPORTED_ANR: Android explicitly classified this session as an ANR."
        exit.reason == 5 -> "OS_REPORTED_NATIVE_CRASH: inspect matching tombstone/trace."
        exit.reason == 4 -> "OS_REPORTED_JAVA_CRASH: inspect matching exception/trace."
        exit.reason == 2 && exit.signal == 9 -> {
            val verified = origin.contains("COVERAGE tsync=all-current-threads future-threads=inherit probe=1") &&
                origin.contains("FEX_REGISTERED_THREAD_PROBE_COMPLETE")
            if (verified) "EXTERNAL_SIGKILL_UNATTRIBUTED: monitored internal send-signal syscalls were covered; sender and motive are NOT established. Inspect OS trace and correlated session evidence."
            else "SIGKILL_COVERAGE_UNVERIFIED: cannot exclude internal signal paths. Inspect installation errors."
        }
        else -> "OS_EXIT reason=${exit.reason} status=${exit.signal}; no more specific attribution is proven."
    }
}
