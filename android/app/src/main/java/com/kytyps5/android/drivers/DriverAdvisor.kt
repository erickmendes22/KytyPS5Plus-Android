package com.kytyps5.android.drivers

/**
 * Per-GPU hints for driver packages, from their release tag and file name. Rules follow the community
 * guidance collected in research/frontends.md (pocket-gaming.org guide + release notes, 2026-10):
 * Adreno 8xx (8 Elite) wants Turnip Gen8 V32+, Mainline Turnip v32+ or v26.2+ "A8XX" builds and must
 * avoid legacy v25 builds and Qualcomm packages made for Adreno 840; Adreno 7xx must avoid Gen8 builds.
 */
object DriverAdvisor {

    enum class Verdict(val rank: Int, val badge: String) {
        RECOMMENDED(0, "★ Recommended"),
        UNKNOWN(1, ""),
        AVOID(2, "⚠ Not for this GPU"),
    }

    data class Advice(val verdict: Verdict, val reason: String)

    private const val MIN_GEN8_VERSION = 32
    private const val MIN_MAINLINE_VERSION = 32

    private val GEN8 = Regex("""gen8[_-]v(\d+)""")
    private val MAINLINE = Regex("""mainline-turnip(?:-sync)?-v(\d+)""")
    private val A8XX = Regex("""a8xx""")
    private val A7XX = Regex("""a7xx""")
    private val TURNIP_V26_MINOR = Regex("""turnip_v26\.(\d+)""")
    private val LEGACY_V25 = Regex("""(?:^|[^0-9.])v25\.""")
    private val QUALCOMM_A840 = Regex("""qualcomm_84\d|(?:^|\s)v84\d(?:\s|$)""")

    fun advise(adrenoModel: Int?, tag: String, assetName: String): Advice {
        val text = "$tag $assetName".lowercase()
        return when (adrenoModel) {
            null -> Advice(Verdict.UNKNOWN, "GPU not detected.")
            in 800..899 -> adviseGen8(text)
            in 700..799 -> adviseGen7(text)
            else -> Advice(Verdict.UNKNOWN, "No recommendations for Adreno $adrenoModel.")
        }
    }

    private fun adviseGen8(text: String): Advice {
        GEN8.find(text)?.let { match ->
            val version = match.groupValues[1].toInt()
            return if (version >= MIN_GEN8_VERSION) {
                Advice(Verdict.RECOMMENDED, "Turnip Gen8 V$version supports Adreno 8xx.")
            } else {
                Advice(Verdict.AVOID, "Turnip Gen8 before V$MIN_GEN8_VERSION has known Adreno 830 issues.")
            }
        }
        MAINLINE.find(text)?.let { match ->
            val version = match.groupValues[1].toInt()
            return if (version >= MIN_MAINLINE_VERSION) {
                Advice(Verdict.RECOMMENDED, "Mainline Turnip v$version (gen8 branch) supports Adreno 8xx.")
            } else {
                Advice(Verdict.AVOID, "Older than the bundled Mainline Turnip v$MIN_MAINLINE_VERSION.")
            }
        }
        return when {
            A7XX.containsMatchIn(text) -> Advice(Verdict.AVOID, "Built for Adreno 7xx.")
            QUALCOMM_A840.containsMatchIn(text) -> Advice(Verdict.AVOID, "Qualcomm driver for Adreno 840, not 830.")
            LEGACY_V25.containsMatchIn(text) ->
                Advice(Verdict.AVOID, "Legacy v25 builds black-screen on Adreno 8xx (\"Unsupported GPU Architecture\").")
            A8XX.containsMatchIn(text) -> Advice(Verdict.RECOMMENDED, "Built for Adreno 8xx.")
            (TURNIP_V26_MINOR.find(text)?.groupValues?.get(1)?.toInt() ?: -1) >= 2 ->
                Advice(Verdict.RECOMMENDED, "Turnip v26.2+ supports Adreno 8xx.")
            else -> Advice(Verdict.UNKNOWN, "Untested on Adreno 8xx.")
        }
    }

    private fun adviseGen7(text: String): Advice = when {
        GEN8.containsMatchIn(text) || A8XX.containsMatchIn(text) || MAINLINE.containsMatchIn(text) ->
            Advice(Verdict.AVOID, "Built for Adreno 8xx (gen8).")
        A7XX.containsMatchIn(text) -> Advice(Verdict.RECOMMENDED, "Built for Adreno 7xx.")
        Regex("""turnip_v2[5-9]\.""").containsMatchIn(text) -> Advice(Verdict.RECOMMENDED, "Turnip v25+ supports Adreno 7xx.")
        else -> Advice(Verdict.UNKNOWN, "Untested on Adreno 7xx.")
    }
}
