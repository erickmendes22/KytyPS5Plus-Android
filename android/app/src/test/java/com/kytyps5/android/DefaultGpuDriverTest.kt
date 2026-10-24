package com.kytyps5.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DefaultGpuDriverTest {

    private val turnip = BundledDriver.MAINLINE_TURNIP

    @Test
    fun parsesAdrenoModelFromGlRendererStrings() {
        assertEquals(830, AdrenoGpu.parseModel("Adreno (TM) 830"))
        assertEquals(740, AdrenoGpu.parseModel("ANGLE (Qualcomm, Adreno (TM) 740, OpenGL ES 3.2)"))
        assertEquals(810, AdrenoGpu.parseModel("Adreno 810"))
        assertNull(AdrenoGpu.parseModel("Mali-G715-Immortalis MC11"))
        assertNull(AdrenoGpu.parseModel(""))
        assertNull(AdrenoGpu.parseModel(null))
    }

    @Test
    fun bundledTurnipTargetsAdreno8xxOnly() {
        assertTrue(turnip.supports(830))
        assertTrue(turnip.supports(840))
        assertFalse(turnip.supports(740))
        assertFalse(turnip.supports(null))
    }

    @Test
    fun selectsBundledDriverOnceWhenGlobalIsSystem() {
        val decision = DefaultDriverPolicy.decide(
            adrenoModel = 830, bundle = turnip, appliedVersion = 0, currentGlobal = LauncherSettings.SYSTEM_DRIVER,
        )
        assertEquals(DefaultDriverPolicy.Decision.INSTALL_AND_SELECT, decision)
    }

    @Test
    fun keepsAnImportedGlobalDriverButStillInstallsTheBundle() {
        val decision = DefaultDriverPolicy.decide(
            adrenoModel = 830, bundle = turnip, appliedVersion = 0, currentGlobal = "turnip-gen8-v37-vulkan-1-4-359",
        )
        assertEquals(DefaultDriverPolicy.Decision.INSTALL_ONLY, decision)
    }

    @Test
    fun upgradesOriginalBundledV32ButKeepsUserChoice() {
        assertTrue(turnip.assetName.endsWith("turnip-gen8-v38-patched.zip"))
        assertEquals(2, turnip.version)
        assertEquals(
            DefaultDriverPolicy.Decision.INSTALL_AND_SELECT,
            DefaultDriverPolicy.decide(830, turnip, 1, "mainline-turnip-v32-vulkan-1-4-335"),
        )
        assertEquals(
            DefaultDriverPolicy.Decision.INSTALL_ONLY,
            DefaultDriverPolicy.decide(830, turnip, 1, LauncherSettings.SYSTEM_DRIVER),
        )
        assertEquals(
            DefaultDriverPolicy.Decision.INSTALL_ONLY,
            DefaultDriverPolicy.decide(830, turnip, 1, "turnip-gen8-v37-patched"),
        )
    }

    @Test
    fun doesNothingAfterTheBundleVersionWasApplied() {
        // The user may have switched back to the system driver since; that choice stays.
        val decision = DefaultDriverPolicy.decide(
            adrenoModel = 830, bundle = turnip, appliedVersion = turnip.version, currentGlobal = LauncherSettings.SYSTEM_DRIVER,
        )
        assertEquals(DefaultDriverPolicy.Decision.NOTHING, decision)
    }

    @Test
    fun doesNothingOnUnsupportedGpus() {
        assertEquals(
            DefaultDriverPolicy.Decision.NOTHING,
            DefaultDriverPolicy.decide(740, turnip, 0, LauncherSettings.SYSTEM_DRIVER),
        )
        assertEquals(
            DefaultDriverPolicy.Decision.NOTHING,
            DefaultDriverPolicy.decide(null, turnip, 0, LauncherSettings.SYSTEM_DRIVER),
        )
    }
}
