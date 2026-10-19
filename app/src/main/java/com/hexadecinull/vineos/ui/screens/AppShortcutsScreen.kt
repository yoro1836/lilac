package com.hexadecinull.vineos.ui.screens

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.outlined.AddToHomeScreen
import androidx.compose.material.icons.outlined.Android
import androidx.compose.material.icons.outlined.KeyboardArrowRight
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import coil.compose.AsyncImage
import com.hexadecinull.vineos.data.models.VMInstance
import com.hexadecinull.vineos.data.models.VMStatus
import com.hexadecinull.vineos.helper.GuestApp
import com.hexadecinull.vineos.ui.viewmodel.AppShortcutsUiState
import com.hexadecinull.vineos.ui.viewmodel.AppShortcutsViewModel

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AppShortcutsScreen(onBack: () -> Unit, modifier: Modifier = Modifier, vm: AppShortcutsViewModel = hiltViewModel()) {
    val state by vm.uiState.collectAsStateWithLifecycle()
    val snackbarHost = remember { SnackbarHostState() }
    val selected = state.selected

    BackHandler(enabled = selected != null) { vm.closeInstance() }
    LaunchedEffect(state.message) {
        val message = state.message ?: return@LaunchedEffect
        snackbarHost.showSnackbar(message)
        vm.clearMessage()
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(selected?.name ?: "App Shortcuts") },
                navigationIcon = {
                    IconButton(onClick = { if (selected != null) vm.closeInstance() else onBack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
                actions = {
                    if (selected != null && state.status == VMStatus.RUNNING) {
                        IconButton(onClick = vm::refresh, enabled = !state.loading) {
                            Icon(Icons.Filled.Refresh, contentDescription = "Reload the app list")
                        }
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbarHost) },
        modifier = modifier,
    ) { padding ->
        Box(Modifier.fillMaxSize().padding(padding)) {
            if (selected == null) {
                InstancePicker(state.instances, onPick = vm::selectInstance)
            } else {
                AppPicker(state, onQuery = vm::setQuery, onStart = vm::startSelected, onPick = vm::createShortcut)
            }
        }
    }
}

@Composable
private fun InstancePicker(instances: List<VMInstance>, onPick: (VMInstance) -> Unit) {
    if (instances.isEmpty()) {
        Box(Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
            Text(
                "Create an instance first. Then you can put its apps on your home screen.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        return
    }
    LazyColumn(Modifier.fillMaxSize()) {
        item {
            Text(
                "Pick an instance, then pick an app. Tapping the shortcut later starts the instance and opens the app.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
            )
        }
        items(instances, key = { it.id }) { instance ->
            ListItem(
                headlineContent = { Text(instance.name) },
                supportingContent = { Text(instance.androidVersionDisplay) },
                leadingContent = { Text(instance.iconEmoji, style = MaterialTheme.typography.titleLarge) },
                trailingContent = { Icon(Icons.Outlined.KeyboardArrowRight, contentDescription = null) },
                modifier = Modifier.clickable { onPick(instance) },
            )
            HorizontalDivider()
        }
    }
}

@Composable
private fun AppPicker(state: AppShortcutsUiState, onQuery: (String) -> Unit, onStart: () -> Unit, onPick: (GuestApp) -> Unit) {
    val running = state.status == VMStatus.RUNNING
    val starting = state.status == VMStatus.BOOTING
    Column(Modifier.fillMaxSize()) {
        if (!running) {
            Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    when {
                        starting -> "Starting the instance. Its apps will show up here once it has booted."
                        state.apps.isEmpty() -> "Start this instance once so its installed apps can be listed."
                        else -> "This instance is stopped, so this is the list from last time. A shortcut starts it automatically."
                    },
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                if (!starting) {
                    Button(onClick = onStart) {
                        Text(if (state.apps.isEmpty()) "Start instance" else "Start to refresh the list")
                    }
                }
            }
        }
        if (state.loading) {
            Row(
                Modifier.fillMaxWidth().padding(16.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                Text("Reading installed apps", style = MaterialTheme.typography.bodyMedium)
            }
        }
        if (state.apps.isNotEmpty()) {
            OutlinedTextField(
                value = state.query,
                onValueChange = onQuery,
                label = { Text("Search apps") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
            )
            LazyColumn(Modifier.fillMaxSize()) {
                items(state.visibleApps, key = { it.packageName }) { app ->
                    ListItem(
                        headlineContent = { Text(app.label) },
                        supportingContent = { Text(app.packageName, style = MaterialTheme.typography.bodySmall) },
                        leadingContent = { AppIcon(state.icons[app.packageName]) },
                        trailingContent = { Icon(Icons.Outlined.AddToHomeScreen, contentDescription = "Add to home screen") },
                        modifier = Modifier.clickable { onPick(app) },
                    )
                }
            }
        }
    }
}

@Composable
private fun AppIcon(file: java.io.File?) {
    if (file != null) {
        AsyncImage(model = file, contentDescription = null, contentScale = ContentScale.Fit, modifier = Modifier.size(40.dp))
    } else {
        Icon(
            Icons.Outlined.Android,
            contentDescription = null,
            modifier = Modifier.size(40.dp),
            tint = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}
