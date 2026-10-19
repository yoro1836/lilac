package com.hexadecinull.vineos.domain.shortcuts

import com.hexadecinull.vineos.helper.HelperProtocol

// What a home-screen shortcut opens: one app inside one instance
data class ShortcutTarget(val instanceId: String, val packageName: String) {
    val shortcutId: String get() = "$ID_PREFIX$instanceId:$packageName"

    companion object {
        const val ACTION_LAUNCH_APP = "com.hexadecinull.vineos.action.LAUNCH_APP"
        const val EXTRA_INSTANCE_ID = "com.hexadecinull.vineos.extra.INSTANCE_ID"
        const val EXTRA_PACKAGE = "com.hexadecinull.vineos.extra.PACKAGE"
        const val ID_PREFIX = "app:"
        private val instancePattern = Regex("^[A-Za-z0-9_.-]{1,64}$")

        fun isValidInstanceId(id: String): Boolean = instancePattern.matches(id) && id != "." && id != ".."

        // Intent extras come from outside the app, so both parts are checked before anything is started
        fun fromExtras(instanceId: String?, packageName: String?): ShortcutTarget? {
            if (instanceId == null || packageName == null) return null
            if (!isValidInstanceId(instanceId) || !HelperProtocol.isValidPackageName(packageName)) return null
            return ShortcutTarget(instanceId, packageName)
        }

        fun instanceOfShortcutId(shortcutId: String): String? {
            if (!shortcutId.startsWith(ID_PREFIX)) return null
            val rest = shortcutId.removePrefix(ID_PREFIX)
            val colon = rest.indexOf(':')
            return if (colon > 0) rest.substring(0, colon) else null
        }
    }
}
