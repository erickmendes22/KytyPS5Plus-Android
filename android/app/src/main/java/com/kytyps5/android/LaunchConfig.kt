package com.kytyps5.android

/** Output resolution the emulator renders at. Values are passed as --screen-width / --screen-height. */
enum class Resolution(
    val label: String,
    val width: Int,
    val height: Int,
    val storageValue: String,
) {
    HD_720(label = "720p", width = 1280, height = 720, storageValue = "720"),
    FULL_HD_1080(label = "1080p", width = 1920, height = 1080, storageValue = "1080");

    companion object {
        fun fromStorage(value: String?): Resolution =
            entries.firstOrNull { it.storageValue == value } ?: HD_720
    }
}

/**
 * Builds the emulator command line for one game. The emulator (src/main.cpp) reads
 * these flags starting at argv[1]; SDL prepends argv[0] itself.
 */
object LaunchArgs {
    /** Intent extra carrying the Array<String> handed to the emulator by GameActivity.getArguments(). */
    const val EXTRA_GAME_ARGS = "com.kytyps5.android.extra.GAME_ARGS"

    private const val FLAG_GAME = "--game"
    private const val FLAG_SCREEN_WIDTH = "--screen-width"
    private const val FLAG_SCREEN_HEIGHT = "--screen-height"

    fun forGame(gamePath: String, resolution: Resolution): Array<String> = arrayOf(
        FLAG_GAME, gamePath,
        FLAG_SCREEN_WIDTH, resolution.width.toString(),
        FLAG_SCREEN_HEIGHT, resolution.height.toString(),
    )
}
