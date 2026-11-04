#!/usr/bin/env python3
"""Patch the original Android file picker with a direct-storage PKG browser."""
from pathlib import Path
import sys

root = Path(sys.argv[1])
activity = root / "app/src/main/java/com/example/pkgextractor/MainActivity.kt"
engine = root / "app/src/main/java/com/example/pkgextractor/Engine.kt"

def replace_once(path, old, new):
    source = path.read_text()
    if source.count(old) != 1:
        raise RuntimeError(f"Expected exactly one anchor in {path}: {old[:100]!r}; got {source.count(old)}")
    path.write_text(source.replace(old, new, 1))

replace_once(
    activity,
    'import android.app.Activity\n',
    'import android.app.Activity\nimport android.app.AlertDialog\n',
)

replace_once(
    activity,
    '        add(label("Unpack PS4 packages right on your phone", 14f), gap(2))',
    '        add(label("Extract PS4 and PS5 .pkg files on your phone", 14f), gap(2))',
)

replace_once(
    activity,
    '''        pickBtn.setOnClickListener {
            startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { addCategory(Intent.CATEGORY_OPENABLE); type = "*/*" }, 1)
        }''',
    '''        pickBtn.setOnClickListener {
            // Android DocumentsUI may hide unknown-MIME .pkg files even for "*/*".
            // MANAGE_EXTERNAL_STORAGE is already requested by the original app.
            if (!hasAllFiles()) {
                Toast.makeText(this, "Grant All files access to browse PKG files directly.", Toast.LENGTH_LONG).show()
                askAllFiles()
            } else {
                val recent = prefs().getString("pkg_last_dir", null)?.let { File(it) }
                val downloads = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
                showDirectPkgBrowser(if (recent?.isDirectory == true) recent else downloads)
            }
        }''',
)

hook = '''    // ---------- file picking ----------
'''
browser = '''    // Direct-storage file explorer: avoids DocumentsUI's per-provider MIME catalog.
    // Read file paths in place: never copy an enormous PKG into app cache.
    private fun showDirectPkgBrowser(folder: File) {
        if (!hasAllFiles()) {
            askAllFiles()
            return
        }
        val root = Environment.getExternalStorageDirectory().canonicalFile
        val directory = try { folder.canonicalFile } catch (_: Exception) { root }
        if (!directory.isDirectory || !directory.canRead()) {
            Toast.makeText(this, "Unable to read this folder", Toast.LENGTH_LONG).show()
            showSystemPkgPicker()
            return
        }
        val listed = directory.listFiles()
        if (listed == null) {
            Toast.makeText(this, "Access denied to folder", Toast.LENGTH_LONG).show()
            showSystemPkgPicker()
            return
        }
        val shown = listed.filter {
            it.isDirectory || (it.isFile && it.name.endsWith(".pkg", ignoreCase = true))
        }.sortedWith(compareBy<File> { !it.isDirectory }.thenBy { it.name.lowercase() })
        val entries = mutableListOf<File>()
        val labels = mutableListOf<String>()
        val insideRoot = directory.path == root.path || directory.path.startsWith(root.path + File.separator)
        if (insideRoot && directory != root && directory.parentFile != null) {
            entries.add(directory.parentFile!!)
            labels.add("⬆  .. (parent folder)")
        }
        for (entry in shown) {
            entries.add(entry)
            labels.add(if (entry.isDirectory) "📁  \${entry.name}/"
                       else "📦  \${entry.name}  (\${fmt(entry.length())})")
        }
        if (entries.isEmpty()) {
            AlertDialog.Builder(this)
                .setTitle("No .pkg files here")
                .setMessage(directory.absolutePath + "\\n\\nBrowse another folder or use the Android picker.")
                .setPositiveButton("Downloads") { _, _ ->
                    showDirectPkgBrowser(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS))
                }
                .setNeutralButton("Android picker") { _, _ -> showSystemPkgPicker() }
                .setNegativeButton("Close", null)
                .show()
            return
        }
        AlertDialog.Builder(this)
            .setTitle("PKG browser: " + directory.absolutePath)
            .setItems(labels.toTypedArray()) { _, position ->
                val selected = entries[position]
                if (selected.isDirectory) showDirectPkgBrowser(selected)
                else selectDirectPkg(selected)
            }
            .setNeutralButton("Android picker") { _, _ -> showSystemPkgPicker() }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun showSystemPkgPicker() {
        startActivityForResult(
            Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
            }, 1
        )
    }

    private fun selectDirectPkg(file: File) {
        if (Engine.running) return
        if (!file.isFile || !file.canRead() || !file.name.endsWith(".pkg", ignoreCase = true)) {
            Toast.makeText(this, "Cannot read selected PKG", Toast.LENGTH_LONG).show()
            return
        }
        Engine.clearResult()
        pkg = Uri.fromFile(file)
        pkgName = file.name
        pkgSize = file.length()
        prefs().edit().putString("pkg_last_dir", file.parentFile?.absolutePath).apply()
        pkgText.text = pkgName
        fileText.text = pkgName
        fileSub.text = "\${fmt(pkgSize)} · ready to extract"
        ring.reset()
        openBtn.visibility = View.GONE
        setEnabledLook(goBtn, true)
        status.text = ""
        refreshStorage()
    }

'''
# The two spellings below intentionally use normal Kotlin template interpolation.
browser = browser.replace(chr(92) + chr(36) + "{", chr(36) + "{")
replace_once(activity, hook, browser + hook)

replace_once(
    engine,
    '''    fun realPath(uri: Uri): String? {
        if (uri.authority == "com.android.externalstorage.documents") {''',
    '''    fun realPath(uri: Uri): String? {
        if (uri.scheme == "file") {
            return uri.path?.let { File(it) }?.takeIf { it.isFile && it.canRead() }?.absolutePath
        }
        if (uri.authority == "com.android.externalstorage.documents") {''',
)
print("Android .pkg direct browser, 64-bit file size and file:// native routing patched.")
