package com.kytyps5.android.drivers

import java.io.File
import java.io.IOException
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

sealed interface DownloadResult {
    /** [verified]: true = matches GitHub's published sha256, null = GitHub published none. */
    data class Done(val file: File, val sha256: String, val verified: Boolean?) : DownloadResult
    data class Failure(val message: String) : DownloadResult
}

/**
 * Downloads a release asset into [dir] while hashing it. A published digest that does not match
 * deletes the file; the caller installs [DownloadResult.Done.file] through GpuDriverStore.import.
 */
class DriverDownloader(
    private val dir: File,
    private val maxBytes: Long = MAX_BYTES,
    private val open: (String) -> InputStream = ::openHttps,
) {
    fun download(
        asset: DriverAsset,
        onProgress: (done: Long, total: Long) -> Unit = { _, _ -> },
        isCancelled: () -> Boolean = { false },
    ): DownloadResult {
        if (!GitHubReleases.isTrustedDownloadUrl(asset.downloadUrl)) {
            return DownloadResult.Failure("Refusing to download from ${asset.downloadUrl}")
        }
        if (!dir.isDirectory && !dir.mkdirs()) return DownloadResult.Failure("Cannot create ${dir.path}")
        val safeName = asset.name.replace(Regex("[^A-Za-z0-9_.-]"), "_")
        val partial = File(dir, "$safeName.part")
        val target = File(dir, safeName)
        val digest = MessageDigest.getInstance("SHA-256")
        return try {
            open(asset.downloadUrl).use { input ->
                partial.outputStream().use { output -> copy(input, output::write, digest, asset.size, onProgress, isCancelled) }
            }
            val sha = digest.digest().joinToString("") { "%02x".format(it) }
            val verified = asset.sha256?.let { it == sha }
            if (verified == false) {
                partial.delete()
                return DownloadResult.Failure("checksum mismatch: got $sha, GitHub published ${asset.sha256}")
            }
            target.delete()
            if (!partial.renameTo(target)) {
                partial.delete()
                return DownloadResult.Failure("Could not save ${target.path}")
            }
            DownloadResult.Done(target, sha, verified)
        } catch (e: IOException) {
            partial.delete()
            DownloadResult.Failure("Download failed: ${e.message}")
        }
    }

    private fun copy(
        input: InputStream,
        write: (ByteArray, Int, Int) -> Unit,
        digest: MessageDigest,
        total: Long,
        onProgress: (Long, Long) -> Unit,
        isCancelled: () -> Boolean,
    ) {
        val buffer = ByteArray(BUFFER_SIZE)
        var done = 0L
        while (true) {
            if (isCancelled()) throw IOException("cancelled")
            val read = input.read(buffer)
            if (read < 0) break
            done += read
            if (done > maxBytes) throw IOException("package is larger than ${maxBytes shr 20} MiB")
            digest.update(buffer, 0, read)
            write(buffer, 0, read)
            onProgress(done, total)
        }
    }

    companion object {
        private const val BUFFER_SIZE = 64 * 1024
        private const val MAX_BYTES = 256L shl 20
        private const val TIMEOUT_MS = 30_000

        /** HTTPS GET following GitHub's redirect to its asset CDN (https to https only). */
        fun openHttps(url: String): InputStream {
            val connection = URL(url).openConnection() as HttpURLConnection
            connection.connectTimeout = TIMEOUT_MS
            connection.readTimeout = TIMEOUT_MS
            connection.instanceFollowRedirects = true
            connection.setRequestProperty("User-Agent", "KytyPS5-Android-Launcher")
            val code = connection.responseCode
            if (code != HttpURLConnection.HTTP_OK) {
                connection.disconnect()
                throw IOException("HTTP $code")
            }
            if (connection.url.protocol != "https") {
                connection.disconnect()
                throw IOException("redirected to a non-HTTPS URL")
            }
            return connection.inputStream
        }
    }
}
