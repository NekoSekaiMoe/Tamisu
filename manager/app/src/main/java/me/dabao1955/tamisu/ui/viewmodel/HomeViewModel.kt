package me.dabao1955.tamisu.ui.viewmodel

import android.content.Context
import android.os.Build
import android.system.Os
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import me.dabao1955.tamisu.KernelVersion
import me.dabao1955.tamisu.Natives
import me.dabao1955.tamisu.getKernelVersion
import me.dabao1955.tamisu.tamisuApp
import me.dabao1955.tamisu.ui.util.*
import me.dabao1955.tamisu.ui.util.module.LatestVersionInfo
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

class HomeViewModel : ViewModel() {

    data class SystemStatus(
        val isManager: Boolean = false,
        val ksuVersion: Int? = null,
        val ksuFullVersion : String? = null,
        val kernelVersion: KernelVersion = getKernelVersion(),
        val isRootAvailable: Boolean = false,
        val requireNewKernel: Boolean = false,
        val kernelUapiVersion: Int = 0,
        val managerUapiVersion: Int = 0
    )

    data class SystemInfo(
        val kernelRelease: String = "",
        val androidVersion: String = "",
        val deviceModel: String = "",
        val managerVersion: Pair<String, Long> = Pair("", 0L),
        val seLinuxStatus: String = "",
        val zygiskImplement: String = "",
        // Manager process seccomp mode: -1 unsupported, 0 disabled, 1 strict,
        // 2 filter (mirrors upstream's prctl(PR_GET_SECCOMP)).
        val seccompStatus: Int = 2
    )

    var systemStatus by mutableStateOf(SystemStatus())
        private set

    var systemInfo by mutableStateOf(SystemInfo())
        private set

    var latestVersionInfo by mutableStateOf(LatestVersionInfo())
        private set

    var isSimpleMode by mutableStateOf(false)
        private set
    var isKernelSimpleMode by mutableStateOf(false)
        private set


    var isCoreDataLoaded by mutableStateOf(false)
        private set
    var isExtendedDataLoaded by mutableStateOf(false)
        private set
    var isRefreshing by mutableStateOf(false)
        private set

    private val _dataRefreshTrigger = MutableStateFlow(0L)
    val dataRefreshTrigger: StateFlow<Long> = _dataRefreshTrigger

    private var loadingJobs = mutableListOf<Job>()
    private var lastRefreshTime = 0L
    private val refreshCooldown = 2000L

    fun loadUserSettings(context: Context) {
        viewModelScope.launch(Dispatchers.IO) {
            val settingsPrefs = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
            isSimpleMode = settingsPrefs.getBoolean("is_simple_mode", false)
            isKernelSimpleMode = settingsPrefs.getBoolean("is_kernel_simple_mode", false)
        }
    }

    fun loadCoreData() {
        if (isCoreDataLoaded) return

        val job = viewModelScope.launch(Dispatchers.IO) {
            try {
                val kernelVersion = getKernelVersion()
                val isManager = try {
                    Natives.isManager
                } catch (_: Exception) {
                    false
                }

                val ksuVersion = if (isManager) Natives.version else null

                val fullVersion = try {
                    Natives.getFullVersion()
                } catch (_: Exception) {
                    "Unknown"
                }

                val ksuFullVersion = if (isKernelSimpleMode) {
                    try {
                        val startIndex = fullVersion.indexOf('v')
                        if (startIndex >= 0) {
                            val endIndex = fullVersion.indexOf('-', startIndex)
                            val versionStr = if (endIndex > startIndex) {
                                fullVersion.substring(startIndex, endIndex)
                            } else {
                                fullVersion.substring(startIndex)
                            }
                            val numericVersion = "v" + (Regex("""\d+(\.\d+)*""").find(versionStr)?.value ?: versionStr)
                            numericVersion
                        } else {
                            fullVersion
                        }
                    } catch (_: Exception) {
                        fullVersion
                    }
                } else {
                    fullVersion
                }

                val isRootAvailable = try {
                    rootAvailable()
                } catch (_: Exception) {
                    false
                }

                val requireNewKernel = try {
                    isManager && Natives.requireNewKernel()
                } catch (_: Exception) {
                    false
                }

                val kernelUapiVersion = if (isManager) {
                    try { Natives.getUapiVersion() } catch (_: Exception) { 0 }
                } else 0

                val managerUapiVersion = try {
                    Natives.getManagerUapiVersion()
                } catch (_: Exception) { 0 }

                systemStatus = SystemStatus(
                    isManager = isManager,
                    ksuVersion = ksuVersion,
                    ksuFullVersion = ksuFullVersion,
                    kernelVersion = kernelVersion,
                    isRootAvailable = isRootAvailable,
                    requireNewKernel = requireNewKernel,
                    kernelUapiVersion = kernelUapiVersion,
                    managerUapiVersion = managerUapiVersion
                )

                isCoreDataLoaded = true
            } catch (_: Exception) {
            }
        }
        loadingJobs.add(job)
    }

