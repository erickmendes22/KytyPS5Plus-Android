package com.kytyps5.android

import android.content.Context
import java.io.File

/** The emulator's NDK library, packaged as libmain.so for arm64-v8a. */
object NativeLibrary {
    const val FILE_NAME = "libmain.so"

    const val MISSING_MESSAGE =
        "Native library missing: $FILE_NAME is not in this build. " +
            "Build the emulator to _Build/android/$FILE_NAME (or pass -PkytyNativeLib=<path>) and rebuild the app."

    /** With legacy packaging enabled the library is extracted to nativeLibraryDir. */
    fun isInstalled(context: Context): Boolean =
        File(context.applicationInfo.nativeLibraryDir, FILE_NAME).isFile
}
