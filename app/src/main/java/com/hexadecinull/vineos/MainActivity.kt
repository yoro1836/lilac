package com.hexadecinull.vineos

import android.content.Intent
import android.os.Bundle
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.ScaffoldDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.lifecycleScope
import androidx.navigation.NavController
import androidx.navigation.NavDestination.Companion.hierarchy
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.hexadecinull.vineos.domain.shortcuts.ShortcutLauncher
import com.hexadecinull.vineos.domain.shortcuts.ShortcutTarget
import com.hexadecinull.vineos.ui.navigation.Screen
import com.hexadecinull.vineos.ui.navigation.bottomNavItems
import com.hexadecinull.vineos.ui.screens.AboutScreen
import com.hexadecinull.vineos.ui.screens.AppSettings
import com.hexadecinull.vineos.ui.screens.AppShortcutsScreen
import com.hexadecinull.vineos.ui.screens.CreateInstanceScreen
import com.hexadecinull.vineos.ui.screens.HomeScreen
import com.hexadecinull.vineos.ui.screens.InstanceDetailScreen
import com.hexadecinull.vineos.ui.screens.LicenseDetailScreen
import com.hexadecinull.vineos.ui.screens.LicensesScreen
import com.hexadecinull.vineos.ui.screens.ROMDetailScreen
import com.hexadecinull.vineos.ui.screens.ROMsScreen
import com.hexadecinull.vineos.ui.screens.SettingsScreen
import com.hexadecinull.vineos.ui.screens.VMDisplayScreen
import com.hexadecinull.vineos.ui.theme.WayondroidTheme
import com.hexadecinull.vineos.ui.viewmodel.AboutViewModel
import com.hexadecinull.vineos.ui.viewmodel.HomeViewModel
import com.hexadecinull.vineos.ui.viewmodel.ROMsViewModel
import com.hexadecinull.vineos.ui.viewmodel.SettingsViewModel
import com.hexadecinull.vineos.ui.viewmodel.ShizukuViewModel
import dagger.hilt.android.AndroidEntryPoint
import javax.inject.Inject
import kotlinx.coroutines.launch

@AndroidEntryPoint
class MainActivity : ComponentActivity() {
    @Inject lateinit var shortcutLauncher: ShortcutLauncher

