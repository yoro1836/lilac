package com.hexadecinull.vineos.domain.shortcuts

import com.hexadecinull.vineos.data.repository.InstanceRepository
import com.hexadecinull.vineos.domain.VineVMManager
import com.hexadecinull.vineos.ui.navigation.Screen
import javax.inject.Inject
import javax.inject.Singleton

// Handles a tap on a home-screen shortcut: starts the instance if needed and queues the app to open once it has booted
@Singleton
class ShortcutLauncher @Inject constructor(
    private val instanceRepo: InstanceRepository,
    private val vmManager: VineVMManager,
    private val store: AppShortcutStore,
) {
    // Returns the screen to show, or null when the instance is gone
    suspend fun prepare(target: ShortcutTarget): String? {
        val instance = instanceRepo.getById(target.instanceId)
        if (instance == null) {
            store.disable(target, "This instance was deleted")
            return null
        }
        vmManager.launchAppWhenReady(instance.id, target.packageName)
        if (!vmManager.isActive(instance.id)) {
            instanceRepo.touchLastUsed(instance.id)
            vmManager.startInstance(instance)
        }
        return Screen.VMDisplay.createRoute(instance.id)
    }
}
