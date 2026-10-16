package com.kytyps5.android.drivers

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class GitHubReleasesTest {

    private val sample = """
        [
          {
            "tag_name": "v37",
            "name": "Turnip Gen8 V37 / Turnip v26.3.0-R6",
            "published_at": "2026-09-30T10:10:21Z",
            "prerelease": false,
            "assets": [
              {"name": "Turnip_Gen8_V37.zip", "size": 2767183,
               "browser_download_url": "https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/download/v37/Turnip_Gen8_V37.zip",
               "digest": "sha256:D0335542EC996FE33E83DFBEC0D86AED301DA95D22B671A88B40A4722D115B48"},
              {"name": "notes.txt", "size": 10,
               "browser_download_url": "https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/download/v37/notes.txt"},
              {"name": "evil.zip", "size": 10,
               "browser_download_url": "http://example.com/evil.zip"}
            ]
          },
          {
            "tag_name": "draft-ish",
            "name": "",
            "published_at": null,
            "prerelease": true,
            "assets": [
              {"name": "pkg.adpkg.zip", "size": 5,
               "browser_download_url": "https://github.com/o/r/releases/download/x/pkg.adpkg.zip"}
            ]
          },
          {"tag_name": "empty", "assets": []}
        ]
    """.trimIndent()

    @Test
    fun parsesReleasesAndKeepsOnlyZipAssetsFromGitHub() {
        val releases = GitHubReleases.parse(sample)

        assertEquals(2, releases.size)
        val first = releases[0]
        assertEquals("v37", first.tag)
        assertEquals("Turnip Gen8 V37 / Turnip v26.3.0-R6", first.title)
        assertEquals("2026-09-30", first.publishedDate)
        assertEquals(listOf("Turnip_Gen8_V37.zip"), first.assets.map { it.name })
        assertEquals(2767183L, first.assets[0].size)
        assertEquals(
            "d0335542ec996fe33e83dfbec0d86aed301da95d22b671a88b40a4722d115b48",
            first.assets[0].sha256,
        )
    }

    @Test
    fun blankNameFallsBackToTagAndMissingDigestIsNull() {
        val second = GitHubReleases.parse(sample)[1]
        assertEquals("draft-ish", second.title)
        assertTrue(second.prerelease)
        assertEquals("", second.publishedDate)
        assertNull(second.assets[0].sha256)
    }

    @Test
    fun malformedJsonYieldsEmptyList() {
        assertTrue(GitHubReleases.parse("{not json").isEmpty())
        assertTrue(GitHubReleases.parse("{\"message\":\"API rate limit exceeded\"}").isEmpty())
    }

    @Test
    fun onlyHttpsGitHubDownloadUrlsAreTrusted() {
        assertTrue(GitHubReleases.isTrustedDownloadUrl("https://github.com/o/r/releases/download/t/a.zip"))
        assertTrue(!GitHubReleases.isTrustedDownloadUrl("http://github.com/o/r/releases/download/t/a.zip"))
        assertTrue(!GitHubReleases.isTrustedDownloadUrl("https://github.com.evil.io/a.zip"))
        assertTrue(!GitHubReleases.isTrustedDownloadUrl("https://example.com/a.zip"))
    }

    @Test
    fun digestMustBeSha256Hex() {
        assertNull(GitHubReleases.sha256FromDigest("md5:abcd"))
        assertNull(GitHubReleases.sha256FromDigest("sha256:xyz"))
        assertNull(GitHubReleases.sha256FromDigest(null))
    }
}
