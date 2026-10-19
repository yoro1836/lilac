package com.hexadecinull.vineos.ui.components

import android.content.Context
import android.os.StatFs
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.hexadecinull.vineos.domain.StorageSizing
import kotlin.math.roundToInt

// Largest disk size to offer, based on the phone's free space right now
@Composable
fun rememberMaxStorageMb(): Int {
    val context = LocalContext.current
    return remember { StorageSizing.maxMb(freeBytes(context)) }
}

private fun freeBytes(context: Context): Long = runCatching { StatFs(context.filesDir.path).availableBytes }.getOrDefault(0L)

// A slider over the usual sizes plus a box to type any size in GB, up to maxMb
@Composable
fun StorageSizePicker(valueMb: Int, maxMb: Int, onValueChange: (Int) -> Unit, modifier: Modifier = Modifier) {
    val presets = remember(maxMb) { StorageSizing.presetsMb(maxMb) }
    val index = StorageSizing.nearestPresetIndex(valueMb, presets)
    var text by remember { mutableStateOf(StorageSizing.formatGbInput(valueMb)) }
    var focused by remember { mutableStateOf(false) }
    val focusManager = LocalFocusManager.current

    LaunchedEffect(valueMb) { if (!focused) text = StorageSizing.formatGbInput(valueMb) }

    fun commit() {
        val parsed = StorageSizing.parseGb(text)
        if (parsed != null) {
            val size = StorageSizing.clamp(parsed, maxMb)
            onValueChange(size)
            text = StorageSizing.formatGbInput(size)
        } else {
            text = StorageSizing.formatGbInput(valueMb)
        }
    }

    Column(modifier = modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            if (presets.size > 1) {
                Slider(
                    value = index.toFloat(),
                    onValueChange = { onValueChange(presets[it.roundToInt().coerceIn(0, presets.lastIndex)]) },
                    valueRange = 0f..presets.lastIndex.toFloat(),
                    steps = (presets.size - 2).coerceAtLeast(0),
                    modifier = Modifier.weight(1f),
                )
            }
            OutlinedTextField(
                value = text,
                onValueChange = { input -> text = input.filter { it.isDigit() || it == '.' || it == ',' }.take(7) },
                suffix = { Text("GB") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal, imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = {
                    commit()
                    focusManager.clearFocus()
                }),
                modifier = Modifier
                    .width(120.dp)
                    .onFocusChanged {
                        if (focused && !it.isFocused) commit()
                        focused = it.isFocused
                    },
            )
        }
        Text(
            "Up to ${StorageSizing.format(maxMb)}, based on the free space on this phone. The disk only uses space as the guest fills it.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}
