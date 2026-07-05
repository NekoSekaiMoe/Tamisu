package me.dabao1955.tamisu.ui.util

import android.net.Uri
import android.os.Environment
import android.os.Parcelable
import android.os.SystemClock
import android.system.Os
import android.util.Log
import com.topjohnwu.superuser.CallbackList
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ShellUtils
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.parcelize.Parcelize
import me.dabao1955.tamisu.BuildConfig
import me.dabao1955.tamisu.tamisu_paths.TamisuPaths
import me.dabao1955.tamisu.tamisuApp
import me.dabao1955.tamisu.utils.AssetsUtil
import com.topjohnwu.superuser.io.SuFile
import java.io.File
import java.util.Properties


/**
 * @date 2023/1/1.
 */
private const val TAG = "TamisuCli"

private fun getTamisuDaemonPath(): String {
    return tamisuApp.applicationInfo.nativeLibraryDir + File.separator + "libtamisu_daemon.so"
}

/**
 * Public function to get tamisu_daemon path for other modules
 */
fun getTamisuDaemon(): String = getTamisuDaemonPath()

object TamisuCli {
    var SHELL: Shell = createRootShell()
        private set
    var GLOBAL_MNT_SHELL: Shell = createRootShell(true)
        private set

    /**
     * Check if tamisu_daemon needs to be installed or updated.
     */
    private fun checkAndInstallTamisuDaemon() {
        try {
            val apkTamisuDaemonVersion = getApkTamisuDaemonVersion()
            val installedTamisuDaemonVersion = getInstalledTamisuDaemonVersion()

            Log.i(TAG, "checkAndInstallTamisuDaemon: apk=$apkTamisuDaemonVersion, installed=$installedTamisuDaemonVersion")

            // Install if: tamisu_daemon not installed, or version mismatch
            if (installedTamisuDaemonVersion == null || apkTamisuDaemonVersion != installedTamisuDaemonVersion) {
                Log.i(TAG, "Installing/updating tamisu_daemon daemon only: apk=$apkTamisuDaemonVersion, installed=$installedTamisuDaemonVersion")
                installOrUpdateTamisuDaemonDaemon()
            } else {
                Log.d(TAG, "tamisu_daemon is up-to-date: $installedTamisuDaemonVersion")
            }
        } catch (e: Exception) {
            Log.e(TAG, "checkAndInstallTamisuDaemon failed, falling back to install", e)
            // Fallback: always try to sync tamisu_daemon daemon on error
            installOrUpdateTamisuDaemonDaemon()
        }
    }

    /**
     * The APK-bundled tamisu_daemon version is pinned at build time by
     * apk/build.gradle.kts (`computeTamisuDaemonBundledVersion`), which mirrors
     * daemon/tamisud_core/scripts/generate_version.py. No need to fork-exec the
     * daemon just to read its version.
     */
    private fun getApkTamisuDaemonVersion(): String? =
        BuildConfig.TAMISU_DAEMON_BUNDLED_VERSION.takeIf { it.isNotBlank() }

    /**
     * Normalize tamisu_daemon version string to app-style display: vx.x.x-xxxxxxxx.
     * e.g. "1.3.0-1-g56b0efb0" -> "v1.3.0-56b0efb0"
     */
    fun formatTamisuDaemonVersionForDisplay(raw: String?): String? {
        if (raw.isNullOrBlank()) return null
        val s = raw.trim().removePrefix("v")
        // Match x.x.x optionally followed by -anything; capture semver and trailing alphanumeric for 8-char
        val semverMatch = Regex("""^(\d+\.\d+\.\d+)""").find(s) ?: return "v$s"
        val semver = semverMatch.value
        val rest = s.drop(semver.length).trimStart('-')
        val describeHash = Regex("""(?:^|-)g([a-fA-F0-9]{7,40})""")
            .find(rest)
            ?.groupValues
            ?.get(1)
        val hashPart = describeHash
            ?: Regex("""[a-fA-F0-9]{7,40}""").find(rest)?.value
        val suffix = when {
            hashPart != null -> hashPart.take(8)
            rest.isNotEmpty() -> rest.filter { it.isLetterOrDigit() }.take(8)
            else -> ""
        }
        return if (suffix.isNotEmpty()) "v$semver-$suffix" else "v$semver"
    }

    /**
     * Get installed tamisu_daemon version from /data/tamisu_daemon.
     */
    private fun getInstalledTamisuDaemonVersion(): String? {
        return try {
            val result = ShellUtils.fastCmd(SHELL, "${TamisuPaths.TAMISU_DAEMON_BIN} version 2>/dev/null")
            if (result.isBlank()) return null
            val match = Regex("""version\s+([^\s]+)""").find(result)
            match?.groupValues?.get(1)
        } catch (e: Exception) {
            Log.w(TAG, "Failed to get installed tamisu_daemon version", e)
            null
        }
    }

