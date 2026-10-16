package com.kytyps5.android.drivers

import com.kytyps5.android.drivers.DriverAdvisor.Verdict
import org.junit.Assert.assertEquals
import org.junit.Test

class DriverAdvisorTest {

    private fun verdict(model: Int?, tag: String, asset: String) =
        DriverAdvisor.advise(model, tag, asset).verdict

    @Test
    fun adreno8xxPrefersGen8AndMainlineV32Plus() {
        assertEquals(Verdict.RECOMMENDED, verdict(830, "v37", "Turnip_Gen8_V37.zip"))
        assertEquals(Verdict.RECOMMENDED, verdict(830, "v32", "Turnip_Gen8_V32.zip"))
        assertEquals(Verdict.RECOMMENDED, verdict(830, "tu_v32", "mainline-turnip-V32.zip"))
        assertEquals(Verdict.RECOMMENDED, verdict(830, "tu_v33", "mainline-turnip-sync-V33.zip"))
        assertEquals(Verdict.RECOMMENDED, verdict(830, "v26.0.0-rc08", "turnip_a8xx.zip"))
        assertEquals(Verdict.RECOMMENDED, verdict(830, "v37", "Turnip_v26.3.0-R6-Oneui.zip"))
    }

    @Test
    fun adreno8xxAvoidsOldOrWrongGenerationBuilds() {
        assertEquals(Verdict.AVOID, verdict(830, "tu_v30", "mainline-turnip-V30.zip"))
        assertEquals(Verdict.AVOID, verdict(830, "v31", "Turnip_Gen8_V31.zip"))
        assertEquals(Verdict.AVOID, verdict(830, "v25.1.0", "Turnip_v25.1.0_R3.zip"))
        assertEquals(Verdict.AVOID, verdict(830, "v863.1_a7xx", "v863.1.zip"))
        assertEquals(Verdict.AVOID, verdict(830, "v840", "Qualcomm_840_adpkg.zip"))
        assertEquals(Verdict.AVOID, verdict(830, "v849", "Qualcomm_849_adpkg.zip"))
    }

    @Test
    fun adreno840MayUseItsOwnQualcommDriverAndHostileTagsDoNotCrash() {
        assertEquals(Verdict.UNKNOWN, verdict(840, "v840", "Qualcomm_840_adpkg.zip"))
        assertEquals(Verdict.AVOID, verdict(830, "v99999999999", "Turnip_Gen8_V99999999999.zip"))
    }

    @Test
    fun adreno8xxTreatsOtherPackagesAsUntested() {
        assertEquals(Verdict.UNKNOWN, verdict(830, "vturnip_mrpurple_T30", "turnip_mrpurple_T30-toasted.adpkg.zip"))
        assertEquals(Verdict.UNKNOWN, verdict(830, "gamesir", "Turnip-Rutabaga-v3.zip"))
    }

    @Test
    fun adreno7xxAvoidsGen8Builds() {
        assertEquals(Verdict.AVOID, verdict(740, "v37", "Turnip_Gen8_V37.zip"))
        assertEquals(Verdict.AVOID, verdict(740, "v26.0.0-rc08", "turnip_a8xx.zip"))
        assertEquals(Verdict.RECOMMENDED, verdict(740, "v26.0.0-rc07", "Turnip_v26.0.0_R7.zip"))
        assertEquals(Verdict.RECOMMENDED, verdict(740, "v863.1_a7xx", "v863.1.zip"))
    }

    @Test
    fun unknownGpuGetsNoAdvice() {
        assertEquals(Verdict.UNKNOWN, verdict(null, "v37", "Turnip_Gen8_V37.zip"))
        assertEquals(Verdict.UNKNOWN, verdict(650, "v37", "Turnip_Gen8_V37.zip"))
    }

    @Test
    fun sortOrderPutsRecommendedFirst() {
        val names = listOf("a.zip", "Turnip_Gen8_V37.zip", "Qualcomm_840_adpkg.zip")
        val sorted = names.sortedBy { DriverAdvisor.advise(830, "x", it).verdict.rank }
        assertEquals(listOf("Turnip_Gen8_V37.zip", "a.zip", "Qualcomm_840_adpkg.zip"), sorted)
    }
}
