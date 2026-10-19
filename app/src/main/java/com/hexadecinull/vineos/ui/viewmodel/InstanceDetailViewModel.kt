package com.hexadecinull.vineos.ui.viewmodel

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.hexadecinull.vineos.data.models.VMInstance
import com.hexadecinull.vineos.data.models.VMStatus
import com.hexadecinull.vineos.data.repository.InstanceRepository
import com.hexadecinull.vineos.domain.VineVMManager
import com.hexadecinull.vineos.helper.SnapshotInfo
import dagger.hilt.android.lifecycle.HiltViewModel
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class InstanceDetailUiState(val instance: VMInstance? = null, val diagnostics: String = "")

data class SnapshotsUiState(val items: List<SnapshotInfo> = emptyList(), val busy: String? = null, val message: String? = null)

@HiltViewModel
class InstanceDetailViewModel @Inject constructor(private val instanceRepo: InstanceRepository, private val vmManager: VineVMManager) :
    ViewModel() {
    private val instanceIdFlow = MutableStateFlow("")
    private val diagnosticsFlow = MutableStateFlow("")

    // No grace period on uiState: sources are already-cached hot flows, so a rebuild on resubscribe is cheap
    val uiState: StateFlow<InstanceDetailUiState> = combine(
        instanceIdFlow,
        instanceRepo.observeAll(),
        diagnosticsFlow,
    ) { id, all, diagnostics ->
        InstanceDetailUiState(instance = all.find { it.id == id }, diagnostics = diagnostics)
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(), InstanceDetailUiState())

    fun load(id: String) {
        instanceIdFlow.value = id
    }

    fun launch(instance: VMInstance) {
        viewModelScope.launch {
            instanceRepo.updateStatus(instance.id, VMStatus.BOOTING)
            instanceRepo.touchLastUsed(instance.id)
            vmManager.startInstance(instance)
        }
    }

    fun stop(instance: VMInstance) {
        viewModelScope.launch {
            vmManager.stopInstance(instance)
            instanceRepo.updateStatus(instance.id, VMStatus.STOPPED)
        }
    }

    fun delete(instance: VMInstance) {
        viewModelScope.launch {
            if (instance.status != VMStatus.STOPPED) vmManager.killInstance(instance.id)
            vmManager.deleteInstanceFiles(instance)
            instanceRepo.delete(instance)
        }
    }

    fun refreshDiagnostics() {
        val id = instanceIdFlow.value
        viewModelScope.launch { diagnosticsFlow.value = vmManager.fetchDiagnostics(id) }
    }

    private val snapshotsFlow = MutableStateFlow(SnapshotsUiState())
    val snapshots: StateFlow<SnapshotsUiState> = snapshotsFlow.asStateFlow()

    // Snapshot copies run in the background, this follows their results
    suspend fun observeSnapshotEvents() {
        vmManager.snapshotEvents.collect { event ->
            if (event.instanceId != instanceIdFlow.value) return@collect
            val what = if (event.kind == "restore") "Restore" else "Snapshot"
            val text = if (event.ok) "$what finished" else "$what failed: ${event.message}"
            snapshotsFlow.update { it.copy(busy = null, message = text) }
            loadSnapshots()
        }
    }

    fun loadSnapshots() {
        viewModelScope.launch {
            val instance = instanceRepo.getById(instanceIdFlow.value) ?: return@launch
            val items = vmManager.listSnapshots(instance)
            snapshotsFlow.update { it.copy(items = items) }
        }
    }

    fun createSnapshot() {
        val name = "snapshot-" + SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(Date())
        runSnapshotJob("Creating a snapshot") { vmManager.createSnapshot(it, name) }
    }

    fun restoreSnapshot(name: String) = runSnapshotJob("Restoring $name") { vmManager.restoreSnapshot(it, name) }

    fun deleteSnapshot(name: String) {
        viewModelScope.launch {
            val instance = instanceRepo.getById(instanceIdFlow.value) ?: return@launch
            val reply = vmManager.deleteSnapshot(instance, name)
            if (!reply.ok) snapshotsFlow.update { it.copy(message = reply.payload) }
            loadSnapshots()
        }
    }

    fun clearSnapshotMessage() = snapshotsFlow.update { it.copy(message = null) }

    private fun runSnapshotJob(busyText: String, call: suspend (VMInstance) -> com.hexadecinull.vineos.helper.HelperReply) {
        viewModelScope.launch {
            val instance = instanceRepo.getById(instanceIdFlow.value) ?: return@launch
            snapshotsFlow.update { it.copy(busy = busyText, message = null) }
            val reply = call(instance)
            if (!reply.ok) snapshotsFlow.update { it.copy(busy = null, message = reply.payload) }
        }
    }
}
