package com.hexadecinull.vineos.helper

import java.io.IOException
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withTimeoutOrNull

sealed interface HelperState {
    data object Idle : HelperState

    data object Starting : HelperState

    data class Ready(val uid: Int) : HelperState

    data class Failed(val reason: String) : HelperState
}

// Talks to the root helper over su's stdin and stdout, replies matched by id, events on `events`
class HelperClient(private val scope: CoroutineScope, private val spawn: () -> Process) {
    private val connectLock = Mutex()
    private val nextId = AtomicLong(1)
    private val pending = ConcurrentHashMap<String, CompletableDeferred<HelperReply>>()
    private val writeQueue = Channel<String>(Channel.UNLIMITED)
    private val backlog = AtomicInteger(0)
    private val stderrTail = ArrayDeque<String>()
    private val eventFlow = MutableSharedFlow<HelperEvent>(extraBufferCapacity = 256)
    private val stateFlow = MutableStateFlow<HelperState>(HelperState.Idle)

    @Volatile private var process: Process? = null
    private var ready: CompletableDeferred<Unit>? = null
    private var jobs = emptyList<Job>()

    val events: SharedFlow<HelperEvent> = eventFlow.asSharedFlow()
    val state: StateFlow<HelperState> = stateFlow.asStateFlow()

    val isReady: Boolean get() = stateFlow.value is HelperState.Ready

    // Starts the helper if needed. Safe to call from several places at once.
    suspend fun connect(timeoutMs: Long = 60_000): HelperState = connectLock.withLock {
        if (stateFlow.value is HelperState.Ready && process?.isAlive == true) return@withLock stateFlow.value
        teardown()
        stateFlow.value = HelperState.Starting
        val proc = try {
            spawn()
        } catch (e: IOException) {
            return@withLock fail("Root access is not available (su could not be started)")
        } catch (e: SecurityException) {
            return@withLock fail("Root access is not available (su is not allowed to start)")
        }
        process = proc
        val readyGate = CompletableDeferred<Unit>()
        ready = readyGate
        jobs = listOf(
            scope.launch(Dispatchers.IO) { readLoop(proc, readyGate) },
            scope.launch(Dispatchers.IO) { drainErrors(proc) },
            scope.launch(Dispatchers.IO) { writeLoop(proc) },
        )

        val up = withTimeoutOrNull(timeoutMs) {
            try {
                readyGate.await()
                true
            } catch (_: IOException) {
                false
            }
        } ?: false
        if (!up) {
            val reason = if (proc.isAlive) {
                "Timed out waiting for root access. Grant VineOS superuser permission and try again."
            } else {
                helperFailureText(proc)
            }
            proc.destroy()
            return@withLock fail(reason)
        }

        val hello = rawRequest("hello", emptyList(), 10_000)
        val uid = HelperProtocol.parseKeyValues(hello.payload)["uid"]?.toIntOrNull()
        if (!hello.ok || uid != 0) {
            proc.destroy()
            return@withLock fail("The helper did not get root (uid=${uid ?: "unknown"}). Check that su grants root to VineOS.")
        }
        stateFlow.value = HelperState.Ready(uid)
        stateFlow.value
    }

    suspend fun request(command: String, vararg args: String, timeoutMs: Long = 20_000): HelperReply {
        if (!isReady) return HelperReply(false, "The root helper is not running")
        return rawRequest(command, args.toList(), timeoutMs)
    }

    // Fire and forget, used for touch input. Droppable lines are skipped when the helper is behind.
    fun send(command: String, vararg args: String, droppable: Boolean = false) {
        if (!isReady) return
        if (droppable && backlog.get() > MAX_BACKLOG) return
        enqueue(HelperProtocol.encodeRequest("-", command, args.toList()))
    }

    // Asks the helper to stop every guest and exit, then makes sure the process is gone
    suspend fun shutdown() {
        if (isReady) rawRequest("quit", emptyList(), 8_000)
        delay(100)
        connectLock.withLock { teardown() }
        stateFlow.value = HelperState.Idle
    }