    /**
     * Public helper for UI: get tamisu_daemon versions (APK-bundled and installed daemon),
     * formatted as vx.x.x-xxxxxxxx. Returns (formattedApk, formattedInstalled).
     */
    suspend fun getTamisuDaemonVersionsForUi(): Pair<String?, String?> = withContext(Dispatchers.IO) {
        val apk = getApkTamisuDaemonVersion()
        val installed = getInstalledTamisuDaemonVersion()
        formatTamisuDaemonVersionForDisplay(apk) to formatTamisuDaemonVersionForDisplay(installed)
    }

    /**
     * Public helper for UI: sync tamisu_daemon daemon binary from APK into /data/tamisu_daemon.
     */
    suspend fun updateKsudDaemonForUi(): Boolean = withContext(Dispatchers.IO) {
        installOrUpdateTamisuDaemonDaemon()
        true
    }

    /**
     * Cold-start auto-sync entry point: only runs the version-aware install
     * path when we actually have a root shell. Cheap to call from
     * MainActivity's first LaunchedEffect.
     */
    suspend fun autoSyncKsudIfNeeded(): Unit = withContext(Dispatchers.IO) {
        if (!SHELL.isRoot) {
            Log.d(TAG, "autoSyncKsudIfNeeded: no root shell, skip")
            return@withContext
        }
        checkAndInstallTamisuDaemon()
    }

    /**
     * Install or update the tamisu_daemon daemon binary itself, without touching boot image.
     *
     * This mirrors APatch's "安装/升级系统补丁(apd)" flow:
     * we copy the manager-bundled tamisu_daemon ELF (`libtamisu_daemon.so`) into:
     *   - `/data/tamisu_daemon` (daemon binary used by core/su wrapper)
     *   - `/data/tamisu/bin/tamisu_daemon` (symlink for convenience/tools)
     */
    private fun installOrUpdateTamisuDaemonDaemon() {
        val shell = getRootShell()
        if (!shell.isRoot) {
            Log.w(TAG, "installOrUpdateTamisuDaemonDaemon: shell is not root, skip")
            return
        }

        val nativeDir = tamisuApp.applicationInfo.nativeLibraryDir
        val tamisu_daemonSo = File(nativeDir, "libtamisu_daemon.so")
        if (!tamisu_daemonSo.exists()) {
            Log.e(TAG, "installOrUpdateTamisuDaemonDaemon: libtamisu_daemon.so not found in $nativeDir")
            return
        }

        val cmds = arrayOf(
            // Ensure directories
            "mkdir -p ${TamisuPaths.TAMISU_BIN_DIR}",
            "mkdir -p ${TamisuPaths.TAMISU_LOG_DIR}",
            // Copy new daemon binary (multi-call: tamisu_daemon + magiskboot via argv0)
            "cp -f ${tamisu_daemonSo.absolutePath} ${TamisuPaths.TAMISU_DAEMON_BIN}",
            "chmod 0755 ${TamisuPaths.TAMISU_DAEMON_BIN}",
            // Symlinks in tamisu/bin: tamisu_daemon, magiskboot, bootctl, resetprop all point to the same binary (multi-call)
            "ln -sf ${TamisuPaths.TAMISU_DAEMON_BIN} ${TamisuPaths.TAMISU_BIN_DIR}/tamisu_daemon",
            "ln -sf ${TamisuPaths.TAMISU_DAEMON_BIN} ${TamisuPaths.TAMISU_BIN_DIR}/magiskboot",
            "ln -sf ${TamisuPaths.TAMISU_DAEMON_BIN} ${TamisuPaths.TAMISU_BIN_DIR}/bootctl",
            "ln -sf ${TamisuPaths.TAMISU_DAEMON_BIN} ${TamisuPaths.TAMISU_BIN_DIR}/resetprop",
            // Fix SELinux contexts (ignore errors on non-SEAndroid systems)
            "restorecon ${TamisuPaths.TAMISU_DAEMON_BIN} || true",
            "restorecon -R ${TamisuPaths.TAMISU_ROOT} || true"
        )

        Log.i(TAG, "installOrUpdateTamisuDaemonDaemon: syncing ${tamisu_daemonSo.absolutePath} -> /data/tamisu_daemon")
        val result = shell.newJob().add(*cmds).exec()
        Log.i(TAG, "installOrUpdateTamisuDaemonDaemon: result code=${result.code}, isSuccess=${result.isSuccess}")
    }
}

