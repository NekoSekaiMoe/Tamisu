package ui.screen.zygisk

import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.Article
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Switch
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import coil.compose.AsyncImage
import coil.request.ImageRequest
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import me.dabao1955.tamisu.Natives
import me.dabao1955.tamisu.R
import me.dabao1955.tamisu.ui.util.execKsud
import me.dabao1955.tamisu.ui.util.getRootShell
import me.dabao1955.tamisu.ui.util.withNewRootShell
import com.topjohnwu.superuser.ShellUtils
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import ui.screen.moreSettings.component.SettingsCard
import ui.screen.moreSettings.component.SwitchSettingItem

private const val TAMISU_CONFIG_DIR = "/data/adb/ksu/tamisu"
private const val TAMISU_CONFIG_PATH = "$TAMISU_CONFIG_DIR/tamisu_config.json"

/** Mirrors uapi/tamisu.h tamisu_config + the tamisu_config.json schema.
 *  zygoteModules: when grantFilterActive is true, only modules whose id is in
 *  this list are loaded into zygote. grantFilterActive=false (the
 *  "zygote_modules" key absent) means load everything (back-compat). */
data class TamisuConfig(
    val yukilinker: Boolean = false,
    val denylistMode: Int = 0, // 0=off, 1=force-umount+no-inject, 2=inject+umount
    val dmesgLog: Boolean = false,
    val grantFilterActive: Boolean = false,
    val zygoteModules: List<String> = emptyList(),
)

private suspend fun readTamisuConfig(): TamisuConfig = withContext(Dispatchers.IO) {
    val raw = ShellUtils.fastCmd(getRootShell(), "cat $TAMISU_CONFIG_PATH 2>/dev/null")
    if (raw.isNullOrBlank()) return@withContext TamisuConfig()
    try {
        val o = JSONObject(raw)
        val hasGrant = o.has("zygote_modules")
        val granted = if (hasGrant) {
            o.optJSONArray("zygote_modules")?.let { a ->
                (0 until a.length()).map { a.getString(it) }
            } ?: emptyList()
        } else emptyList()
        TamisuConfig(
            yukilinker = o.optBoolean("yukilinker", false),
            denylistMode = o.optInt("denylist_mode", 0),
            dmesgLog = o.optBoolean("dmesg_log", false),
            grantFilterActive = hasGrant,
            zygoteModules = granted,
        )
    } catch (_: Exception) {
        TamisuConfig()
    }
}

/** Write tamisu_config.json then fire a netlink reload so it applies immediately. */
private suspend fun writeTamisuConfig(cfg: TamisuConfig) = withContext(Dispatchers.IO) {
    val json = JSONObject().apply {
        put("yukilinker", cfg.yukilinker)
        put("denylist_mode", cfg.denylistMode)
        put("dmesg_log", cfg.dmesgLog)
        if (cfg.grantFilterActive) {
            put("zygote_modules", JSONArray(cfg.zygoteModules))
        }
    }.toString()
    withNewRootShell {
        newJob().add("mkdir -p $TAMISU_CONFIG_DIR").exec()
        newJob().add("echo '$json' > $TAMISU_CONFIG_PATH").exec()
    }
    // Fires KSU_IOCTL_TAMISU_RELOAD -> kernel multicasts TAMISU_EV_RELOAD -> zygiskd
    // re-reads the file; takes effect on the next specialize, no reboot.
    execKsud("tamisu reload")
}

/** One injected app for the recent list: appid + resolved package identity. */
private data class RecentApp(
    val uid: Int,
    val label: String,             // "" when the package can't be resolved
    val packageName: String?,      // null when unresolved
    val packageInfo: PackageInfo?, // feeds Coil for the icon
)

/** One zygote process that reported a successful core injection to zygiskd. */
private data class InjectedZygote(
    val pid: Int,
    val name: String,
    val abi: String,
)

/** Parsed view of zygiskd's status JSON (Natives.tamisuQueryStatus). */
private data class TamisuStatus(
    val count: Int,
    val recent: List<Int>, // appids, most-recent first
    val zygotes: List<InjectedZygote>,
    val modules: List<String>,
)

