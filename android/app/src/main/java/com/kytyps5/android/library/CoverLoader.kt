package com.kytyps5.android.library

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Handler
import android.os.Looper
import android.util.LruCache
import android.widget.ImageView
import java.io.File
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/** Decodes sce_sys/icon0.png covers off the main thread, downsampled and memory-cached. */
class CoverLoader(private val targetPx: Int) {
    private val executor: ExecutorService = Executors.newFixedThreadPool(2)
    private val main = Handler(Looper.getMainLooper())
    /** Covers that could not be decoded; not retried on every scroll. */
    private val failed: MutableSet<String> = java.util.concurrent.ConcurrentHashMap.newKeySet()
    private val cache = object : LruCache<String, Bitmap>(CACHE_BYTES) {
        override fun sizeOf(key: String, value: Bitmap): Int = value.byteCount
    }

    /** Shows [icon] in [view] when decoded; [onMissing] runs when there is no usable cover. */
    fun load(icon: File?, view: ImageView, onMissing: () -> Unit) {
        val key = icon?.absolutePath
        view.tag = key
        view.setImageDrawable(null)
        if (key == null || key in failed || executor.isShutdown) return onMissing()
        cache.get(key)?.let { return view.setImageBitmap(it) }
        executor.execute {
            val bitmap = decode(icon)
            if (bitmap != null) cache.put(key, bitmap) else failed += key
            main.post {
                if (view.tag != key) return@post
                if (bitmap != null) view.setImageBitmap(bitmap) else onMissing()
            }
        }
    }

    fun shutdown() = executor.shutdownNow()

    private fun decode(file: File): Bitmap? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(file.path, bounds)
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
        var sample = 1
        while (bounds.outWidth / (sample * 2) >= targetPx && bounds.outHeight / (sample * 2) >= targetPx) sample *= 2
        return BitmapFactory.decodeFile(file.path, BitmapFactory.Options().apply { inSampleSize = sample })
    }

    private companion object {
        const val CACHE_BYTES = 32 * 1024 * 1024
    }
}