fun getRootShell(globalMnt: Boolean = false): Shell {
    return if (globalMnt) TamisuCli.GLOBAL_MNT_SHELL else {
        TamisuCli.SHELL
    }
}

inline fun <T> withNewRootShell(
    globalMnt: Boolean = false,
    block: Shell.() -> T
): T {
    return createRootShell(globalMnt).use(block)
}

fun createRootShell(globalMnt: Boolean = false): Shell {
    Shell.enableVerboseLogging = BuildConfig.DEBUG
    val builder = Shell.Builder.create()
    return try {
        val shell = if (globalMnt) {
            builder.build(getTamisuDaemonPath(), "debug", "su", "-g")
        } else {
            builder.build(getTamisuDaemonPath(), "debug", "su")
        }
        Log.d(TAG, "tamisu_daemon shell created, isRoot=${shell.isRoot}, globalMnt=$globalMnt")
        shell
    } catch (e: Throwable) {
        Log.w(TAG, "tamisu failed (globalMnt=$globalMnt): ", e)
        try {
            val shell = if (globalMnt) {
                builder.build("su", "-mm")
            } else {
                builder.build("su")
            }
            Log.d(TAG, "su shell created, isRoot=${shell.isRoot}, globalMnt=$globalMnt")
            shell
        } catch (e: Throwable) {
            Log.e(TAG, "su failed (globalMnt=$globalMnt): ", e)
            val shell = builder.build("sh")
            Log.w(TAG, "fallback to sh, isRoot=${shell.isRoot}")
            shell
        }
    }
}

/** Build a "tamisu_daemon <args>" command string. Use this instead of pasting
 *  `${getTamisuDaemonPath()}` next to a subcommand literal. */
internal fun tamisu_daemonCmd(args: String): String = "${getTamisuDaemonPath()} $args"

fun execTamisuDaemon(args: String, newShell: Boolean = false): Boolean {
    return if (newShell) {
        withNewRootShell {
            ShellUtils.fastCmdResult(this, tamisu_daemonCmd(args))
        }
    } else {
        ShellUtils.fastCmdResult(getRootShell(), tamisu_daemonCmd(args))
    }
}

/** Run a tamisu_daemon subcommand and return its trimmed stdout (single value). */
internal fun tamisu_daemonReadString(args: String, shell: Shell = getRootShell()): String =
    ShellUtils.fastCmd(shell, tamisu_daemonCmd(args)).trim()

/** Run a tamisu_daemon subcommand and return non-blank trimmed stdout lines. */
internal fun tamisu_daemonReadLines(args: String, shell: Shell = getRootShell()): List<String> =
    shell.newJob().add(tamisu_daemonCmd(args)).to(ArrayList(), null).exec().out
        .filter { it.isNotBlank() }.map { it.trim() }

suspend fun getFeatureStatus(feature: String): String = withContext(Dispatchers.IO) {
    tamisu_daemonReadLines("feature check $feature")
        .firstOrNull { it == "supported" || it == "unsupported" || it == "managed" }
        .orEmpty()
}

/** Read a feature's current on/off value via `tamisu_daemon feature get` (parses the
 *  "Status: enabled/disabled" line). Returns false when unsupported. */
suspend fun getFeatureValue(feature: String): Boolean = withContext(Dispatchers.IO) {
    tamisu_daemonReadLines("feature get $feature").any { it.equals("Status: enabled", ignoreCase = true) }
}

/** Set a feature value and persist it; returns whether tamisu_daemon reported success. */
suspend fun setFeatureValue(feature: String, enabled: Boolean): Boolean =
    withContext(Dispatchers.IO) {
        execTamisuDaemon("feature set $feature ${if (enabled) 1 else 0}", true) &&
            execTamisuDaemon("feature save", true)
    }

fun install() {
    val start = SystemClock.elapsedRealtime()
    val tamisu_daemonPath = getTamisuDaemonPath()
    // magiskboot is built into tamisu_daemon (multi-call binary); pass tamisu_daemon path so it can exec itself as magiskboot
    Log.i(TAG, "install: tamisu_daemon=$tamisu_daemonPath")
    val result = execTamisuDaemon("install --magiskboot $tamisu_daemonPath", true)
    Log.w(TAG, "install result: $result, cost: ${SystemClock.elapsedRealtime() - start}ms")
}

