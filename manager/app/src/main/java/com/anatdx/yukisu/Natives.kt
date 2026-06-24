package com.anatdx.yukisu

import android.os.Parcelable
import androidx.annotation.Keep
import androidx.compose.runtime.Immutable
import kotlinx.parcelize.Parcelize

/**
 * @author weishu
 * @date 2022/12/8.
 */
object Natives {
    // minimal supported kernel version
    // 10915: allowlist breaking change, add app profile
    // 10931: app profile struct add 'version' field
    // 10946: add capabilities
    // 10977: change groups_count and groups to avoid overflow write
    // 11071: Fix the issue of failing to set a custom SELinux type.
    // 12143: breaking: new supercall impl
    const val MINIMAL_SUPPORTED_KERNEL = 10000

    // 12040: Support disable sucompat mode
    const val KERNEL_SU_DOMAIN = "u:r:su:s0"

    const val MINIMAL_SUPPORTED_KERNEL_FULL = "v1.0.0"

    const val MINIMAL_NEW_IOCTL_KERNEL = 10000

    const val ROOT_UID = 0
    const val ROOT_GID = 0

    /** root_profile.flags: block re-escalation for this profile and its children. */
    const val FLAG_KSU_NO_NEW_PRIVS = 1L

    external fun getFullVersion(): String

    /** Kernel UAPI contract version (KERNEL_SU_UAPI_VERSION); 0 if unsupported. */
    external fun getUapiVersion(): Int

    /** UAPI contract version this manager binary was built against. */
    external fun getManagerUapiVersion(): Int

    /** True when the kernel's UAPI version differs from the manager's (skew). */
    fun checkUapiMismatch(): Boolean = getUapiVersion() != getManagerUapiVersion()

    fun isVersionLessThan(v1Full: String, v2Full: String): Boolean {
        fun extractVersionParts(version: String): List<Int> {
            val match = Regex("""v\d+(\.\d+)*""").find(version)
            val simpleVersion = match?.value ?: version
            return simpleVersion.trimStart('v').split('.').map { it.toIntOrNull() ?: 0 }
        }

        val v1Parts = extractVersionParts(v1Full)
        val v2Parts = extractVersionParts(v2Full)
        val maxLength = maxOf(v1Parts.size, v2Parts.size)
        for (i in 0 until maxLength) {
            val num1 = v1Parts.getOrElse(i) { 0 }
            val num2 = v2Parts.getOrElse(i) { 0 }
            if (num1 != num2) return num1 < num2
        }
        return false
    }

    fun getSimpleVersionFull(): String = getFullVersion().let { version ->
        Regex("""v\d+(\.\d+)*""").find(version)?.value ?: version
    }

    init {
        System.loadLibrary("kernelsu")
    }

    val version: Int
        external get

    // get the uid list of allowed su processes.
    val allowList: IntArray
        external get

    /** Returns total number of apps in allow list (count only, no full list fetch). */
    external fun getSuperuserCount(): Int

    val isSafeMode: Boolean
        external get

    val isLateLoadMode: Boolean
        external get
    val isManager: Boolean
        external get

    external fun uidShouldUmount(uid: Int): Boolean
    external fun getDynamicManagers(): IntArray

    const val DYNAMIC_MANAGER_FLAG_PRESET = 1 shl 0
    const val DYNAMIC_MANAGER_FLAG_TRUSTED = 1 shl 1

    external fun getHookType(): String

    external fun getUserName(uid: Int): String?

    /**
     * Check if KSU driver is present.
     * @return true if driver fd can be found, false otherwise
     */
    external fun isKsuDriverPresent(): Boolean

    /**
     * YukiZygisk injection status as a JSON string, or null when the daemon is
     * down or this process is not the kernel-authenticated manager.
     *
     * Native (jni.c): connects straight to zygiskd's abstract socket and asks
     * for a snapshot. zygiskd gates the reply with SO_PEERCRED -- it compares the
     * real uid the kernel stamped on our connection against the manager uid, so a
     * hostile app spoofing our package name can't read it. JSON shape:
     * `{ "count": Int, "recent": [appId...], "modules": ["name"...],
     *    "yukilinker": Bool, "denylist_mode": Int, "dmesg_log": Bool }`.
     */
    external fun yzQueryStatus(): String?

    fun requireNewKernel(): Boolean {
        if (version != -1 && version < MINIMAL_SUPPORTED_KERNEL) return true
        return isVersionLessThan(getFullVersion(), MINIMAL_SUPPORTED_KERNEL_FULL)
    }
}
