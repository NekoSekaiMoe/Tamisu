package me.dabao1955.tamisu.ui.webui

import android.os.Build
import android.os.Bundle
import android.webkit.WebView
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.runtime.*
import androidx.lifecycle.lifecycleScope
import com.dergoogler.mmrl.platform.Platform
import com.dergoogler.mmrl.platform.model.ModId
import com.dergoogler.mmrl.ui.component.Loading
import com.dergoogler.mmrl.webui.model.WebUIConfig
import com.dergoogler.mmrl.webui.screen.WebUIScreen
import com.dergoogler.mmrl.webui.util.rememberWebUIOptions
import me.dabao1955.tamisu.BuildConfig
import me.dabao1955.tamisu.ui.theme.KernelSUTheme
import me.dabao1955.tamisu.ui.util.setTaskDescriptionLabel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

class WebUIXActivity : ComponentActivity() {
    private lateinit var webView: WebView

    private val userAgent
        get(): String {
            val platform = Platform.get("Unknown") {
                platform.name
            }

            val platformVersion = Platform.get(-1) {
                moduleManager.versionCode
            }

            val osVersion = Build.VERSION.RELEASE
            val deviceModel = Build.MODEL

            return "Tamisu/${BuildConfig.VERSION_NAME} (Linux; Android $osVersion; $deviceModel; $platform/$platformVersion)"
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        webView = WebView(this)

        val moduleId = intent.getStringExtra("id")!!
        val name = intent.getStringExtra("name")!!
        setTaskDescriptionLabel("Tamisu - $name")

        val prefs = getSharedPreferences("settings", MODE_PRIVATE)

        setContent {
            KernelSUTheme {
                var isLoading by remember { mutableStateOf(true) }

                LaunchedEffect(Platform.isAlive) {
                    while (!Platform.isAlive) {
                        delay(1000)
                    }

                    isLoading = false
                }

                if (isLoading) {
                    Loading()
                    return@KernelSUTheme
                }

                val webDebugging = prefs.getBoolean("enable_web_debugging", false)
                val erudaInject = prefs.getBoolean("use_webuix_eruda", false)
                val dark = isSystemInDarkTheme()

                val options = rememberWebUIOptions(
                    modId = ModId(moduleId),
                    debug = webDebugging,
                    appVersionCode = BuildConfig.VERSION_CODE,
                    isDarkMode = dark,
                    enableEruda = erudaInject,
                    cls = WebUIXActivity::class.java,
                    userAgentString = userAgent
                )

                // idk why webuix not allow root impl change webuiConfig
                // so we use magic to force exitConfirm shutdown
                val field = WebUIConfig::class.java.getDeclaredField("exitConfirm")
                field.isAccessible = true
                field.set(options.config, false)
                field.isAccessible = false

                WebUIScreen(
                    webView = webView,
                    options = options,
                    interfaces = listOf(
                        WebViewInterface.factory()
                    )
                )
            }
        }
    }
}
