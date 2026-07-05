package me.dabao1955.tamisu.ui

import android.content.Context
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
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.lifecycle.lifecycleScope
import androidx.navigation.NavBackStackEntry
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.ramcosta.composedestinations.DestinationsNavHost
import com.ramcosta.composedestinations.animations.NavHostAnimatedDestinationStyle
import com.ramcosta.composedestinations.generated.NavGraphs
import com.ramcosta.composedestinations.generated.destinations.HomeScreenDestination
import com.ramcosta.composedestinations.spec.NavHostGraphSpec
import com.ramcosta.composedestinations.utils.rememberDestinationsNavigator
import me.dabao1955.tamisu.ui.activity.component.BottomBar
import me.dabao1955.tamisu.ui.activity.util.AnimatedBottomBar
import me.dabao1955.tamisu.ui.activity.util.DataRefreshUtils
import me.dabao1955.tamisu.ui.activity.util.DisplayUtils
import me.dabao1955.tamisu.ui.activity.util.ThemeChangeContentObserver
import me.dabao1955.tamisu.ui.activity.util.ThemeUtils
import me.dabao1955.tamisu.ui.screen.BottomBarDestination
import me.dabao1955.tamisu.ui.theme.TamisuTheme
import me.dabao1955.tamisu.ui.util.TamisuCli
import me.dabao1955.tamisu.ui.util.LocalSnackbarHost
import me.dabao1955.tamisu.ui.util.resetTaskDescriptionToAppName
import me.dabao1955.tamisu.ui.viewmodel.HomeViewModel
import kotlinx.coroutines.launch
import ui.screen.moreSettings.util.LocaleHelper

class MainActivity : ComponentActivity() {
    private lateinit var homeViewModel: HomeViewModel

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

            setContent {
                TamisuTheme {
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

                    LaunchedEffect(Unit) {
                        if (getSharedPreferences("settings", MODE_PRIVATE)
                                .getBoolean("auto_update_tamisu_daemon", false)
                        ) {
                            TamisuCli.autoSyncKsudIfNeeded()
                        }
                    }

                    CompositionLocalProvider(
                        LocalSnackbarHost provides snackBarHostState
                    ) {
                        Scaffold(
                            snackbarHost = { SnackbarHost(hostState = snackBarHostState) },
                            bottomBar = {
                                AnimatedBottomBar.AnimatedBottomBarWrapper(
                                    showBottomBar = true,
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
}

