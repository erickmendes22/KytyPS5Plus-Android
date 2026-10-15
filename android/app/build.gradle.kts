plugins {
    id("com.android.application")
}

// SDL3's Java glue (SDLActivity and friends) is consumed straight from the SDL3 submodule.
val sdlJavaDir = rootProject.file("../3rdparty/SDL3/android-project/app/src/main/java")

// The emulator's native libraries from the NDK build (_Build/android at the repo root by default;
// override with -PkytyNativeDir=...). Staged into src/main/jniLibs/arm64-v8a by syncKytyNativeLibs
// so the normal jniLibs merge picks them up. Missing sources are not an error: GameActivity
// reports the missing library at runtime instead.
val kytyNativeDir: File = rootProject.file(
    providers.gradleProperty("kytyNativeDir").getOrElse("../_Build/android")
)
// SDL3 is built shared and must ship beside libmain.so (SDLActivity loads it first).
val kytyNativeLibs: Map<String, File> = mapOf(
    "libSDL3.so" to kytyNativeDir.resolve("3rdparty/SDL3/libSDL3.so"),
    "libmain.so" to kytyNativeDir.resolve("libmain.so"),
)
val kytyJniLibsDir: File = layout.projectDirectory.dir("src/main/jniLibs/arm64-v8a").asFile

val syncKytyNativeLibs = tasks.register("syncKytyNativeLibs") {
    description = "Copies the emulator's native libraries into src/main/jniLibs/arm64-v8a (if present)."
    kytyNativeLibs.forEach { (name, source) ->
        inputs.files(source).optional().withPropertyName("kytyNative_$name")
        outputs.file(kytyJniLibsDir.resolve(name))
    }
    doLast {
        kytyNativeLibs.forEach { (name, source) ->
            val staged = kytyJniLibsDir.resolve(name)
            if (source.isFile) {
                kytyJniLibsDir.mkdirs()
                source.copyTo(staged, overwrite = true)
                logger.lifecycle("Staged $name from ${source.path}")
            } else {
                staged.delete()
                logger.warn("$name not found at ${source.path}; building without it")
            }
        }
    }
}

tasks.named("preBuild") {
    dependsOn(syncKytyNativeLibs)
}

android {
    namespace = "com.kytyps5.android"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.kytyps5.android"
        minSdk = 31
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        // SDL's nativeRunMain dlopen()s libmain.so by its nativeLibraryDir path, so it must be extracted.
        jniLibs {
            useLegacyPackaging = true
        }
    }

    sourceSets {
        getByName("main") {
            java.directories.add(sdlJavaDir.path)
        }
    }

    testOptions {
        unitTests.isReturnDefaultValues = true
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20180813")
}
