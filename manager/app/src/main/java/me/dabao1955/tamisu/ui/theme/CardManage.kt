package me.dabao1955.tamisu.ui.theme

import android.content.Context
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.material3.CardColors
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CardElevation
import androidx.compose.ui.graphics.Color

@Stable
object CardConfig {
    var isUserDarkModeEnabled by mutableStateOf(false)
        internal set
    var isUserLightModeEnabled by mutableStateOf(false)
        internal set

    fun updateThemePreference(darkMode: Boolean?, lightMode: Boolean?) {
        isUserDarkModeEnabled = darkMode ?: false
        isUserLightModeEnabled = lightMode ?: false
    }

    fun reset() {
        isUserDarkModeEnabled = false
        isUserLightModeEnabled = false
    }

    fun setThemeDefaults(isDarkMode: Boolean) {
    }

    fun save(context: Context) {
    }

    fun load(context: Context) {
    }
}

object CardStyleProvider {

    @Stable
    @Composable
    fun getCardColors(originalColor: Color): CardColors =
        CardDefaults.cardColors(containerColor = originalColor)

    @Stable
    @Composable
    fun getCardElevation(): CardElevation = CardDefaults.cardElevation()
}

// 向后兼容
@Composable
fun getCardColors(originalColor: Color): CardColors =
    CardStyleProvider.getCardColors(originalColor)

@Composable
fun getCardElevation(): CardElevation = CardStyleProvider.getCardElevation()
