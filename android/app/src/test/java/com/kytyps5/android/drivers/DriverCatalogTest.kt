package com.kytyps5.android.drivers

import java.io.File
import java.nio.file.Files
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class DriverCatalogTest {

    private lateinit var dir: File
    private var now = 1_000_000L
    private val repo = DriverRepo("whitebelyash", "AdrenoToolsDrivers", "whitebelyash")

    private val body = """
        [{"tag_name":"tu_v32","name":"Mainline Turnip v32","published_at":"2026-09-19T13:30:05Z",
          "assets":[{"name":"mainline-turnip-V32.zip","size":1,
                     "browser_download_url":"https://github.com/whitebelyash/AdrenoToolsDrivers/releases/download/tu_v32/mainline-turnip-V32.zip"}]}]
    """.trimIndent()

    /** Records requests and answers with [next]. */
    private inner class FakeSource(var next: FetchResult) : ReleaseSource {
        val etags = mutableListOf<String?>()
        override fun fetch(repo: DriverRepo, etag: String?): FetchResult {
            etags += etag
            return next
        }
    }

    @Before
    fun setUp() {
        dir = Files.createTempDirectory("catalog").toFile()
    }

    @After
    fun tearDown() {
        dir.deleteRecursively()
    }

    private fun catalog(source: ReleaseSource) =
        DriverCatalog(ReleaseCache(dir), source, clock = { now })

    @Test
    fun firstFetchHitsNetworkAndCaches() {
        val source = FakeSource(FetchResult.Ok(body, etag = "\"e1\""))
        val result = catalog(source).releases(repo) as CatalogResult.Releases

        assertEquals("tu_v32", result.releases.single().tag)
        assertFalse(result.fromCache)
        assertEquals(listOf<String?>(null), source.etags)
        assertEquals("\"e1\"", ReleaseCache(dir).read(repo)?.etag)
    }

    @Test
    fun freshCacheAvoidsTheNetwork() {
        val source = FakeSource(FetchResult.Ok(body, etag = "\"e1\""))
        catalog(source).releases(repo)
        now += DriverCatalog.DEFAULT_TTL_MS - 1
        val result = catalog(source).releases(repo) as CatalogResult.Releases

        assertTrue(result.fromCache)
        assertEquals(1, source.etags.size)
    }

    @Test
    fun staleCacheRevalidatesWithEtagAndKeepsBodyOnNotModified() {
        val source = FakeSource(FetchResult.Ok(body, etag = "\"e1\""))
        catalog(source).releases(repo)
        now += DriverCatalog.DEFAULT_TTL_MS + 1
        source.next = FetchResult.NotModified
        val result = catalog(source).releases(repo) as CatalogResult.Releases

        assertEquals(listOf<String?>(null, "\"e1\""), source.etags)
        assertEquals("tu_v32", result.releases.single().tag)
        assertEquals(now, ReleaseCache(dir).read(repo)?.fetchedAtMs)
    }

    @Test
    fun forceRefreshBypassesFreshCache() {
        val source = FakeSource(FetchResult.Ok(body, etag = null))
        catalog(source).releases(repo)
        catalog(source).releases(repo, forceRefresh = true)
        assertEquals(2, source.etags.size)
    }

    @Test
    fun networkFailureFallsBackToStaleCacheWithNote() {
        val source = FakeSource(FetchResult.Ok(body, etag = null))
        catalog(source).releases(repo)
        now += DriverCatalog.DEFAULT_TTL_MS * 5
        source.next = FetchResult.Failed("GitHub rate limit reached", rateLimited = true)
        val result = catalog(source).releases(repo) as CatalogResult.Releases

        assertTrue(result.fromCache)
        assertTrue(result.note!!.contains("rate limit"))
    }

    @Test
    fun networkFailureWithoutCacheIsAFailure() {
        val source = FakeSource(FetchResult.Failed("offline", rateLimited = false))
        val result = catalog(source).releases(repo)
        assertTrue(result is CatalogResult.Failure)
        assertEquals("offline", (result as CatalogResult.Failure).message)
    }

    @Test
    fun nonJsonResponseIsNotCachedAndFallsBack() {
        val source = FakeSource(FetchResult.Ok(body, etag = null))
        catalog(source).releases(repo)
        now += DriverCatalog.DEFAULT_TTL_MS + 1
        source.next = FetchResult.Ok("<html>captive portal</html>", etag = null)
        val result = catalog(source).releases(repo) as CatalogResult.Releases
        assertTrue(result.fromCache)
        assertEquals("tu_v32", result.releases.single().tag)
        assertTrue(ReleaseCache(dir).read(repo)!!.body.startsWith("["))
    }

    @Test
    fun corruptCacheFileIsIgnored() {
        File(dir, ReleaseCache.fileNameFor(repo)).writeText("garbage")
        assertNull(ReleaseCache(dir).read(repo))
    }

    @Test
    fun curatedReposAreTheFourKnownSources() {
        assertEquals(
            listOf(
                "K11MCH1/AdrenoToolsDrivers",
                "whitebelyash/AdrenoToolsDrivers",
                "StevenMXZ/Adreno-Tools-Drivers",
                "MrPurple666/purple-turnip",
            ),
            DriverRepo.CURATED.map { it.slug },
        )
    }
}
