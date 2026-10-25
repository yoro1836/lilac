package com.hexadecinull.vineos.domain

import android.content.Context
import android.content.Intent
import android.os.Build
import android.view.WindowManager
import androidx.core.content.ContextCompat
import com.hexadecinull.vineos.BuildConfig
import com.hexadecinull.vineos.data.models.AbiCompat
import com.hexadecinull.vineos.data.models.ROMImage
import com.hexadecinull.vineos.data.models.VMInstance
import com.hexadecinull.vineos.data.models.VMStatus
import com.hexadecinull.vineos.data.repository.AppPreferences
import com.hexadecinull.vineos.data.repository.InstanceRepository
import com.hexadecinull.vineos.domain.shortcuts.AppShortcutStore
import com.hexadecinull.vineos.helper.GuestApp
import com.hexadecinull.vineos.helper.GuestProtocol
import com.hexadecinull.vineos.helper.HelperClient
import com.hexadecinull.vineos.helper.HelperEvent
import com.hexadecinull.vineos.helper.HelperLauncher
import com.hexadecinull.vineos.helper.HelperProtocol
import com.hexadecinull.vineos.helper.HelperReply
import com.hexadecinull.vineos.helper.HelperState
import com.hexadecinull.vineos.helper.SnapshotInfo
import com.hexadecinull.vineos.helper.TouchPoint
import com.hexadecinull.vineos.native.VineRuntime
import com.hexadecinull.vineos.service.VineService
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.File
import java.io.IOException
import java.util.concurrent.ConcurrentHashMap
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

data class SnapshotEvent(val kind: String, val instanceId: String, val name: String, val ok: Boolean, val message: String)

