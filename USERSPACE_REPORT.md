# Userspace Specialization Reporting Implementation

## Overview

Added netlink-based reporting from zygisk core to kernel for app specialization events.

## Changes Made

### File: `userspace/zygisk/core/src/core.cpp`

**1. Added netlink header:**
```cpp
#include <linux/netlink.h>
```

**2. Implemented `report_specialization_to_kernel()` function:**
```cpp
static void report_specialization_to_kernel(pid_t pid, uid_t uid) {
  // Create netlink socket with YZ_NETLINK_PROTO
  // Send YZ_EV_REPORT_SPECIALIZE event to kernel
  // Event contains: pid, uid (full uid, kernel extracts appid)
}
```

**3. Integrated into `run_app_post_impl()`:**
```cpp
void run_app_post_impl(const zygisk::AppSpecializeArgs *args) {
  // Report to kernel BEFORE running module hooks
  report_specialization_to_kernel(getpid(), args->uid);
  
  // Then run module postAppSpecialize hooks...
}
```

## Protocol Details

**Message Structure:**
```c
struct {
  nlmsghdr hdr;
  yz_event ev;
} msg;
```

**Fields:**
- `hdr.nlmsg_type = YZ_NL_MSG_EVENT`
- `ev.type = YZ_EV_REPORT_SPECIALIZE`
- `ev.pid = getpid()`
- `ev.appid = args->uid` (full UID, kernel extracts appid % 100000)

**Kernel Handler:** `ksu_zygote_nl_recv()` in `kernel/feature/zygote_nl.c`

## Error Handling

- Socket creation failure: logged, continues without reporting
- Sendto failure: logged, continues without reporting
- Non-blocking: does not fail specialization if reporting fails

## Timing

Report is sent **immediately after specialization**, before:
- Module `postAppSpecialize` hooks
- Injection hiding
- Maps spoofing

This ensures kernel gets notification as early as possible.

## Testing

To verify:
1. Build and flash kernel + zygisk core
2. Launch any app
3. Check dmesg for:
   ```
   zygote_nl: userspace report pid=<pid> uid=<uid>
   zygote_orch: [specialize-userspace] pid=<pid> uid=<uid> appid=<appid>
   ```
4. Compare with kernel-detected events (marked `[specialize-kernel]`)

## Benefits

- **More reliable**: Direct report from the process itself
- **Earlier detection**: Reports immediately, not waiting for UID change
- **Redundancy**: Kernel LSM hook provides fallback if injection fails
- **Debugging**: Can identify injection failures (kernel detects but no userspace report)
