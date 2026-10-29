package com.kytyps5.android

import android.annotation.TargetApi
import android.app.ActivityManager
import android.content.Context
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.os.SystemClock
import android.os.ProfilingManager
import android.os.ProfilingResult
import android.util.AtomicFile
import java.io.File
import java.io.FileOutputStream
import java.util.Properties
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.function.Consumer
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/** App-owned flight recorder + OS-provided, app-scoped profiling. Never uses logcat/root. */
object CrashDiagnostics {
    private val executor = Executors.newSingleThreadScheduledExecutor()
    private val main = Handler(Looper.getMainLooper())
    private val pendingBeat = AtomicBoolean(false)
    @Volatile private var lastBeat = SystemClock.elapsedRealtime()
    @Volatile private var titleAt = 0L
    @Volatile private var phase = "launcher"
    @Volatile private var session: File? = null
    private var sampling: ScheduledFuture<*>? = null
    private var app: Context? = null
    private var profileListenerRegistered = false
    private var lastStack = 0L
    private var summaryErrorReported = false
    private var resourceSources: List<File> = emptyList()
    private const val MAX_TRACE = 64L * 1024 * 1024
    private const val MAX_TEXT = 2L * 1024 * 1024
    private fun root(c: Context) = File(c.filesDir, "crash-diagnostics").apply { mkdirs() }
    private fun safe(block: () -> String): String = try { block() } catch (e: Exception) {
        "UNAVAILABLE ${e.javaClass.simpleName}: ${e.message}"
    }
    private fun read(file: File, limit: Int = 16384): String = safe {
        file.inputStream().bufferedReader().use { it.readTextLimited(limit) }
    }
    private fun java.io.Reader.readTextLimited(limit: Int): String {
        val b = CharArray(limit); var used = 0
        while (used < limit) { val n = read(b, used, limit - used); if (n < 0) break; used += n }
        return String(b, 0, used)
    }
    private fun write(file: File, value: String) {
        val atomic = AtomicFile(file)
        var out: FileOutputStream? = null
        try { out = atomic.startWrite(); out.write(value.toByteArray()); atomic.finishWrite(out) }
        catch (e: Exception) { if (out != null) atomic.failWrite(out) }
    }
    private fun append(file: File, value: String, limit: Long = MAX_TEXT) {
        try {
            if (file.length() > limit) {
                val previous = File(file.parentFile, file.name + ".previous")
                previous.delete(); file.renameTo(previous)
            }
            FileOutputStream(file, true).use { it.write(value.toByteArray()); it.fd.sync() }
        } catch (_: Exception) { /* original app/game behavior must survive recorder errors */ }
    }
    private fun event(c: Context, value: String) {
        append(File(root(c), "profiling-events.txt"), "wall=${System.currentTimeMillis()} elapsed=${SystemClock.elapsedRealtime()} $value\n", 128 * 1024)
    }

    fun initialize(context: Context) {
        val c = context.applicationContext
        app = c
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        if (previous != null) Thread.setDefaultUncaughtExceptionHandler { thread, failure ->
            try {
                val directory = session ?: root(c)
                append(File(directory, "uncaught-process-${Process.myPid()}.txt"),
                    "wall=${System.currentTimeMillis()} elapsed=${SystemClock.elapsedRealtime()} pid=${Process.myPid()} thread=${thread.name} phase=$phase\n" +
                        failure.stackTraceToString().take(65536) + "\n", 128 * 1024)
            } catch (_: Throwable) {
                // An OOM may prevent even constructing this report; always preserve Android's handler.
            } finally { previous.uncaughtException(thread, failure) }
        }
        executor.execute {
            recover(c)
            if (Build.VERSION.SDK_INT >= 35) {
                try { Profile35.register(c) }
                catch (e: Exception) { event(c, "PROFILING_UNAVAILABLE ${e.javaClass.simpleName}: ${e.message}") }
                catch (e: LinkageError) { event(c, "PROFILING_UNAVAILABLE ${e.javaClass.simpleName}: ${e.message}") }
            } else event(c, "PROFILING_UNAVAILABLE API=${Build.VERSION.SDK_INT}; API35 required")
        }
    }