private fun parseTamisuStatus(json: String): TamisuStatus? = runCatching {
    val o = JSONObject(json)
    val recent = o.optJSONArray("recent")?.let { a ->
        (0 until a.length()).map { a.getInt(it) }
    } ?: emptyList()
    val modules = o.optJSONArray("modules")?.let { a ->
        (0 until a.length()).map { a.getString(it) }
    } ?: emptyList()
    val zygotes = o.optJSONArray("zygotes")?.let { a ->
        (0 until a.length()).map { i ->
            val z = a.getJSONObject(i)
            InjectedZygote(
                pid = z.optInt("pid", 0),
                name = z.optString("name", "zygote"),
                abi = z.optString("abi", "unknown"),
            )
        }
    } ?: emptyList()
    TamisuStatus(o.optInt("count", 0), recent, zygotes, modules)
}.getOrNull()

/**
 * Resolve an injected appid to a displayable app. zygiskd reports appids
 * (uid % 100000); for the primary user that equals the uid, so user-0 apps
 * resolve directly and others fall back to the same package identity.
 */
private fun resolveRecentApp(pm: PackageManager, appId: Int): RecentApp {
    val pkg = runCatching { pm.getPackagesForUid(appId)?.firstOrNull() }.getOrNull()
        ?: return RecentApp(appId, "", null, null)
    val info = runCatching { pm.getPackageInfo(pkg, 0) }.getOrNull()
    val label = info?.applicationInfo?.loadLabel(pm)?.toString() ?: pkg
    return RecentApp(appId, label, pkg, info)
}

private data class TamisuSnapshot(
    val count: Int,
    val recentApps: List<RecentApp>,
    val zygotes: List<InjectedZygote>,
    val modulesLoadedCount: Int,
)

