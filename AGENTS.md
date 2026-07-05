# AGENTS.md

This file provides guidance to LLM when working with code in this repository.

## Project overview

Tamisu is a kernel-level Zygisk provider (not a root provider). It coexists with KernelSU/Magisk/APatch — Tamisu owns zygote injection, the root solution owns `su`/modules. Delivered as: loadable kernel module (`tamisu.ko`) + userspace daemon (`tamisu_daemon`) + zygisk payload + Manager APK.

Architecture is aarch64-only for the kernel module. The daemon and APK support arm64-v8a, x86_64, armeabi-v7a.

## Build commands

Full local build (needs Docker for DDK LKM step):
```sh
ANDROID_NDK_HOME=... ./scripts/build.sh -k android16-6.12 -a arm64-v8a
# Flags: --skip-lkm, --skip-kasumi, -i (adb install after)
```

Manager APK only:
```sh
cd apk && ./gradlew assembleRelease -PABI=arm64-v8a
```

Daemon only (must stage assets first):
```sh
cd daemon/tamisud_core && mkdir build && cd build
cmake -G Ninja -DCMAKE_SYSTEM_NAME=Android \
  -DCMAKE_ANDROID_ARCH_ABI=arm64-v8a \
  -DCMAKE_ANDROID_NDK=$ANDROID_NDK_HOME \
  -DCMAKE_BUILD_TYPE=Release ..
ninja
```

Kernel LKM (inside DDK docker container, from `core/`):
```sh
CONFIG_TAMISU=m CC=clang make
```

## Lint and format

Kernel module C code (core/):
```sh
cd core && make format          # apply clang-format
cd core && make check-format    # dry-run check (used in CI)
```

Daemon C++ (daemon/tamisud_core/): clang-tidy runs automatically during CMake build with `WarningsAsErrors: '*'`. Disable with `-DTAMISU_DAEMON_ENABLE_CLANG_TIDY=OFF`. Config is at `.clang-tidy` (repo root).

Shell scripts: ShellCheck in CI. `installer.sh` is excluded from checks.

## Architecture notes

The daemon is a multi-call binary. When invoked as `magiskboot`, `bootctl`, `resetprop`, `busybox`, or `zygiskd` (via argv[0] / symlinks), it dispatches to the respective embedded tool. Third-party tools are compiled from git submodules under `daemon/tamisud_core/third_party/`.

Zygisk injection is two-stage:
1. Kernel rewrites zygote's `AT_ENTRY` → single-page ARM64 stub calls system linker to load `libzygisk_linker.so`
2. yukilinker (custom ELF loader) loads `libzygisk.so` and modules from memfd without bionic

The kernel module hooks: `sys_ni_syscall` slot (TSR dispatcher), `sched_process_fork/free` tracepoints, LSM `bprm_committed_creds`, and a static-key-gated `__NR_execve` hook (disabled after init second stage).

Privileged ioctls are gated by cmdline-based trust check (`core/tamisu_perm.c`): root, the manager app package, `tamisu_daemon`, or `zygiskd`.

## Key conventions

- Ioctl magic: `'T'` (distinct from KernelSU's `'K'`)
- Driver fd name: `[tamisu]` (discovered via `/proc/self/fd/*` readlink)
- Manager package: `me.dabao1955.tamisu` (compile-time override via `TAMISU_MANAGER_PACKAGE`)
- UAPI headers live in `uapi/` (repo root) and `core/include/uapi/`
- Kbuild compat flags in `core/Kbuild` auto-detect kernel features via grep on srctree
- Git hooks in `.githooks/` (e.g. `fix_endif_comments.py`)
- Submodules must be initialized: `git submodule update --init`
- DDK container images: `ghcr.io/ylarod/ddk-min:<kmi>-<release>`
- CI builds LKM for KMI matrix: android12-5.10 through android16-6.12
