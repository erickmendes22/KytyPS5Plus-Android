package com.kytyps5.android.drivers

import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Unauthenticated GitHub REST client for "list releases" with ETag revalidation. */
class GitHubReleaseSource(private val perPage: Int = PER_PAGE) : ReleaseSource {

    override fun fetch(repo: DriverRepo, etag: String?): FetchResult {
        val url = URL("https://api.github.com/repos/${repo.owner}/${repo.name}/releases?per_page=$perPage")
        val connection = try {
            url.openConnection() as HttpURLConnection
        } catch (e: IOException) {
            return FetchResult.Failed("Cannot reach GitHub: ${e.message}", rateLimited = false)
        }
        return try {
            connection.connectTimeout = TIMEOUT_MS
            connection.readTimeout = TIMEOUT_MS
            connection.setRequestProperty("Accept", "application/vnd.github+json")
            connection.setRequestProperty("X-GitHub-Api-Version", "2022-11-28")
            connection.setRequestProperty("User-Agent", USER_AGENT)
            etag?.let { connection.setRequestProperty("If-None-Match", it) }
            when (val code = connection.responseCode) {
                HttpURLConnection.HTTP_OK -> FetchResult.Ok(
                    body = connection.inputStream.bufferedReader().use { it.readText() },
                    etag = connection.getHeaderField("ETag"),
                )
                HttpURLConnection.HTTP_NOT_MODIFIED -> FetchResult.NotModified
                HttpURLConnection.HTTP_FORBIDDEN, HTTP_TOO_MANY_REQUESTS -> rateLimitOrError(connection, code)
                else -> FetchResult.Failed("GitHub returned HTTP $code for ${repo.slug}.", rateLimited = false)
            }
        } catch (e: IOException) {
            FetchResult.Failed("Cannot reach GitHub: ${e.message}", rateLimited = false)
        } finally {
            connection.disconnect()
        }
    }

    private fun rateLimitOrError(connection: HttpURLConnection, code: Int): FetchResult {
        val remaining = connection.getHeaderField("X-RateLimit-Remaining")
        if (remaining != "0" && code != HTTP_TOO_MANY_REQUESTS) {
            return FetchResult.Failed("GitHub refused the request (HTTP $code).", rateLimited = false)
        }
        val resetEpoch = connection.getHeaderField("X-RateLimit-Reset")?.toLongOrNull()
        val until = resetEpoch?.let { " until " + SimpleDateFormat("HH:mm", Locale.US).format(Date(it * 1000L)) } ?: ""
        return FetchResult.Failed("GitHub rate limit reached (60 requests/hour)$until.", rateLimited = true)
    }

    private companion object {
        const val PER_PAGE = 30
        const val TIMEOUT_MS = 15_000
        const val HTTP_TOO_MANY_REQUESTS = 429
        const val USER_AGENT = "KytyPS5-Android-Launcher"
    }
}