private fun flashWithIO(
    cmd: String,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit
): Shell.Result {

    val stdoutCallback: CallbackList<String?> = object : CallbackList<String?>() {
        override fun onAddElement(s: String?) {
            onStdout(s ?: "")
        }
    }

    val stderrCallback: CallbackList<String?> = object : CallbackList<String?>() {
        override fun onAddElement(s: String?) {
            onStderr(s ?: "")
        }
    }

    // Set TMPDIR to app cache directory so tamisu_daemon can create temp files without root
    val tmpDir = tamisuApp.cacheDir.absolutePath
    val cmdWithEnv = "TMPDIR=$tmpDir $cmd"

    return withNewRootShell {
        newJob().add(cmdWithEnv).to(stdoutCallback, stderrCallback).exec()
    }
}

fun restoreBoot(
    onFinish: (Boolean, Int) -> Unit, onStdout: (String) -> Unit, onStderr: (String) -> Unit
): Boolean {
    val tamisu_daemonPath = getTamisuDaemonPath()
    val result = flashWithIO(
        tamisu_daemonCmd("boot-restore -f --magiskboot $tamisu_daemonPath"),
        onStdout,
        onStderr
    )
    onFinish(result.isSuccess, result.code)
    return result.isSuccess
}

fun uninstallPermanently(
    onFinish: (Boolean, Int) -> Unit, onStdout: (String) -> Unit, onStderr: (String) -> Unit
): Boolean {
    val tamisu_daemonPath = getTamisuDaemonPath()
    val result =
        flashWithIO(tamisu_daemonCmd("uninstall --magiskboot $tamisu_daemonPath"), onStdout, onStderr)
    onFinish(result.isSuccess, result.code)
    return result.isSuccess
}

@Parcelize
sealed class LkmSelection : Parcelable {
    data class LkmUri(val uri: Uri) : LkmSelection()
    data class KmiString(val value: String) : LkmSelection()
    data object KmiNone : LkmSelection()
}

fun installBoot(
    bootUri: Uri?,
    lkm: LkmSelection,
    ota: Boolean,
    partition: String?,
    enableAdb: Boolean = false,
    kasumiInCpio: Boolean = false,  // Experimental: embed Kasumi LKM in cpio, load after Tamisu
    kasumiLkmUri: Uri? = null,      // Custom Kasumi LKM file; when null, use embedded
    onFinish: (Boolean, Int) -> Unit,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit,
): Boolean {
    val resolver = tamisuApp.contentResolver

    val bootFile = bootUri?.let { uri ->
        with(resolver.openInputStream(uri)) {
            val bootFile = File(tamisuApp.cacheDir, "boot.img")
            bootFile.outputStream().use { output ->
                this?.copyTo(output)
            }

            bootFile
        }
    }

    val tamisu_daemonPath = getTamisuDaemonPath()
    var cmd = "boot-patch --magiskboot $tamisu_daemonPath"

    // Extract kernel-version-specific mkbootfs tools for 5.10/5.15+ ramdisk format compatibility
    val mkbootfsDir = File(tamisuApp.cacheDir, "mkbootfs").apply { mkdirs() }
    try {
        AssetsUtil.exportFiles(tamisuApp, "5_10-mkbootfs", File(mkbootfsDir, "5_10-mkbootfs").absolutePath)
        AssetsUtil.exportFiles(tamisuApp, "5_15+-mkbootfs", File(mkbootfsDir, "5_15+-mkbootfs").absolutePath)
        File(mkbootfsDir, "5_10-mkbootfs").setExecutable(true, false)
        File(mkbootfsDir, "5_15+-mkbootfs").setExecutable(true, false)
        cmd += " --mkbootfs-dir ${mkbootfsDir.absolutePath}"
    } catch (e: Exception) {
        Log.w(TAG, "Failed to extract mkbootfs assets, using built-in magiskboot only", e)
    }

    cmd += if (bootFile == null) {
        // no boot.img, use -f to force install
        " -f"
    } else {
        " -b ${bootFile.absolutePath}"
    }

    if (ota) {
        cmd += " -u"
    }

    var lkmFile: File? = null
    when (lkm) {
        is LkmSelection.LkmUri -> {
            lkmFile = with(resolver.openInputStream(lkm.uri)) {
                val file = File(tamisuApp.cacheDir, "tamisu-tmp-lkm.ko")
                file.outputStream().use { output ->
                    this?.copyTo(output)
                }

                file
            }
            cmd += " -m ${lkmFile.absolutePath}"
        }

        is LkmSelection.KmiString -> {
            cmd += " --kmi ${lkm.value}"
        }

        LkmSelection.KmiNone -> {
            // do nothing
        }
    }

    // output dir
    val downloadsDir =
        Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
    cmd += " -o $downloadsDir"

    partition?.let { part ->
        cmd += " --partition $part"
    }

    if (enableAdb) {
        cmd += " --enable-adbd"
    }

    var kasumiFile: File? = null
    if (kasumiInCpio) {
        cmd += " --kasumi"
        kasumiLkmUri?.let { uri ->
            val file = with(resolver.openInputStream(uri)) {
                val f = File(tamisuApp.cacheDir, "kasumi-tmp-lkm.ko")
                f.outputStream().use { output ->
                    this?.copyTo(output)
                }
                f
            }
            kasumiFile = file
            cmd += " --kasumi-module ${file.absolutePath}"
        }
    }

    val result = flashWithIO(tamisu_daemonCmd(cmd), onStdout, onStderr)
    Log.i("Tamisu", "install boot result: ${result.isSuccess}")

    bootFile?.delete()
    lkmFile?.delete()
    kasumiFile?.delete()

    // if boot uri is empty, it is direct install, when success, we should show reboot button
    onFinish(bootUri == null && result.isSuccess, result.code)

    if (bootUri == null && result.isSuccess) {
        install()
    }

    return result.isSuccess
}

