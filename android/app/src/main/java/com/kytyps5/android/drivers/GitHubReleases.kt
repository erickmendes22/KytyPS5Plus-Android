package com.kytyps5.android.drivers

import com.kytyps5.android.stringOr
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject

/** One downloadable driver package attached to a GitHub release. */
data class DriverAsset(
    val name: String,
    val size: Long,
    val downloadUrl: String,
    /** Lower-case hex sha256 published by GitHub ("digest" field), or null for older uploads. */
    val sha256: String?,
)

data class DriverRelease(
    val tag: String,
    val title: String,
    /** yyyy-MM-dd, or "" when GitHub reports no publish date. */
    val publishedDate: String,
    val prerelease: Boolean,
    val assets: List<DriverAsset>,
)

/**
 * Parses the GitHub REST "list releases" response (GET /repos/{owner}/{repo}/releases). Only .zip
 * assets served from https://github.com are kept: driver packages are zips, and anything else in a
 * release (logs, screenshots) cannot be installed. Releases without such assets are dropped.
 */
object GitHubReleases {
    /** Release assets of some repository on github.com; redirects are checked by [DriverDownloader]. */
    private val TRUSTED_URL = Regex("""^https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/releases/download/[^?#]+$""")
    private val SHA256_HEX = Regex("^[0-9a-f]{64}$")

    /** True when [json] is a JSON array, i.e. a release list and not an error page or portal HTML. */
    fun isReleaseList(json: String): Boolean = try {
        JSONArray(json)
        true
    } catch (e: JSONException) {
        false
    }

    fun parse(json: String): List<DriverRelease> {
        val array = try {
            JSONArray(json)
        } catch (e: JSONException) {
            return emptyList()
        }
        return (0 until array.length())
            .mapNotNull { array.optJSONObject(it) }
            .mapNotNull(::parseRelease)
    }

    private fun parseRelease(obj: JSONObject): DriverRelease? {
        val tag = obj.stringOr("tag_name")
        val assetsJson = obj.optJSONArray("assets") ?: return null
        val assets = (0 until assetsJson.length())
            .mapNotNull { assetsJson.optJSONObject(it) }
            .mapNotNull(::parseAsset)
        if (tag.isEmpty() || assets.isEmpty()) return null
        val published = obj.stringOr("published_at")
        return DriverRelease(
            tag = tag,
            title = obj.stringOr("name").trim().ifEmpty { tag },
            publishedDate = published.take(DATE_LENGTH),
            prerelease = obj.optBoolean("prerelease", false),
            assets = assets,
        )
    }

    private fun parseAsset(obj: JSONObject): DriverAsset? {
        val name = obj.stringOr("name")
        val url = obj.stringOr("browser_download_url")
        if (!name.endsWith(".zip", ignoreCase = true) || !isTrustedDownloadUrl(url)) return null
        return DriverAsset(
            name = name,
            size = obj.optLong("size", 0L),
            downloadUrl = url,
            sha256 = sha256FromDigest(obj.stringOr("digest").ifEmpty { null }),
        )
    }

    fun isTrustedDownloadUrl(url: String): Boolean = TRUSTED_URL.matches(url)

    /** "sha256:ABC…" -> "abc…"; null for other algorithms or malformed values. */
    fun sha256FromDigest(digest: String?): String? {
        val value = digest?.takeIf { it.startsWith("sha256:") }?.removePrefix("sha256:")?.lowercase()
        return value?.takeIf { SHA256_HEX.matches(it) }
    }

    private const val DATE_LENGTH = 10
}
