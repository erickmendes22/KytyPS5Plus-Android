package com.kytyps5.android

import android.app.Activity
import android.app.AlertDialog
import android.content.ActivityNotFoundException
import android.content.Intent
import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.provider.DocumentsContract
import android.provider.Settings
import android.util.Log
import android.view.Gravity
import android.view.View
import android.view.WindowInsets
import android.view.ViewGroup
import android.widget.Button
import android.widget.CheckBox
import android.widget.GridView
import android.widget.FrameLayout
import android.widget.ScrollView
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.TextView
import com.kytyps5.android.input.OverlayProfileStore
import com.kytyps5.android.library.CoverLoader
import com.kytyps5.android.library.GameGridAdapter
import com.kytyps5.android.library.GameSettingsDialog
import com.kytyps5.android.presets.GamePresets
import java.io.File
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/** Game picker: checks all-files access, picks and scans the games folder, launches [GameActivity]. */
class MainActivity : Activity() {

    private lateinit var settings: LauncherSettings
    private lateinit var drivers: GpuDriverPanel
    private val worker: ExecutorService = Executors.newSingleThreadExecutor()
    private val mainHandler = Handler(Looper.getMainLooper())
    private var scanRequest = 0

    private lateinit var nativeStatus: TextView
    private lateinit var storageStatus: TextView
    private lateinit var grantButton: Button
    private lateinit var folderPath: TextView
    private lateinit var resolutionGroup: RadioGroup
    private lateinit var renderScaleGroup: RadioGroup
    private lateinit var message: TextView
    private lateinit var scanStatus: TextView
    private lateinit var covers: CoverLoader
    private lateinit var grid: GameGridAdapter
    private lateinit var gameSettings: GameSettingsDialog

