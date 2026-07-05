# Dual-Path Specialization Detection - Summary

## What Was Done

Completed the replacement of `__NR_setresuid` syscall hook with a hybrid detection system consisting of:

### 1. Kernel Side (Commit `598e5ea`)
- **Removed:** `__NR_setresuid` syscall hook
- **Added:** LSM `task_fix_setuid` hook for kernel-side detection
- **Added:** Netlink receive handler for userspace reports
- **Modified:** Orchestrator to handle both detection paths

**Files Changed:**
- `kernel/hook/setuid_hook.c` - LSM hook implementation
- `kernel/hook/syscall_event_bridge.c` - removed setresuid hook
- `kernel/hook/syscall_hook_manager.c` - removed registration
- `kernel/feature/zygote_nl.c` - added receive handler
- `kernel/feature/zygote_orch.c` - dual-path logic
- `uapi/yukizygisk.h` - added `YZ_EV_REPORT_SPECIALIZE` event

### 2. Userspace Side (Commit `8fc198b`)
- **Added:** `report_specialization_to_kernel()` function
- **Integrated:** Netlink reporting in `run_app_post_impl()`
- **Protocol:** Sends `YZ_EV_REPORT_SPECIALIZE` with pid and uid

**Files Changed:**
- `userspace/zygisk/core/src/core.cpp`

### 3. Documentation
- **TESTING.md** - comprehensive test guide
- **USERSPACE_REPORT.md** - implementation details
- **IMPLEMENTATION.md** - kernel changes summary

## Architecture

```
App Launch
    ↓
Zygote Fork
    ↓
Specialization
    ↓
    ├─→ [Userspace] zygisk core reports via netlink (PRIMARY)
    │       ↓
    │   Kernel receives YZ_EV_REPORT_SPECIALIZE
    │       ↓
    │   Logs: [specialize-userspace]
    │
    └─→ [Kernel] LSM task_fix_setuid hook detects UID change (FALLBACK)
            ↓
        Logs: [specialize-kernel]
```

## Benefits

1. **More Comprehensive:** LSM hook captures all UID change methods (not just `setresuid`)
2. **More Reliable:** Userspace report is direct and immediate
3. **Redundancy:** Kernel fallback detects injection failures
4. **Less Intrusive:** LSM hooks are cleaner than syscall hooks
5. **Better Debugging:** Can identify when injection fails (fork seen but no userspace report)

## Log Markers

- `[specialize-userspace]` - Reported by zygisk core (primary path)
- `[specialize-kernel]` - Detected by kernel LSM hook (fallback path)

## Testing

See **TESTING.md** for comprehensive test procedures.

**Quick Test:**
```bash
am start -n com.android.settings/.Settings
dmesg | grep zygote_orch | tail -2
```

**Expected:** One line with `[specialize-userspace]` or `[specialize-kernel]`

## Addressing log.md Concerns

**Original Question:** "是不是 __NR_setresuid 不太需要呢"

**Answer:** 正确！已完成：
1. ✅ 移除了 `__NR_setresuid` syscall hook
2. ✅ 用更合适的 LSM hook 替代（作为 fallback）
3. ✅ 添加了更可靠的用户态上报机制（primary）
4. ✅ 保持了双重保障，可以检测注入失败

代码现在更清晰、更合理，符合 Tamisu 作为 Zygisk 注入器的实际需求。

## Commits

1. `598e5ea` - refactor: replace setresuid syscall hook with LSM hook + userspace reporting
2. `8fc198b` - feat(userspace): add netlink reporting for app specialization
