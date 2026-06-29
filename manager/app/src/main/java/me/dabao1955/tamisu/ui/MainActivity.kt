package me.dabao1955.tamisu.ui

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.animation.AnimatedContentTransitionScope
import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.core.net.toUri
import androidx.lifecycle.lifecycleScope
import androidx.navigation.NavBackStackEntry
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.ramcosta.composedestinations.DestinationsNavHost
import com.ramcosta.composedestinations.animations.NavHostAnimatedDestinationStyle
import com.ramcosta.composedestinations.generated.NavGraphs
import com.ramcosta.composedestinations.generated.destinations.ExecuteModuleActionScreenDestination
import com.ramcosta.composedestinations.generated.destinations.HomeScreenDestination
import com.ramcosta.composedestinations.spec.NavHostGraphSpec
import com.ramcosta.composedestinations.utils.rememberDestinationsNavigator
import me.dabao1955.tamisu.Natives
import me.dabao1955.tamisu.R
import me.dabao1955.tamisu.ui.activity.component.BottomBar
import me.dabao1955.tamisu.ui.activity.util.AnimatedBottomBar
import me.dabao1955.tamisu.ui.activity.util.DataRefreshUtils
import me.dabao1955.tamisu.ui.activity.util.DisplayUtils
import me.dabao1955.tamisu.ui.activity.util.ThemeChangeContentObserver
import me.dabao1955.tamisu.ui.activity.util.ThemeUtils
import me.dabao1955.tamisu.ui.activity.util.UltraActivityUtils
import me.dabao1955.tamisu.ui.component.InstallConfirmationDialog
import me.dabao1955.tamisu.ui.component.ZipFileInfo
import me.dabao1955.tamisu.ui.screen.BottomBarDestination
import me.dabao1955.tamisu.ui.theme.KernelSUTheme
import me.dabao1955.tamisu.ui.util.KsuCli
import me.dabao1955.tamisu.ui.util.LocalSnackbarHost
import me.dabao1955.tamisu.ui.util.install
import me.dabao1955.tamisu.ui.util.resetTaskDescriptionToAppName
import me.dabao1955.tamisu.ui.viewmodel.HomeViewModel
import kotlinx.coroutines.launch
import ui.screen.moreSettings.util.LocaleHelper

class MainActivity : ComponentActivity() {
    private lateinit var homeViewModel: HomeViewModel

    private var showConfirmationDialog = mutableStateOf(false)
    private var pendingZipFiles = mutableStateOf<List<ZipFileInfo>>(emptyList())

    private lateinit var themeChangeObserver: ThemeChangeContentObserver
    private var isInitialized = false

