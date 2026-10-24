package io.github.yoro1836.lilac.ui.theme

import android.app.Activity
import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.core.view.WindowCompat

private val LilacLightColorScheme = lightColorScheme(
    primary = LilacPurple40,
    onPrimary = LilacNeutral99,
    primaryContainer = LilacPurple90,
    onPrimaryContainer = LilacPurple10,
    secondary = LilacMauve40,
    onSecondary = LilacNeutral99,
    secondaryContainer = LilacMauve90,
    onSecondaryContainer = LilacMauve10,
    tertiary = LilacLeaf40,
    onTertiary = LilacNeutral99,
    tertiaryContainer = LilacLeaf90,
    onTertiaryContainer = LilacLeaf10,
    error = LilacError40,
    onError = LilacNeutral99,
    errorContainer = LilacError90,
    onErrorContainer = LilacError10,
    background = LilacNeutral99,
    onBackground = LilacNeutral10,
    surface = LilacNeutral99,
    onSurface = LilacNeutral10,
    surfaceVariant = LilacNeutralVariant90,
    onSurfaceVariant = LilacNeutralVariant30,
    outline = LilacNeutralVariant50,
    outlineVariant = LilacNeutralVariant80,
)

private val LilacDarkColorScheme = darkColorScheme(
    primary = LilacPurple80,
    onPrimary = LilacPurple20,
    primaryContainer = LilacPurple30,
    onPrimaryContainer = LilacPurple90,
    secondary = LilacMauve80,
    onSecondary = LilacMauve20,
    secondaryContainer = LilacMauve30,
    onSecondaryContainer = LilacMauve90,
    tertiary = LilacLeaf80,
    onTertiary = LilacLeaf20,
    tertiaryContainer = LilacLeaf30,
    onTertiaryContainer = LilacLeaf90,
    error = LilacError80,
    onError = LilacError20,
    errorContainer = LilacError30,
    onErrorContainer = LilacError90,
    background = LilacNeutral10,
    onBackground = LilacNeutral90,
    surface = LilacNeutral10,
    onSurface = LilacNeutral90,
    surfaceVariant = LilacNeutralVariant30,
    onSurfaceVariant = LilacNeutralVariant80,
    outline = LilacNeutralVariant60,
    outlineVariant = LilacNeutralVariant30,
)

@Composable
fun LilacTheme(darkTheme: Boolean = isSystemInDarkTheme(), dynamicColor: Boolean = true, content: @Composable () -> Unit) {
    val colorScheme = when {
        dynamicColor && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> {
            val context = LocalContext.current
            if (darkTheme) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        }
        darkTheme -> LilacDarkColorScheme
        else -> LilacLightColorScheme
    }

    val view = LocalView.current
    if (!view.isInEditMode) {
        SideEffect {
            val window = (view.context as Activity).window
            // enableEdgeToEdge() in MainActivity already makes the bars transparent, only icon appearance is set here
            WindowCompat.getInsetsController(window, view).apply {
                isAppearanceLightStatusBars = !darkTheme
                isAppearanceLightNavigationBars = !darkTheme
            }
        }
    }

    MaterialTheme(
        colorScheme = colorScheme,
        typography = LilacTypography,
        content = content,
    )
}
