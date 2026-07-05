package ui.screen.moreSettings

import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import androidx.core.content.edit
import android.content.res.Configuration
import android.widget.Toast
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CleaningServices
import androidx.compose.material.icons.filled.Groups
import androidx.compose.material.icons.filled.Scanner
import androidx.compose.material3.SnackbarHostState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.res.stringResource
import me.dabao1955.tamisu.R
import me.dabao1955.tamisu.ui.component.ConfirmResult
import me.dabao1955.tamisu.ui.component.rememberConfirmDialog
import me.dabao1955.tamisu.ui.screen.SettingItem
import me.dabao1955.tamisu.ui.screen.SwitchItem
import me.dabao1955.tamisu.ui.theme.*
import me.dabao1955.tamisu.ui.util.execTamisuDaemon
import me.dabao1955.tamisu.ui.util.*
import me.dabao1955.tamisu.ui.util.getRootShell
import me.dabao1955.tamisu.ui.util.isSELinuxEnforcing
import me.dabao1955.tamisu.ui.util.tamisu_daemonReadString
import com.topjohnwu.superuser.ShellUtils
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ui.screen.moreSettings.state.MoreSettingsState

class MoreSettingsHandlers(
    val context: Context,
    private val prefs: SharedPreferences,
    private val state: MoreSettingsState
) {

    fun initializeSettings() {
        CardConfig.load(context)

        state.themeMode = when (ThemeConfig.forceDarkMode) {
            true -> 2
            false -> 1
            null -> 0
        }

        // 确保卡片样式跟随主题模式
        when (state.themeMode) {
            2 -> { // 深色
                CardConfig.isUserDarkModeEnabled = true
                CardConfig.isUserLightModeEnabled = false
            }
            1 -> { // 浅色
                CardConfig.isUserDarkModeEnabled = false
                CardConfig.isUserLightModeEnabled = true
            }
            0 -> { // 跟随系统
                CardConfig.isUserDarkModeEnabled = false
                CardConfig.isUserLightModeEnabled = false
            }
        }

        // 如果启用了系统跟随且系统是深色模式，应用深色模式默认值
        if (state.themeMode == 0 && state.systemIsDark) {
            CardConfig.setThemeDefaults(true)
        }

        state.currentDpi = prefs.getInt("app_dpi", state.systemDpi)
        state.tempDpi = state.currentDpi

        CardConfig.save(context)

        state.selinuxEnabled = isSELinuxEnforcing()
        state.hideBlEnabled = tamisu_daemonReadString("feature hide-bl").contains("enabled")
    }

    fun handleThemeModeChange(index: Int) {
        state.themeMode = index
        val newThemeMode = when (index) {
            0 -> null // 跟随系统
            1 -> false // 浅色
            2 -> true // 深色
            else -> null
        }
        context.saveThemeMode(newThemeMode)
        ThemeConfig.updateTheme(darkMode = newThemeMode)

        when (index) {
            2 -> { // 深色
                ThemeConfig.updateTheme(darkMode = true)
                CardConfig.updateThemePreference(darkMode = true, lightMode = false)
                CardConfig.setThemeDefaults(true)
                CardConfig.save(context)
            }
            1 -> { // 浅色
                ThemeConfig.updateTheme(darkMode = false)
                CardConfig.updateThemePreference(darkMode = false, lightMode = true)
                CardConfig.setThemeDefaults(false)
                CardConfig.save(context)
            }
            0 -> { // 跟随系统
                ThemeConfig.updateTheme(darkMode = null)
                CardConfig.updateThemePreference(darkMode = null, lightMode = null)
                val isNightModeActive = (context.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES
                CardConfig.setThemeDefaults(isNightModeActive)
                CardConfig.save(context)
            }
        }
    }

    fun handleDynamicColorChange(enabled: Boolean) {
        state.useDynamicColor = enabled
        context.saveDynamicColorState(enabled)
        ThemeConfig.updateTheme(dynamicColor = enabled)
    }

    @Composable
    fun getDpiFriendlyName(dpi: Int): String {
        return when (dpi) {
            240 -> stringResource(R.string.dpi_size_small)
            320 -> stringResource(R.string.dpi_size_medium)
            420 -> stringResource(R.string.dpi_size_large)
            560 -> stringResource(R.string.dpi_size_extra_large)
            else -> stringResource(R.string.dpi_size_custom)
        }
    }

    fun handleDpiApply() {
        if (state.tempDpi != state.currentDpi) {
            prefs.edit {
                putInt("app_dpi", state.tempDpi)
            }

            state.currentDpi = state.tempDpi
            Toast.makeText(
                context,
                context.getString(R.string.dpi_applied_success, state.tempDpi),
                Toast.LENGTH_SHORT
            ).show()

            val restartIntent = context.packageManager.getLaunchIntentForPackage(context.packageName)
            restartIntent?.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK or Intent.FLAG_ACTIVITY_NEW_TASK)
            context.startActivity(restartIntent)

            state.showDpiConfirmDialog = false
        }
    }

    fun handleSimpleModeChange(newValue: Boolean) {
        prefs.edit { putBoolean("is_simple_mode", newValue) }
        state.isSimpleMode = newValue
    }

    fun handleKernelSimpleModeChange(newValue: Boolean) {
        prefs.edit { putBoolean("is_kernel_simple_mode", newValue) }
        state.isKernelSimpleMode = newValue
    }

    fun handleSelinuxChange(enabled: Boolean) {
        val ok = ShellUtils.fastCmdResult(getRootShell(), if (enabled) "setenforce 1" else "setenforce 0")
        if (ok) {
            state.selinuxEnabled = enabled
            val msg = if (enabled) R.string.selinux_enabled_toast else R.string.selinux_disabled_toast
            Toast.makeText(context, context.getString(msg), Toast.LENGTH_SHORT).show()
        } else {
            Toast.makeText(context, context.getString(R.string.selinux_change_failed), Toast.LENGTH_SHORT).show()
        }
    }

    fun handleHideBlChange(enabled: Boolean) {
        val ok = execTamisuDaemon(if (enabled) "feature hide-bl enable" else "feature hide-bl disable")
        if (ok) {
            state.hideBlEnabled = enabled
            val msg = if (enabled) R.string.hide_bl_enabled_toast else R.string.hide_bl_disabled_toast
            Toast.makeText(context, context.getString(msg), Toast.LENGTH_SHORT).show()
        } else {
            Toast.makeText(context, context.getString(R.string.hide_bl_change_failed), Toast.LENGTH_SHORT).show()
        }
    }
}
