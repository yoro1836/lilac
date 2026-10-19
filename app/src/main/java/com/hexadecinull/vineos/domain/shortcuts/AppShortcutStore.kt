package com.hexadecinull.vineos.domain.shortcuts

import android.content.Context
import android.content.Intent
import android.graphics.BitmapFactory
import androidx.core.content.pm.ShortcutInfoCompat
import androidx.core.content.pm.ShortcutManagerCompat
import androidx.core.graphics.drawable.IconCompat
import com.hexadecinull.vineos.MainActivity
import com.hexadecinull.vineos.R
import com.hexadecinull.vineos.data.models.VMInstance
import com.hexadecinull.vineos.helper.GuestApp
import com.hexadecinull.vineos.helper.HelperProtocol
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.File
import javax.inject.Inject
import javax.inject.Singleton

sealed interface PinResult {
    data object Requested : PinResult

    data object Unsupported : PinResult
}

// Remembers each instance's app list and icons so the shortcut picker works while the instance is off, and creates the pinned shortcuts
@Singleton
class AppShortcutStore @Inject constructor(@ApplicationContext private val context: Context) {
    private val root = File(context.filesDir, "app_cache")

    private fun dirOf(instanceId: String): File? = if (ShortcutTarget.isValidInstanceId(instanceId)) File(root, instanceId) else null

    fun saveApps(instanceId: String, apps: List<GuestApp>) {
        val dir = dirOf(instanceId) ?: return
        dir.mkdirs()
        val text = apps.joinToString("\n") { it.packageName + "\t" + it.label.replace('\t', ' ').replace('\n', ' ') }
        runCatching { File(dir, "apps.tsv").writeText(text) }
    }

    fun loadApps(instanceId: String): List<GuestApp> {
        val file = dirOf(instanceId)?.let { File(it, "apps.tsv") } ?: return emptyList()
        return runCatching { if (file.isFile) HelperProtocol.parseApps(file.readText()) else emptyList() }.getOrDefault(emptyList())
    }

    fun saveIcon(instanceId: String, packageName: String, png: ByteArray) {
        if (!HelperProtocol.isValidPackageName(packageName)) return
        val dir = dirOf(instanceId)?.let { File(it, "icons") } ?: return
        dir.mkdirs()
        runCatching { File(dir, "$packageName.png").writeBytes(png) }
    }

    fun iconFile(instanceId: String, packageName: String): File? {
        if (!HelperProtocol.isValidPackageName(packageName)) return null
        val file = dirOf(instanceId)?.let { File(it, "icons/$packageName.png") } ?: return null
        return file.takeIf { it.isFile && it.length() > 0 }
    }

    fun launchIntent(target: ShortcutTarget): Intent = Intent(context, MainActivity::class.java).apply {
        action = ShortcutTarget.ACTION_LAUNCH_APP
        putExtra(ShortcutTarget.EXTRA_INSTANCE_ID, target.instanceId)
        putExtra(ShortcutTarget.EXTRA_PACKAGE, target.packageName)
        addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP)
    }

    // Asks the launcher to pin a shortcut with the app's name and, when it was fetched, its own icon
    fun requestPin(instance: VMInstance, app: GuestApp): PinResult {
        if (!ShortcutManagerCompat.isRequestPinShortcutSupported(context)) return PinResult.Unsupported
        val target = ShortcutTarget(instance.id, app.packageName)
        val bitmap = iconFile(instance.id, app.packageName)?.let { runCatching { BitmapFactory.decodeFile(it.path) }.getOrNull() }
        val icon = if (bitmap != null) {
            IconCompat.createWithBitmap(bitmap)
        } else {
            IconCompat.createWithResource(context, R.mipmap.ic_launcher)
        }
        val info = ShortcutInfoCompat.Builder(context, target.shortcutId)
            .setShortLabel(app.label.take(MAX_SHORT_LABEL))
            .setLongLabel("${app.label} (${instance.name})".take(MAX_LONG_LABEL))
            .setIcon(icon)
            .setIntent(launchIntent(target))
            .build()
        return if (ShortcutManagerCompat.requestPinShortcut(context, info, null)) PinResult.Requested else PinResult.Unsupported
    }

    // Drops the cache and greys out the pinned shortcuts of a deleted instance
    fun forgetInstance(instanceId: String) {
        dirOf(instanceId)?.deleteRecursively()
        runCatching {
            val stale = ShortcutManagerCompat.getShortcuts(context, ShortcutManagerCompat.FLAG_MATCH_PINNED)
                .filter { ShortcutTarget.instanceOfShortcutId(it.id) == instanceId }
                .map { it.id }
            if (stale.isNotEmpty()) ShortcutManagerCompat.disableShortcuts(context, stale, "This instance was deleted")
        }
    }

    // Greys out one shortcut whose instance or app is gone
    fun disable(target: ShortcutTarget, message: String) {
        runCatching { ShortcutManagerCompat.disableShortcuts(context, listOf(target.shortcutId), message) }
    }

    companion object {
        private const val MAX_SHORT_LABEL = 24
        private const val MAX_LONG_LABEL = 64
    }
}
