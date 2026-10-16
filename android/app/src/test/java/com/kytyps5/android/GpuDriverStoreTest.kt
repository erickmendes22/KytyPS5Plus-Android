package com.kytyps5.android

import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Files
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class GpuDriverStoreTest {
    private lateinit var filesDir: File
    private lateinit var store: GpuDriverStore

    private val turnipMeta = """
        {"schemaVersion":1,"name":"Mainline Turnip v32","description":"gen8","author":"whitebelyash",
         "packageVersion":"1","vendor":"Mesa","driverVersion":"Vulkan 1.4.335","minApi":28,
         "libraryName":"libvulkan_freedreno.so"}
    """.trimIndent()

    @Before
    fun setUp() {
        filesDir = Files.createTempDirectory("gpu-driver-test").toFile()
        store = GpuDriverStore(filesDir)
    }

    @After
    fun tearDown() {
        filesDir.deleteRecursively()
    }

    private fun zip(vararg entries: Pair<String, String>): java.io.InputStream {
        val bytes = ByteArrayOutputStream()
        ZipOutputStream(bytes).use { out ->
            entries.forEach { (name, content) ->
                out.putNextEntry(ZipEntry(name))
                out.write(content.toByteArray())
                out.closeEntry()
            }
        }
        return bytes.toByteArray().inputStream()
    }

    @Test
    fun parsesAdrenoToolsMeta() {
        val meta = GpuDriverMeta.parse(turnipMeta)!!
        assertEquals("Mainline Turnip v32", meta.name)
        assertEquals("Vulkan 1.4.335", meta.driverVersion)
        assertEquals("libvulkan_freedreno.so", meta.libraryName)
    }

    @Test
    fun rejectsMetaWithoutPlainLibraryName() {
        assertNull(GpuDriverMeta.parse("not json"))
        assertNull(GpuDriverMeta.parse("""{"name":"x"}"""))
        assertNull(GpuDriverMeta.parse("""{"libraryName":"../evil.so"}"""))
        assertNull(GpuDriverMeta.parse("""{"libraryName":"libvulkan.txt"}"""))
    }

    @Test
    fun importsAndListsPackage() {
        val result = store.import(zip("meta.json" to turnipMeta, "libvulkan_freedreno.so" to "ELF"))
        val driver = (result as DriverImportResult.Imported).driver
        assertEquals("mainline-turnip-v32-vulkan-1-4-335", driver.id)
        assertTrue(File(driver.directory, "libvulkan_freedreno.so").isFile)
        assertEquals(listOf(driver), store.list())
        assertEquals(driver, store.find(driver.id))
    }

    @Test
    fun flattensNestedEntriesAndIgnoresTraversal() {
        val result = store.import(
            zip("pkg/meta.json" to turnipMeta, "pkg/libvulkan_freedreno.so" to "ELF", "../../escape.so" to "x"),
        )
        val driver = (result as DriverImportResult.Imported).driver
        assertFalse(File(filesDir, "escape.so").exists())
        assertTrue(File(driver.directory, "escape.so").isFile) // kept, but only inside the package
    }

    @Test
    fun failsWithoutLibraryOrMeta() {
        assertTrue(store.import(zip("meta.json" to turnipMeta)) is DriverImportResult.Failure)
        assertTrue(store.import(zip("libvulkan_freedreno.so" to "ELF")) is DriverImportResult.Failure)
        assertTrue(store.list().isEmpty())
    }

    @Test
    fun deleteRemovesOnlySafeIds() {
        val driver = (store.import(zip("meta.json" to turnipMeta, "libvulkan_freedreno.so" to "ELF"))
            as DriverImportResult.Imported).driver
        assertFalse(store.delete("../"))
        assertTrue(store.delete(driver.id))
        assertTrue(store.list().isEmpty())
    }

    @Test
    fun choiceResolvesOverrideThenGlobal() {
        val imported = setOf("turnip")
        assertNull(GpuDriverChoice.resolve("", null, imported::contains))
        assertEquals("turnip", GpuDriverChoice.resolve("turnip", null, imported::contains))
        assertNull(GpuDriverChoice.resolve("turnip", "", imported::contains))
        assertEquals("turnip", GpuDriverChoice.resolve("", "turnip", imported::contains))
        assertNull(GpuDriverChoice.resolve("deleted", null, imported::contains))
    }

    @Test
    fun launchArgsCarryDriverDirectory() {
        val args = LaunchArgs.forGame("/g", Resolution.HD_720, "/data/files/gpu_drivers/turnip")
        assertArrayEquals(
            arrayOf(
                "--game", "/g", "--screen-width", "1280", "--screen-height", "720",
                "--gpu-driver", "/data/files/gpu_drivers/turnip",
            ),
            args,
        )
    }
}
