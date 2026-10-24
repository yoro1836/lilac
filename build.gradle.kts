plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.kotlin.compose) apply false
    // ktlint applied at root so the task is available project-wide via `./gradlew ktlintCheck`
    alias(libs.plugins.ktlint) apply false
}