    fun preserveNativeBeforeLaunch(context: Context) {
        try {
            val name = File(root(context), "active-session.txt").takeIf { it.isFile }?.readText()?.trim() ?: return
            if (!Regex("session-[0-9]+-[0-9]+").matches(name)) return
            val target = File(root(context), name)
            if (!target.isDirectory) return
            val origin = File(context.filesDir, "logs/signal-origin.txt")
            val expected = name.substringAfterLast('-').toIntOrNull() ?: return
            if (!origin.isFile || origin.useLines { it.firstOrNull() } != "SIGKILL syscall provenance v1 pid=$expected") return
            for (file in listOf("signal-origin.txt", "signal-origin-maps.txt", "signal-origin.txt.stack")) {
                val source = File(context.filesDir, "logs/$file")
                if (source.isFile && source.length() <= MAX_TEXT) source.copyTo(File(target, file), overwrite = true)
            }
        } catch (_: Exception) { }
    }

    fun start(context: Context, game: String) {
        // Include Neptunia ReVerse in the same per-PID crash evidence pipeline as GT.
        // Without this the exported active-session and signal-origin data remain from
        // an unrelated GT process, preventing accurate SIGKILL attribution.
        if (!game.contains("PPSA24156", true) && !game.contains("PPSA02721", true)) return
        val c = context.applicationContext
        phase = "game-create"; titleAt = 0L
        executor.execute {
            sampling?.cancel(false)
            val now = System.currentTimeMillis()
            val dir = File(root(c), "session-$now-${Process.myPid()}").apply { mkdirs() }
            val pkg = c.packageManager.getPackageInfo(c.packageName, 0)
            write(File(dir, "session.properties"), "pid=${Process.myPid()}\nuid=${Process.myUid()}\nstarted=$now\nelapsed=${SystemClock.elapsedRealtime()}\nversionCode=${pkg.longVersionCode}\ncollector=2\n")
            write(File(dir, "launch.txt"), "game=$game\ndevice=${Build.FINGERPRINT}\nkernel=${System.getProperty("os.version")}\n")
            write(File(root(c), "active-session.txt"), dir.name)
            session = dir; summaryErrorReported = false
            lastBeat = SystemClock.elapsedRealtime(); pendingBeat.set(false)
            resourceSources = discoverSources(c, dir)
            sampling = executor.scheduleWithFixedDelay({
                try { sample(c, dir) } catch (e: Exception) { append(File(dir, "recorder-errors.txt"), "$e\n") }
            }, 0, 1, TimeUnit.SECONDS)
            if (Build.VERSION.SDK_INT >= 35) {
                try { Profile35.request(c, dir.name) }
                catch (e: Exception) { event(c, "TRACE_REQUEST_FAILED $e") }
                catch (e: LinkageError) { event(c, "TRACE_REQUEST_FAILED $e") }
            }
            // Keep four complete sessions; never replace the preceding crash with a new boot.
            root(c).listFiles()?.filter { it.isDirectory && it.name.startsWith("session-") }
                ?.sortedByDescending { it.name }?.drop(4)?.forEach { it.deleteRecursively() }
        }
    }

    fun phase(value: String) {
        phase = value
        val dir = session ?: return
        executor.execute { append(File(dir, "lifecycle.txt"), "${System.currentTimeMillis()} ${SystemClock.elapsedRealtime()} $value\n", 128 * 1024) }
    }
    fun title() { titleAt = SystemClock.elapsedRealtime() }
    fun stop() {
        phase("game-destroy")
        executor.execute { sampling?.cancel(false); sampling = null; session = null }
    }

