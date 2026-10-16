package com.kytyps5.android.drivers

import java.io.File
import java.io.IOException
import org.json.JSONException
import org.json.JSONObject

/** A GitHub repository publishing AdrenoTools driver packages as release assets. */
data class DriverRepo(val owner: String, val name: String, val label: String) {
    val slug: String get() = "$owner/$name"

    companion object {
        /** Curated sources (research/frontends.md); all ship meta.json-style AdrenoTools zips. */
        val CURATED = listOf(
            DriverRepo("K11MCH1", "AdrenoToolsDrivers", "K11MCH1 (Turnip R-series, Qualcomm)"),
            DriverRepo("whitebelyash", "AdrenoToolsDrivers", "whitebelyash (Mainline Turnip)"),
            DriverRepo("StevenMXZ", "Adreno-Tools-Drivers", "StevenMXZ (Turnip Gen8, Qualcomm)"),
            DriverRepo("MrPurple666", "purple-turnip", "MrPurple666 (Purple Turnip)"),
        )
    }
}

sealed interface FetchResult {
    data class Ok(val body: String, val etag: String?) : FetchResult
    data object NotModified : FetchResult
    data class Failed(val message: String, val rateLimited: Boolean) : FetchResult
}

/** Network side of the catalog; [GitHubReleaseSource] in the app, a fake in tests. */
fun interface ReleaseSource {
    fun fetch(repo: DriverRepo, etag: String?): FetchResult
}

data class CachedReleases(val fetchedAtMs: Long, val etag: String?, val body: String)

/** One JSON file per repo under [dir]; unreadable or corrupt entries read as absent. */
class ReleaseCache(private val dir: File) {

    fun read(repo: DriverRepo): CachedReleases? = try {
        val obj = JSONObject(File(dir, fileNameFor(repo)).readText())
        CachedReleases(
            fetchedAtMs = obj.getLong(KEY_FETCHED_AT),
            etag = if (obj.isNull(KEY_ETAG)) null else obj.optString(KEY_ETAG).ifEmpty { null },
            body = obj.getString(KEY_BODY),
        )
    } catch (e: IOException) {
        null
    } catch (e: JSONException) {
        null
    }

    fun write(repo: DriverRepo, entry: CachedReleases) {
        if (!dir.isDirectory && !dir.mkdirs()) return
        val json = JSONObject()
            .put(KEY_FETCHED_AT, entry.fetchedAtMs)
            .put(KEY_ETAG, entry.etag ?: "")
            .put(KEY_BODY, entry.body)
        val target = File(dir, fileNameFor(repo))
        val partial = File(dir, target.name + ".part")
        try {
            partial.writeText(json.toString())
            if (!partial.renameTo(target)) partial.delete()
        } catch (e: IOException) {
            partial.delete()
        }
    }

    companion object {
        private const val KEY_FETCHED_AT = "fetchedAt"
        private const val KEY_ETAG = "etag"
        private const val KEY_BODY = "body"

        fun fileNameFor(repo: DriverRepo): String =
            (repo.owner + "_" + repo.name).replace(Regex("[^A-Za-z0-9_.-]"), "_") + ".json"
    }
}

sealed interface CatalogResult {
    data class Releases(
        val releases: List<DriverRelease>,
        val fromCache: Boolean,
        /** Why cached data is shown despite a refresh attempt (offline, rate limited), else null. */
        val note: String?,
    ) : CatalogResult

    data class Failure(val message: String) : CatalogResult
}

/**
 * Release lists per repo, cached on disk for [ttlMs]. Unauthenticated GitHub API calls are limited to
 * 60 per hour per IP; revalidating with the stored ETag returns 304, which does not count against it.
 * When the network fails, the last good list is shown with a note instead of an error.
 */
class DriverCatalog(
    private val cache: ReleaseCache,
    private val source: ReleaseSource,
    private val clock: () -> Long = System::currentTimeMillis,
    private val ttlMs: Long = DEFAULT_TTL_MS,
) {
    fun releases(repo: DriverRepo, forceRefresh: Boolean = false): CatalogResult {
        val cached = cache.read(repo)
        val now = clock()
        if (cached != null && !forceRefresh && now - cached.fetchedAtMs in 0 until ttlMs) {
            return CatalogResult.Releases(GitHubReleases.parse(cached.body), fromCache = true, note = null)
        }
        return when (val fetched = source.fetch(repo, cached?.etag)) {
            is FetchResult.Ok -> {
                cache.write(repo, CachedReleases(now, fetched.etag, fetched.body))
                CatalogResult.Releases(GitHubReleases.parse(fetched.body), fromCache = false, note = null)
            }
            FetchResult.NotModified -> {
                val body = cached?.body ?: return CatalogResult.Failure("GitHub answered 304 without a cached list.")
                cache.write(repo, CachedReleases(now, cached.etag, body))
                CatalogResult.Releases(GitHubReleases.parse(body), fromCache = true, note = null)
            }
            is FetchResult.Failed -> if (cached != null) {
                CatalogResult.Releases(
                    GitHubReleases.parse(cached.body),
                    fromCache = true,
                    note = "${fetched.message} Showing the list cached ${ageText(now - cached.fetchedAtMs)} ago.",
                )
            } else {
                CatalogResult.Failure(fetched.message)
            }
        }
    }

    private fun ageText(ms: Long): String {
        val minutes = (ms / 60_000L).coerceAtLeast(0)
        return if (minutes < 120) "$minutes min" else "${minutes / 60} h"
    }

    companion object {
        const val DEFAULT_TTL_MS = 60L * 60L * 1000L
    }
}
