package com.kytyps5.android

import org.junit.Assert.*
import org.junit.Test

class CrashEvidenceTest {
    private val coverage = "COVERAGE tsync=all-current-threads future-threads=inherit probe=1\nFEX_REGISTERED_THREAD_PROBE_COMPLETE\n"
    @Test fun matchesExactProcessAndSession() {
        assertTrue(CrashEvidence.matches(CrashEvidence.Exit(12, 200, 2, 9), 12, 100))
        assertFalse(CrashEvidence.matches(CrashEvidence.Exit(13, 200, 2, 9), 12, 100))
        assertFalse(CrashEvidence.matches(CrashEvidence.Exit(12, 99, 2, 9), 12, 100))
        assertFalse(CrashEvidence.matches(CrashEvidence.Exit(12, 300, 2, 9), 12, 100, 250))
    }
    @Test fun selfTestAndZeroGuestPcAreNotAFaultOrInternalKill() {
        val origin = coverage + "PROBE_PASS syscall=0x81\nFEX_SAVED_GUEST_STATE rip=0x0\n"
        val result = CrashEvidence.conclusion(CrashEvidence.Exit(12, 200, 2, 9), origin)
        assertTrue(result.startsWith("EXTERNAL_SIGKILL_UNATTRIBUTED:"))
        assertFalse(result.contains("OS_REPORTED_LOW_MEMORY"))
    }
    @Test fun failedCoverageDoesNotExcludeInternalKill() {
        assertTrue(CrashEvidence.conclusion(CrashEvidence.Exit(12, 200, 2, 9), "INSTALL_FAILED tsync=-1").startsWith("SIGKILL_COVERAGE_UNVERIFIED:"))
    }
    @Test fun explicitOsReasonsAreKeptSeparate() {
        assertTrue(CrashEvidence.conclusion(CrashEvidence.Exit(12, 200, 3, 9), coverage).startsWith("OS_REPORTED_LOW_MEMORY:"))
        assertTrue(CrashEvidence.conclusion(CrashEvidence.Exit(12, 200, 6, 0), coverage).startsWith("OS_REPORTED_ANR:"))
        assertTrue(CrashEvidence.conclusion(CrashEvidence.Exit(12, 200, 5, 11), coverage).startsWith("OS_REPORTED_NATIVE_CRASH:"))
        assertTrue(CrashEvidence.conclusion(null, coverage).startsWith("UNRESOLVED:"))
    }
    @Test fun internalAttemptMustBeAnActualEventLine() {
        assertFalse(CrashEvidence.conclusion(CrashEvidence.Exit(12, 200, 2, 9), "If no INTERNAL_SIGKILL_ATTEMPT is present...").startsWith("INTERNAL_SIGNAL_ATTEMPT:"))
        assertTrue(CrashEvidence.conclusion(CrashEvidence.Exit(12, 200, 1, 190), coverage + "INTERNAL_SIGKILL_ATTEMPT pid=0xc syscall=0x81\n").startsWith("INTERNAL_SIGNAL_ATTEMPT:"))
    }
}
