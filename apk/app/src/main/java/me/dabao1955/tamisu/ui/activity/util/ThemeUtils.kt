package me.dabao1955.tamisu.ui.activity.util

import android.content.Context
import android.database.ContentObserver
import android.os.Handler
import android.provider.Settings
import androidx.core.content.edit
import me.dabao1955.tamisu.ui.MainActivity
import me.dabao1955.tamisu.ui.theme.CardConfig

class ThemeChangeContentObserver(
    handler: Handler,
    private val onThemeChanged: () -> Unit
) : ContentObserver(handler) {
    override fun onChange(selfChange: Boolean) {
        super.onChange(selfChange)
        onThemeChanged()
    }
}

object ThemeUtils {

    fun initializeThemeSettings(activity: MainActivity) {
        val prefs = activity.getSharedPreferences("settings", Context.MODE_PRIVATE)
        val isFirstRun = prefs.getBoolean("is_first_run", true)

        if (isFirstRun) {
            prefs.edit { putBoolean("is_first_run", false) }
        }

        loadThemeMode()
        loadDynamicColorState()
        CardConfig.load(activity.applicationContext)
    }

    fun registerThemeChangeObserver(activity: MainActivity): ThemeChangeContentObserver {
        val contentObserver = ThemeChangeContentObserver(Handler(activity.mainLooper)) {
            activity.runOnUiThread {
                loadCustomBackground()
            }
        }

        activity.contentResolver.registerContentObserver(
            Settings.System.getUriFor("ui_night_mode"),
            false,
            contentObserver
        )

        return contentObserver
    }

    fun unregisterThemeChangeObserver(activity: MainActivity, observer: ThemeChangeContentObserver) {
        activity.contentResolver.unregisterContentObserver(observer)
    }

    fun onActivityPause(activity: MainActivity) {
        CardConfig.save(activity.applicationContext)
    }

    fun onActivityResume() {
    }

    @Suppress("unused")
    private fun loadThemeMode() {
    }

    @Suppress("unused")
    private fun loadDynamicColorState() {
    }

    @Suppress("unused", "UNUSED_PARAMETER")
    private fun loadCustomBackground() {
    }
}
