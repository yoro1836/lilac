package com.hexadecinull.vineos.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.outlined.HighQuality
import androidx.compose.material.icons.outlined.Speed
import androidx.compose.material.icons.outlined.Sync
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.hexadecinull.vineos.data.models.GuestResolution
import com.hexadecinull.vineos.data.models.VMInstance
import com.hexadecinull.vineos.data.models.VMStatus
import com.hexadecinull.vineos.domain.StorageSizing
import com.hexadecinull.vineos.helper.SnapshotInfo
import com.hexadecinull.vineos.ui.viewmodel.InstanceDetailViewModel
import com.hexadecinull.vineos.ui.viewmodel.SnapshotsUiState
import java.text.DateFormat
import java.util.Date

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun InstanceDetailScreen(
    instanceId: String,
    onBack: () -> Unit,
    onLaunch: (String) -> Unit,
    onDeleted: () -> Unit,
    modifier: Modifier = Modifier,
    vm: InstanceDetailViewModel = hiltViewModel(),
) {
    val uiState by vm.uiState.collectAsStateWithLifecycle()
    val snapshots by vm.snapshots.collectAsStateWithLifecycle()
    val appResolution by vm.appResolution.collectAsStateWithLifecycle()
    val snackbarHost = remember { SnackbarHostState() }
    var showDiagnostics by remember { mutableStateOf(false) }
    var showDeleteConfirm by remember { mutableStateOf(false) }

    LaunchedEffect(instanceId) {
        vm.load(instanceId)
        vm.loadSnapshots()
    }
    LaunchedEffect(instanceId) { vm.observeSnapshotEvents() }
    LaunchedEffect(snapshots.message) {
        val message = snapshots.message ?: return@LaunchedEffect
        snackbarHost.showSnackbar(message)
        vm.clearSnapshotMessage()
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(uiState.instance?.name ?: "Instance") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbarHost) },
        modifier = modifier,
    ) { padding ->
        val instance = uiState.instance
        if (instance == null) {
            Box(Modifier.fillMaxSize().padding(padding), contentAlignment = Alignment.Center) {
                CircularProgressIndicator()
            }
            return@Scaffold
        }

        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(instance.iconEmoji, style = MaterialTheme.typography.displaySmall)
                Column {
                    Text(instance.androidVersionDisplay, style = MaterialTheme.typography.titleMedium)
                    Text(
                        instance.status.name.lowercase().replaceFirstChar { it.uppercase() },
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }

            DetailRow("RAM", "${instance.ramMb} MB")
            DetailRow("Storage", StorageSizing.format(instance.storageMb))
            DetailRow("Root access", if (instance.isRooted) "Enabled" else "Disabled")
            DetailRow("Created", DateFormat.getDateTimeInstance().format(Date(instance.createdAt)))
            DetailRow("Last used", DateFormat.getDateTimeInstance().format(Date(instance.lastUsedAt)))

            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                if (instance.status == VMStatus.STOPPED || instance.status == VMStatus.ERROR) {
                    Button(
                        onClick = {
                            vm.launch(instance)
                            onLaunch(instance.id)
                        },
                        modifier = Modifier.weight(1f),
                    ) {
                        Icon(Icons.Filled.PlayArrow, contentDescription = null, modifier = Modifier.size(18.dp))
                        Spacer(Modifier.width(8.dp))
                        Text("Launch")
                    }
                } else {
                    OutlinedButton(onClick = { vm.stop(instance) }, modifier = Modifier.weight(1f)) {
                        Icon(Icons.Filled.Stop, contentDescription = null, modifier = Modifier.size(18.dp))
                        Spacer(Modifier.width(8.dp))
                        Text("Stop")
                    }
                }
                OutlinedButton(
                    onClick = { showDeleteConfirm = true },
                    enabled = instance.status == VMStatus.STOPPED,
                    colors = ButtonDefaults.outlinedButtonColors(contentColor = MaterialTheme.colorScheme.error),
                ) {
                    Icon(Icons.Filled.Delete, contentDescription = "Delete")
                }
            }

            DisplaySection(
                instance = instance,
                appResolution = appResolution,
                onRefreshHz = vm::setRefreshHz,
                onResolution = vm::setResolution,
                onVsync = vm::setVsync,
            )

            SnapshotsSection(
                state = snapshots,
                running = instance.status != VMStatus.STOPPED && instance.status != VMStatus.ERROR,
                onCreate = vm::createSnapshot,
                onRestore = vm::restoreSnapshot,
                onDelete = vm::deleteSnapshot,
            )

            TextButton(onClick = {
                showDiagnostics = !showDiagnostics
                if (showDiagnostics) vm.refreshDiagnostics()
            }) {
                Text(if (showDiagnostics) "Hide diagnostics" else "Show diagnostics")
            }

            if (showDiagnostics) {
                Surface(
                    color = MaterialTheme.colorScheme.surfaceVariant,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    SelectionContainer {
                        Text(
                            uiState.diagnostics.ifBlank { "No diagnostics available. Launch the instance first." },
                            modifier = Modifier.padding(12.dp),
                            fontFamily = FontFamily.Monospace,
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                }
            }
        }
    }

    val dialogInstance = uiState.instance
    if (showDeleteConfirm && dialogInstance != null) {
        AlertDialog(
            onDismissRequest = { showDeleteConfirm = false },
            title = { Text("Delete ${dialogInstance.name}?") },
            text = { Text("This removes the instance and its storage. This can't be undone.") },
            confirmButton = {
                TextButton(onClick = {
                    vm.delete(dialogInstance)
                    showDeleteConfirm = false
                    onDeleted()
                }) { Text("Delete", color = MaterialTheme.colorScheme.error) }
            },
            dismissButton = {
                TextButton(onClick = { showDeleteConfirm = false }) { Text("Cancel") }
            },
        )
    }
}