    override fun attachBaseContext(newBase: Context?) {
        super.attachBaseContext(newBase?.let { LocaleHelper.applyLanguage(it) })
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        try {
            DisplayUtils.applyCustomDpi(this)

            // Enable edge to edge
            enableEdgeToEdge()

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                window.isNavigationBarContrastEnforced = false
            }

            super.onCreate(savedInstanceState)
            resetTaskDescriptionToAppName()

            if (!isInitialized) {
                initializeViewModels()
                initializeData()
                isInitialized = true
            }

            // Check if launched with a ZIP file
            val zipUri: ArrayList<Uri>? = when (intent?.action) {
                Intent.ACTION_SEND -> {
                    val uri = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
                    } else {
                        @Suppress("DEPRECATION")
                        intent.getParcelableExtra(Intent.EXTRA_STREAM)
                    }
                    uri?.let { arrayListOf(it) }
                }

                Intent.ACTION_SEND_MULTIPLE -> {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM, Uri::class.java)
                    } else {
                        @Suppress("DEPRECATION")
                        intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM)
                    }
                }

                else -> when {
                    intent?.data != null -> arrayListOf(intent.data!!)
                    Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU -> {
                        intent.getParcelableArrayListExtra("uris", Uri::class.java)
                    }
                    else -> {
                        @Suppress("DEPRECATION")
                        intent.getParcelableArrayListExtra("uris")
                    }
                }
            }

            setContent {
                KernelSUTheme {
                    val navController = rememberNavController()
                    val snackBarHostState = remember { SnackbarHostState() }
                    val currentDestination = navController.currentBackStackEntryAsState().value?.destination

                    val bottomBarRoutes = remember {
                        BottomBarDestination.entries.map { it.direction.route }.toSet()
                    }

                    val navigator = navController.rememberDestinationsNavigator()

                    BackHandler(currentDestination != null && currentDestination.route != HomeScreenDestination.route) {
                        navigator.navigate(HomeScreenDestination) {
                            navigator.clearBackStack(NavGraphs.root as NavHostGraphSpec)
                            launchSingleTop = true
                        }
                    }

                    InstallConfirmationDialog(
                        show = showConfirmationDialog.value,
                        zipFiles = pendingZipFiles.value,
                        onConfirm = { confirmedFiles ->
                            showConfirmationDialog.value = false
                            UltraActivityUtils.navigateToFlashScreen(this, confirmedFiles, navigator)
                        },
                        onDismiss = {
                            showConfirmationDialog.value = false
                            pendingZipFiles.value = emptyList()
                            finish()
                        }
                    )

                    LaunchedEffect(zipUri) {
                        if (!zipUri.isNullOrEmpty()) {
                            lifecycleScope.launch {
                                UltraActivityUtils.detectZipTypeAndShowConfirmation(this@MainActivity, zipUri) { infos ->
                                    if (infos.isNotEmpty()) {
                                        pendingZipFiles.value = infos
                                        showConfirmationDialog.value = true
                                    } else {
                                        lifecycleScope.launch {
                                            snackBarHostState.showSnackbar(
                                                getString(R.string.unsupported_file_format),
                                                withDismissAction = true
                                            )
                                        }
                                        // 停留在当前 Activity，让用户看到提示
                                    }
                                }
                            }
                        }
                    }

                    val showBottomBar = when (currentDestination?.route) {
                        ExecuteModuleActionScreenDestination.route -> false
                        else -> true
                    }

                    LaunchedEffect(Unit) {
                        if (getSharedPreferences("settings", MODE_PRIVATE)
                                .getBoolean("auto_update_ksud", false)
                        ) {
                            KsuCli.autoSyncKsudIfNeeded()
                        }
                    }

                    CompositionLocalProvider(
                        LocalSnackbarHost provides snackBarHostState
                    ) {
                        Scaffold(
                            snackbarHost = { SnackbarHost(hostState = snackBarHostState) },
                            bottomBar = {
                                AnimatedBottomBar.AnimatedBottomBarWrapper(
                                    showBottomBar = showBottomBar,
                                    content = { BottomBar(navController) }
                                )
                            },
                            contentWindowInsets = WindowInsets(0, 0, 0, 0)
                        ) { innerPadding ->
                            DestinationsNavHost(
                                modifier = Modifier.padding(innerPadding),
                                navGraph = NavGraphs.root as NavHostGraphSpec,
                                navController = navController,
                                defaultTransitions = object : NavHostAnimatedDestinationStyle() {
                                    override val enterTransition: AnimatedContentTransitionScope<NavBackStackEntry>.() -> EnterTransition = {
                                        // If the target is a detail page (not a bottom navigation page), slide in from the right
                                        if (targetState.destination.route !in bottomBarRoutes) {
                                            slideInHorizontally(initialOffsetX = { it })
                                        } else {
                                            // Otherwise (switching between bottom navigation pages), use fade in
                                            fadeIn(animationSpec = tween(340))
                                        }
                                    }

                                    override val exitTransition: AnimatedContentTransitionScope<NavBackStackEntry>.() -> ExitTransition = {
                                        // If navigating from the home page (bottom navigation page) to a detail page, slide out to the left
                                        if (initialState.destination.route in bottomBarRoutes && targetState.destination.route !in bottomBarRoutes) {
                                            slideOutHorizontally(targetOffsetX = { -it / 4 }) + fadeOut()
                                        } else {
                                            // Otherwise (switching between bottom navigation pages), use fade out
                                            fadeOut(animationSpec = tween(340))
                                        }
                                    }

                                    override val popEnterTransition: AnimatedContentTransitionScope<NavBackStackEntry>.() -> EnterTransition = {
                                        // If returning to the home page (bottom navigation page), slide in from the left
                                        if (targetState.destination.route in bottomBarRoutes) {
                                            slideInHorizontally(initialOffsetX = { -it / 4 }) + fadeIn()
                                        } else {
                                            // Otherwise (e.g., returning between multiple detail pages), use default fade in
                                            fadeIn(animationSpec = tween(340))
                                        }
                                    }

                                    override val popExitTransition: AnimatedContentTransitionScope<NavBackStackEntry>.() -> ExitTransition = {
                                        // If returning from a detail page (not a bottom navigation page), scale down and fade out
                                        if (initialState.destination.route !in bottomBarRoutes) {
                                            scaleOut(targetScale = 0.9f) + fadeOut()
                                        } else {
                                            // Otherwise, use default fade out
                                            fadeOut(animationSpec = tween(340))
                                        }
                                    }
                                }
                            )
                        }
                    }
                }
            }
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    private fun initializeViewModels() {
        homeViewModel = HomeViewModel()

        themeChangeObserver = ThemeUtils.registerThemeChangeObserver(this)
    }

    private fun initializeData() {
        DataRefreshUtils.startDataRefreshCoroutine(lifecycleScope)

        ThemeUtils.initializeThemeSettings(this)
    }

    override fun onResume() {
        try {
            super.onResume()
            resetTaskDescriptionToAppName()
            ThemeUtils.onActivityResume()

            if (isInitialized) {
                refreshData()
            }
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    private fun refreshData() {
        lifecycleScope.launch {
            try {
                DataRefreshUtils.refreshData(lifecycleScope)
            } catch (e: Exception) {
                e.printStackTrace()
            }
        }
    }

    override fun onPause() {
        try {
            super.onPause()
            ThemeUtils.onActivityPause(this)
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    override fun onDestroy() {
        try {
            ThemeUtils.unregisterThemeChangeObserver(this, themeChangeObserver)
            super.onDestroy()
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
    }
}

