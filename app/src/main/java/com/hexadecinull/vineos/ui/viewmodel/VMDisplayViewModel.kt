package com.hexadecinull.vineos.ui.viewmodel

import android.view.KeyEvent
import android.view.Surface
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.hexadecinull.vineos.data.models.NavButtonsMode
import com.hexadecinull.vineos.data.models.ScreenFit
import com.hexadecinull.vineos.data.models.VMInstance
import com.hexadecinull.vineos.data.models.VMStatus
import com.hexadecinull.vineos.data.repository.AppPreferences
import com.hexadecinull.vineos.data.repository.InstanceRepository
import com.hexadecinull.vineos.domain.VineVMManager
import com.hexadecinull.vineos.helper.TouchPoint
import com.hexadecinull.vineos.native.VineRuntime
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

@HiltViewModel
class VMDisplayViewModel @Inject constructor(
    private val vmManager: VineVMManager,
    private val instanceRepo: InstanceRepository,
    prefs: AppPreferences,
) : ViewModel() {

    val navButtons: StateFlow<NavButtonsMode> = prefs.navButtons.stateIn(viewModelScope, SharingStarted.Eagerly, NavButtonsMode.HIDDEN)
    val screenFit: StateFlow<ScreenFit> = prefs.screenFit.stateIn(viewModelScope, SharingStarted.Eagerly, ScreenFit.FILL)
    val avoidCutout: StateFlow<Boolean> = prefs.avoidCutout.stateIn(viewModelScope, SharingStarted.Eagerly, false)

    var instanceName by mutableStateOf("")
        private set
    var status by mutableStateOf(VMStatus.STOPPED)
        private set
    var errorText by mutableStateOf<String?>(null)
        private set

    // Guest resolution once the guest has drawn, 0 until then
    var guestWidth by mutableStateOf(0)
        private set
    var guestHeight by mutableStateOf(0)
        private set
    var hasFrame by mutableStateOf(false)
        private set

    val isActive: Boolean get() = status == VMStatus.BOOTING || status == VMStatus.RUNNING || status == VMStatus.PAUSED

    private var instance: VMInstance? = null
    private var displayHandle = 0L
    private var surface: Surface? = null
    private val lock = Any()
    private var jobs = emptyList<Job>()

    fun attach(id: String) {
        detach()
        val statusJob = viewModelScope.launch {
            vmManager.statusFlow(id).collect { status = it }
        }
        val errorJob = viewModelScope.launch {
            vmManager.errorFlow(id).collect { errorText = it }
        }
        val displayJob = viewModelScope.launch(Dispatchers.Default) {
            val found = instanceRepo.getById(id) ?: return@launch
            instance = found
            instanceName = found.name
            displayLoop(found)
        }
        jobs = listOf(statusJob, errorJob, displayJob)
    }

    fun detach() {
        jobs.forEach { it.cancel() }
        jobs = emptyList()
        closeDisplay()
        instance = null
        hasFrame = false
    }

    // Opens the guest's frame buffer as soon as it exists and keeps the guest size up to date
    private suspend fun displayLoop(found: VMInstance) {
        while (true) {
            val handle = synchronized(lock) { displayHandle }
            if (handle == 0L) {
                if (isActive && VineRuntime.isLoaded) openDisplay(found)
            } else {
                val info = VineRuntime.displayInfo(handle)
                hasFrame = info.size > 4 && info[4] > 0
                if (info.size > 3 && info[2] > 0) {
                    guestWidth = info[2].toInt()
                    guestHeight = info[3].toInt()
                }
                if (!isActive) closeDisplay()
            }
            delay(if (handle == 0L) OPEN_RETRY_MS else INFO_POLL_MS)
        }
    }

    private fun openDisplay(found: VMInstance) {
        val handle = VineRuntime.openDisplay(vmManager.vinefbPath(found))
        if (handle == 0L) return
        VineRuntime.setDisplayTiming(handle, found.refreshHz, found.vsync)
        synchronized(lock) {
            displayHandle = handle
            surface?.let {
                VineRuntime.attachSurface(handle, it)
                VineRuntime.startRendering(handle)
            }
        }
    }

    private fun closeDisplay() {
        synchronized(lock) {
            val handle = displayHandle
            if (handle != 0L) {
                VineRuntime.stopRendering(handle)
                VineRuntime.detachSurface(handle)
                VineRuntime.closeDisplay(handle)
                displayHandle = 0L
            }
        }
        hasFrame = false
    }

    fun onSurfaceReady(newSurface: Surface) {
        synchronized(lock) {
            surface = newSurface
            if (displayHandle != 0L) {
                VineRuntime.attachSurface(displayHandle, newSurface)
                VineRuntime.startRendering(displayHandle)
            }
        }
    }

    fun onSurfaceDestroyed() {
        synchronized(lock) {
            if (displayHandle != 0L) {
                VineRuntime.stopRendering(displayHandle)
                VineRuntime.detachSurface(displayHandle)
            }
            surface = null
        }
    }

    fun onTouch(action: Int, actionIndex: Int, points: List<TouchPoint>) {
        val id = instance?.id ?: return
        vmManager.sendTouch(id, action, actionIndex, points)
    }

    fun pressBack() = pressKey(KeyEvent.KEYCODE_BACK)

    fun pressHome() = pressKey(KeyEvent.KEYCODE_HOME)

    fun pressRecents() = pressKey(KeyEvent.KEYCODE_APP_SWITCH)

    // Volume keys go to the guest while it runs; it makes its own repeats
    fun onHardwareKey(event: KeyEvent): Boolean {
        if (event.keyCode !in GUEST_KEYS || !isActive) return false
        val id = instance?.id ?: return false
        when (event.action) {
            KeyEvent.ACTION_DOWN -> if (event.repeatCount == 0) vmManager.sendKey(id, event.keyCode, true)
            KeyEvent.ACTION_UP -> vmManager.sendKey(id, event.keyCode, false)
        }
        return true
    }

    private fun pressKey(keycode: Int) {
        val id = instance?.id ?: return
        viewModelScope.launch { vmManager.pressKey(id, keycode) }
    }

    fun stopInstance() {
        val current = instance ?: return
        vmManager.stopInstance(current)
    }

    override fun onCleared() {
        closeDisplay()
        super.onCleared()
    }

    companion object {
        private const val OPEN_RETRY_MS = 500L
        private const val INFO_POLL_MS = 400L
        private val GUEST_KEYS = setOf(KeyEvent.KEYCODE_VOLUME_UP, KeyEvent.KEYCODE_VOLUME_DOWN, KeyEvent.KEYCODE_VOLUME_MUTE)
    }
}
