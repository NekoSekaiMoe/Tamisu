package me.dabao1955.tamisu.ui.theme

import android.content.Context
import android.os.Build
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.enableEdgeToEdge
import androidx.annotation.RequiresApi
import androidx.compose.foundation.background
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.edit

@Stable
object ThemeConfig {
    var forceDarkMode by mutableStateOf<Boolean?>(null)
    var useDynamicColor by mutableStateOf(false)

    private var lastDarkModeState: Boolean? = null

    fun detectThemeChange(currentDarkMode: Boolean): Boolean {
        val hasChanged = lastDarkModeState != null && lastDarkModeState != currentDarkMode
        lastDarkModeState = currentDarkMode
        return hasChanged
    }

    fun updateTheme(
        dynamicColor: Boolean? = null,
        darkMode: Boolean? = null
    ) {
        dynamicColor?.let { useDynamicColor = it }
        darkMode?.let { forceDarkMode = it }
    }

    fun reset() {
        forceDarkMode = null
        useDynamicColor = false
        lastDarkModeState = null
    }
}

object ThemeManager {
    private const val PREFS_NAME = "theme_prefs"

    fun saveThemeMode(context: Context, forceDark: Boolean?) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit {
            putString("theme_mode", when (forceDark) {
                true -> "dark"
                false -> "light"
                null -> "system"
            })
        }
        ThemeConfig.forceDarkMode = forceDark
    }

    fun loadThemeMode(context: Context) {
        val mode = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString("theme_mode", "system")

        ThemeConfig.forceDarkMode = when (mode) {
            "dark" -> true
            "light" -> false
            else -> null
        }
    }

    fun saveDynamicColorState(context: Context, enabled: Boolean) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit {
            putBoolean("use_dynamic_color", enabled)
        }
        ThemeConfig.useDynamicColor = enabled
    }


    fun loadDynamicColorState(context: Context) {
        val enabled = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getBoolean("use_dynamic_color", Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
        ThemeConfig.useDynamicColor = enabled
    }
}

@Composable
fun KernelSUTheme(
    darkTheme: Boolean = when(ThemeConfig.forceDarkMode) {
        true -> true
        false -> false
        null -> isSystemInDarkTheme()
    },
    dynamicColor: Boolean = ThemeConfig.useDynamicColor,
    showBackground: Boolean = true,
    content: @Composable () -> Unit
) {
    val context = LocalContext.current
    val systemIsDark = isSystemInDarkTheme()

    ThemeInitializer(context = context, systemIsDark = systemIsDark)

    val colorScheme = createColorScheme(context, darkTheme, dynamicColor)

    // 系统栏样式
    SystemBarController(darkTheme)

    MaterialTheme(
        colorScheme = colorScheme,
        typography = Typography
    ) {
        if (showBackground) {
            Box(modifier = Modifier.fillMaxSize()) {
                content()
            }
        } else {
            content()
        }
    }
}

@Composable
private fun ThemeInitializer(context: Context, systemIsDark: Boolean) {
    LaunchedEffect(Unit) {
        ThemeManager.loadThemeMode(context)
        ThemeManager.loadDynamicColorState(context)
        CardConfig.load(context)
    }
}

@Composable
private fun createColorScheme(
    context: Context,
    darkTheme: Boolean,
    dynamicColor: Boolean
): ColorScheme {
    return when {
        dynamicColor && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> {
            if (darkTheme) createDynamicDarkColorScheme(context)
            else createDynamicLightColorScheme(context)
        }
        darkTheme -> createDarkColorScheme()
        else -> createLightColorScheme()
    }
}

@Composable
private fun SystemBarController(darkMode: Boolean) {
    val context = LocalContext.current
    val activity = context as ComponentActivity

    SideEffect {
        activity.enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.auto(
                Color.Transparent.toArgb(),
                Color.Transparent.toArgb(),
            ) { darkMode },
            navigationBarStyle = if (darkMode) {
                SystemBarStyle.dark(Color.Transparent.toArgb())
            } else {
                SystemBarStyle.light(
                    Color.Transparent.toArgb(),
                    Color.Transparent.toArgb()
                )
            }
        )
    }
}

@RequiresApi(Build.VERSION_CODES.S)
@Composable
private fun createDynamicDarkColorScheme(context: Context): ColorScheme {
    return dynamicDarkColorScheme(context)
}

@RequiresApi(Build.VERSION_CODES.S)
@Composable
private fun createDynamicLightColorScheme(context: Context): ColorScheme {
    return dynamicLightColorScheme(context)
}