    private fun discoverSources(c: Context, dir: File): List<File> {
        val candidates = mutableListOf(
            File("/proc/self/oom_score_adj"), File("/proc/self/cgroup"),
            File("/proc/pressure/memory"), File("/proc/pressure/cpu"), File("/proc/pressure/io"),
        )
        val kgsl = File("/sys/class/kgsl/kgsl-3d0")
        for (name in listOf("reset_count", "gpu_reset_count", "fault_count", "gpu_faults", "gpu_fault"))
            candidates.add(File(kgsl, name))
        // Actual exported cgroup controller files only. Missing/denied is recorded as such.
        read(File("/proc/self/cgroup")).lineSequence().forEach { line ->
            val parts = line.split(':', limit = 3)
            if (parts.size == 3 && !parts[2].split('/').contains("..")) {
                val mounts = if (parts[1].isEmpty()) listOf("/sys/fs/cgroup")
                    else if (parts[1].split(',').contains("memory")) listOf("/dev/memcg", "/sys/fs/cgroup/memory") else emptyList()
                for (mount in mounts) {
                    val leaf = File(mount, parts[2].trimStart('/'))
                    for (base in listOfNotNull(leaf, leaf.parentFile).distinct())
                        for (name in listOf("memory.events", "memory.events.local", "memory.oom_control", "memory.failcnt"))
                            candidates.add(File(base, name))
                }
            }
        }
        val report = buildString {
            appendLine("pid=${Process.myPid()} uid=${Process.myUid()}")
            appendLine("lowMemoryKillReportSupported=${safe { ActivityManager.isLowMemoryKillReportSupported().toString() }}")
            appendLine("NOTE: global counters/pressure are context, not attribution to this process.")
            appendLine("NOTE: absent GPU fault counters do NOT prove absence of a GPU fault.")
            candidates.distinctBy { it.path }.forEach { f -> appendLine("${f.path}: ${read(f, 4096)}") }
        }
        write(File(dir, "source-capabilities.txt"), report)
        return candidates.distinctBy { it.path }.filter { it.canRead() }
    }

    private fun sample(c: Context, dir: File) {
        val elapsed = SystemClock.elapsedRealtime()
        if (pendingBeat.compareAndSet(false, true)) main.post {
            lastBeat = SystemClock.elapsedRealtime(); pendingBeat.set(false)
        }
        val info = ActivityManager.RunningAppProcessInfo()
        ActivityManager.getMyMemoryState(info)
        val lag = elapsed - lastBeat
        val summary = "v2 pid=${Process.myPid()} session=${dir.name} phase=$phase mainLag=$lag frameAge=${if (titleAt == 0L) -1 else elapsed-titleAt}"
        val summaryResult = safe { (c.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager).setProcessStateSummary(summary.take(128).toByteArray(Charsets.US_ASCII)); "ok" }
        if (summaryResult != "ok" && !summaryErrorReported) {
            summaryErrorReported = true
            append(File(dir, "recorder-errors.txt"), "PROCESS_STATE_SUMMARY $summaryResult\n")
        }
        val text = buildString {
            appendLine("--- wall=${System.currentTimeMillis()} elapsed=$elapsed pid=${Process.myPid()} importance=${info.importance} phase=$phase mainLagMs=$lag titleAgeMs=${if (titleAt == 0L) -1 else elapsed-titleAt}")
            appendLine(read(File("/proc/self/status"), 12000))
            for (source in resourceSources) appendLine("${source.path}: ${read(source, 4096)}")
            if (lag > 4000 && elapsed - lastStack > 5000) {
                lastStack = elapsed
                appendLine("OWN_MAIN_THREAD_STACK (lag evidence, NOT an ANR verdict):")
                Looper.getMainLooper().thread.stackTrace.take(48).forEach { appendLine(it) }
            }
        }
        append(File(dir, "timeline.txt"), text)
    }