    private suspend fun rawRequest(command: String, args: List<String>, timeoutMs: Long): HelperReply {
        val id = nextId.getAndIncrement().toString()
        val deferred = CompletableDeferred<HelperReply>()
        pending[id] = deferred
        enqueue(HelperProtocol.encodeRequest(id, command, args))
        val reply = withTimeoutOrNull(timeoutMs) { deferred.await() }
        pending.remove(id)
        return reply ?: HelperReply(false, "The helper did not answer in time ($command)")
    }

    private fun enqueue(line: String) {
        backlog.incrementAndGet()
        if (writeQueue.trySend(line).isFailure) backlog.decrementAndGet()
    }

    private fun readLoop(proc: Process, readyGate: CompletableDeferred<Unit>) {
        try {
            proc.inputStream.bufferedReader().useLines { lines ->
                for (line in lines) {
                    when (val msg = HelperProtocol.parseLine(line)) {
                        is HelperMessage.Reply -> pending.remove(msg.id)?.complete(msg.reply)
                        is HelperMessage.Event -> {
                            if (msg.event.name == "ready") readyGate.complete(Unit) else eventFlow.tryEmit(msg.event)
                        }
                        null -> Unit
                    }
                }
            }
        } catch (_: IOException) {
            // The pipe closed, handled below like any other exit
        }
        val reason = helperFailureText(proc)
        readyGate.completeExceptionally(IOException(reason))
        for ((_, d) in pending) d.complete(HelperReply(false, reason))
        pending.clear()
        if (process === proc) {
            if (stateFlow.value is HelperState.Ready) {
                eventFlow.tryEmit(HelperEvent("helper_exit", listOf(reason)))
                stateFlow.value = HelperState.Failed(reason)
            }
        }
    }

    private fun drainErrors(proc: Process) {
        try {
            proc.errorStream.bufferedReader().useLines { lines ->
                for (line in lines) {
                    synchronized(stderrTail) {
                        stderrTail.addLast(line)
                        while (stderrTail.size > 12) stderrTail.removeFirst()
                    }
                }
            }
        } catch (_: IOException) {
            // Process ended
        }
    }

    private suspend fun writeLoop(proc: Process) {
        val out = proc.outputStream
        try {
            for (line in writeQueue) {
                backlog.decrementAndGet()
                out.write((line + "\n").toByteArray(Charsets.UTF_8))
                out.flush()
            }
        } catch (_: IOException) {
            // The reader notices the exit and reports it
        }
    }

    private fun helperFailureText(proc: Process): String {
        val code = runCatching {
            proc.waitFor(1, java.util.concurrent.TimeUnit.SECONDS)
            if (proc.isAlive) null else proc.exitValue()
        }.getOrNull()
        val tail = synchronized(stderrTail) { stderrTail.takeLast(3).joinToString(" ").trim() }
        return when {
            code == 3 -> "The helper started without root. Check that su grants root to VineOS."
            code == 2 -> "The helper was started with wrong arguments"
            tail.isNotEmpty() -> "The root helper stopped: $tail"
            code != null -> "The root helper stopped (exit code $code). Root permission may have been denied."
            else -> "The root helper stopped"
        }
    }

    private fun fail(reason: String): HelperState {
        process = null
        val failed = HelperState.Failed(reason)
        stateFlow.value = failed
        return failed
    }

    private fun teardown() {
        process?.let { runCatching { it.destroy() } }
        process = null
        jobs.forEach { it.cancel() }
        jobs = emptyList()
        for ((_, d) in pending) d.complete(HelperReply(false, "The root helper stopped"))
        pending.clear()
        while (writeQueue.tryReceive().isSuccess) backlog.set(0)
        synchronized(stderrTail) { stderrTail.clear() }
    }

    companion object {
        private const val MAX_BACKLOG = 128
    }
}
