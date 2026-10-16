package com.kytyps5.android

import android.app.Activity
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
import android.view.Gravity
import android.view.View
import android.view.WindowInsets
import android.view.ViewGroup
import android.widget.Button
import android.widget.CheckBox
import android.widget.GridView
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.TextView
import com.kytyps5.android.input.OverlayProfileStore
import com.kytyps5.android.library.CoverLoader
import com.kytyps5.android.library.GameGridAdapter
import com.kytyps5.android.library.GameSettingsDialog
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
    private lateinit var message: TextView
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
        folderPath = label(settings.gamesRoot, sizeSp = 16f)
        resolutionGroup = buildResolutionGroup()
        message = label("", secondary = true)

        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(16.dp, 16.dp, 16.dp, 16.dp)
            addView(label("KytyPS5", sizeSp = 22f, bold = true))
            addView(nativeStatus)
            addView(storageStatus)
            addView(grantButton)
            addView(label("Games folder", bold = true))
            addView(
                LinearLayout(context).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                    addView(folderPath, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
                    addView(Button(context).apply {
                        text = "Choose folder…"
                        setOnClickListener { openFolderPicker() }
                    })
                    addView(Button(context).apply {
                        text = "Rescan"
                        setOnClickListener { onScanRequested() }
                    })
                },
            )
            addView(
                LinearLayout(context).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                    addView(label("Resolution  ", bold = true))
                    addView(resolutionGroup)
                    addView(toggle("Performance HUD", settings.hudEnabled) { settings.hudEnabled = it })
                    addView(toggle("Touch controls", settings.touchControlsEnabled) { settings.touchControlsEnabled = it })
                },
            )
            addView(label("GPU driver", bold = true))
            addView(drivers.build(onChanged = {}))
            addView(message)
            addView(
                GridView(context).apply {
                    adapter = grid
                    numColumns = GridView.AUTO_FIT
                    columnWidth = (COVER_DP + 12).dp
                    stretchMode = GridView.STRETCH_SPACING_UNIFORM
                    verticalSpacing = 4.dp
                },
                LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f),
            )
        }
        // Edge-to-edge (targetSdk 35): keep content below the status bar and above the nav bar.
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

    @Deprecated("Platform Activity result API; this launcher has no AndroidX dependency.")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
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
        showMessage("Scanning $root ...")
        worker.execute {
            val result = GameLibrary.scan(File(root))
            mainHandler.post {
                if (request == scanRequest && !isFinishing) renderScan(result)
            }
        }
    }

    private fun renderScan(result: ScanResult) {
        grid.games = (result as? ScanResult.Found)?.games.orEmpty()
        when (result) {
            is ScanResult.Failure -> showMessage(result.message)
            is ScanResult.Found -> when {
                result.games.isEmpty() ->
                    showMessage("No games found here. Choose a folder containing game dumps (a folder with sce_sys/param.json, or a .zar file).")
                else -> showMessage("${result.games.size} game(s) found. Tap to play, ⚙ or long-press for game settings.")
            }
        }
    }

    private fun launch(game: GameEntry) {
        val options = LaunchOptions.resolve(
            globalResolution = settings.resolution,
            globalHud = settings.hudEnabled,
            globalTouch = settings.touchControlsEnabled,
            game = settings.gameSettings(game.path),
        )
        val args = LaunchArgs.forGame(game.path, options.resolution, drivers.driverDirFor(game.path))
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