private const val TAMISU_POLL_INTERVAL_MS = 2000L

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun ZygiskScreen(navigator: DestinationsNavigator) {
    val scrollBehavior =
        TopAppBarDefaults.pinnedScrollBehavior(androidx.compose.material3.rememberTopAppBarState())
    val scope = rememberCoroutineScope()
    val snackBarHost = remember { SnackbarHostState() }

    val context = LocalContext.current
    var config by remember { mutableStateOf(TamisuConfig()) }
    var injectionActive by remember { mutableStateOf(false) }
    var injectionCount by remember { mutableIntStateOf(0) }
    var recentApps by remember { mutableStateOf<List<RecentApp>>(emptyList()) }
    var injectedZygotes by remember { mutableStateOf<List<InjectedZygote>>(emptyList()) }
    var modulesLoadedCount by remember { mutableIntStateOf(0) }

    LaunchedEffect(Unit) {
        config = readTamisuConfig()
        injectionActive =
            ShellUtils.fastCmd(getRootShell(), "ksud feature get tamisu 2>/dev/null")
                ?.contains("enabled", ignoreCase = true) == true
    }

    // Injection telemetry: poll the running zygiskd in-process via Natives
    // (jni.c connects to the daemon socket; zygiskd SO_PEERCRED-gates the reply
    // to the kernel-authenticated manager uid). A non-null reply means injection
    // is live; null means the daemon is down or we are not the manager, so the
    // last-known values just stand.
    LaunchedEffect(Unit) {
        val pm = context.packageManager
        while (true) {
            val snapshot = withContext(Dispatchers.IO) {
                val json = runCatching { Natives.tamisuQueryStatus() }.getOrNull()
                    ?: return@withContext null
                val st = parseTamisuStatus(json) ?: return@withContext null
                TamisuSnapshot(
                    count = st.count,
                    recentApps = st.recent.map { resolveRecentApp(pm, it) },
                    zygotes = st.zygotes,
                    modulesLoadedCount = st.modules.size,
                )
            }
            if (snapshot != null) {
                injectionActive = true
                injectionCount = snapshot.count
                recentApps = snapshot.recentApps
                injectedZygotes = snapshot.zygotes
                modulesLoadedCount = snapshot.modulesLoadedCount
            }
            delay(TAMISU_POLL_INTERVAL_MS)
        }
    }

    fun save(newCfg: TamisuConfig) {
        config = newCfg
        scope.launch { writeTamisuConfig(newCfg) }
    }

    Scaffold(
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            TopAppBar(
                title = {
                    Text(
                        stringResource(R.string.settings_zygisk),
                        style = MaterialTheme.typography.titleLarge,
                    )
                },
                windowInsets = WindowInsets.safeDrawing.only(
                    WindowInsetsSides.Top + WindowInsetsSides.Horizontal
                ),
                scrollBehavior = scrollBehavior,
            )
        },
        snackbarHost = { SnackbarHost(snackBarHost) },
        contentWindowInsets = WindowInsets.safeDrawing.only(
            WindowInsetsSides.Top + WindowInsetsSides.Horizontal
        ),
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp)
                .padding(top = 8.dp),
        ) {
            // --- Injection status ---
            SettingsCard(title = stringResource(R.string.zygisk_injection_status)) {
                StatusRow(
                    stringResource(R.string.zygisk_kernel_injection),
                    if (injectionActive) stringResource(R.string.zygisk_status_active)
                    else stringResource(R.string.zygisk_status_off),
                )
                StatusRow(
                    stringResource(R.string.zygisk_module_loader),
                    if (config.yukilinker) stringResource(R.string.zygisk_loader_anon)
                    else stringResource(R.string.zygisk_loader_system),
                )
                StatusRow(
                    stringResource(R.string.zygisk_denylist_behaviour),
                    when (config.denylistMode) {
                        1 -> stringResource(R.string.zygisk_denylist_force_long)
                        2 -> stringResource(R.string.zygisk_denylist_restore_long)
                        else -> stringResource(R.string.zygisk_status_off)
                    },
                )
                StatusRow(
                    stringResource(R.string.zygisk_injections_session),
                    injectionCount.toString(),
                )
            }

            // --- Injected zygotes ---
            SettingsCard(title = stringResource(R.string.zygisk_injected_zygotes)) {
                if (injectedZygotes.isEmpty()) {
                    Text(
                        stringResource(R.string.zygisk_no_zygotes),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                    )
                } else {
                    injectedZygotes.forEach { zygote -> InjectedZygoteRow(zygote) }
                }
            }

            // --- Recent injections ---
            SettingsCard(title = stringResource(R.string.zygisk_recent_injections)) {
                if (recentApps.isEmpty()) {
                    Text(
                        stringResource(R.string.zygisk_no_injections),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                    )
                } else {
                    recentApps.forEach { app -> RecentAppRow(app) }
                }
            }

            // --- Module loading ---
            SettingsCard(title = stringResource(R.string.zygisk_module_loading)) {
                SwitchSettingItem(
                    icon = Icons.Filled.Memory,
                    title = stringResource(R.string.zygisk_anon_loading_title),
                    summary = stringResource(R.string.zygisk_anon_loading_summary),
                    checked = config.yukilinker,
                    onChange = { save(config.copy(yukilinker = it)) },
                )
                Text(
                    stringResource(R.string.zygisk_loaded_modules_count, modulesLoadedCount),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp)
                        .padding(top = 4.dp, bottom = 4.dp),
                )
            }

            // --- Per-module zygote grant ---
            SettingsCard(title = stringResource(R.string.zygisk_grant_title)) {
                SwitchSettingItem(
                    icon = Icons.Filled.Memory,
                    title = stringResource(R.string.zygisk_grant_filter_title),
                    summary = stringResource(R.string.zygisk_grant_filter_summary),
                    checked = config.grantFilterActive,
                    onChange = { save(config.copy(grantFilterActive = it)) },
                )
                if (config.grantFilterActive) {
                    val pm = context.packageManager
                    ZygoteModuleGrantList(
                        granted = config.zygoteModules.toSet(),
                        onToggle = { id, on ->
                            val next = if (on) {
                                (config.zygoteModules + id).distinct()
                            } else {
                                config.zygoteModules - id
                            }
                            save(config.copy(zygoteModules = next))
                        },
                    )
                }
            }

            // --- Denylist behaviour ---
            SettingsCard(title = stringResource(R.string.zygisk_denylist_behaviour)) {
                Text(
                    stringResource(R.string.zygisk_denylist_desc),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier
                        .padding(horizontal = 16.dp)
                        .padding(bottom = 8.dp),
                )
                DenylistModeSelector(
                    mode = config.denylistMode,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp),
                ) { save(config.copy(denylistMode = it)) }
            }

            // --- Logging ---
            SettingsCard(title = stringResource(R.string.zygisk_log_dmesg_title)) {
                SwitchSettingItem(
                    icon = Icons.AutoMirrored.Filled.Article,
                    title = stringResource(R.string.zygisk_log_dmesg_title),
                    summary = stringResource(R.string.zygisk_log_dmesg_summary),
                    checked = config.dmesgLog,
                    onChange = { save(config.copy(dmesgLog = it)) },
                )
            }
        }
    }
}

