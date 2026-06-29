package ui.screen.moreSettings.state

import android.content.Context
import android.content.SharedPreferences
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import ui.screen.moreSettings.util.LocaleHelper
import me.dabao1955.tamisu.R
import me.dabao1955.tamisu.ui.theme.ThemeConfig

@Stable
class MoreSettingsState(
    val context: Context,
    val prefs: SharedPreferences,
    val systemIsDark: Boolean
) {
    var themeMode by mutableIntStateOf(
        when (ThemeConfig.forceDarkMode) {
            true -> 2 // 深色
            false -> 1 // 浅色
            null -> 0 // 跟随系统
        }
    )

    var useDynamicColor by mutableStateOf(ThemeConfig.useDynamicColor)

    var showLanguageDialog by mutableStateOf(false)
    var currentAppLocale by mutableStateOf(LocaleHelper.getCurrentAppLocale(context))

    var showThemeModeDialog by mutableStateOf(false)
    var showDpiConfirmDialog by mutableStateOf(false)

    var isSimpleMode by mutableStateOf(prefs.getBoolean("is_simple_mode", false))
    var isKernelSimpleMode by mutableStateOf(prefs.getBoolean("is_kernel_simple_mode", false))

    var selinuxEnabled by mutableStateOf(false)

    var hideBlEnabled by mutableStateOf(false)

    val systemDpi = context.resources.displayMetrics.densityDpi
    var currentDpi by mutableIntStateOf(prefs.getInt("app_dpi", systemDpi))
    var tempDpi by mutableIntStateOf(currentDpi)
    var isDpiCustom by mutableStateOf(true)

    val themeOptions = listOf(
        context.getString(R.string.theme_follow_system),
        context.getString(R.string.theme_light),
        context.getString(R.string.theme_dark)
    )

    val dpiPresets = mapOf(
        context.getString(R.string.dpi_size_small) to 240,
        context.getString(R.string.dpi_size_medium) to 320,
        context.getString(R.string.dpi_size_large) to 420,
        context.getString(R.string.dpi_size_extra_large) to 560
    )
}