@Composable
private fun DetailRow(label: String, value: String) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        Text(label, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, fontWeight = FontWeight.Medium)
    }
}

private val FRAME_RATES = listOf(30, 60, 90, 120)

// The rates the phone's screen can show, plus whatever the instance already has
@Composable
private fun frameRateOptions(current: Int): List<Int> {
    val context = LocalContext.current
    val maxHz = remember(context) {
        val display = (context.getSystemService(android.content.Context.DISPLAY_SERVICE) as android.hardware.display.DisplayManager)
            .getDisplay(android.view.Display.DEFAULT_DISPLAY)
        display?.supportedModes?.maxOfOrNull { it.refreshRate } ?: 60f
    }
    return (FRAME_RATES.filter { it <= maxHz + 1 } + current).distinct().sorted()
}

@Composable
private fun DisplaySection(
    instance: VMInstance,
    appResolution: GuestResolution,
    onRefreshHz: (Int) -> Unit,
    onResolution: (GuestResolution?) -> Unit,
    onVsync: (Boolean) -> Unit,
) {
    Column {
        Text("Display", style = MaterialTheme.typography.titleSmall)
        ChoiceSettingsItem(
            icon = Icons.Outlined.Speed,
            title = "Frame rate",
            options = frameRateOptions(instance.refreshHz),
            selected = instance.refreshHz,
            label = { "$it Hz" },
            description = {
                when {
                    it < 60 -> "Saves battery, less smooth"
                    it == 60 -> "The usual rate"
                    else -> "Smoother, uses more power"
                }
            },
            onSelect = onRefreshHz,
        )
        val resolutions = listOf<GuestResolution?>(null) + GuestResolution.entries
        ChoiceSettingsItem(
            icon = Icons.Outlined.HighQuality,
            title = "Resolution",
            options = resolutions,
            selected = instance.resolution?.let(GuestResolution::fromName),
            label = { it?.label ?: "App setting (${appResolution.label})" },
            description = { it?.description ?: "Follows the resolution in Settings" },
            onSelect = onResolution,
        )
        SwitchSettingsItem(
            icon = Icons.Outlined.Sync,
            title = "Vsync",
            subtitle = "Show frames on the screen's refresh. Off shows them as they come: less lag, more battery.",
            checked = instance.vsync,
            onCheckedChange = onVsync,
        )
        Text(
            "Frame rate and resolution apply the next time the instance starts, vsync when its screen opens.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun SnapshotsSection(
    state: SnapshotsUiState,
    running: Boolean,
    onCreate: () -> Unit,
    onRestore: (String) -> Unit,
    onDelete: (String) -> Unit,
) {
    var pendingRestore by remember { mutableStateOf<String?>(null) }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
            Text("Snapshots", style = MaterialTheme.typography.titleSmall)
            OutlinedButton(onClick = onCreate, enabled = state.busy == null) { Text("Create snapshot") }
        }
        if (state.busy != null) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                Text(state.busy, style = MaterialTheme.typography.bodySmall)
            }
        }
        if (state.items.isEmpty()) {
            Text(
                "A snapshot saves the instance's disk so you can go back to it later.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        state.items.forEach { item ->
            SnapshotRow(
                item = item,
                canRestore = !running && state.busy == null,
                onRestore = { pendingRestore = item.name },
                onDelete = { onDelete(item.name) },
            )
        }
    }

    pendingRestore?.let { name ->
        AlertDialog(
            onDismissRequest = { pendingRestore = null },
            title = { Text("Restore $name?") },
            text = { Text("The instance's current disk is replaced by this snapshot. This can't be undone.") },
            confirmButton = {
                TextButton(onClick = {
                    onRestore(name)
                    pendingRestore = null
                }) { Text("Restore") }
            },
            dismissButton = { TextButton(onClick = { pendingRestore = null }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun SnapshotRow(item: SnapshotInfo, canRestore: Boolean, onRestore: () -> Unit, onDelete: () -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(item.name, style = MaterialTheme.typography.bodyMedium)
            Text(
                "${StorageSizing.format((item.usedBytes / (1024 * 1024)).toInt().coerceAtLeast(1))} used, " +
                    DateFormat.getDateTimeInstance().format(Date(item.createdAtSec * 1000)),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        TextButton(onClick = onRestore, enabled = canRestore) { Text("Restore") }
        IconButton(onClick = onDelete) { Icon(Icons.Filled.Delete, contentDescription = "Delete snapshot") }
    }
}
