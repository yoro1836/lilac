package io.github.yoro1836.lilac

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import io.github.yoro1836.lilac.ui.aboutSections
import io.github.yoro1836.lilac.ui.screens.AboutScreen
import io.github.yoro1836.lilac.ui.screens.HomeScreen
import io.github.yoro1836.lilac.ui.screens.LicenseDetailScreen
import io.github.yoro1836.lilac.ui.screens.LicensesScreen
import io.github.yoro1836.lilac.ui.theme.LilacTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            LilacTheme {
                LilacApp()
            }
        }
    }
}

private object Routes {
    const val HOME = "home"
    const val ABOUT = "about"
    const val LICENSES = "licenses"
    const val LICENSE_DETAIL = "license/{libraryIndex}"

    fun licenseDetail(libraryIndex: Int) = "license/$libraryIndex"
}

@Composable
fun LilacApp() {
    val navController = rememberNavController()

    NavHost(
        navController = navController,
        startDestination = Routes.HOME,
        modifier = Modifier.fillMaxSize(),
        enterTransition = { fadeIn() + slideInHorizontally { it / 8 } },
        exitTransition = { fadeOut() },
        popEnterTransition = { fadeIn() },
        popExitTransition = { fadeOut() + slideOutHorizontally { it / 8 } },
    ) {
        composable(Routes.HOME) {
            HomeScreen(
                onAboutClick = { navController.navigate(Routes.ABOUT) },
                modifier = Modifier.fillMaxSize(),
            )
        }

        composable(Routes.ABOUT) {
            val context = LocalContext.current
            val sections = remember { aboutSections(context) }
            AboutScreen(
                sections = sections,
                onBack = { navController.popBackStack() },
                onLicenses = { navController.navigate(Routes.LICENSES) },
                modifier = Modifier.fillMaxSize(),
            )
        }

        composable(Routes.LICENSES) {
            LicensesScreen(
                onBack = { navController.popBackStack() },
                onLibraryClick = { navController.navigate(Routes.licenseDetail(it)) },
                modifier = Modifier.fillMaxSize(),
            )
        }

        composable(Routes.LICENSE_DETAIL) { back ->
            val index = back.arguments?.getString("libraryIndex")?.toIntOrNull() ?: return@composable
            LicenseDetailScreen(
                libraryIndex = index,
                onBack = { navController.popBackStack() },
                modifier = Modifier.fillMaxSize(),
            )
        }
    }
}
