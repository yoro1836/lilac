package io.github.yoro1836.lilac.ui

import android.app.ActivityManager
import android.content.Context
import android.content.res.Resources
import android.os.Build
import io.github.yoro1836.lilac.BuildConfig

data class AboutInfoItem(val label: String, val value: String)
data class AboutInfoSection(val title: String, val items: List<AboutInfoItem>)

// Host ABIs LILAC ships for; the guest runs natively, so the device's primary ABI has to be one of these
val SUPPORTED_HOST_ABIS = listOf("arm64-v8a", "x86_64")

fun isHostAbiSupported(abis: Array<String> = Build.SUPPORTED_ABIS): Boolean = abis.firstOrNull() in SUPPORTED_HOST_ABIS

fun aboutSections(context: Context): List<AboutInfoSection> = listOf(appSection(), hostOsSection(), deviceSection(context))

private fun appSection() = AboutInfoSection(
    title = "LILAC",
    items = listOf(
        AboutInfoItem("Version", "${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})"),
        AboutInfoItem("Application ID", BuildConfig.APPLICATION_ID),
        AboutInfoItem("Build type", "${BuildConfig.BUILD_TYPE}${if (BuildConfig.DEBUG) ", debuggable" else ""}"),
    ),
)

private fun hostOsSection() = AboutInfoSection(
    title = "Host Android",
    items = listOf(
        AboutInfoItem("Version", "Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})"),
        AboutInfoItem("Security patch", Build.VERSION.SECURITY_PATCH ?: "Unknown"),
        AboutInfoItem("Build", Build.DISPLAY),
        AboutInfoItem("Fingerprint", Build.FINGERPRINT),
    ),
)

private fun deviceSection(context: Context): AboutInfoSection {
    val memInfo = ActivityManager.MemoryInfo()
    (context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager)?.getMemoryInfo(memInfo)
    val totalRamMb = memInfo.totalMem / (1024 * 1024)

    val metrics = Resources.getSystem().displayMetrics
    val soc = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        "${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}"
    } else {
        "Unknown (needs Android 12+)"
    }

    return AboutInfoSection(
        title = "Device",
        items = listOf(
            AboutInfoItem("Model", "${Build.MANUFACTURER} ${Build.MODEL} (${Build.DEVICE})"),
            AboutInfoItem("SoC", soc),
            AboutInfoItem("Board / hardware", "${Build.BOARD} / ${Build.HARDWARE}"),
            AboutInfoItem("CPU cores", Runtime.getRuntime().availableProcessors().toString()),
            AboutInfoItem("Total RAM", "$totalRamMb MB"),
            AboutInfoItem("Screen", "${metrics.widthPixels}x${metrics.heightPixels} @ ${metrics.densityDpi}dpi"),
            AboutInfoItem("Supported ABIs", Build.SUPPORTED_ABIS.joinToString(", ")),
            AboutInfoItem(
                "Host ABI supported",
                if (isHostAbiSupported()) "Yes" else "No, LILAC needs ${SUPPORTED_HOST_ABIS.joinToString(" or ")}",
            ),
        ),
    )
}