    @Synchronized
    private fun recover(c: Context) {
        val am = c.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val exits = try { am.getHistoricalProcessExitReasons(c.packageName, 0, 32) } catch (_: Exception) { emptyList() }
        val origin = File(c.filesDir, "logs/signal-origin.txt").takeIf { it.isFile }?.readText().orEmpty()
        root(c).listFiles()?.filter { it.isDirectory && it.name.startsWith("session-") }?.forEach { dir ->
            val properties = Properties()
            try { File(dir, "session.properties").inputStream().use(properties::load) } catch (_: Exception) { return@forEach }
            val pid = properties.getProperty("pid")?.toIntOrNull() ?: return@forEach
            val started = properties.getProperty("started")?.toLongOrNull() ?: return@forEach
            val nextStart = root(c).listFiles().orEmpty().asSequence().filter { it.isDirectory && it != dir }
                .mapNotNull { other ->
                    try {
                        val p = Properties(); File(other, "session.properties").inputStream().use(p::load)
                        p.getProperty("started")?.toLongOrNull()?.takeIf { it > started && p.getProperty("pid")?.toIntOrNull() == pid }
                    } catch (_: Exception) { null }
                }.minOrNull() ?: Long.MAX_VALUE
            val exit = exits.firstOrNull { CrashEvidence.matches(CrashEvidence.Exit(it.pid, it.timestamp, it.reason, it.status), pid, started, nextStart) }
            // Only use the native file when its PID matches this exact session.
            val native = origin.takeIf { it.lineSequence().firstOrNull() == "SIGKILL syscall provenance v1 pid=$pid" }.orEmpty()
            if (native.isNotEmpty()) write(File(dir, "signal-origin.txt"), native)
            val recorded = File(dir, "signal-origin.txt").takeIf { it.isFile }?.readText().orEmpty()
            write(File(dir, "ANALYSIS.txt"), buildString {
                appendLine(CrashEvidence.conclusion(exit?.let { CrashEvidence.Exit(it.pid, it.timestamp, it.reason, it.status) }, recorded))
                appendLine("Session=$dir\nMatchedPID=$pid StartedWall=$started")
                if (exit != null) {
                    appendLine("exitWall=${exit.timestamp} reason=${exit.reason} status=${exit.status} importance=${exit.importance}")
                    appendLine("description=${exit.description}")
                    appendLine("processStateSummary=${exit.processStateSummary?.toString(Charsets.UTF_8)}")
                    appendLine("pssKb=${exit.pss} rssKb=${exit.rss}")
                }
                appendLine("Profiling trigger/result metadata and traces are exported separately, including any API/permission/rate-limit failures.")
                appendLine("Missing evidence means unresolved. No source in this APK can guarantee access to an arbitrary external sender's PID on an unprivileged device.")
            })
        }
    }

    fun export(c: Context, zip: ZipOutputStream): Int {
        recover(c)
        var count = 0; var remaining = 96L * 1024 * 1024
        val directory = root(c)
        val files = directory.walkTopDown().maxDepth(3).filter { it.isFile && !it.name.endsWith(".bak") }
            .sortedWith(compareBy<File> { it.extension == "perfetto-trace" }.thenByDescending { it.lastModified() }).toList()
        val inventory = StringBuilder("Export inventory; wall=${System.currentTimeMillis()}\n")
        for (file in files) {
            val size = file.length()
            if (size <= 0 || size > MAX_TRACE || size > remaining) {
                inventory.appendLine("SKIPPED ${file.relativeTo(directory).invariantSeparatorsPath} bytes=$size remainingBudget=$remaining")
                continue
            }
            inventory.appendLine("INCLUDED ${file.relativeTo(directory).invariantSeparatorsPath} bytes=$size")
            zip.putNextEntry(ZipEntry("crash-diagnostics/${file.relativeTo(directory).invariantSeparatorsPath}"))
            file.inputStream().use { it.copyTo(zip) }; zip.closeEntry(); remaining -= size; count++
        }
        zip.putNextEntry(ZipEntry("crash-diagnostics/EXPORT-INVENTORY.txt"))
        zip.write(inventory.toString().toByteArray()); zip.closeEntry()
        return count + 1
    }

