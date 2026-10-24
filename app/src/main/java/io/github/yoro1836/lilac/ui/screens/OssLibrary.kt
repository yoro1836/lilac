package io.github.yoro1836.lilac.ui.screens

import io.github.yoro1836.lilac.R

enum class LicenseType(val displayName: String, val rawResId: Int) {
    APACHE_2_0("Apache License 2.0", R.raw.license_apache_2_0),
}

data class OssLibrary(val name: String, val license: LicenseType, val url: String)

// Runtime dependencies actually shipped in the APK; test-only libraries are left out since they never leave the build
val ossLibraries = listOf(
    OssLibrary("Kotlin", LicenseType.APACHE_2_0, "https://github.com/JetBrains/kotlin"),
    OssLibrary("Kotlin Coroutines", LicenseType.APACHE_2_0, "https://github.com/Kotlin/kotlinx.coroutines"),
    OssLibrary("AndroidX Core", LicenseType.APACHE_2_0, "https://github.com/androidx/androidx"),
    OssLibrary("AndroidX Lifecycle", LicenseType.APACHE_2_0, "https://github.com/androidx/androidx"),
    OssLibrary("AndroidX Activity", LicenseType.APACHE_2_0, "https://github.com/androidx/androidx"),
    OssLibrary("Jetpack Compose", LicenseType.APACHE_2_0, "https://github.com/androidx/androidx"),
    OssLibrary("AndroidX Navigation", LicenseType.APACHE_2_0, "https://github.com/androidx/androidx"),
)
