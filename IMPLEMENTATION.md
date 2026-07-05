# Implementation Changes: Replacing setresuid Hook

## Summary

Replaced the `__NR_setresuid` syscall hook with a hybrid detection system:
1. **LSM hook** (`task_fix_setuid`) for kernel-side detection (fallback)
2. **Netlink protocol** for userspace reporting (primary path)

## Motivation

The original `__NR_setresuid` syscall hook had several limitations:
- Only captured `setresuid()` calls, missing other UID change methods
- Was inherited from KernelSU but not needed for Tamisu's core functionality
- Syscall hooks are more intrusive than LSM hooks
- No way for userspace to directly report specialization

## Changes Made

### 1. Kernel Side: LSM Hook (`kernel/hook/setuid_hook.c`)

**Before:**
```c
int ksu_handle_setresuid(uid_t old_uid, uid_t new_uid)
{
    ksu_zygote_orch_on_setresuid(old_uid, new_uid);
    return 0;
}
```

**After:**
```c
static int __nocfi my_task_fix_setuid(ksu_cred_arg_t *new,
                                      const struct cred *old, int flags)
{
    uid_t old_uid = old->uid.val;
    uid_t new_uid = new->uid.val;
    
    if (old_uid != new_uid) {
        ksu_zygote_orch_on_uid_change(old_uid, new_uid);
    }
    return 0;
}

static struct ksu_lsm_hook setuid_lsm_hook = KSU_LSM_HOOK_INIT(
    task_fix_setuid, "apparmor_task_setrlimit", my_task_fix_setuid, 0);
```

**Benefits:**
- Captures ALL UID change methods: `setuid`, `setreuid`, `setresuid`, `setfsuid`
- Uses existing LSM hook infrastructure
- Less intrusive than syscall hooks

### 2. Userspace Reporting (`kernel/feature/zygote_nl.c`)

Added netlink receive handler:
```c
static void ksu_zygote_nl_recv(struct sk_buff *skb)
{
    struct nlmsghdr *nlh = nlmsg_hdr(skb);
    struct yz_event *ev = nlmsg_data(nlh);
    
    if (ev->type == YZ_EV_REPORT_SPECIALIZE) {
        ksu_zygote_orch_on_userspace_report(ev->pid, ev->appid);
    }
}
```

**Protocol Extension (`uapi/yukizygisk.h`):**
```c
enum yz_event_type {
    YZ_EV_SPECIALIZE = 1,        // kernel -> userspace
    YZ_EV_RELOAD = 2,            // kernel -> userspace
    YZ_EV_REPORT_SPECIALIZE = 3, // userspace -> kernel (NEW)
};
```

### 3. Dual-Path Detection (`kernel/feature/zygote_orch.c`)

**Kernel detection (fallback):**
```c
void ksu_zygote_orch_on_uid_change(uid_t old_uid, uid_t new_uid)
{
    // ... validate and update state ...
    pr_info("zygote_orch: [specialize-kernel] pid=%d uid=%u\n", pid, new_uid);
    ksu_zygote_nl_emit_specialize(pid, new_uid % 100000);
}
```

**Userspace report (primary):**
```c
void ksu_zygote_orch_on_userspace_report(pid_t pid, uid_t uid)
{
    // ... validate and update state ...
    pr_info("zygote_orch: [specialize-userspace] pid=%d uid=%u\n", pid, uid);
    // No netlink emit - userspace already knows
}
```

### 4. Cleanup

Removed:
- `__NR_setresuid` syscall hook registration
- `ksu_hook_setresuid` function
- Syscall event bridge for setresuid

## Testing Checklist

- [ ] Verify kernel detection path works (LSM hook triggers on UID change)
- [ ] Verify userspace reporting path works (netlink message received)
- [ ] Test both paths independently
- [ ] Ensure specialization is detected for all UID change methods
- [ ] Check logs for `[specialize-kernel]` and `[specialize-userspace]` markers
- [ ] Verify isolated processes (UID 90000-99999) are correctly skipped

## Future Work

Once userspace integration is complete:
- Monitor which path is more reliable in production
- Consider removing kernel detection if userspace reporting is 100% reliable
- Add metrics to track which detection method is used

## Compatibility

**Kernel:** No ABI changes to existing netlink events (only added new event type)
**Userspace:** Needs to be updated to send `YZ_EV_REPORT_SPECIALIZE` events