@Singleton
class VineVMManager @Inject constructor(
    @ApplicationContext private val context: Context,
    private val instanceRepo: InstanceRepository,
    private val prefs: AppPreferences,
    private val provisioner: InstanceProvisioner,
    private val shortcutStore: AppShortcutStore,
) {
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    private val statusFlows = ConcurrentHashMap<String, MutableStateFlow<VMStatus>>()
    private val errorFlows = ConcurrentHashMap<String, MutableStateFlow<String?>>()
    private val pendingLaunch = ConcurrentHashMap<String, String>()
    private val startLocks = ConcurrentHashMap<String, Mutex>()
    private val persistQueue = Channel<Pair<String, VMStatus>>(Channel.UNLIMITED)
    private val snapshotEventFlow = MutableSharedFlow<SnapshotEvent>(extraBufferCapacity = 16)
    private var idleJob: Job? = null
    private var serviceCount = 0

    val hostAbis: List<String> = Build.SUPPORTED_ABIS.toList()
    val hostSupports32bit: Boolean by lazy { VineRuntime.isLoaded && VineRuntime.hostSupportsAArch32() }

    private val helper = HelperClient(scope) {
        val libDir = context.applicationInfo.nativeLibraryDir
        val qemu = File(libDir, HelperLauncher.QEMU_LIB).takeIf { it.isFile }?.path
        ProcessBuilder(HelperLauncher.command(libDir, provisioner.instancesDir.path, android.os.Process.myUid(), qemu)).start()
    }

    // Waydroid images under tawcroot, no root needed (docs/rootless.md)
    private val rootlessHelper = HelperClient(scope, requireRoot = false) {
        val debugSocket = if (BuildConfig.DEBUG) File(context.filesDir, "rootless.sock").path else null
        ProcessBuilder(HelperLauncher.rootlessCommand(context.applicationInfo.nativeLibraryDir, provisioner.instancesDir.path, debugSocket))
            .start()
    }

    val helperState: StateFlow<HelperState> = helper.state
    val snapshotEvents: SharedFlow<SnapshotEvent> = snapshotEventFlow.asSharedFlow()

    init {
        scope.launch { instanceRepo.resetAllStatuses() }
        scope.launch { for ((id, status) in persistQueue) runCatching { instanceRepo.updateStatus(id, status) } }
        scope.launch { helper.events.collect { onHelperEvent(it, rootless = false) } }
        scope.launch { rootlessHelper.events.collect { onHelperEvent(it, rootless = true) } }
    }

    // System-as-root images (Waydroid, no ramdisk) boot on the rootless engine, the others need root
    fun isRootless(instanceId: String): Boolean = !File(provisioner.instanceDir(instanceId), "ramdisk.img").exists()

    private fun helperFor(instanceId: String): HelperClient = if (isRootless(instanceId)) rootlessHelper else helper

    // Starts the instance's helper if needed; the failure text or null
    private suspend fun connectFor(instanceId: String): String? = (helperFor(instanceId).connect() as? HelperState.Failed)?.reason

    fun isRomCompatible(rom: ROMImage): Boolean = AbiCompat.romRunMode(rom, hostAbis) != AbiCompat.RunMode.UNAVAILABLE

    fun statusFlow(instanceId: String): StateFlow<VMStatus> = flowFor(instanceId).asStateFlow()

    // Why the last start failed, null while all is well
    fun errorFlow(instanceId: String): StateFlow<String?> = errorFlows.getOrPut(instanceId) { MutableStateFlow(null) }.asStateFlow()

    fun isActive(instanceId: String): Boolean = flowFor(instanceId).value in ACTIVE

    fun vinefbPath(instance: VMInstance): String = File(instance.storagePath, "shared/vinefb").path

    // Tries to get root for the helper, which may show the superuser prompt. Returns the failure text or null on success.
    suspend fun checkRoot(): String? = (helper.connect() as? HelperState.Failed)?.reason

    fun startInstance(instance: VMInstance) {
        scope.launch { start(instance) }
    }

    private suspend fun start(instance: VMInstance) = startLocks.getOrPut(instance.id) { Mutex() }.withLock {
        if (isActive(instance.id)) return@withLock
        errorFlows[instance.id]?.value = null
        setStatus(instance.id, VMStatus.BOOTING)

        if (isRootless(instance.id) && hostAbis.none { it == "arm64-v8a" || it == "x86_64" }) {
            return@withLock fail(instance.id, "The rootless engine needs a 64-bit phone")
        }
        val engine = helperFor(instance.id)
        val up = engine.connect()
        if (up is HelperState.Failed) return@withLock fail(instance.id, up.reason)

        val manifest = provisioner.readManifest(File(instance.storagePath))
        val needs32 = manifest.isArm32Only && !hostSupports32bit
        val (screenW, screenH) = deviceScreenSize()
        val plan = DisplayPlanner.plan(screenW, screenH, prefs.screenFit.first(), prefs.guestResolution.first())
        val reply = engine.request(
            "start",
            instance.id,
            instance.storagePath,
            "ram_mb=${instance.ramMb}",
            "cpu_cores=${instance.cpuCores}",
            "data_size_mb=${instance.storageMb}",
            "width=${plan.width}",
            "height=${plan.height}",
            "dpi=${plan.dpi}",
            "network=${if (prefs.guestNetwork.first()) "nat" else "none"}",
            "graphics=${if (prefs.gpuAcceleration.first()) "gfxstream" else "cpu"}",
            "qemu32=${if (needs32) 1 else 0}",
            timeoutMs = 120_000,
        )
        if (!reply.ok) fail(instance.id, reply.payload.ifBlank { "The instance could not be started" })
    }

    fun stopInstance(instance: VMInstance) {
        scope.launch { halt(instance.id, force = false) }
    }

    fun killInstance(instanceId: String) {
        scope.launch { halt(instanceId, force = true) }
    }

    private suspend fun halt(instanceId: String, force: Boolean) {
        pendingLaunch.remove(instanceId)
        val engine = helperFor(instanceId)
        if (engine.isReady) engine.request(if (force) "kill" else "stop", instanceId, timeoutMs = 30_000)
        setStatus(instanceId, VMStatus.STOPPED)
    }

    // Stops the guest, then removes its files. Files made by the root helper need the helper to delete them.
    suspend fun deleteInstanceFiles(instance: VMInstance) {
        halt(instance.id, force = true)
        val dir = File(instance.storagePath)
        if (dir.exists()) dir.deleteRecursively()
        if (dir.exists() && connectFor(instance.id) == null) helperFor(instance.id).request("rmtree", dir.path, timeoutMs = 120_000)
        shortcutStore.forgetInstance(instance.id)
        statusFlows.remove(instance.id)
        errorFlows.remove(instance.id)
    }

    fun pauseInstance(instanceId: String) {
        scope.launch {
            val r = helperFor(instanceId).request("pause", instanceId)
            if (r.ok) setStatus(instanceId, VMStatus.PAUSED)
        }
    }

    fun resumeInstance(instanceId: String) {
        scope.launch {
            val r = helperFor(instanceId).request("resume", instanceId)
            if (r.ok) setStatus(instanceId, VMStatus.RUNNING)
        }
    }

    fun sendTouch(instanceId: String, action: Int, actionIndex: Int, points: List<TouchPoint>) {
        helperFor(instanceId).send(
            "agent_send",
            instanceId,
            GuestProtocol.touchLine(action, actionIndex, points),
            droppable = action == GuestProtocol.ACTION_MOVE,
        )
    }

    // Keys are fire and forget for the agent, so a ping decides between it and the guest's own `input` tool
    suspend fun pressKey(instanceId: String, keycode: Int) {
        val engine = helperFor(instanceId)
        if (engine.request("agent", instanceId, "1500", "ping").ok) {
            engine.send("agent_send", instanceId, GuestProtocol.keyLine(keycode, true))
            engine.send("agent_send", instanceId, GuestProtocol.keyLine(keycode, false))
            return
        }
        engine.request("exec", instanceId, "5000", "input", "keyevent", keycode.toString())
    }

    fun sendText(instanceId: String, text: String) {
        helperFor(instanceId).send("agent_send", instanceId, GuestProtocol.textLine(text))
    }

    suspend fun listApps(instanceId: String): Result<List<GuestApp>> {
        val failure = connectFor(instanceId)
        if (failure != null) return Result.failure(IOException(failure))
        val reply = helperFor(instanceId).request("apps", instanceId, timeoutMs = 60_000)
        if (!reply.ok) return Result.failure(IOException(reply.payload))
        val apps = HelperProtocol.parseApps(reply.payload)
        shortcutStore.saveApps(instanceId, apps)
        return Result.success(apps)
    }

    suspend fun fetchIcon(instanceId: String, packageName: String): ByteArray? {
        val reply = helperFor(instanceId).request("icon", instanceId, packageName, timeoutMs = 20_000)
        if (!reply.ok) return null
        val png = HelperProtocol.decodeIcon(reply.payload) ?: return null
        shortcutStore.saveIcon(instanceId, packageName, png)
        return png
    }

    // Opens an app now when the instance is up, otherwise as soon as it has booted
    fun launchAppWhenReady(instanceId: String, packageName: String) {
        if (!HelperProtocol.isValidPackageName(packageName)) return
        if (flowFor(instanceId).value == VMStatus.RUNNING) {
            scope.launch { launchWithRetry(instanceId, packageName) }
        } else {
            pendingLaunch[instanceId] = packageName
        }
    }

    private suspend fun launchWithRetry(instanceId: String, packageName: String) {
        repeat(LAUNCH_ATTEMPTS) {
            if (helperFor(instanceId).request("launch", instanceId, packageName, timeoutMs = 20_000).ok) return
            delay(LAUNCH_RETRY_MS)
        }
    }

    suspend fun listSnapshots(instance: VMInstance): List<SnapshotInfo> {
        val engine = helperFor(instance.id)
        if (engine.isReady) {
            val reply = engine.request("snapshots", instance.storagePath)
            if (reply.ok) return HelperProtocol.parseSnapshots(reply.payload)
        }
        val dir = File(instance.storagePath, "snapshots")
        val files = dir.listFiles { f -> f.isFile && f.name.endsWith(".img") }.orEmpty()
        return files.map { SnapshotInfo(it.name.removeSuffix(".img"), it.length(), it.length(), it.lastModified() / 1000) }
            .sortedByDescending { it.createdAtSec }
    }

    suspend fun createSnapshot(instance: VMInstance, name: String): HelperReply = snapshotCall("snapshot", instance, name)

    suspend fun restoreSnapshot(instance: VMInstance, name: String): HelperReply = snapshotCall("restore", instance, name)

    suspend fun deleteSnapshot(instance: VMInstance, name: String): HelperReply {
        val failure = connectFor(instance.id)
        if (failure != null) return HelperReply(false, failure)
        return helperFor(instance.id).request("delete_snapshot", instance.storagePath, name)
    }

    private suspend fun snapshotCall(command: String, instance: VMInstance, name: String): HelperReply {
        val failure = connectFor(instance.id)
        if (failure != null) return HelperReply(false, failure)
        return helperFor(instance.id).request(command, instance.id, instance.storagePath, name, timeoutMs = 30_000)
    }

    suspend fun fetchDiagnostics(instanceId: String): String {
        val lastError = errorFlows[instanceId]?.value
        val errorLine = if (lastError.isNullOrBlank()) "" else "Last error: $lastError\n\n"
        val engine = helperFor(instanceId)
        if (engine.isReady && isActive(instanceId)) {
            val reply = engine.request("diag", instanceId)
            if (reply.ok) return errorLine + reply.payload
        }
        return errorLine + deviceReport()
    }

    // What this phone offers the engine, plus the state of the root helper
    suspend fun deviceReport(): String {
        val sb = StringBuilder()
        sb.append("Host ABIs: ").append(hostAbis.joinToString(", ")).append('\n')
        if (VineRuntime.isLoaded) sb.append(VineRuntime.getRuntimeVersion()).append('\n').append(VineRuntime.probeHost()).append('\n')
        when (val s = helper.state.value) {
            is HelperState.Ready -> {
                sb.append("Root helper: running\n")
                val probe = helper.request("probe")
                if (probe.ok) sb.append(probe.payload)
            }
            is HelperState.Failed -> sb.append("Root helper: ").append(s.reason).append('\n')
            else -> sb.append("Root helper: not started yet. It starts with the first instance.\n")
        }
        when (val s = rootlessHelper.state.value) {
            is HelperState.Ready -> sb.append("Rootless engine: running\n")
            is HelperState.Failed -> sb.append("Rootless engine: ").append(s.reason).append('\n')
            else -> sb.append("Rootless engine: not started yet\n")
        }
        return sb.toString()
    }

    private fun onHelperEvent(event: HelperEvent, rootless: Boolean) {
        val f = event.fields
        when (event.name) {
            "state" -> {
                val id = f.getOrNull(0) ?: return
                val status = statusOf(f.getOrNull(1).orEmpty())
                if (status == VMStatus.ERROR) {
                    errorFlows.getOrPut(id) { MutableStateFlow(null) }.value =
                        f.getOrNull(2).orEmpty().ifBlank { "The instance stopped unexpectedly" }
                }
                setStatus(id, status)
                when (status) {
                    VMStatus.RUNNING -> pendingLaunch.remove(id)?.let { pkg -> scope.launch { launchWithRetry(id, pkg) } }
                    VMStatus.STOPPED, VMStatus.ERROR -> pendingLaunch.remove(id)
                    else -> Unit
                }
            }
            "snapshot", "restore" -> {
                val e =
                    SnapshotEvent(
                        event.name,
                        f.getOrNull(0).orEmpty(),
                        f.getOrNull(1).orEmpty(),
                        f.getOrNull(2) == "ok",
                        f.getOrNull(3).orEmpty()
                    )
                snapshotEventFlow.tryEmit(e)
            }
            "helper_exit" -> {
                val reason = f.getOrNull(0).orEmpty()
                for ((id, flow) in statusFlows) {
                    if (flow.value in ACTIVE && isRootless(id) == rootless) {
                        errorFlows.getOrPut(id) { MutableStateFlow(null) }.value = reason
                        setStatus(id, VMStatus.ERROR)
                    }
                }
            }
        }
    }

    private fun statusOf(state: String): VMStatus = when (state) {
        "booting" -> VMStatus.BOOTING
        "running" -> VMStatus.RUNNING
        "paused" -> VMStatus.PAUSED
        "error" -> VMStatus.ERROR
        else -> VMStatus.STOPPED
    }

    private fun flowFor(instanceId: String): MutableStateFlow<VMStatus> = statusFlows.getOrPut(instanceId) {
        MutableStateFlow(VMStatus.STOPPED)
    }

    private fun fail(instanceId: String, reason: String) {
        errorFlows.getOrPut(instanceId) { MutableStateFlow(null) }.value = reason
        setStatus(instanceId, VMStatus.ERROR)
    }

    private fun setStatus(instanceId: String, status: VMStatus) {
        val flow = flowFor(instanceId)
        if (flow.value == status) return
        flow.value = status
        persistQueue.trySend(instanceId to status)
        refreshService()
    }

    private fun activeCount(): Int = statusFlows.values.count { it.value in ACTIVE }

    @Synchronized
    private fun refreshService() {
        val now = activeCount()
        if (now == serviceCount) return
        val before = serviceCount
        serviceCount = now
        when {
            now > 0 && before == 0 -> {
                idleJob?.cancel()
                idleJob = null
                val intent = Intent(context, VineService::class.java)
                    .setAction(VineService.ACTION_START)
                    .putExtra(VineService.EXTRA_RUNNING, now)
                runCatching { ContextCompat.startForegroundService(context, intent) }
            }
            now > 0 -> VineService.postCount(context, now)
            else -> {
                context.stopService(Intent(context, VineService::class.java))
                idleJob = scope.launch {
                    delay(IDLE_SHUTDOWN_MS)
                    if (activeCount() == 0) {
                        helper.shutdown()
                        rootlessHelper.shutdown()
                    }
                }
            }
        }
    }

    // Real size of the screen including the system bars, which is what the guest should fill
    private fun deviceScreenSize(): Pair<Int, Int> {
        val wm = context.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val b = wm.maximumWindowMetrics.bounds
            b.width() to b.height()
        } else {
            val m = android.util.DisplayMetrics()

            @Suppress("DEPRECATION")
            wm.defaultDisplay.getRealMetrics(m)
            m.widthPixels to m.heightPixels
        }
    }

    fun shutdown() {
        scope.launch {
            helper.shutdown()
            rootlessHelper.shutdown()
        }
    }

    fun cleanup() {
        scope.cancel()
    }

    companion object {
        private val ACTIVE = setOf(VMStatus.BOOTING, VMStatus.RUNNING, VMStatus.PAUSED)
        private const val IDLE_SHUTDOWN_MS = 60_000L
        private const val LAUNCH_ATTEMPTS = 8
        private const val LAUNCH_RETRY_MS = 2_500L
    }
}