fun reboot(reason: String = "") {
    val shell = getRootShell()
    if (reason == "soft_reboot") {
        ShellUtils.fastCmd(shell, "setprop ctl.restart zygote")
        return
    }
    if (reason == "recovery") {
        // KEYCODE_POWER = 26, hide incorrect "Factory data reset" message
        ShellUtils.fastCmd(shell, "/system/bin/input keyevent 26")
    }
    ShellUtils.fastCmd(shell, "/system/bin/svc power reboot $reason || /system/bin/reboot $reason")
}

fun rootAvailable(): Boolean {
    val shell = getRootShell()
    return shell.isRoot
}


suspend fun getCurrentKmi(): String = withContext(Dispatchers.IO) {
    tamisu_daemonReadString("boot-info current-kmi")
}

suspend fun getSupportedKmis(): List<String> = withContext(Dispatchers.IO) {
    tamisu_daemonReadLines("boot-info supported-kmis")
}

suspend fun isAbDevice(): Boolean = withContext(Dispatchers.IO) {
    tamisu_daemonReadString("boot-info is-ab-device").toBoolean()
}

suspend fun getDefaultPartition(): String = withContext(Dispatchers.IO) {
    if (getRootShell().isRoot) {
        tamisu_daemonReadString("boot-info default-partition")
    } else {
        if (!Os.uname().release.contains("android12-")) "init_boot" else "boot"
    }
}

suspend fun getSlotSuffix(ota: Boolean): String = withContext(Dispatchers.IO) {
    val args = if (ota) "boot-info slot-suffix --ota" else "boot-info slot-suffix"
    tamisu_daemonReadString(args)
}

suspend fun getAvailablePartitions(): List<String> = withContext(Dispatchers.IO) {
    tamisu_daemonReadLines("boot-info available-partitions")
}

fun hasMagisk(): Boolean {
    val shell = getRootShell(true)
    val result = shell.newJob().add("which magisk").exec()
    Log.i(TAG, "has magisk: ${result.isSuccess}")
    return result.isSuccess
}

fun runCmd(shell: Shell, cmd: String): String {
    return shell.newJob()
        .add(cmd)
        .to(mutableListOf<String>(), null)
        .exec().out
        .joinToString("\n")
}


/** Module IDs of known third-party Zygisk implementations ("zygisksu" covers
 *  both ZygiskNext and NeoZygisk -- they share that id). Built-in Tamisu Zygisk is
 *  a kernel feature (detected via its flag), not a module, so it's not here.
 *  These are the implementations Tamisu Zygisk force-disables to avoid conflicts. */
val ZYGISK_IMPL_MODULE_IDS = listOf("zygisksu", "rezygisk")

suspend fun getZygiskImplement(): String = withContext(Dispatchers.IO) {
    // Built-in Tamisu Zygisk wins: it's a kernel feature, not a /data/adb module.
    if (getFeatureValue("zygisk")) return@withContext "Tamisu Zygisk"

    for (moduleId in ZYGISK_IMPL_MODULE_IDS) {
        // skip disabled / pending-removal modules
        if (SuFile.open("/data/adb/modules/$moduleId/disable").isFile || SuFile.open("/data/adb/modules/$moduleId/remove").isFile) continue

        val propFile = SuFile.open("/data/adb/modules/$moduleId/module.prop")
        if (!propFile.isFile) continue

        val prop = Properties()
        prop.load(propFile.newInputStream())

        val name = prop.getProperty("name")
        Log.i(TAG, "Zygisk implement: $name")
        return@withContext name
    }

    Log.i(TAG, "Zygisk implement: None")
    "None"
}