    fun loadExtendedData(context: Context) {
        if (isExtendedDataLoaded) return

        val job = viewModelScope.launch(Dispatchers.IO) {
            try {
                delay(50)

                val basicInfo = loadBasicSystemInfo(context)
                systemInfo = systemInfo.copy(
                    kernelRelease = basicInfo.first,
                    androidVersion = basicInfo.second,
                    deviceModel = basicInfo.third,
                    managerVersion = basicInfo.fourth,
                    seLinuxStatus = basicInfo.fifth,
                    seccompStatus = readSeccompStatus()
                )

                delay(100)

                if (!isSimpleMode) {
                    val zygiskImplement = try {
                        getZygiskImplement()
                    } catch (_: Exception) {
                        "None"
                    }
                    systemInfo = systemInfo.copy(
                        zygiskImplement = zygiskImplement
                    )
                }

                delay(100)

                isExtendedDataLoaded = true
            } catch (_: Exception) {
            }
        }
        loadingJobs.add(job)
    }

    fun refreshData(context: Context, forceRefresh: Boolean = false) {
        val currentTime = System.currentTimeMillis()

        if (!forceRefresh && currentTime - lastRefreshTime < refreshCooldown) {
            return
        }

        lastRefreshTime = currentTime

        viewModelScope.launch {
            isRefreshing = true

            try {
                loadingJobs.forEach { it.cancel() }
                loadingJobs.clear()

                isCoreDataLoaded = false
                isExtendedDataLoaded = false

                _dataRefreshTrigger.value = currentTime

                loadUserSettings(context)

                loadCoreData()
                delay(100)

                loadExtendedData(context)

                val settingsPrefs = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
                val checkUpdate = settingsPrefs.getBoolean("check_update", true)
                if (checkUpdate) {
                    try {
                        val newVersionInfo = withContext(Dispatchers.IO) {
                            checkNewVersion()
                        }
                        latestVersionInfo = newVersionInfo
                    } catch (_: Exception) {
                    }
                }
            } catch (_: Exception) {
            } finally {
                isRefreshing = false
            }
        }
    }

    fun onPullRefresh(context: Context) {
        refreshData(context, forceRefresh = true)
    }

    fun autoRefreshIfNeeded(context: Context) {
        viewModelScope.launch {
            val needsRefresh = checkIfDataNeedsRefresh()
            if (needsRefresh) {
                refreshData(context)
            }
        }
    }

    private suspend fun checkIfDataNeedsRefresh(): Boolean {
        return withContext(Dispatchers.IO) {
            try {
                val currentKsuVersion = try {
                    if (Natives.isManager) {
                        Natives.version
                    } else null
                } catch (_: Exception) {
                    null
                }

                if (currentKsuVersion != systemStatus.ksuVersion) {
                    return@withContext true
                }

                false
            } catch (_: Exception) {
                false
            }
        }
    }

    // Reads the manager process seccomp mode from /proc/self/status.
    // -1 = unsupported (no Seccomp line), 0 = disabled, 1 = strict, 2 = filter.
    private fun readSeccompStatus(): Int = runCatching {
        java.io.File("/proc/self/status").useLines { lines ->
            lines.firstOrNull { it.startsWith("Seccomp:") }
                ?.substringAfter(':')?.trim()?.toIntOrNull() ?: -1
        }
    }.getOrDefault(-1)

    private suspend fun loadBasicSystemInfo(context: Context): Tuple5<String, String, String, Pair<String, Long>, String> {
        return withContext(Dispatchers.IO) {
            val uname = try {
                Os.uname()
            } catch (_: Exception) {
                null
            }

            val deviceModel = try {
                resolveDeviceName()
            } catch (_: Exception) {
                "Unknown"
            }

            val managerVersion = try {
                getManagerVersion(context)
            } catch (_: Exception) {
                Pair("Unknown", 0L)
            }

            val seLinuxStatus = try {
                getSELinuxStatus(tamisuApp.applicationContext)
            } catch (_: Exception) {
                "Unknown"
            }

            Tuple5(
                uname?.release ?: "Unknown",
                Build.VERSION.RELEASE ?: "Unknown",
                deviceModel,
                managerVersion,
                seLinuxStatus
            )
        }
    }

    private fun getManagerVersion(context: Context): Pair<String, Long> {
        return try {
            val packageInfo = context.packageManager.getPackageInfo(context.packageName, 0)
            val versionCode = androidx.core.content.pm.PackageInfoCompat.getLongVersionCode(packageInfo)
            val versionName = packageInfo.versionName ?: "Unknown"
            Pair(versionName, versionCode)
        } catch (_: Exception) {
            Pair("Unknown", 0L)
        }
    }

    data class Tuple5<T1, T2, T3, T4, T5>(
        val first: T1,
        val second: T2,
        val third: T3,
        val fourth: T4,
        val fifth: T5
    )

    override fun onCleared() {
        super.onCleared()
        loadingJobs.forEach { it.cancel() }
        loadingJobs.clear()
    }
}
