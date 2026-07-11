# Repository Guidelines

Tamisu is a **kernel-level Zygisk provider** (not a root provider). It coexists with KernelSU/Magisk/APatch: Tamisu owns zygote injection; the root solution owns `su`/modules. Four deliverables: `tamisu.ko`, `tamisu_daemon`, zygisk payloads (`libzygisk*.so`), Manager APK.

Only `CONFIG_TAMISU=m` is supported (`=y` is not). Kernel LKM is **aarch64-only**.

## Layout

| Path | Role |
|---|---|
| `core/` | Flat-layout LKM (`tamisu_*.c` → `tamisu.ko`), `Kbuild`/`Makefile`, `include/uapi/` |
| `daemon/tamisud_core/` | C++17 multi-call daemon (CMake+Ninja); embeds tools + staged assets |
| `daemon/zygisk/` | `libzygisk_linker.so` / `libzygisk.so` / `libzygisk_zncore.so` + `zygiskd` |
| `apk/` | Kotlin/Compose Manager; JNI under `app/src/main/cpp/` |
| `uapi/` | Userspace UAPI contract (keep in sync with `core/include/uapi/`) |
| `repack_apk.py` | Injects built `tamisu_daemon` into the Gradle APK and resigns |
| `YukiZygisk/` | Upstream reference tree (not part of the build) |

Submodules (required for daemon tools): `git submodule update --init`  
→ `daemon/tamisud_core/third_party/{MagiskbootAlone,bootctlAlone,resetpropAlone,ndk-busybox}`

There is **no** top-level `scripts/build.sh` (docs that mention it are stale). Build components separately as CI does.

## Build commands

**Kernel LKM** (inside matching DDK image `ghcr.io/ylarod/ddk-min:<kmi>-20260313`, from `core/`):
```sh
CONFIG_TAMISU=m CC=clang make
# format check (CI): make check-format   # apply: make format
```
KMI matrix (CI): `android12-5.10` … `android16-6.12`. Match device KMI/Module.symvers; non-exported GKI symbols are resolved at runtime via kallsyms.

**Daemon** — stage assets **before** CMake configure (`.ko` + zygisk `.so` into `daemon/tamisud_core/assets/`; empty dir still configures but embeds nothing useful):
```sh
cd daemon/tamisud_core && mkdir -p build && cd build
cmake -G Ninja -DCMAKE_SYSTEM_NAME=Android \
  -DCMAKE_ANDROID_ARCH_ABI=arm64-v8a \
  -DCMAKE_ANDROID_NDK="$ANDROID_NDK_HOME" \
  -DCMAKE_BUILD_TYPE=Release ..
ninja
```
ABI: `arm64-v8a` | `x86_64` | `armeabi-v7a`. Min Android API 28. Host builds run clang-tidy with `WarningsAsErrors: '*'` (root `.clang-tidy`); Android NDK builds skip tidy. Disable: `-DTAMISU_DAEMON_ENABLE_CLANG_TIDY=OFF`. LTO link failures: `-DTAMISU_DAEMON_DISABLE_LTO=ON`.

**Manager APK**:
```sh
cd apk && ./gradlew assembleRelease -PABI=arm64-v8a
# then package daemon into APK (CI does this):
python3 repack_apk.py repack -b release -t release -a arm64-v8a -K <jks> -A <alias> -P <storepass> -S <keypass> --strip
```
Signing: copy `apk/sign.example.properties` or env `TAMISU_KEYSTORE*` / Gradle props. Java 21.

**ShellCheck** (CI): all `*.sh` except `gradlew` and `daemon/tamisud_core/assets/installer.sh`.

## Architecture agents miss

- **Ioctl magic** `'T'` (not KernelSU `'K'`); driver fd discovered by `readlink(/proc/self/fd/*)` matching `[tamisu]`.
- **Trust**: `core/tamisu_perm.c` — uid 0, or cmdline prefix `me.dabao1955.tamisu` / `tamisu_daemon` / `zygiskd`. Package override: `TAMISU_MANAGER_PACKAGE` in Kbuild.
- **TSR**: hooks a `sys_ni_syscall` slot for ioctl dispatch (`tamisu_syscall_hook*.c`).
- **Zygote**: two-stage injection — kernel AT_ENTRY stub → system linker loads stage-1 SO → `zygisk_linker` loads core/modules from memfd (`tamisu_zygote_*.c`, `daemon/zygisk/`).
- **SELinux**: in-kernel policydb rewrite (`tamisu_selinux.c` / `sepolicy` / `rules`).
- **Kbuild compat**: `core/Kbuild` greps srctree and sets `TAMISU_COMPAT_*` / `TAMISU_OPTIONAL_*` / `TAMISU_HAS_*`. New version-dependent code must follow that pattern.
- **KCFI**: Never add `-fsanitize=kcfi` in `core/Kbuild` (`CONFIG_CFI_CLANG` is kernel-side only). Kallsyms-resolved calls still use `TAMISU_INDIRECT_CALL` (`no_sanitize` + `noinline`) like YukiZygisk's `YZ_INDIRECT_CALL`.
- **Daemon multi-call**: argv0/symlinks `magiskboot`, `bootctl`, `resetprop`, `busybox`, `zygiskd` dispatch into embedded tools.
- **UAPI dual copy**: edit both `uapi/` and `core/include/uapi/` (or mirror deliberately). Netlink: `YZ_NETLINK_PROTO 27` in `zygisk.h`.

## Style / hooks

- C/C++: existing clang-format (`core/.clang-format`, `daemon/tamisud_core/.clang-format`).
- Symbols: `tamisu_` / `Tamisu` prefixes.
- `git config core.hooksPath .githooks` — pre-commit runs `fix_endif_comments.py` then clang-format on staged C/C++ and re-stages.

## Testing & PR

No repo-wide unit suite. Verify with the component build you touched + `make check-format` for `core/` C. APK: `./gradlew test` when UI changes; device smoke-test for install/JNI/privilege. Kernel: load on matching KMI after DDK build.

Commits: short imperative subjects. PRs: name affected component, commands run, ABI + KMI. **Never commit** signing keys, APKs, `*.ko`, or device secrets.