@Composable
private fun createDarkColorScheme() = darkColorScheme(
    primary = ThemeColors.Default.primaryDark,
    onPrimary = ThemeColors.Default.onPrimaryDark,
    primaryContainer = ThemeColors.Default.primaryContainerDark,
    onPrimaryContainer = ThemeColors.Default.onPrimaryContainerDark,
    secondary = ThemeColors.Default.secondaryDark,
    onSecondary = ThemeColors.Default.onSecondaryDark,
    secondaryContainer = ThemeColors.Default.secondaryContainerDark,
    onSecondaryContainer = ThemeColors.Default.onSecondaryContainerDark,
    tertiary = ThemeColors.Default.tertiaryDark,
    onTertiary = ThemeColors.Default.onTertiaryDark,
    tertiaryContainer = ThemeColors.Default.tertiaryContainerDark,
    onTertiaryContainer = ThemeColors.Default.onTertiaryContainerDark,
    error = ThemeColors.Default.errorDark,
    onError = ThemeColors.Default.onErrorDark,
    errorContainer = ThemeColors.Default.errorContainerDark,
    onErrorContainer = ThemeColors.Default.onErrorContainerDark,
    background = ThemeColors.Default.backgroundDark,
    onBackground = ThemeColors.Default.onBackgroundDark,
    surface = ThemeColors.Default.surfaceDark,
    onSurface = ThemeColors.Default.onSurfaceDark,
    surfaceVariant = ThemeColors.Default.surfaceVariantDark,
    onSurfaceVariant = ThemeColors.Default.onSurfaceVariantDark,
    outline = ThemeColors.Default.outlineDark,
    outlineVariant = ThemeColors.Default.outlineVariantDark,
    scrim = ThemeColors.Default.scrimDark,
    inverseSurface = ThemeColors.Default.inverseSurfaceDark,
    inverseOnSurface = ThemeColors.Default.inverseOnSurfaceDark,
    inversePrimary = ThemeColors.Default.inversePrimaryDark,
    surfaceDim = ThemeColors.Default.surfaceDimDark,
    surfaceBright = ThemeColors.Default.surfaceBrightDark,
    surfaceContainerLowest = ThemeColors.Default.surfaceContainerLowestDark,
    surfaceContainerLow = ThemeColors.Default.surfaceContainerLowDark,
    surfaceContainer = ThemeColors.Default.surfaceContainerDark,
    surfaceContainerHigh = ThemeColors.Default.surfaceContainerHighDark,
    surfaceContainerHighest = ThemeColors.Default.surfaceContainerHighestDark,
)

@Composable
private fun createLightColorScheme() = lightColorScheme(
    primary = ThemeColors.Default.primaryLight,
    onPrimary = ThemeColors.Default.onPrimaryLight,
    primaryContainer = ThemeColors.Default.primaryContainerLight,
    onPrimaryContainer = ThemeColors.Default.onPrimaryContainerLight,
    secondary = ThemeColors.Default.secondaryLight,
    onSecondary = ThemeColors.Default.onSecondaryLight,
    secondaryContainer = ThemeColors.Default.secondaryContainerLight,
    onSecondaryContainer = ThemeColors.Default.onSecondaryContainerLight,
    tertiary = ThemeColors.Default.tertiaryLight,
    onTertiary = ThemeColors.Default.onTertiaryLight,
    tertiaryContainer = ThemeColors.Default.tertiaryContainerLight,
    onTertiaryContainer = ThemeColors.Default.onTertiaryContainerLight,
    error = ThemeColors.Default.errorLight,
    onError = ThemeColors.Default.onErrorLight,
    errorContainer = ThemeColors.Default.errorContainerLight,
    onErrorContainer = ThemeColors.Default.onErrorContainerLight,
    background = ThemeColors.Default.backgroundLight,
    onBackground = ThemeColors.Default.onBackgroundLight,
    surface = ThemeColors.Default.surfaceLight,
    onSurface = ThemeColors.Default.onSurfaceLight,
    surfaceVariant = ThemeColors.Default.surfaceVariantLight,
    onSurfaceVariant = ThemeColors.Default.onSurfaceVariantLight,
    outline = ThemeColors.Default.outlineLight,
    outlineVariant = ThemeColors.Default.outlineVariantLight,
    scrim = ThemeColors.Default.scrimLight,
    inverseSurface = ThemeColors.Default.inverseSurfaceLight,
    inverseOnSurface = ThemeColors.Default.inverseOnSurfaceLight,
    inversePrimary = ThemeColors.Default.inversePrimaryLight,
    surfaceDim = ThemeColors.Default.surfaceDimLight,
    surfaceBright = ThemeColors.Default.surfaceBrightLight,
    surfaceContainerLowest = ThemeColors.Default.surfaceContainerLowestLight,
    surfaceContainerLow = ThemeColors.Default.surfaceContainerLowLight,
    surfaceContainer = ThemeColors.Default.surfaceContainerLight,
    surfaceContainerHigh = ThemeColors.Default.surfaceContainerHighLight,
    surfaceContainerHighest = ThemeColors.Default.surfaceContainerHighestLight,
)

// 向后兼容
fun Context.saveThemeMode(forceDark: Boolean?) {
    ThemeManager.saveThemeMode(this, forceDark)
}


fun Context.saveDynamicColorState(enabled: Boolean) {
    ThemeManager.saveDynamicColorState(this, enabled)
}
