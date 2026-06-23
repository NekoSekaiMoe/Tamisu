# AGENTS.md

YukiSU / KernelSU fork: Android kernel-level root delivered as a loadable kernel module (LKM) + userspace daemon (`ksud`) + Manager APK. Built-in kernel (=y) is **not** supported — only `=m`.

## Repo layout (what owns what)

- `kernel/` — the `kernelsu.ko` LKM source (C). Built via DDK against a specific Android KMI (e.g. `android16-6.12`). `kernel/Kconfig` + `kernel/Kbuild` are the source of truth for which objects/CONFIG flags exist. `CONFIG_KSU=m` is mandatory.
- `userspace/ksud/` — the daemon. Embeds assets (`.ko` modules, zygisk payloads) via `scripts/embed_assets.py` at configure time. Requires CMake + Ninja + Clang (no GCC — LTO/`-faddrsig` need Clang). Git submodules (`third_party/MagiskbootAlone`, `bootctlAlone`, `resetpropAlone`, `ndk-busybox`) are required: `git submodule update --init --recursive`.
- `userspace/zygisk/{loader,core}/` — standalone CMake+Ninja projects. Each is built **separately**, then its binary is copied into `userspace/ksud/assets/` **before** ksud configures, so `embed_assets.py` picks it up. ksud does **not** compile zygisk itself.
- `manager/` — Android app (Gradle 9.2, AGP 8.13, Kotlin 2.2, Compose, JDK 17). Packages `ksud` as `app/src/main/jniLibs/<abi>/libksud.so`. Version code/name derived from `git describe --tags`.
- `uapi/` — shared UAPI headers; `kernel/include/uapi` mirrors these. If you change a supercall/profile struct here, update both sides.
- `scripts/build.sh` — one-shot local build orchestrator (LKM → zygisk → ksud → Manager). See it for the exact stage order and asset-staging rules.

## Build & verify commands

Local full build (macOS/Linux, needs Docker for the DDK LKM step):

```
ANDROID_NDK_HOME=... ./scripts/build.sh -k android16-6.12 -a arm64-v8a
```

Flags: `--skip-lkm`, `--skip-kasumi` (Kasumi LKM is a separate external repo, default `/Volumes/Workspace/Kasumi`; override with `--kasumi-dir` or `KASUMI_DIR`), `-i` to `adb install` after. ABI ∈ {arm64-v8a, x86_64, armeabi-v7a}.

Manager APK only:

```
cd manager && ./gradlew assembleRelease -PABI=arm64-v8a
```

Single ksud build (must stage assets first):

```
cd userspace/ksud && mkdir build && cd build
cmake -G Ninja -DCMAKE_SYSTEM_NAME=Android -DCMAKE_ANDROID_ARCH_ABI=arm64-v8a \
  -DCMAKE_ANDROID_NDK=$ANDROID_NDK_HOME -DCMAKE_BUILD_TYPE=Release ..
ninja
```

Kernel LKM (inside DDK docker, `kernel/`):

```
CONFIG_KSU=m CONFIG_KSU_SUPERKEY=y CC=clang make -j$(nproc)
```

## Lint / format / checks (CI-enforced)

- **C/C++ format** (kernel): `cd kernel && make check-format` (clang-format `--Werror`). Apply with `make format`. CI: `clang-format.yml`.
- **clang-tidy** (ksud CMake): **on by default and `WarningsAsErrors: '*'`** — any tidy diagnostic fails the build. Driven by root `.clang-tidy`. Disable only with `-DKSUD_ENABLE_CLANG_TIDY=OFF` (do not do this casually). Only applies to ksud's own sources; `third_party/` and FetchContent are filtered out.
- **ShellCheck** on all `.sh` except `gradlew` and `userspace/ksud/assets/installer.sh`.
- **pre-commit hook** (`.githooks/pre-commit`): auto-runs `fix_endif_comments.py` then `clang-format -i` on staged C/C++ files and re-stages. Install with `git config core.hooksPath .githooks`. Note: the hook rewrites `#endif` lines to add matching comments — expect diffs on preprocessor blocks.
- Manager: `lint { abortOnError = true }`.

## Conventions & gotchas

- **Asset staging order matters.** `.ko` modules and zygisk payloads must exist in `userspace/ksud/assets/` before ksud CMake configure, or they won't be embedded. `scripts/build.sh` and the `ksud.yml` workflow enforce this; if you build ksud manually you must replicate it.
- **KMI tagging.** Kasumi LKMs are named `<KMI>_<arch>_kasumi_lkm.ko` (e.g. `android16-6.12_arm64_kasumi_lkm.ko`); `lkm.cpp` and CI assert this exact form. ksud for arm64 fails CI if no KMI-tagged Kasumi asset is embedded.
- **Sign via env, not files.** Manager signing reads `YUKISU_KEYSTORE`, `YUKISU_KEYSTORE_PASSWORD`, `YUKISU_KEY_ALIAS`, `YUKISU_KEY_PASSWORD` (mapped to gradle props by `app/build.gradle.kts`). Example config in `manager/sign.example.properties`. Never commit real keystore creds.
- **Kasumi LKM = arm64-only** in DDK CI; `full_archs` only expands ksud to x86_64/armv7.
- **NDK min API 28** for ksud (bionic symbols); Manager `minSdk` 26.
- **No built-in support / no `=y`.** `kernel/setup.sh` integrates into an out-of-tree kernel source tree; it symlinks `kernel/` into `<kernel>/drivers/kernelsu`.
- Branch `dev` exists alongside `main`; `feat/kasumi*` branches auto-select Kasumi `dev` ref in CI.

## Testing

No unit-test suite. Verification = CI workflows building every KMI/arch matrix + `check-format` + `shellcheck` + clang-tidy. Treat a clean `scripts/build.sh` run + passing CI as the bar.