/** One recent-injection row: app icon + label + "package · uid", like SuperUser. */
@Composable
private fun RecentAppRow(app: RecentApp) {
    val context = LocalContext.current
    ListItem(
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        leadingContent = {
            AsyncImage(
                model = ImageRequest.Builder(context)
                    .data(app.packageInfo)
                    .crossfade(true)
                    .build(),
                contentDescription = null,
                modifier = Modifier.size(40.dp),
            )
        },
        headlineContent = {
            Text(
                if (app.label.isNotEmpty()) app.label
                else stringResource(R.string.zygisk_uid_fallback, app.uid),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        },
        supportingContent = {
            Text(
                app.packageName?.let { "$it  ·  uid ${app.uid}" }
                    ?: stringResource(R.string.zygisk_uid_fallback, app.uid),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        },
    )
}

@Composable
private fun InjectedZygoteRow(zygote: InjectedZygote) {
    ListItem(
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        headlineContent = {
            Text(
                zygote.name,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        },
        supportingContent = {
            Text(
                stringResource(
                    R.string.zygisk_zygote_detail,
                    zygote.abi,
                    zygote.pid,
                ),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        },
    )
}

@Composable
private fun StatusRow(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(
            label,
            style = MaterialTheme.typography.bodyLarge,
            modifier = Modifier.weight(1f, fill = false),
        )
        Text(
            value,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.primary,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(start = 12.dp),
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun DenylistModeSelector(
    mode: Int,
    modifier: Modifier = Modifier,
    onSelect: (Int) -> Unit,
) {
    val options = listOf(
        stringResource(R.string.zygisk_denylist_off),
        stringResource(R.string.zygisk_denylist_force),
        stringResource(R.string.zygisk_denylist_restore),
    )
    SingleChoiceSegmentedButtonRow(modifier = modifier) {
        options.forEachIndexed { index, label ->
            SegmentedButton(
                selected = mode == index,
                onClick = { onSelect(index) },
                shape = SegmentedButtonDefaults.itemShape(index, options.size),
            ) {
                Text(label)
            }
        }
    }
}

/** Lists every module under /data/adb/modules that ships a zygisk/<abi>.so,
 *  with a per-module toggle that adds/removes its id from the grant set. */
@Composable
private fun ZygoteModuleGrantList(
    granted: Set<String>,
    onToggle: (id: String, on: Boolean) -> Unit,
) {
    val shell = getRootShell()
    val modules by produceState(initialValue = emptyList<String>()) {
        value = withContext(Dispatchers.IO) {
            val out = ShellUtils.fastCmd(shell,
                "for d in /data/adb/modules/*/zygisk/arm64.so /data/adb/modules/*/zygisk/x86_64.so /data/adb/modules/*/zygisk/armeabi-v7a.so; do [ -f \"\$d\" ] && echo \"\$d\"; done")
            out?.lineSequence()?.mapNotNull { line ->
                // /data/adb/modules/<id>/zygisk/<abi>.so -> <id>
                val parts = line.trim().split('/')
                if (parts.size >= 5) parts[3] else null
            }?.distinct()?.toList() ?: emptyList()
        }
    }
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp),
    ) {
        if (modules.isEmpty()) {
            Text(
                stringResource(R.string.zygisk_grant_empty),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(vertical = 8.dp),
            )
        }
        modules.forEach { id ->
            ListItem(
                headlineContent = { Text(id) },
                trailingContent = {
                    Switch(
                        checked = id in granted,
                        onCheckedChange = { onToggle(id, it) },
                    )
                },
                colors = ListItemDefaults.colors(
                    containerColor = androidx.compose.ui.graphics.Color.Transparent,
                ),
            )
        }
    }
}
