package com.kytyps5.android.drivers

import java.io.ByteArrayInputStream
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.nio.file.Files
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class DriverDownloaderTest {

    private lateinit var dir: File
    private val payload = "hello driver".toByteArray()
    private val wrongSha = "0".repeat(64)

    private lateinit var realSha: String

    @Before
    fun setUp() {
        dir = Files.createTempDirectory("dl").toFile()
        realSha = java.security.MessageDigest.getInstance("SHA-256").digest(payload)
            .joinToString("") { "%02x".format(it) }
    }

    @After
    fun tearDown() {
        dir.deleteRecursively()
    }

    private fun asset(sha: String?) = DriverAsset(
        name = "Turnip_Gen8_V37.zip",
        size = payload.size.toLong(),
        downloadUrl = "https://github.com/o/r/releases/download/t/Turnip_Gen8_V37.zip",
        sha256 = sha,
    )

    private fun downloader(stream: () -> InputStream = { ByteArrayInputStream(payload) }) =
        DriverDownloader(dir) { stream() }

    @Test
    fun downloadComputesSha256AndVerifiesPublishedDigest() {
        val progress = mutableListOf<Long>()
        val result = downloader().download(asset(realSha), onProgress = { done, _ -> progress += done }) as DownloadResult.Done

        assertEquals(realSha, result.sha256)
        assertEquals(true, result.verified)
        assertTrue(result.file.isFile)
        assertEquals(payload.size.toLong(), progress.last())
    }

    @Test
    fun missingDigestIsReportedAsUnverified() {
        val result = downloader().download(asset(null)) as DownloadResult.Done
        assertNull(result.verified)
        assertEquals(realSha, result.sha256)
    }

    @Test
    fun digestMismatchFailsAndDeletesTheFile() {
        val result = downloader().download(asset(wrongSha))
        assertTrue(result is DownloadResult.Failure)
        assertTrue((result as DownloadResult.Failure).message.contains("checksum"))
        assertTrue(dir.listFiles().orEmpty().isEmpty())
    }

    @Test
    fun untrustedUrlIsRejectedWithoutOpening() {
        var opened = false
        val result = DriverDownloader(dir) { opened = true; ByteArrayInputStream(payload) }
            .download(asset(null).copy(downloadUrl = "https://example.com/x.zip"))
        assertTrue(result is DownloadResult.Failure)
        assertFalse(opened)
    }

    @Test
    fun ioErrorBecomesFailure() {
        val result = downloader { throw IOException("boom") }.download(asset(null))
        assertTrue((result as DownloadResult.Failure).message.contains("boom"))
    }

    @Test
    fun cancellationStopsAndCleansUp() {
        val result = downloader().download(asset(null), isCancelled = { true })
        assertTrue(result is DownloadResult.Failure)
        assertTrue(dir.listFiles().orEmpty().none { it.isFile })
    }

    @Test
    fun truncatedDownloadIsRejected() {
        val result = downloader().download(asset(null).copy(size = payload.size + 10L))
        assertTrue((result as DownloadResult.Failure).message.contains("incomplete"))
    }

    @Test
    fun finalUrlMustStayOnGitHub() {
        assertTrue(DriverDownloader.isTrustedFinalUrl(java.net.URL("https://objects.githubusercontent.com/x")))
        assertTrue(DriverDownloader.isTrustedFinalUrl(java.net.URL("https://github.com/o/r/releases/download/t/a.zip")))
        assertFalse(DriverDownloader.isTrustedFinalUrl(java.net.URL("https://evil.com/githubusercontent.com")))
        assertFalse(DriverDownloader.isTrustedFinalUrl(java.net.URL("http://objects.githubusercontent.com/x")))
    }

    @Test
    fun oversizedDownloadIsRejected() {
        val result = DriverDownloader(dir, maxBytes = 4) { ByteArrayInputStream(payload) }.download(asset(null))
        assertTrue((result as DownloadResult.Failure).message.contains("larger"))
    }
}