    private val Int.dp: Int get() = (this * resources.displayMetrics.density).toInt()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = LauncherSettings(this)
        drivers = GpuDriverPanel(
            activity = this,
            settings = settings,
            worker = worker,
            post = { mainHandler.post(it) },
            showMessage = ::showMessage,
            label = { label(it) },
        )
        covers = CoverLoader(targetPx = COVER_DP.dp)
        grid = GameGridAdapter(this, covers, COVER_DP.dp, onPlay = ::launch, onSettings = { gameSettings.show(it) })
        gameSettings = GameSettingsDialog(
            activity = this,
            settings = settings,
            layouts = OverlayProfileStore(File(filesDir, OverlayProfileStore.DIRECTORY_NAME)),
            presets = GamePresets(this, settings),
            driverLabel = { drivers.gameButtonText(it.path) },
            chooseDriver = { game, done -> drivers.chooseForGame(game, done) },
        )
        setContentView(buildUi())
        applyDefaultGpuDriver()
    }

    /** Installs the bundled Turnip on Adreno 8xx on first run (off the main thread: EGL probe + unzip). */
    private fun applyDefaultGpuDriver() {
        val setup = DefaultGpuDriverSetup(this, settings, drivers.store)
        worker.execute {
            val changed = setup.apply() ?: return@execute
            mainHandler.post {
                drivers.refresh()
                showMessage(changed)
            }
        }
    }

    override fun onResume() {
        super.onResume()
        refreshStorageState()
        if (Environment.isExternalStorageManager()) {
            ensureDefaultGamesFolder()
            scanGames(settings.gamesRoot)
        } else {
            showMessage("Grant all-files access to scan for games.")
        }
    }

    override fun onDestroy() {
        worker.shutdownNow()
        covers.shutdown()
        super.onDestroy()
    }

    /** The game library and device options are separate pages so neither can hide the other. */
    private fun buildUi(): View {
        nativeStatus = label(
            if (NativeLibrary.isInstalled(this)) {
                "Emulator library: installed"
            } else {
                "Emulator library: missing (${NativeLibrary.FILE_NAME} is not in this build)"
            },
        )
        storageStatus = label("")
        grantButton = Button(this).apply {
            text = "Grant all-files access"
            setOnClickListener { openAllFilesAccessSettings() }
        }
        folderPath = label(settings.gamesRoot, sizeSp = 14f)
        resolutionGroup = buildResolutionGroup()
        renderScaleGroup = buildRenderScaleGroup()
        scanStatus = label("Games: waiting for first scan", sizeSp = 15f, bold = true)
        message = label("", secondary = true)

        val gamesPage = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(label("Games folder", bold = true))
            // Keeping the path on its own line avoids squeezing it between two buttons.
            addView(folderPath)
            addView(
                LinearLayout(context).apply {
                    orientation = LinearLayout.HORIZONTAL
                    addView(Button(context).apply {
                        text = "Choose folder"
                        setOnClickListener { openFolderPicker() }
                    }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
                    addView(Button(context).apply {
                        text = "Rescan"
                        setOnClickListener { onScanRequested() }
                    }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
                },
            )
            addView(
                LinearLayout(context).apply {
                    orientation = LinearLayout.HORIZONTAL
                    addView(Button(context).apply {
                        text = "Choose eboot / ELF"
                        setOnClickListener { openGameFilePicker() }
                    }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
                    addView(Button(context).apply {
                        text = "Enter path"
                        setOnClickListener { openManualGamePath() }
                    }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
                },
            )
            addView(scanStatus)
            val gameGrid = GridView(context).apply {
                adapter = grid
                numColumns = GridView.AUTO_FIT
                columnWidth = (COVER_DP + 12).dp
                stretchMode = GridView.STRETCH_SPACING_UNIFORM
                verticalSpacing = 4.dp
            }
            val emptyState = label(
                "No games are displayed. The scan status above shows whether " +
                    "the directory is empty, inaccessible or still scanning.",
                secondary = true,
            ).apply {
                gravity = Gravity.CENTER
            }
            addView(
                FrameLayout(context).apply {
                    addView(
                        gameGrid,
                        FrameLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT,
                            ViewGroup.LayoutParams.MATCH_PARENT,
                        ),
                    )
                    addView(
                        emptyState,
                        FrameLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT,
                            ViewGroup.LayoutParams.WRAP_CONTENT,
                            Gravity.CENTER,
                        ),
                    )
                    gameGrid.emptyView = emptyState
                },
                LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f),
            )
        }

        val settingsPage = ScrollView(this).apply {
            visibility = View.GONE
            fillViewport = false
        }
        settingsPage.addView(
            LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                addView(label("Video", sizeSp = 18f, bold = true))
                addView(label("Output resolution", bold = true))
                addView(resolutionGroup)
                addView(label("Internal render scale", bold = true))
                addView(renderScaleGroup)
                addView(label("Performance and controls", sizeSp = 18f, bold = true))
                addView(toggle("Performance HUD", settings.hudEnabled) { settings.hudEnabled = it })
                addView(toggle("Touch controls", settings.touchControlsEnabled) { settings.touchControlsEnabled = it })
                addView(toggle(GameSettingsDialog.GPU_TURBO_LABEL, settings.gpuTurbo) { settings.gpuTurbo = it })
                addView(label("GPU driver", sizeSp = 18f, bold = true))
                addView(drivers.build(onChanged = {}))
                addView(label("Options are saved automatically. Game-specific options " +
                    "remain available from the game's settings button.", secondary = true))
            },
            ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            ),
        )

        val gamesTab = Button(this).apply { text = "Games" }
        val settingsTab = Button(this).apply { text = "Settings" }
        fun showPage(games: Boolean) {
            gamesPage.visibility = if (games) View.VISIBLE else View.GONE
            settingsPage.visibility = if (games) View.GONE else View.VISIBLE
            gamesTab.isEnabled = !games
            settingsTab.isEnabled = games
        }
        gamesTab.setOnClickListener { showPage(true) }
        settingsTab.setOnClickListener { showPage(false) }

        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(16.dp, 16.dp, 16.dp, 16.dp)
            addView(label("KytyPS5+", sizeSp = 22f, bold = true))
            addView(nativeStatus)
            addView(storageStatus)
            addView(grantButton)
            addView(
                LinearLayout(context).apply {
                    orientation = LinearLayout.HORIZONTAL
                    addView(gamesTab, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
                    addView(settingsTab, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
                },
            )
            // Keep general messages readable without overwriting game scan results.
            addView(message)
            addView(gamesPage, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
            addView(settingsPage, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        showPage(true)
        content.setOnApplyWindowInsetsListener { view, insets ->
            val bars = insets.getInsets(WindowInsets.Type.systemBars())
            view.setPadding(16.dp + bars.left, 16.dp + bars.top, 16.dp + bars.right, 16.dp + bars.bottom)
            insets
        }
        return content
    }

    private fun buildResolutionGroup(): RadioGroup {
        val group = RadioGroup(this).apply { orientation = RadioGroup.HORIZONTAL }
        Resolution.entries.forEach { resolution ->
            group.addView(
                RadioButton(this).apply {
                    id = View.generateViewId()
                    text = resolution.label
                    tag = resolution
                    isChecked = resolution == settings.resolution
                },
            )
        }
        group.setOnCheckedChangeListener { _, checkedId ->
            (group.findViewById<View>(checkedId)?.tag as? Resolution)?.let { settings.resolution = it }
        }
        return group
    }

    private fun buildRenderScaleGroup(): RadioGroup {
        val group = RadioGroup(this).apply { orientation = RadioGroup.HORIZONTAL }
        RenderScale.entries.forEach { scale ->
            group.addView(
                RadioButton(this).apply {
                    id = View.generateViewId()
                    text = scale.label
                    tag = scale
                    isChecked = scale == settings.renderScale
                },
            )
        }
        group.setOnCheckedChangeListener { _, checkedId ->
            (group.findViewById<View>(checkedId)?.tag as? RenderScale)?.let { settings.renderScale = it }
        }
        return group
    }

    private fun onScanRequested() {
        if (!Environment.isExternalStorageManager()) {
            showMessage("Grant all-files access before scanning.")
            return
        }
        scanGames(settings.gamesRoot)
    }

    /** First run: create the default games folder so the picker and the scan start somewhere useful. */
    private fun ensureDefaultGamesFolder() {
        if (settings.gamesRoot != LauncherSettings.DEFAULT_GAMES_ROOT) return
        val folder = File(LauncherSettings.DEFAULT_GAMES_ROOT)
        if (!folder.isDirectory && !folder.mkdirs()) {
            showMessage("Could not create ${folder.path}; choose a games folder instead.")
        }
    }

    /** System folder picker, opened at the current games folder (or its closest existing parent). */
    private fun openFolderPicker() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
        StoragePaths.nearestExistingFolder(settings.gamesRoot)
            ?.let(StoragePaths::initialUriFor)
            ?.let { intent.putExtra(DocumentsContract.EXTRA_INITIAL_URI, it) }
        try {
            startActivityForResult(intent, REQUEST_PICK_GAMES_FOLDER)
        } catch (e: ActivityNotFoundException) {
            showMessage("No folder picker is available on this device.")
        }
    }

    /** Directly select an executable when a folder picker cannot locate a valid PS5 dump. */
    private fun openGameFilePicker() {
        if (!Environment.isExternalStorageManager()) {
            showMessage("Grant all-files access before opening a game.")
            return
        }
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        try {
            startActivityForResult(intent, REQUEST_PICK_GAME_FILE)
        } catch (e: ActivityNotFoundException) {
            showMessage("No file picker is available. Use Enter game path instead.")
        }
    }

    /** Accept a native filesystem path as a fallback for file providers exposing opaque URIs. */
    private fun openManualGamePath() {
        val input = EditText(this).apply {
            setSingleLine(true)
            setText(settings.gamesRoot)
            selectAll()
            hint = "/storage/emulated/0/Games/PPSAxxxxx/eboot.bin"
        }
        AlertDialog.Builder(this)
            .setTitle("Game folder or executable path")
            .setView(input)
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Open") { _, _ -> openGameAtPath(input.text.toString()) }
            .show()
    }

    private fun openGameAtPath(input: String) {
        if (!Environment.isExternalStorageManager()) {
            showMessage("Grant all-files access before opening a game.")
            return
        }
        val path = input.trim().removeSurrounding("\"")
        if (path.isEmpty()) {
            showMessage("Enter a game directory or an executable path.")
            return
        }
        val game = File(path)
        when {
            game.isDirectory -> {
                if (File(game, GameLibrary.EBOOT).isFile) {
                    launch(GameLibrary.readEntry(game))
                } else {
                    settings.gamesRoot = game.absolutePath
                    folderPath.text = settings.gamesRoot
                    scanGames(settings.gamesRoot)
                }
            }
            game.isFile && game.name.equals(GameLibrary.EBOOT, ignoreCase = true) -> {
                val folder = game.parentFile ?: return
                // Direct launch deliberately bypasses the library scanner.
                launch(GameLibrary.readEntry(folder))
            }
            game.isFile && game.extension.lowercase() in setOf("elf", "self", "zar", "exfat") -> {
                // This route starts the selected file directly, bypassing library discovery.
                launch(GameEntry(game, game.nameWithoutExtension))
            }
            game.isFile -> showMessage("Unsupported file: ${game.name}. Select eboot.bin, ELF, SELF, ZAR or exFAT.")
            else -> showMessage("File/folder not accessible: ${game.path}. Check storage permission and path.")
        }
    }

    @Deprecated("Platform Activity result API; this launcher has no AndroidX dependency.")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQUEST_PICK_GAME_FILE && resultCode == RESULT_OK) {
            val uri = data?.data
            val path = if (uri?.authority == "com.android.externalstorage.documents") {
                try {
                    StoragePaths.documentIdToPath(DocumentsContract.getDocumentId(uri))
                } catch (e: IllegalArgumentException) {
                    null
                }
            } else {
                null
            }
            if (path == null) {
                showMessage("This file provider does not expose a real file path. " +
                    "Choose Internal storage in the Android picker or use Enter game path.")
            } else {
                openGameAtPath(path)
            }
            return
        }
        if (requestCode == GpuDriverPanel.REQUEST_PICK_DRIVER && resultCode == RESULT_OK) {
            data?.data?.let(drivers::onPicked)
            return
        }
        if (requestCode != REQUEST_PICK_GAMES_FOLDER || resultCode != RESULT_OK) return
        val path = data?.data?.let(StoragePaths::treeUriToPath)
        if (path == null) {
            showMessage("Pick a folder on device storage or an SD card (cloud folders can't be used).")
            return
        }
        settings.gamesRoot = path
        folderPath.text = path
        onScanRequested()
    }

    /** Scans off the main thread. Only the most recent request may update the list. */
    private fun scanGames(root: String) {
        val request = ++scanRequest
        scanStatus.text = "Scanning games in $root …"
        worker.execute {
            val result = try {
                GameLibrary.scan(File(root))
            } catch (e: Exception) {
                Log.e("KytyGameScan", "Scan failed for $root", e)
                ScanResult.Failure("Scan error in $root: ${e.message ?: e.javaClass.simpleName}")
            }
            mainHandler.post {
                if (request == scanRequest && !isFinishing) renderScan(result)
            }
        }
    }

    private fun renderScan(result: ScanResult) {
        grid.games = (result as? ScanResult.Found)?.games.orEmpty()
        when (result) {
            is ScanResult.Failure -> scanStatus.text = "Scan failed: ${result.message}"
            is ScanResult.Found -> {
                val diagnostics = result.diagnostics
                val summary = "${diagnostics.visitedDirectories} folders checked"
                val warnings = mutableListOf<String>()
                if (diagnostics.unreadableDirectories.isNotEmpty()) {
                    warnings += "${diagnostics.unreadableDirectories.size} folders could not be read " +
                        "(first: ${diagnostics.unreadableDirectories.first()})"
                }
                if (diagnostics.depthLimitedDirectories > 0) {
                    warnings += "scan depth limit reached"
                }
                if (diagnostics.hitDirectoryLimit) {
                    warnings += "scan stopped at folder limit"
                }
                val detail = (listOf(summary) + warnings).joinToString("; ")
                if (result.games.isEmpty()) {
                    scanStatus.text = "0 games detected in ${settings.gamesRoot}. $detail. " +
                        "Try Choose eboot / ELF or Enter path to test a dump directly."
                } else {
                    scanStatus.text = "${result.games.size} game(s) detected ($detail). " +
                        "Tap to play; long-press for game settings."
                }
            }
        }
    }

    private fun launch(game: GameEntry) {
        val options = LaunchOptions.resolve(
            globalResolution = settings.resolution,
            globalHud = settings.hudEnabled,
            globalTouch = settings.touchControlsEnabled,
            game = settings.gameSettings(game.path),
            globalRenderScale = settings.renderScale,
            globalGpuTurbo = settings.gpuTurbo,
        )
        val args = LaunchArgs.forGame(
            game.path, options.resolution, drivers.driverDirFor(game.path), options.renderScale, options.gpuTurbo,
        )
        startActivity(
            Intent(this, GameActivity::class.java)
                .putExtra(LaunchArgs.EXTRA_GAME_ARGS, args)
                .putExtra(LaunchArgs.EXTRA_GAME_PATH, game.path)
                .putExtra(LaunchArgs.EXTRA_ENV, options.env.map { it.toString() }.toTypedArray())
                .putExtra(LaunchArgs.EXTRA_HUD, options.hud)
                .putExtra(LaunchArgs.EXTRA_TOUCH_CONTROLS, options.touchControls),
        )
    }

    private fun toggle(text: String, checked: Boolean, onChange: (Boolean) -> Unit) = CheckBox(this).apply {
        this.text = text
        isChecked = checked
        setPadding(0, 0, 12.dp, 0)
        setOnCheckedChangeListener { _, value -> onChange(value) }
    }

    private fun refreshStorageState() {
        val granted = Environment.isExternalStorageManager()
        storageStatus.text = if (granted) {
            "All-files access: granted"
        } else {
            "All-files access: not granted. Games live on shared storage, so the launcher needs it."
        }
        grantButton.visibility = if (granted) View.GONE else View.VISIBLE
    }

    private fun openAllFilesAccessSettings() {
        val appScoped = Intent(
            Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
            Uri.fromParts("package", packageName, null),
        )
        try {
            startActivity(appScoped)
        } catch (e: ActivityNotFoundException) {
            startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
        }
    }

    private fun showMessage(text: String) {
        message.text = text
    }

    private companion object {
        const val REQUEST_PICK_GAMES_FOLDER = 1
        const val REQUEST_PICK_GAME_FILE = 3
        const val COVER_DP = 150
    }

    private fun label(
        text: String,
        sizeSp: Float = 14f,
        bold: Boolean = false,
        secondary: Boolean = false,
    ): TextView = TextView(this).apply {
        this.text = text
        textSize = sizeSp
        if (bold) setTypeface(typeface, Typeface.BOLD)
        if (secondary) alpha = 0.7f
        setPadding(0, 4.dp, 0, 4.dp)
    }
}