    private var pendingRoute by mutableStateOf<String?>(null)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        if (savedInstanceState == null) handleShortcutIntent(intent)
        setContent {
            val settingsVm: SettingsViewModel = hiltViewModel()
            val settings by settingsVm.settings.collectAsStateWithLifecycle()

            LaunchedEffect(settings.keepScreenOn) {
                if (settings.keepScreenOn) {
                    window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                } else {
                    window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                }
            }

            WayondroidTheme(dynamicColor = settings.dynamicColor) {
                WayondroidApp(
                    settings = settings,
                    onSettingsChange = { settingsVm.update(it) },
                    pendingRoute = pendingRoute,
                    onRouteHandled = { pendingRoute = null },
                )
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleShortcutIntent(intent)
    }

    // A tap on a home-screen shortcut: start the instance, open the app in it, and show the guest screen
    private fun handleShortcutIntent(intent: Intent?) {
        if (intent?.action != ShortcutTarget.ACTION_LAUNCH_APP) return
        val target = ShortcutTarget.fromExtras(
            intent.getStringExtra(ShortcutTarget.EXTRA_INSTANCE_ID),
            intent.getStringExtra(ShortcutTarget.EXTRA_PACKAGE),
        ) ?: return
        lifecycleScope.launch {
            val route = shortcutLauncher.prepare(target)
            if (route != null) {
                pendingRoute = route
            } else {
                Toast.makeText(this@MainActivity, "That instance no longer exists", Toast.LENGTH_LONG).show()
            }
        }
    }
}

@Composable
fun WayondroidApp(settings: AppSettings, onSettingsChange: (AppSettings) -> Unit, pendingRoute: String?, onRouteHandled: () -> Unit) {
    val navController = rememberNavController()
    val backStackEntry by navController.currentBackStackEntryAsState()
    // The guest screen draws edge to edge, so the Scaffold must not reserve space for the system bars there
    val immersive = backStackEntry?.destination?.route == Screen.VMDisplay.route

    LaunchedEffect(pendingRoute) {
        val route = pendingRoute ?: return@LaunchedEffect
        navController.navigate(route) { launchSingleTop = true }
        onRouteHandled()
    }

    Scaffold(
        modifier = Modifier.fillMaxSize(),
        contentWindowInsets = if (immersive) WindowInsets(0, 0, 0, 0) else ScaffoldDefaults.contentWindowInsets,
        bottomBar = { VineBottomNavBar(navController) },
    ) { innerPadding ->
        NavHost(
            navController = navController,
            startDestination = Screen.Home.route,
            // Each destination owns a Scaffold+TopAppBar that already consumes its own status/nav-bar insets; without consumeWindowInsets those get reserved a second time here, which is the empty gap at the top of every screen
            modifier = Modifier
                .padding(innerPadding)
                .consumeWindowInsets(innerPadding),
            enterTransition = { fadeIn() + slideInHorizontally { it / 8 } },
            exitTransition = { fadeOut() },
            popEnterTransition = { fadeIn() },
            popExitTransition = { fadeOut() + slideOutHorizontally { it / 8 } },
        ) {
            composable(Screen.Home.route) {
                val vm: HomeViewModel = hiltViewModel()
                val uiState by vm.uiState.collectAsStateWithLifecycle()
                HomeScreen(
                    instances = uiState.instances,
                    onLaunchInstance = { instance ->
                        vm.launchInstance(instance)
                        navController.navigate(Screen.VMDisplay.createRoute(instance.id))
                    },
                    onStopInstance = vm::stopInstance,
                    onInstanceDetail = { navController.navigate(Screen.InstanceDetail.createRoute(it.id)) },
                    onDeleteInstance = vm::deleteInstance,
                    onCreateInstance = { navController.navigate(Screen.ROMs.route) },
                    modifier = Modifier.fillMaxSize(),
                )
            }

            composable(Screen.ROMs.route) {
                val vm: ROMsViewModel = hiltViewModel()
                val uiState by vm.uiState.collectAsStateWithLifecycle()
                val importError by vm.importError.collectAsStateWithLifecycle()
                ROMsScreen(
                    roms = uiState.roms,
                    downloadProgress = uiState.downloadProgress,
                    onDownloadROM = vm::download,
                    onDeleteROM = vm::delete,
                    onROMDetail = { navController.navigate(Screen.ROMDetail.createRoute(it.id)) },
                    onImportRom = vm::importLocalRom,
                    importError = importError,
                    onImportErrorShown = vm::clearImportError,
                    isLoading = uiState.isLoading,
                    modifier = Modifier.fillMaxSize(),
                )
            }

            composable(Screen.Settings.route) {
                val shizukuVm: ShizukuViewModel = hiltViewModel()
                val shizukuStatus by shizukuVm.status.collectAsStateWithLifecycle()
                val probeState by shizukuVm.probeState.collectAsStateWithLifecycle()
                SettingsScreen(
                    settings = settings,
                    onSettingsChange = onSettingsChange,
                    onAboutClick = { navController.navigate(Screen.About.route) },
                    onOpenShortcuts = { navController.navigate(Screen.AppShortcuts.route) },
                    shizukuStatus = shizukuStatus,
                    onRequestShizukuPermission = shizukuVm::requestPermission,
                    probeState = probeState,
                    onRunProbe = shizukuVm::runProbe,
                    modifier = Modifier.fillMaxSize(),
                )
            }

            composable(Screen.AppShortcuts.route) {
                AppShortcutsScreen(
                    onBack = { navController.popBackStack() },
                    modifier = Modifier.fillMaxSize(),
                )
            }

            composable(Screen.About.route) {
                val vm: AboutViewModel = hiltViewModel()
                AboutScreen(
                    sections = vm.sections,
                    onBack = { navController.popBackStack() },
                    onLicenses = { navController.navigate(Screen.Licenses.route) },
                    modifier = Modifier.fillMaxSize(),
                )
            }

            composable(Screen.Licenses.route) {
                LicensesScreen(
                    onBack = { navController.popBackStack() },
                    onLibraryClick = { navController.navigate(Screen.LicenseDetail.createRoute(it)) },
                    modifier = Modifier.fillMaxSize(),
                )
            }

            composable(Screen.LicenseDetail.route) { back ->
                val index = back.arguments?.getString("libraryIndex")?.toIntOrNull() ?: return@composable
                LicenseDetailScreen(
                    libraryIndex = index,
                    onBack = { navController.popBackStack() },
                    modifier = Modifier.fillMaxSize(),
                )
            }

            // Full-screen, no bottom bar: direct view into the running guest
            composable(Screen.VMDisplay.route) { back ->
                val id = back.arguments?.getString("instanceId") ?: return@composable
                VMDisplayScreen(
                    instanceId = id,
                    onBack = { navController.popBackStack() },
                    modifier = Modifier.fillMaxSize(),
                )
            }

            composable(Screen.InstanceDetail.route) { back ->
                val id = back.arguments?.getString("instanceId") ?: return@composable
                InstanceDetailScreen(
                    instanceId = id,
                    onBack = { navController.popBackStack() },
                    onLaunch = { navController.navigate(Screen.VMDisplay.createRoute(it)) },
                    onDeleted = { navController.popBackStack() },
                    modifier = Modifier.fillMaxSize(),
                )
            }

            composable(Screen.ROMDetail.route) { back ->
                val id = back.arguments?.getString("romId") ?: return@composable
                ROMDetailScreen(
                    romId = id,
                    onBack = { navController.popBackStack() },
                    onCreateInstance = { navController.navigate(Screen.CreateInstance.createRoute(it)) },
                    modifier = Modifier.fillMaxSize(),
                )
            }

            composable(Screen.CreateInstance.route) { back ->
                val id = back.arguments?.getString("romId") ?: return@composable
                CreateInstanceScreen(
                    romId = id,
                    onBack = { navController.popBackStack() },
                    onCreated = {
                        navController.navigate(Screen.Home.route) {
                            popUpTo(Screen.Home.route) { inclusive = true }
                        }
                    },
                    modifier = Modifier.fillMaxSize(),
                )
            }
        }
    }
}

@Composable
private fun VineBottomNavBar(navController: NavController) {
    val navBackStackEntry by navController.currentBackStackEntryAsState()
    val current = navBackStackEntry?.destination

    // Only shown on the three bottom-nav destinations
    val showBar = bottomNavItems.any { item ->
        current?.hierarchy?.any { it.route == item.screen.route } == true
    }
    if (!showBar) return

    NavigationBar {
        bottomNavItems.forEach { item ->
            val selected = current?.hierarchy?.any { it.route == item.screen.route } == true
            NavigationBarItem(
                icon = { Icon(if (selected) item.selectedIcon else item.unselectedIcon, item.label) },
                label = { Text(item.label) },
                selected = selected,
                onClick = {
                    navController.navigate(item.screen.route) {
                        popUpTo(navController.graph.findStartDestination().id) { saveState = true }
                        launchSingleTop = true
                        restoreState = true
                    }
                },
            )
        }
    }
}
