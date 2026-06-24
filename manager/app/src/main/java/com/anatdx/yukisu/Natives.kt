package com.anatdx.yukisu

/**
 * @author weishu
 * @date 2022/12/8.
 */
object Natives {
    // minimal supported kernel version
    const val MINIMAL_SUPPORTED_KERNEL = 10000
    const val MINIMAL_SUPPORTED_KERNEL_FULL = "v1.0.0"

    const val ROOT_UID = 0
    const val ROOT_GID = 0

    external fun getFullVersion(): String

    /** Kernel UAPI contract version (KERNEL_SU_UAPI_VERSION); 0 if unsupported. */
    external fun getUapiVersion(): Int

    /** UAPI contract version this manager binary was built against. */
    fun getManagerUapiVersion(): Int = 2

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

    init {
        System.loadLibrary("kernelsu")
    }

    val version: Int
        external get

    val isSafeMode: Boolean
        external get

    /** In the zygisk-only build there is no manager-app concept. "isManager"
     *  is true when the KSU driver fd is accessible (i.e. the kernel module
     *  is loaded and this process is root). */
    val isManager: Boolean
        get() = isKsuDriverPresent()

    external fun getHookType(): String

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
