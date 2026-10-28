package com.kytyps5.android

import android.app.Activity
import android.app.AlertDialog
import android.content.ActivityNotFoundException
import android.content.Intent
import android.graphics.Typeface
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
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
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.TextView
import com.kytyps5.android.hud.HudMode
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
    private var storageDialog: AlertDialog? = null

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
    private var activeGpuLabel: TextView? = null

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
        window.decorView.post { requestInitialStorageAccess() }
    }

    private fun requestInitialStorageAccess() {
        if (isFinishing || isDestroyed || settings.storageAccessPromptShown) return
        // Record the first opening even if access was already granted externally.
        settings.storageAccessPromptShown = true
        if (Environment.isExternalStorageManager()) return
        storageDialog = AlertDialog.Builder(this)
            .setTitle("Acesso aos arquivos")
            .setMessage("Permita o acesso a todos os arquivos para localizar e abrir seus jogos. Na próxima tela, ative a permissão para KytyPS5 Plus.")
            .setPositiveButton("Permitir") { _, _ -> openAllFilesAccessSettings() }
            .setNegativeButton("Agora não", null)
            .create().also { it.show() }
    }

    /** Installs the bundled Turnip on Adreno 8xx on first run (off the main thread: EGL probe + unzip). */
    private fun applyDefaultGpuDriver() {
        val setup = DefaultGpuDriverSetup(this, settings, drivers.store)
        worker.execute {
            val changed = setup.apply()
            mainHandler.post {
                drivers.refresh()
                activeGpuLabel?.text = selectedDriverLabel()
                if (changed != null) showMessage(changed)
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
        storageDialog?.dismiss()
        storageDialog = null
        worker.shutdownNow()
        covers.shutdown()
        super.onDestroy()
    }


    /** Native Android recreation of the approved landscape HTML prototype. */
    private fun buildUi(): View {
        window.statusBarColor = Color.rgb(10, 21, 37)
        window.navigationBarColor = Color.rgb(10, 21, 37)
        nativeStatus = label(if (NativeLibrary.isInstalled(this)) "● Core ativo" else
            "Core ausente: " + NativeLibrary.FILE_NAME, sizeSp = 12f)
        storageStatus = label("", sizeSp = 12f)
        grantButton = dashboardButton("Permitir acesso aos arquivos", true) { openAllFilesAccessSettings() }
        folderPath = label(settings.gamesRoot, sizeSp = 11f, secondary = true).apply {
            maxLines = 2
            ellipsize = android.text.TextUtils.TruncateAt.MIDDLE
        }
        resolutionGroup = buildResolutionGroup()
        renderScaleGroup = buildRenderScaleGroup()
        message = label("", sizeSp = 11f, secondary = true)
        scanStatus = label("Verificando jogos...", sizeSp = 11f, secondary = true)

        val topBar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(14.dp, 5.dp, 14.dp, 5.dp)
            setBackgroundColor(Color.rgb(12, 25, 44))
            addView(label("✦", sizeSp = 25f, bold = true).apply {
                setTextColor(Color.rgb(64, 188, 255))
            })
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(8.dp, 0, 0, 0)
                addView(label("KytyPS5 Plus", sizeSp = 17f, bold = true).apply {
                    setTextColor(Color.WHITE)
                })
                addView(label("DAEDALUS EMULATOR", sizeSp = 9f, bold = true, secondary = true))
            }, LinearLayout.LayoutParams(0, -2, 1f))
            addView(nativeStatus)
        }

        val leftNav = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.rgb(12, 25, 44))
            setPadding(7.dp, 16.dp, 7.dp, 10.dp)
        }
        val libraryNav = dashboardButton("▦  Biblioteca") {}
        val settingsNav = dashboardButton("⚙  Configurações") {}
        leftNav.addView(libraryNav)
        leftNav.addView(settingsNav)
        leftNav.addView(android.widget.Space(this@MainActivity), LinearLayout.LayoutParams(1.dp, 0, 1f))
        leftNav.addView(label("KytyPS5 Plus • ARM64", sizeSp = 9f, secondary = true))

        val gameGrid = GridView(this).apply {
            adapter = grid
            numColumns = 1
            stretchMode = GridView.STRETCH_COLUMN_WIDTH
            verticalSpacing = 9.dp
            horizontalSpacing = 0
            selector = android.graphics.drawable.ColorDrawable(Color.TRANSPARENT)
        }
        val emptyState = label("Nenhum jogo encontrado. Escolha a pasta do jogo.",
            secondary = true).apply {
            gravity = Gravity.CENTER
        }
        val libraryLeft = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(label("Meus jogos", sizeSp = 14f, bold = true))
            addView(FrameLayout(context).apply {
                addView(gameGrid, FrameLayout.LayoutParams(-1, -1))
                addView(emptyState, FrameLayout.LayoutParams(-1, -2, Gravity.CENTER))
                gameGrid.emptyView = emptyState
            }, LinearLayout.LayoutParams(-1, 0, 1f))
        }

        activeGpuLabel = label(selectedDriverLabel(), sizeSp = 12f, bold = true)
        val driverCard = dashboardPanel("DRIVER VULKAN PADRÃO",
            "Driver selecionado para novos jogos. Alterações individuais têm prioridade.",
            activeGpuLabel!!)
        driverCard.setOnClickListener { settingsNav.performClick() }
        val moreActions = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
            addView(dashboardButton("Exportar logs ZIP") { openLogExport() })
            addView(folderPath)
            addView(storageStatus)
            addView(grantButton)
        }
        val more = dashboardButton("Mais opções da biblioteca  ▾") {}
        more.setOnClickListener {
            val show = moreActions.visibility != View.VISIBLE
            moreActions.visibility = if (show) View.VISIBLE else View.GONE
            more.text = if (show) "Ocultar opções  ▴" else "Mais opções da biblioteca  ▾"
        }
        val rightSide = ScrollView(this).apply {
            isFillViewport = false
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                addView(dashboardButton("Escolher pasta do jogo", true) { openFolderPicker() })
                addView(driverCard)
                addView(dashboardButton("↻   Atualizar biblioteca") { onScanRequested() })
                addView(more)
                addView(moreActions)
                addView(scanStatus)
                addView(message)
            })
        }
        val libraryRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(libraryLeft, LinearLayout.LayoutParams(0, -1, 1.75f).apply { marginEnd = 12.dp })
            addView(rightSide, LinearLayout.LayoutParams(0, -1, 1f))
        }
        val libraryPage = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(12.dp, 10.dp, 12.dp, 8.dp)
            addView(label("KYTYPS5 PLUS", sizeSp = 10f, bold = true).apply {
                setTextColor(Color.rgb(85, 208, 255))
            })
            addView(label("Sua biblioteca.", sizeSp = 24f, bold = true))
            addView(label("Seus jogos PS5 em um só lugar, prontos para iniciar.",
                sizeSp = 11f, secondary = true))
            addView(libraryRow, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = 8.dp })
        }

        val generalPage = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(dashboardPanel("BIBLIOTECA E ARMAZENAMENTO", "Pasta dos jogos e permissões de acesso.",
                label("Pasta atual:", bold = true),
                label(settings.gamesRoot, sizeSp = 12f, secondary = true),
                dashboardButton("Escolher pasta") { openFolderPicker() },
                dashboardButton("Exportar diagnósticos") { openLogExport() },
            ))
            addView(dashboardPanel("DESEMPENHO", "Ajustes rápidos do sistema.",
                toggle(GameSettingsDialog.GPU_TURBO_LABEL, settings.gpuTurbo) { settings.gpuTurbo = it },
            ))
        }

        val videoPage = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(dashboardPanel("VÍDEO", "Resolução de saída e escala de renderização.",
                label("Resolução de saída", bold = true), resolutionGroup,
                label("Escala interna", bold = true), renderScaleGroup,
            ))
        }

        val gpuPanel = drivers.build(onChanged = { activeGpuLabel?.text = selectedDriverLabel() })
        val gpuPage = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(dashboardPanel("TURNIP / VULKAN", "Escolha o driver global ou importe um Turnip ZIP.",
                label("Mainline Turnip V32 é o driver padrão para Adreno 8xx.",
                    sizeSp = 12f, secondary = true),
                gpuPanel,
            ))
            addView(dashboardPanel("POR JOGO", "Toque na engrenagem da capa para escolher um driver só para aquele título."))
        }

        val modeGroup = RadioGroup(this).apply {
            orientation = RadioGroup.VERTICAL
            HudMode.entries.forEach { hudMode ->
                addView(RadioButton(context).apply {
                    id = View.generateViewId()
                    text = when (hudMode) {
                        HudMode.FULL -> "Completa  ·  FPS, frame time, GPU, CPU, RAM, swap, bateria"
                        HudMode.COMPACT -> "Compacta  ·  FPS, GPU, CPU, RAM"
                        HudMode.MINIMAL -> "Mínima  ·  apenas FPS"
                    }
                    tag = hudMode
                    textSize = 12f
                    setTextColor(Color.WHITE)
                    isChecked = hudMode == settings.hudMode
                })
            }
            setOnCheckedChangeListener { _, id ->
                (findViewById<View>(id)?.tag as? HudMode)?.let { settings.hudMode = it }
            }
        }
        val controlsPage = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(dashboardPanel("HUD DE DESEMPENHO", "Painel azul translúcido, com três níveis de detalhes.",
                toggle("Exibir HUD no jogo", settings.hudEnabled) { settings.hudEnabled = it },
                modeGroup,
            ))
            addView(dashboardPanel("CONTROLES", "Controles virtuais e configurações durante o jogo.",
                toggle("Controles na tela", settings.touchControlsEnabled) { settings.touchControlsEnabled = it },
                label("O menu ⋯ durante o jogo permite alterar a HUD e sair com segurança.",
                    sizeSp = 12f, secondary = true),
            ))
        }

        val settingsSections = listOf(generalPage, videoPage, gpuPage, controlsPage)
        val settingsTabs = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val settingsPage = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(12.dp, 7.dp, 12.dp, 7.dp)
            addView(label("AJUSTES DO EMULADOR", sizeSp = 10f, bold = true).apply {
                setTextColor(Color.rgb(85, 208, 255))
            })
            addView(label("Configurações.", sizeSp = 23f, bold = true))
            addView(label("Organização por categoria, sem menus sobrecarregados.",
                sizeSp = 11f, secondary = true))
            addView(settingsTabs)
            addView(ScrollView(context).apply {
                addView(FrameLayout(context).apply {
                    settingsSections.forEach { section ->
                        addView(section, FrameLayout.LayoutParams(-1, -2))
                    }
                })
            }, LinearLayout.LayoutParams(-1, 0, 1f))
            visibility = View.GONE
        }
        val tabNames = listOf("Geral", "Vídeo", "GPU / Turnip", "Controles")
        val tabButtons = tabNames.mapIndexed { index, name ->
            dashboardButton(name) {}
        }
        fun selectSettings(index: Int) {
            settingsSections.forEachIndexed { position, section ->
                section.visibility = if (index == position) View.VISIBLE else View.GONE
            }
            tabButtons.forEachIndexed { position, button ->
                decorateButton(button, index == position)
            }
        }
        tabButtons.forEachIndexed { idx, button ->
            settingsTabs.addView(button, LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = 5.dp })
            button.setOnClickListener { selectSettings(idx) }
        }
        selectSettings(0)

        val pages = FrameLayout(this).apply {
            addView(libraryPage, FrameLayout.LayoutParams(-1, -1))
            addView(settingsPage, FrameLayout.LayoutParams(-1, -1))
        }
        fun selectMain(library: Boolean) {
            libraryPage.visibility = if (library) View.VISIBLE else View.GONE
            settingsPage.visibility = if (library) View.GONE else View.VISIBLE
            decorateButton(libraryNav, library)
            decorateButton(settingsNav, !library)
        }
        libraryNav.setOnClickListener { selectMain(true) }
        settingsNav.setOnClickListener { selectMain(false); selectSettings(2) }
        selectMain(true)

        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.rgb(9, 18, 33))
            // Android 15 enforces edge-to-edge; reserve system bars/cutouts explicitly.
            setOnApplyWindowInsetsListener { view, insets ->
                val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
                insets
            }
            addView(topBar, LinearLayout.LayoutParams(-1, -2))
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(ScrollView(context).apply { isFillViewport = true; addView(leftNav) }, LinearLayout.LayoutParams(137.dp, -1))
                addView(pages, LinearLayout.LayoutParams(0, -1, 1f))
            }, LinearLayout.LayoutParams(-1, 0, 1f))
        }
    }

    private fun selectedDriverLabel(): String =
        drivers.store.find(settings.gpuDriverId)?.label ?: "Vulkan do sistema"

    private fun dashboardPanel(title: String, subtitle: String, vararg items: View): LinearLayout =
        LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(13.dp, 10.dp, 13.dp, 12.dp)
            background = GradientDrawable().apply {
                setColor(Color.rgb(20, 34, 55))
                cornerRadius = 14.dp.toFloat()
                setStroke(1.dp, Color.rgb(41, 67, 95))
            }
            addView(label(title, 14f, bold = true).apply {
                setTextColor(Color.rgb(115, 210, 250))
            })
            addView(label(subtitle, 11f, secondary = true))
            items.forEach { addView(it) }
            layoutParams = LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = 10.dp }
        }

    private fun decorateButton(button: Button, selected: Boolean) {
        button.isAllCaps = false
        button.setTextColor(if (selected) Color.rgb(120, 219, 255) else Color.rgb(194, 211, 232))
        button.background = GradientDrawable().apply {
            cornerRadius = 12.dp.toFloat()
            setColor(if (selected) Color.rgb(24, 67, 106) else Color.rgb(20, 38, 60))
            setStroke(1.dp, if (selected) Color.rgb(53, 133, 188) else Color.rgb(38, 66, 92))
        }
    }

    private fun dashboardButton(title: String, highlight: Boolean = false, action: () -> Unit): Button =
        Button(this).apply {
            text = title
            textSize = 12f
            minHeight = 42.dp
            minimumHeight = 42.dp
            setPadding(8.dp, 0, 8.dp, 0)
            decorateButton(this, highlight)
            setOnClickListener { action() }
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

    /** User chooses a destination (including Download); the app never needs root access. */
    private fun openLogExport() {
        val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/zip"
            putExtra(Intent.EXTRA_TITLE, Diagnostics.suggestedFileName())
        }
        try {
            startActivityForResult(intent, REQUEST_EXPORT_LOGS)
        } catch (e: ActivityNotFoundException) {
            showMessage("No document picker is available for exporting logs.")
        }
    }

    @Deprecated("Platform Activity result API; this launcher has no AndroidX dependency.")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQUEST_EXPORT_LOGS) {
            if (resultCode == RESULT_OK) {
                data?.data?.let { destination ->
                    showMessage("Exporting logs…")
                    worker.execute {
                        val outcome = try {
                            Diagnostics.export(this, destination)
                        } catch (e: Exception) {
                            Log.e("KytyDiagnostics", "Log export failed", e)
                            "Log export failed: ${e.message ?: e.javaClass.simpleName}"
                        }
                        mainHandler.post { if (!isFinishing) showMessage(outcome) }
                    }
                }
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
                        "Escolha uma pasta contendo o jogo ou suas subpastas."
                } else {
                    scanStatus.text = "${result.games.size} game(s) detected ($detail). " +
                        "Tap to play; long-press for game settings."
                }
            }
        }
    }

    private fun launch(game: GameEntry) {
        val gameOverrides = settings.gameSettings(game.path)
        val options = LaunchOptions.resolve(
            globalResolution = settings.resolution,
            globalHud = settings.hudEnabled,
            globalTouch = settings.touchControlsEnabled,
            game = gameOverrides,
            globalRenderScale = settings.renderScale,
            globalGpuTurbo = GtSigkillClockPolicy.turboEnabled(
                game.titleId, gameOverrides.gpuTurbo, settings.gpuTurbo
            ),
            globalHudMode = settings.hudMode,
        )
        val args = LaunchArgs.forGame(
            game.path, options.resolution, drivers.driverDirFor(game.path), options.renderScale, options.gpuTurbo,
            titleId = game.titleId,
        )
        startActivity(
            Intent(this, GameActivity::class.java)
                .putExtra(LaunchArgs.EXTRA_GAME_ARGS, args)
                .putExtra(LaunchArgs.EXTRA_GAME_PATH, game.path)
                .putExtra(LaunchArgs.EXTRA_ENV, options.env.map { it.toString() }.toTypedArray())
                .putExtra(LaunchArgs.EXTRA_HUD, options.hud)
                .putExtra(LaunchArgs.EXTRA_HUD_MODE, options.hudMode.storageValue)
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
            try {
                startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
            } catch (missing: ActivityNotFoundException) {
                showMessage("Ative o acesso a todos os arquivos nas configurações do Android para KytyPS5 Plus.")
            }
        }
    }

    private fun showMessage(text: String) {
        message.text = text
    }

    private companion object {
        const val REQUEST_PICK_GAMES_FOLDER = 1
        const val REQUEST_EXPORT_LOGS = 4
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
        setTextColor(if (secondary) Color.rgb(151, 171, 198) else Color.rgb(237, 245, 255))
        setPadding(0, 4.dp, 0, 4.dp)
    }
}
