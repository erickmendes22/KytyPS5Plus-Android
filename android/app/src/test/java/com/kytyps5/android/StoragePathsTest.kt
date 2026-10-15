package com.kytyps5.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class StoragePathsTest {

    @Test
    fun primaryAndSdCardDocumentIdsMapToPaths() {
        assertEquals("/storage/emulated/0/KytyPS5/games", StoragePaths.documentIdToPath("primary:KytyPS5/games"))
        assertEquals("/storage/emulated/0", StoragePaths.documentIdToPath("primary:"))
        assertEquals("/storage/1A2B-3C4D/PS5", StoragePaths.documentIdToPath("1A2B-3C4D:PS5"))
        assertEquals("/storage/emulated/0/Documents/g", StoragePaths.documentIdToPath("home:g"))
        assertEquals("/data/x", StoragePaths.documentIdToPath("raw:/data/x"))
    }

    @Test
    fun providersWithoutPathsAreRejected() {
        assertNull(StoragePaths.documentIdToPath("msf:1234"))
        assertNull(StoragePaths.documentIdToPath("no-separator"))
        assertNull(StoragePaths.documentIdToPath(":empty-volume"))
    }

    @Test
    fun pathsMapBackToDocumentIdsIncludingLegacySdcardAlias() {
        assertEquals("primary:KytyPS5/games", StoragePaths.pathToDocumentId("/sdcard/KytyPS5/games"))
        assertEquals("primary:KytyPS5/games", StoragePaths.pathToDocumentId("/storage/emulated/0/KytyPS5/games/"))
        assertEquals("primary:", StoragePaths.pathToDocumentId("/storage/emulated/0"))
        assertEquals("1A2B-3C4D:PS5", StoragePaths.pathToDocumentId("/storage/1A2B-3C4D/PS5"))
        assertNull(StoragePaths.pathToDocumentId("/data/local/tmp"))
    }

    @Test
    fun roundTripIsStable() {
        val path = "/storage/emulated/0/Games/PS5 Dumps"
        assertEquals(path, StoragePaths.documentIdToPath(StoragePaths.pathToDocumentId(path)!!))
    }

    @Test
    fun nearestExistingFolderWalksUp() {
        val existing = setOf("/storage/emulated/0", "/storage/emulated/0/KytyPS5")
        assertEquals(
            "/storage/emulated/0/KytyPS5",
            StoragePaths.nearestExistingFolder("/storage/emulated/0/KytyPS5/games") { it in existing },
        )
        assertNull(StoragePaths.nearestExistingFolder("/nowhere/at/all") { false })
    }
}