    @TargetApi(35)
    private object Profile35 {
        private var manager: ProfilingManager? = null
        private val listener = Consumer<ProfilingResult> { result ->
            val c = app ?: return@Consumer
            val trigger = safe { result.javaClass.getMethod("getTriggerType").invoke(result).toString() }
            event(c, "PROFILE_RESULT error=${result.errorCode} message=${result.errorMessage} tag=${result.tag} trigger=$trigger path=${result.resultFilePath}")
            val path = result.resultFilePath ?: return@Consumer
            val source = File(path)
            val owned = safe { source.canonicalPath.startsWith(c.filesDir.canonicalPath + "/").toString() } == "true"
            if (!owned || !source.isFile || source.length() > MAX_TRACE) { event(c, "TRACE_REJECTED not app-owned/oversized/missing"); return@Consumer }
            val profiles = File(root(c), "profiles").apply { mkdirs() }
            val destination = File(profiles, "${System.currentTimeMillis()}-${source.name}")
            try { source.copyTo(destination, overwrite = false) }
            catch (e: Exception) { event(c, "TRACE_COPY_FAILED $e"); return@Consumer }
            write(File(profiles, destination.name + ".txt"), "error=${result.errorCode}\ntrigger=$trigger\ntag=${result.tag}\ndeliveredWall=${System.currentTimeMillis()}\noriginal=${source.name}\n")
            profiles.listFiles()?.filter { it.extension == "perfetto-trace" }?.sortedByDescending { it.lastModified() }?.drop(4)?.forEach {
                File(profiles, it.name + ".txt").delete(); it.delete()
            }
        }
        fun register(c: Context) {
            if (profileListenerRegistered) return
            val m = c.getSystemService(ProfilingManager::class.java)
            if (m == null) { event(c, "PROFILING_UNAVAILABLE service=null"); return }
            m.registerForAllProfilingResults(executor, listener)
            manager = m; profileListenerRegistered = true
            event(c, "GLOBAL_PROFILING_LISTENER_REGISTERED API=${Build.VERSION.SDK_INT}; accepts deferred results after app restart")
        }
        fun request(c: Context, sessionName: String) {
            register(c)
            val m = manager ?: return
            // Use only public methods actually advertised by this OS, including optional minor-version additions.
            try {
                val all = m.javaClass.methods.firstOrNull { it.name == "addAllProfilingTriggers" && it.parameterCount == 0 }
                if (all != null) { all.invoke(m); event(c, "ALL_OS_PROFILING_TRIGGERS_REGISTERED") }
                else if (Build.VERSION.SDK_INT >= 36) {
                    val type = Class.forName("android.os.ProfilingTrigger")
                    val builder = Class.forName("android.os.ProfilingTrigger\$Builder")
                    val triggers = mutableListOf<Any>()
                    for (field in type.fields.filter { it.name.startsWith("TRIGGER_TYPE_") && it.name != "TRIGGER_TYPE_NONE" }) {
                        try {
                            val b = builder.getConstructor(Int::class.javaPrimitiveType).newInstance(field.getInt(null))
                            triggers.add(builder.getMethod("build").invoke(b))
                            event(c, "OS_TRIGGER_AVAILABLE ${field.name}=${field.getInt(null)}")
                        } catch (e: Exception) { event(c, "OS_TRIGGER_UNAVAILABLE ${field.name}: $e") }
                    }
                    m.javaClass.getMethod("addProfilingTriggers", List::class.java).invoke(m, triggers)
                    event(c, "OS_PROFILING_TRIGGERS_REGISTERED count=${triggers.size}")
                } else event(c, "OS_TRIGGER_API_UNAVAILABLE; app-requested trace only")
            } catch (e: Exception) { event(c, "OS_TRIGGER_REGISTRATION_FAILED $e") }
            val params = Bundle().apply {
                // Stable wire keys used by the AndroidX request builders (framework constants are hidden on some SDKs).
                putInt("KEY_DURATION_MS", 180000)
                putInt("KEY_SIZE_KB", 16384)
            }
            val tag = "gt-${Process.myPid()}-${SystemClock.elapsedRealtime()}".take(20)
            event(c, "TRACE_REQUEST session=$sessionName tag=$tag durationMs=180000 sizeKb=16384 (requested, NOT yet confirmed)")
            m.requestProfiling(ProfilingManager.PROFILING_TYPE_SYSTEM_TRACE, params, tag, null, null, null)
        }
    }
}
