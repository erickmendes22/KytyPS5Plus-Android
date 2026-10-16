package com.kytyps5.android

import java.io.File
import java.io.IOException
import java.io.InputStream
import java.util.zip.ZipInputStream
import org.json.JSONException
import org.json.JSONObject

/**
 * An imported custom Vulkan driver (e.g. Mesa Turnip). [directory] holds meta.json and the driver
 * library and is what the emulator receives as --gpu-driver.
 */
data class GpuDriver(
    val id: String,
    val name: String,
    val driverVersion: String,
    val description: String,
    val author: String,
    val libraryName: String,
    val directory: File,
) {
    val label: String get() = if (driverVersion.isBlank()) name else "$name — $driverVersion"
}

/** meta.json of the AdrenoTools driver package format used by Eden, Yuzu and Skyline. */
data class GpuDriverMeta(
    val name: String,
    val driverVersion: String,
    val description: String,
    val author: String,
    val libraryName: String,
) {
    companion object {
        const val FILE_NAME = "meta.json"

        /** Parses meta.json; null if it is not JSON or names no plain *.so library. */
        fun parse(json: String): GpuDriverMeta? {
            val obj = try {
                JSONObject(json)
            } catch (e: JSONException) {
                return null
            }
            val library = obj.stringOr("libraryName")
            if (!isPlainSoName(library)) return null
            return GpuDriverMeta(
                name = obj.stringOr("name").ifBlank { library },
                driverVersion = obj.stringOr("driverVersion"),
                description = obj.stringOr("description"),
                author = obj.stringOr("author"),
                libraryName = library,
            )
        }

        fun isPlainSoName(name: String): Boolean =
            name.isNotEmpty() && !name.contains('/') && !name.contains('\\') &&
                !name.startsWith(".") && name.endsWith(".so")
    }
}

sealed interface DriverImportResult {
    data class Imported(val driver: GpuDriver) : DriverImportResult
    data class Failure(val message: String) : DriverImportResult
}

/**
 * Imported drivers live in app-private storage (files/gpu_drivers/<id>/): the dynamic linker
 * refuses to load libraries from shared storage, so a driver must be copied in, never referenced.
 */
class GpuDriverStore(filesDir: File) {
    val root: File = File(filesDir, DIRECTORY_NAME)

    fun list(): List<GpuDriver> =
        (root.listFiles() ?: emptyArray())
            .filter { it.isDirectory && !it.name.startsWith(STAGING_PREFIX) }
            .mapNotNull(::load)
            .sortedBy { it.label.lowercase() }

    fun find(id: String): GpuDriver? =
        if (isSafeId(id)) File(root, id).takeIf { it.isDirectory }?.let(::load) else null

    fun delete(id: String): Boolean = isSafeId(id) && File(root, id).deleteRecursively()

    /** Unpacks a driver .zip (meta.json + library) into the store, replacing a same-named import. */
    fun import(zip: InputStream): DriverImportResult {
        if (!root.isDirectory && !root.mkdirs()) {
            return DriverImportResult.Failure("Cannot create ${root.path}")
        }
        val staging = File(root, STAGING_PREFIX + System.nanoTime())
        return try {
            unzip(zip, staging)
            install(staging)
        } catch (e: IOException) {
            DriverImportResult.Failure("Could not read the driver package: ${e.message}")
        } finally {
            staging.deleteRecursively()
        }
    }

    private fun install(staging: File): DriverImportResult {
        val metaFile = File(staging, GpuDriverMeta.FILE_NAME)
        if (!metaFile.isFile) {
            return DriverImportResult.Failure("Not a driver package: no ${GpuDriverMeta.FILE_NAME} in the zip.")
        }
        val meta = GpuDriverMeta.parse(metaFile.readText())
            ?: return DriverImportResult.Failure("${GpuDriverMeta.FILE_NAME} has no valid libraryName.")
        if (!File(staging, meta.libraryName).isFile) {
            return DriverImportResult.Failure("The zip does not contain ${meta.libraryName}.")
        }
        val id = idFor(meta)
        val target = File(root, id)
        target.deleteRecursively()
        if (!staging.renameTo(target)) {
            return DriverImportResult.Failure("Could not move the driver into ${target.path}")
        }
        return load(target)?.let { DriverImportResult.Imported(it) }
            ?: DriverImportResult.Failure("Imported driver could not be read back.")
    }

    private fun unzip(zip: InputStream, into: File) {
        if (!into.mkdirs()) throw IOException("cannot create ${into.path}")
        var total = 0L
        ZipInputStream(zip.buffered()).use { stream ->
            while (true) {
                val entry = stream.nextEntry ?: break
                // Packages are flat: keep only top-level files, which also rules out zip-slip paths.
                val name = entry.name.substringAfterLast('/')
                if (entry.isDirectory || name.isEmpty() || name.startsWith(".") || '\\' in name) continue
                File(into, name).outputStream().use { out ->
                    val buffer = ByteArray(BUFFER_SIZE)
                    while (true) {
                        val read = stream.read(buffer)
                        if (read < 0) break
                        total += read
                        if (total > MAX_PACKAGE_BYTES) throw IOException("package is larger than ${MAX_PACKAGE_BYTES shr 20} MiB")
                        out.write(buffer, 0, read)
                    }
                }
            }
        }
    }

    private fun load(directory: File): GpuDriver? {
        val metaFile = File(directory, GpuDriverMeta.FILE_NAME)
        val meta = runCatching { metaFile.readText() }.getOrNull()?.let(GpuDriverMeta::parse) ?: return null
        if (!File(directory, meta.libraryName).isFile) return null
        return GpuDriver(
            id = directory.name,
            name = meta.name,
            driverVersion = meta.driverVersion,
            description = meta.description,
            author = meta.author,
            libraryName = meta.libraryName,
            directory = directory,
        )
    }

    companion object {
        const val DIRECTORY_NAME = "gpu_drivers"
        private const val STAGING_PREFIX = ".import-"
        private const val BUFFER_SIZE = 64 * 1024
        private const val MAX_PACKAGE_BYTES = 256L shl 20

        /** Stable directory name from the package's name and version, e.g. "mainline-turnip-v32-vulkan-1-4-335". */
        fun idFor(meta: GpuDriverMeta): String {
            val slug = "${meta.name}-${meta.driverVersion}".lowercase()
                .replace(Regex("[^a-z0-9]+"), "-")
                .trim('-')
                .take(MAX_ID_LENGTH)
            return slug.ifEmpty { "driver" }
        }

        fun isSafeId(id: String): Boolean = id.isNotEmpty() && Regex("^[a-z0-9-]+$").matches(id)

        private const val MAX_ID_LENGTH = 64
    }
}
