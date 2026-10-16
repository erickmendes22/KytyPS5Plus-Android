package com.kytyps5.android.drivers

import android.app.Activity
import android.app.AlertDialog
import android.graphics.Typeface
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.TextView
import com.kytyps5.android.DriverImportResult
import com.kytyps5.android.GpuDriver
import com.kytyps5.android.GpuDriverStore
import java.io.File
import java.io.IOException
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Online driver browser: lists releases of the curated GitHub repos, marks packages that suit this
 * GPU (see [DriverAdvisor]), downloads one with sha256 verification and installs it into the store.
 * Layout and flow follow RPCSX-UI-Android's GpuDriversScreen (GPL-2.0), rebuilt with platform views.
 */
class DriverBrowserDialog(
    private val activity: Activity,
    private val store: GpuDriverStore,
    private val adrenoModel: () -> Int?,
    private val onInstalled: (GpuDriver, useNow: Boolean) -> Unit,
) {
    private val catalog = DriverCatalog(ReleaseCache(File(activity.cacheDir, CACHE_DIR)), GitHubReleaseSource())
    private val downloader = DriverDownloader(File(activity.cacheDir, DOWNLOAD_DIR))
    private val density = activity.resources.displayMetrics.density
    private val Int.dp: Int get() = (this * density).toInt()

    private lateinit var status: TextView
    private lateinit var list: LinearLayout
    private var repo: DriverRepo = DriverRepo.CURATED.first()
    private var request = 0
    @Volatile
    private var model: Int? = null
    private val cancelled = AtomicBoolean(false)
    private var busy = false

    /** Network and install work of this dialog only, so a long download never blocks library scans. */
    private val worker: ExecutorService = Executors.newSingleThreadExecutor()

    fun show() {
        val spinner = Spinner(activity).apply {
            adapter = ArrayAdapter(activity, android.R.layout.simple_spinner_dropdown_item, DriverRepo.CURATED.map { it.label })
        }
        status = text("", 13f, secondary = true)
        list = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        val header = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(spinner, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            addView(Button(activity).apply {
                text = "Refresh"
                setOnClickListener { load(forceRefresh = true) }
            })
        }
        val content = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(20.dp, 8.dp, 20.dp, 0)
            addView(header)
            addView(status)
            addView(ScrollView(activity).apply { addView(list) }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 420.dp))
        }
        spinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                repo = DriverRepo.CURATED[position]
                load(forceRefresh = false)
            }

            override fun onNothingSelected(parent: AdapterView<*>?) = Unit
        }
        AlertDialog.Builder(activity)
            .setTitle("Download GPU drivers")
            .setView(content)
            .setNegativeButton("Close", null)
            .setOnDismissListener {
                cancelled.set(true)
                worker.shutdown()
            }
            .show()
    }

    private fun load(forceRefresh: Boolean) {
        val current = ++request
        val target = repo
        status.text = "Loading ${target.slug}…"
        list.removeAllViews()
        background {
            if (model == null) model = adrenoModel()
            val result = catalog.releases(target, forceRefresh)
            onUi { if (current == request) render(result) }
        }
    }

    private fun render(result: CatalogResult) {
        list.removeAllViews()
        val gpu = model?.let { "Adreno $it" } ?: "GPU not detected"
        when (result) {
            is CatalogResult.Failure -> status.text = "$gpu · ${result.message}"
            is CatalogResult.Releases -> {
                val source = if (result.fromCache) "cached" else "live"
                status.text = listOfNotNull("$gpu · ${result.releases.size} releases ($source)", result.note).joinToString("\n")
                result.releases.forEach(::addRelease)
            }
        }
    }

    private fun addRelease(release: DriverRelease) {
        val pre = if (release.prerelease) " · pre-release" else ""
        list.addView(text("${release.title}  ${release.publishedDate}$pre", 15f, bold = true).apply { setPadding(0, 12.dp, 0, 2.dp) })
        release.assets
            .map { it to DriverAdvisor.advise(model, release.tag, it.name) }
            .sortedBy { it.second.verdict.rank }
            .forEach { (asset, advice) -> list.addView(assetRow(asset, advice)) }
    }

    private fun assetRow(asset: DriverAsset, advice: DriverAdvisor.Advice): View = LinearLayout(activity).apply {
        orientation = LinearLayout.HORIZONTAL
        val info = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            addView(text("${asset.name}  (${megabytes(asset.size)})", 14f))
            val badge = advice.verdict.badge.let { if (it.isEmpty()) "" else "$it: " }
            addView(text(badge + advice.reason, 12f, secondary = advice.verdict != DriverAdvisor.Verdict.RECOMMENDED))
        }
        addView(info, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        addView(Button(activity).apply {
            text = "Get"
            setOnClickListener { download(asset) }
        })
    }

    private fun download(asset: DriverAsset) {
        if (busy) {
            status.text = "Wait for the current download or install to finish."
            return
        }
        busy = true
        status.text = "Downloading ${asset.name}…"
        background {
            var lastPercent = -1
            val result = downloader.download(
                asset,
                onProgress = { done, total ->
                    val percent = if (total > 0) (done * 100 / total).toInt() else (done shr 20).toInt()
                    if (percent != lastPercent) {
                        lastPercent = percent
                        val progress = if (total > 0) "$percent%" else "$percent MB"
                        onUi { status.text = "Downloading ${asset.name}… $progress" }
                    }
                },
                isCancelled = cancelled::get,
            )
            onUi(orElse = { (result as? DownloadResult.Done)?.file?.delete() }) {
                when (result) {
                    is DownloadResult.Failure -> {
                        busy = false
                        status.text = result.message
                    }
                    is DownloadResult.Done -> confirmInstall(asset, result)
                }
            }
        }
    }

    private fun confirmInstall(asset: DriverAsset, done: DownloadResult.Done) {
        status.text = "Downloaded ${asset.name}."
        val check = when (done.verified) {
            true -> "✓ matches the sha256 GitHub published"
            null -> "GitHub published no checksum for this file"
            false -> "✗ mismatch"
        }
        AlertDialog.Builder(activity)
            .setTitle("Install ${asset.name}?")
            .setMessage("sha256: ${done.sha256}\n$check")
            .setPositiveButton("Install and use") { _, _ -> install(done.file, useNow = true) }
            .setNeutralButton("Install only") { _, _ -> install(done.file, useNow = false) }
            .setNegativeButton("Cancel") { _, _ ->
                done.file.delete()
                busy = false
            }
            .setOnCancelListener {
                done.file.delete()
                busy = false
            }
            .show()
    }

    private fun install(file: File, useNow: Boolean) {
        status.text = "Installing…"
        background {
            val result = try {
                file.inputStream().use(store::import)
            } catch (e: IOException) {
                DriverImportResult.Failure("Could not read ${file.name}: ${e.message}")
            } finally {
                file.delete()
            }
            // The install outcome must reach the launcher even if this dialog was closed meanwhile.
            activity.runOnUiThread {
                busy = false
                if (activity.isDestroyed) return@runOnUiThread
                when (result) {
                    is DriverImportResult.Imported -> {
                        status.text = "Installed ${result.driver.label}."
                        onInstalled(result.driver, useNow)
                    }
                    is DriverImportResult.Failure -> status.text = result.message
                }
            }
        }
    }

    /** Runs [task] off the UI thread; after the dialog closed (executor shut down) it is skipped. */
    private fun background(task: () -> Unit) {
        try {
            worker.execute(task)
        } catch (e: RejectedExecutionException) {
            busy = false
        }
    }

    /** Runs [block] on the UI thread unless the dialog or activity has gone away meanwhile. */
    private fun onUi(orElse: () -> Unit = {}, block: () -> Unit) {
        activity.runOnUiThread {
            if (cancelled.get() || activity.isFinishing || activity.isDestroyed) orElse() else block()
        }
    }

    private fun megabytes(bytes: Long): String = String.format(Locale.US, "%.1f MB", bytes / 1_048_576.0)

    private fun text(value: String, sizeSp: Float, bold: Boolean = false, secondary: Boolean = false) =
        TextView(activity).apply {
            text = value
            textSize = sizeSp
            if (bold) setTypeface(typeface, Typeface.BOLD)
            if (secondary) alpha = 0.7f
        }

    private companion object {
        const val CACHE_DIR = "driver_catalog"
        const val DOWNLOAD_DIR = "driver_downloads"
    }
}
