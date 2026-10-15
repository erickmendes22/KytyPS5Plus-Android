package com.kytyps5.android

import android.net.Uri
import android.provider.DocumentsContract
import java.io.File

/**
 * Maps between the system folder picker's document IDs and real filesystem paths. The emulator
 * opens game files by path (the launcher holds all-files access), so a picked folder must live on
 * device storage or an SD card; folders from cloud or app providers have no path and are rejected.
 */
object StoragePaths {
    const val PRIMARY_ROOT = "/storage/emulated/0"
    private const val EXTERNAL_STORAGE_AUTHORITY = "com.android.externalstorage.documents"
    private const val PRIMARY_VOLUME = "primary"
    private val LEGACY_PRIMARY_PREFIXES = listOf("/sdcard", "/storage/self/primary")
    private val VOLUME_UUID = Regex("^[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}$")

    /** Filesystem path of a folder returned by ACTION_OPEN_DOCUMENT_TREE, or null if it has none. */
    fun treeUriToPath(treeUri: Uri): String? {
        if (treeUri.authority != EXTERNAL_STORAGE_AUTHORITY) return null
        return documentIdToPath(DocumentsContract.getTreeDocumentId(treeUri))
    }

    /** Document URI the picker should open at (EXTRA_INITIAL_URI), or null if [path] is not mappable. */
    fun initialUriFor(path: String): Uri? =
        pathToDocumentId(path)?.let { DocumentsContract.buildDocumentUri(EXTERNAL_STORAGE_AUTHORITY, it) }

    /** "primary:KytyPS5/games" -> "/storage/emulated/0/KytyPS5/games"; "1A2B-3C4D:Games" -> "/storage/1A2B-3C4D/Games". */
    fun documentIdToPath(documentId: String, primaryRoot: String = PRIMARY_ROOT): String? {
        val separator = documentId.indexOf(':')
        if (separator <= 0) return null
        val volume = documentId.substring(0, separator)
        val relative = documentId.substring(separator + 1).trim('/')
        val base = when {
            volume == PRIMARY_VOLUME -> primaryRoot
            volume == "home" -> "$primaryRoot/Documents"
            volume == "raw" -> return documentId.substring(separator + 1).takeIf { it.startsWith("/") }
            VOLUME_UUID.matches(volume) -> "/storage/$volume"
            else -> return null
        }
        return if (relative.isEmpty()) base else "$base/$relative"
    }

    /** Inverse of [documentIdToPath] for paths on primary storage or an SD card; null otherwise. */
    fun pathToDocumentId(path: String, primaryRoot: String = PRIMARY_ROOT): String? {
        val normalized = normalize(path, primaryRoot)
        if (normalized == primaryRoot || normalized.startsWith("$primaryRoot/")) {
            return "$PRIMARY_VOLUME:" + normalized.removePrefix(primaryRoot).trim('/')
        }
        val parts = normalized.removePrefix("/storage/").split('/', limit = 2)
        if (normalized.startsWith("/storage/") && VOLUME_UUID.matches(parts[0])) {
            return "${parts[0]}:" + parts.getOrElse(1) { "" }.trim('/')
        }
        return null
    }

    /** Closest existing folder at or above [path], so the picker never opens on a missing folder. */
    fun nearestExistingFolder(path: String, exists: (String) -> Boolean = { File(it).isDirectory }): String? {
        var current: String? = path.trimEnd('/').ifEmpty { "/" }
        while (current != null) {
            if (exists(current)) return current
            current = File(current).parent
        }
        return null
    }

    private fun normalize(path: String, primaryRoot: String): String {
        val trimmed = path.trimEnd('/')
        for (prefix in LEGACY_PRIMARY_PREFIXES) {
            if (trimmed == prefix || trimmed.startsWith("$prefix/")) {
                return primaryRoot + trimmed.removePrefix(prefix)
            }
        }
        return trimmed
    }
}
