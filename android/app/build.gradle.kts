import java.io.IOException
import java.net.URI
import java.security.MessageDigest

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
// libadrenotools' hook libraries are dlopen()ed from nativeLibraryDir when --gpu-driver selects a
// custom Vulkan driver.
val adrenotoolsHookDir: File = kytyNativeDir.resolve("3rdparty/libadrenotools/src/hook")
val kytyNativeLibs: Map<String, File> = mapOf(
    "libSDL3.so" to kytyNativeDir.resolve("3rdparty/SDL3/libSDL3.so"),
    "libmain.so" to kytyNativeDir.resolve("libmain.so"),
) + listOf("libhook_impl.so", "libmain_hook.so", "libfile_redirect_hook.so", "libgsl_alloc_hook.so")
    .associateWith { adrenotoolsHookDir.resolve(it) }
val kytyJniLibsDir: File = layout.projectDirectory.dir("src/main/jniLibs/arm64-v8a").asFile

// Khronos validation layer for KYTY_VK_VALIDATION runs, packaged only on request:
// -PkytyValidationLayer=<path to the arm64-v8a libVkLayer_khronos_validation.so> (Android loads
// layers from the app's own nativeLibraryDir). Without the property a staged copy is removed, so
// normal builds never ship it.
val validationLayerName = "libVkLayer_khronos_validation.so"
val validationLayer: File? = providers.gradleProperty("kytyValidationLayer").orNull?.let { file(it) }

val syncKytyNativeLibs = tasks.register("syncKytyNativeLibs") {
    description = "Copies the emulator's native libraries into src/main/jniLibs/arm64-v8a (if present)."
    kytyNativeLibs.forEach { (name, source) ->
        inputs.files(source).optional().withPropertyName("kytyNative_$name")
        outputs.file(kytyJniLibsDir.resolve(name))
    }
    inputs.property("kytyValidationLayer", validationLayer?.path ?: "")
    validationLayer?.let { inputs.file(it).withPropertyName("kytyValidationLayerFile") }
    outputs.file(kytyJniLibsDir.resolve(validationLayerName))
    doLast {
        val layer = validationLayer
        val stagedLayer = kytyJniLibsDir.resolve(validationLayerName)
        if (layer == null) {
            stagedLayer.delete()
        } else {
            if (!layer.isFile) {
                throw GradleException("kytyValidationLayer: ${layer.path} is not a file")
            }
            kytyJniLibsDir.mkdirs()
            layer.copyTo(stagedLayer, overwrite = true)
            logger.lifecycle("Staged $validationLayerName from ${layer.path}")
        }
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

// GPU driver packages bundled as assets (DefaultGpuDriverSetup installs them on supported GPUs). They are
// downloaded at build time and pinned by sha256 so no binary is committed; -PkytyBundleDrivers=false skips
// them, and a failed download only warns (the app then falls back to the system driver).
data class BundledDriverPackage(val fileName: String, val url: String, val sha256: String)

val bundledDriverPackages = listOf(
    BundledDriverPackage(
        fileName = "turnip-gen8-v38-patched.zip",
        url = "https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/download/v27.0.0-R1/Turnip_Gen8_V38_Patched.zip",
        sha256 = "2bbb279a3da7680503a54a51a3051c7a4302c8fb2a089f8b183b4b5d6ba9ac17",
    ),
    // Known-good Astro Bot alternative: install alongside V38, never select over
    // a user-chosen driver automatically. Preserves compatibility while V38 evolves.
    BundledDriverPackage(
        fileName = "mainline-turnip-v32.zip",
        url = "https://github.com/whitebelyash/AdrenoToolsDrivers/releases/download/tu_v32/mainline-turnip-V32.zip",
        sha256 = "a40b086926916e3960c832fce3014c26ec946b956c46681a726d927e2700ad10",
    ),
)
val bundleDrivers: Boolean = providers.gradleProperty("kytyBundleDrivers").getOrElse("true").toBoolean()
val bundledDriverAssetsDir: File = layout.buildDirectory.dir("generated/bundledDriverAssets").get().asFile
val bundledDriverDir: File = bundledDriverAssetsDir.resolve("gpu_drivers")

fun sha256Of(file: File): String = MessageDigest.getInstance("SHA-256")
    .digest(file.readBytes())
    .joinToString("") { "%02x".format(it) }

val fetchBundledDrivers = tasks.register("fetchBundledDrivers") {
    description = "Downloads the checksum-pinned GPU driver packages bundled into the APK's assets."
    outputs.dir(bundledDriverDir)
    onlyIf { bundleDrivers }
    doLast {
        bundledDriverDir.mkdirs()
        bundledDriverPackages.forEach { pkg ->
            val target = bundledDriverDir.resolve(pkg.fileName)
            if (target.isFile && sha256Of(target) == pkg.sha256) return@forEach
            val partial = bundledDriverDir.resolve(pkg.fileName + ".part")
            try {
                URI(pkg.url).toURL().openStream().use { input ->
                    partial.outputStream().use { input.copyTo(it) }
                }
                val actual = sha256Of(partial)
                if (actual != pkg.sha256) {
                    throw GradleException("${pkg.fileName}: sha256 $actual does not match the pinned ${pkg.sha256}")
                }
                partial.renameTo(target)
                logger.lifecycle("Bundled ${pkg.fileName}")
            } catch (e: IOException) {
                target.delete()
                logger.warn("Could not download ${pkg.url} (${e.message}); building without ${pkg.fileName}")
            } finally {
                partial.delete()
            }
        }
    }
}

tasks.named("preBuild") {
    dependsOn(syncKytyNativeLibs)
    dependsOn(fetchBundledDrivers)
}

android {
    namespace = "com.kytyps5.android"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.kytyps5.android"
        minSdk = 31
        targetSdk = 35
        // CI passes -PkytyVersionCode=<run number> -PkytyVersionName=<tag or 0.1.0-nightly.N> so every
        // published APK installs over the previous one.
        versionCode = providers.gradleProperty("kytyVersionCode").getOrElse("1").toInt()
        versionName = providers.gradleProperty("kytyVersionName").getOrElse("0.1.0")

        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    // Release signing from the environment (CI secrets); without KYTY_KEYSTORE_FILE the release APK
    // is left unsigned and local builds keep using the debug key.
    val releaseKeystore = System.getenv("KYTY_KEYSTORE_FILE")?.takeIf { it.isNotEmpty() }?.let { file(it) }
    signingConfigs {
        if (releaseKeystore != null) {
            create("release") {
                storeFile = releaseKeystore
                storePassword = System.getenv("KYTY_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("KYTY_KEY_ALIAS")
                keyPassword = System.getenv("KYTY_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // A fresh fork does not inherit the upstream GitHub Actions signing secrets.
            // Fall back to an installable debug-signed APK rather than publishing an unsigned APK.
            // Configure KYTY_* signing secrets for stable upgrade-compatible releases.
            signingConfig = if (releaseKeystore != null) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
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
            assets.directories.add(bundledDriverAssetsDir.path)
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
