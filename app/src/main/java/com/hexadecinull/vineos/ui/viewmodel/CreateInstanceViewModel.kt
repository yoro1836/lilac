package com.hexadecinull.vineos.ui.viewmodel

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.hexadecinull.vineos.data.models.ROMImage
import com.hexadecinull.vineos.data.models.VMInstance
import com.hexadecinull.vineos.data.models.VMStatus
import com.hexadecinull.vineos.data.repository.AppPreferences
import com.hexadecinull.vineos.data.repository.InstanceRepository
import com.hexadecinull.vineos.data.repository.ROMRepository
import com.hexadecinull.vineos.domain.InstanceProvisioner
import com.hexadecinull.vineos.domain.StorageSizing
import dagger.hilt.android.lifecycle.HiltViewModel
import java.io.File
import java.util.UUID
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

sealed class CreateInstanceState {
    data object Idle : CreateInstanceState()

    data class Creating(val progress: Float = 0f, val step: String = "Preparing") : CreateInstanceState()

    data class Success(val instanceId: String) : CreateInstanceState()

    data class Error(val message: String) : CreateInstanceState()
}

@HiltViewModel
class CreateInstanceViewModel @Inject constructor(
    private val instanceRepo: InstanceRepository,
    private val romRepo: ROMRepository,
    private val prefs: AppPreferences,
    private val provisioner: InstanceProvisioner,
) : ViewModel() {
    private val stateFlow = MutableStateFlow<CreateInstanceState>(CreateInstanceState.Idle)
    val state: StateFlow<CreateInstanceState> = stateFlow.asStateFlow()

    private val romFlow = MutableStateFlow<ROMImage?>(null)
    val rom: StateFlow<ROMImage?> = romFlow.asStateFlow()

    val instanceName = MutableStateFlow("")
    val selectedRamMb = MutableStateFlow(AppPreferences.DEFAULT_RAM_MB)
    val selectedStorageMb = MutableStateFlow(AppPreferences.DEFAULT_STORAGE_MB)
    val selectedCpuCores = MutableStateFlow(AppPreferences.DEFAULT_CPU_CORES)
    val selectedEmoji = MutableStateFlow("\uD83D\uDFE2")
    val selectedRoot = MutableStateFlow(false)

    val allowRootInstances: StateFlow<Boolean> = prefs.allowRootInstances
        .stateIn(viewModelScope, SharingStarted.Eagerly, false)

    val isFormValid: StateFlow<Boolean> = instanceName
        .map { it.isNotBlank() }
        .stateIn(viewModelScope, SharingStarted.Eagerly, false)

    init {
        viewModelScope.launch {
            selectedRamMb.value = prefs.defaultRamMb.first()
            selectedStorageMb.value = prefs.defaultStorageMb.first()
            selectedCpuCores.value = prefs.defaultCpuCores.first()
        }
    }

    fun loadRom(romId: String) {
        romFlow.value = romRepo.getRom(romId)
        if (instanceName.value.isBlank()) {
            instanceName.value = romFlow.value?.displayName.orEmpty()
        }
    }

    fun createInstance() {
        val selectedRom = romFlow.value ?: return
        if (stateFlow.value is CreateInstanceState.Creating) return
        val romFile = selectedRom.localPath?.let { File(it) }?.takeIf { it.isFile }
        if (romFile == null) {
            stateFlow.value = CreateInstanceState.Error("This ROM is not downloaded yet. Download it from the ROMs tab first.")
            return
        }
        stateFlow.value = CreateInstanceState.Creating()

        viewModelScope.launch {
            val instanceId = UUID.randomUUID().toString()
            val storage = StorageSizing.clamp(selectedStorageMb.value, StorageSizing.maxMb(provisioner.freeBytes()))
            val result = provisioner.create(romFile, instanceId) { progress ->
                stateFlow.value = CreateInstanceState.Creating(progress.fraction, progress.step)
            }
            result.onFailure { error ->
                stateFlow.value = CreateInstanceState.Error(error.message ?: "Could not create the instance.")
            }
            result.onSuccess { dir ->
                val instance = VMInstance(
                    id = instanceId,
                    name = instanceName.value.trim(),
                    romId = selectedRom.id,
                    romVersion = selectedRom.androidVersion,
                    storagePath = dir.absolutePath,
                    status = VMStatus.STOPPED,
                    ramMb = selectedRamMb.value,
                    cpuCores = selectedCpuCores.value,
                    storageMb = storage,
                    androidVersionDisplay = selectedRom.displayName,
                    isRooted = allowRootInstances.value && selectedRoot.value,
                    iconEmoji = selectedEmoji.value,
                )
                instanceRepo.save(instance)
                stateFlow.value = CreateInstanceState.Success(instanceId)
            }
        }
    }

    fun resetState() {
        stateFlow.value = CreateInstanceState.Idle
    }
}
