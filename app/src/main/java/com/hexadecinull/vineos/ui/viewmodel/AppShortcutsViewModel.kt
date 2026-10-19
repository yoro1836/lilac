package com.hexadecinull.vineos.ui.viewmodel

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.hexadecinull.vineos.data.models.VMInstance
import com.hexadecinull.vineos.data.models.VMStatus
import com.hexadecinull.vineos.data.repository.InstanceRepository
import com.hexadecinull.vineos.domain.VineVMManager
import com.hexadecinull.vineos.domain.shortcuts.AppShortcutStore
import com.hexadecinull.vineos.domain.shortcuts.PinResult
import com.hexadecinull.vineos.helper.GuestApp
import dagger.hilt.android.lifecycle.HiltViewModel
import java.io.File
import javax.inject.Inject
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class AppShortcutsUiState(
    val instances: List<VMInstance> = emptyList(),
    val selected: VMInstance? = null,
    val status: VMStatus = VMStatus.STOPPED,
    val apps: List<GuestApp> = emptyList(),
    val icons: Map<String, File> = emptyMap(),
    val loading: Boolean = false,
    val message: String? = null,
    val query: String = "",
) {
    val visibleApps: List<GuestApp>
        get() = if (query.isBlank()) {
            apps
        } else {
            apps.filter { it.label.contains(query, ignoreCase = true) || it.packageName.contains(query, ignoreCase = true) }
        }
}

@HiltViewModel
class AppShortcutsViewModel @Inject constructor(
    private val instanceRepo: InstanceRepository,
    private val vmManager: VineVMManager,
    private val store: AppShortcutStore,
) : ViewModel() {
    private val state = MutableStateFlow(AppShortcutsUiState())
    val uiState: StateFlow<AppShortcutsUiState> = state.asStateFlow()

    private var statusJob: Job? = null
    private var loadJob: Job? = null

    init {
        viewModelScope.launch {
            instanceRepo.observeAll().collect { list ->
                state.update { s -> s.copy(instances = list, selected = s.selected?.let { sel -> list.find { it.id == sel.id } }) }
            }
        }
    }

    fun selectInstance(instance: VMInstance) {
        statusJob?.cancel()
        loadJob?.cancel()
        val cached = store.loadApps(instance.id)
        state.update {
            it.copy(
                selected = instance,
                status = VMStatus.STOPPED,
                apps = cached,
                icons = iconsFor(instance.id, cached),
                loading = false,
                message = null,
                query = "",
            )
        }
        statusJob = viewModelScope.launch {
            var wasRunning = false
            vmManager.statusFlow(instance.id).collect { status ->
                state.update { it.copy(status = status) }
                if (status == VMStatus.RUNNING && !wasRunning) refresh()
                wasRunning = status == VMStatus.RUNNING
            }
        }
    }

    fun closeInstance() {
        statusJob?.cancel()
        loadJob?.cancel()
        state.update { it.copy(selected = null, apps = emptyList(), icons = emptyMap(), loading = false, message = null, query = "") }
    }

    fun setQuery(query: String) = state.update { it.copy(query = query) }

    fun startSelected() {
        val instance = state.value.selected ?: return
        vmManager.startInstance(instance)
        state.update { it.copy(message = "Starting ${instance.name}. The app list updates when it has booted.") }
    }

    fun refresh() {
        val instance = state.value.selected ?: return
        loadJob?.cancel()
        loadJob = viewModelScope.launch {
            state.update { it.copy(loading = true, message = null) }
            val result = vmManager.listApps(instance.id)
            result.onSuccess { apps ->
                state.update { it.copy(apps = apps, icons = iconsFor(instance.id, apps), loading = false) }
                fetchMissingIcons(instance.id, apps)
            }
            result.onFailure { error ->
                state.update { it.copy(loading = false, message = error.message ?: "Could not read the app list") }
            }
        }
    }

    // Icons come from the guest one by one, and the loop gives up when the guest has no agent to ask
    private suspend fun fetchMissingIcons(instanceId: String, apps: List<GuestApp>) {
        var misses = 0
        for (app in apps) {
            currentCoroutineContext().ensureActive()
            if (store.iconFile(instanceId, app.packageName) != null) continue
            if (vmManager.fetchIcon(instanceId, app.packageName) == null) {
                if (++misses >= MAX_ICON_MISSES) return
                continue
            }
            misses = 0
            val file = store.iconFile(instanceId, app.packageName) ?: continue
            state.update { it.copy(icons = it.icons + (app.packageName to file)) }
        }
    }

    fun createShortcut(app: GuestApp) {
        val instance = state.value.selected ?: return
        val message = when (store.requestPin(instance, app)) {
            PinResult.Requested -> "Confirm the shortcut for ${app.label} on your home screen."
            PinResult.Unsupported -> "Your launcher does not support pinned shortcuts."
        }
        state.update { it.copy(message = message) }
    }

    fun clearMessage() = state.update { it.copy(message = null) }

    private fun iconsFor(instanceId: String, apps: List<GuestApp>): Map<String, File> =
        apps.mapNotNull { app -> store.iconFile(instanceId, app.packageName)?.let { app.packageName to it } }.toMap()

    companion object {
        private const val MAX_ICON_MISSES = 3
    }
}
