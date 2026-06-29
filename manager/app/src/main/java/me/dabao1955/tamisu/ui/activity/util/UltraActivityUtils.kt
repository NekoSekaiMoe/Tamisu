package me.dabao1955.tamisu.ui.activity.util

import android.content.Context
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.runtime.Composable
import androidx.lifecycle.LifecycleCoroutineScope
import me.dabao1955.tamisu.Natives
import me.dabao1955.tamisu.ui.util.rootAvailable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

object AnimatedBottomBar {
    @Composable
    fun AnimatedBottomBarWrapper(
        showBottomBar: Boolean,
        content: @Composable () -> Unit
    ) {
        AnimatedVisibility(
            visible = showBottomBar,
            enter = slideInVertically(initialOffsetY = { it }) + fadeIn(),
            exit = slideOutVertically(targetOffsetY = { it }) + fadeOut()
        ) {
            content()
        }
    }
}

object AppData {
    object DataRefreshManager {
        private val _isFullFeatured = MutableStateFlow(false)

        val isFullFeatured: StateFlow<Boolean> = _isFullFeatured.asStateFlow()

        fun refreshData() {
            val ff = isFullFeatured()
            if (_isFullFeatured.value != ff) _isFullFeatured.value = ff
        }
    }

    fun isFullFeatured(): Boolean {
        val isManager = Natives.isManager
        return isManager && !Natives.requireNewKernel() && rootAvailable()
    }
}

object DataRefreshUtils {
    fun startDataRefreshCoroutine(scope: LifecycleCoroutineScope) {
        scope.launch(Dispatchers.IO) {
            while (isActive) {
                AppData.DataRefreshManager.refreshData()
                delay(10000) // 10s，减少轮询频率
            }
        }
    }

    fun refreshData(scope: LifecycleCoroutineScope) {
        scope.launch {
            AppData.DataRefreshManager.refreshData()
        }
    }
}

object DisplayUtils {
    fun applyCustomDpi(context: Context) {
        val prefs = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
        val customDpi = prefs.getInt("app_dpi", 0)

        if (customDpi > 0) {
            try {
                val resources = context.resources
                val metrics = resources.displayMetrics
                metrics.density = customDpi / 160f
                @Suppress("DEPRECATION")
                metrics.scaledDensity = customDpi / 160f
                metrics.densityDpi = customDpi
            } catch (e: Exception) {
                e.printStackTrace()
            }
        }
    }
}

