# Testing Guide: Dual-Path Specialization Detection

## Overview

This guide explains how to test the dual-path app specialization detection system:
1. **Userspace reporting** (primary) - zygisk core reports via netlink
2. **Kernel LSM hook** (fallback) - kernel detects UID changes

## Prerequisites

- Kernel module with commits `598e5ea` and later
- Userspace zygisk core with commit `8fc198b` and later
- Root access via adb
- Basic understanding of dmesg

## Test 1: Verify Both Paths Work

### Step 1: Enable kernel logging
```bash
adb shell
su
# Clear dmesg buffer
dmesg -c > /dev/null
```

### Step 2: Launch a test app
```bash
# Launch any app (e.g., Settings)
am start -n com.android.settings/.Settings

# Wait 2 seconds for specialization to complete
sleep 2
```

### Step 3: Check kernel logs
```bash
dmesg | grep -E "zygote_orch|zygote_nl"
```

### Expected Output (Success):
```
[  123.456789] zygote_nl: userspace report pid=12345 uid=10086
[  123.456790] zygote_orch: [specialize-userspace] pid=12345 uid=10086 appid=10086
```

**OR** (if userspace reporting failed):
```
[  123.456789] zygote_orch: [specialize-kernel] pid=12345 uid=10086 appid=10086
[  123.456790] zygote_nl: zygote_nl: kernel -> userspace: pid=12345 appid=10086
```

### Interpretation:
- `[specialize-userspace]` = userspace report received (primary path working)
- `[specialize-kernel]` = detected via LSM hook (fallback path working)
- Both paths should NOT trigger for the same event (userspace report takes priority)

---

## Test 2: Verify Userspace Priority

This test confirms that userspace reporting takes precedence over kernel detection.

### Setup:
```bash
# Monitor logs in real-time
adb shell "su -c 'dmesg -w | grep zygote_orch'"
```

### Action:
Launch multiple apps in quick succession:
```bash
am start -n com.android.settings/.Settings
sleep 1
am start -n com.android.calculator2/.Calculator
sleep 1
am start -n com.android.deskclock/.DeskClock
```

### Expected Output:
All specializations should show `[specialize-userspace]`:
```
zygote_orch: [specialize-userspace] pid=12345 uid=10086 appid=10086
zygote_orch: [specialize-userspace] pid=12346 uid=10087 appid=10087
zygote_orch: [specialize-userspace] pid=12347 uid=10088 appid=10088
```

**If you see `[specialize-kernel]`:** Userspace reporting failed for that app (injection issue).

---

## Test 3: Fallback Path (Simulate Injection Failure)

This test verifies that kernel detection works when userspace injection fails.

### Method 1: Disable YukiZygisk temporarily
```bash
# Disable via feature flag (if supported)
echo 0 > /sys/kernel/ksu/yukizygisk/enabled

# Launch app
am start -n com.android.settings/.Settings

# Check logs - should see kernel detection
dmesg | tail -10 | grep zygote_orch
```

**Expected:** `[specialize-kernel]` appears

### Method 2: Block netlink socket (advanced)
```bash
# This requires modifying zygisk core or using SELinux to block netlink
# Not recommended for normal testing
```

---

## Test 4: Isolated Process Handling

Verify that isolated processes (UID 90000-99999) are correctly skipped.

### Setup:
```bash
# Find an isolated process (e.g., WebView renderer)
ps -A | grep isolated
```

### Check logs:
```bash
dmesg | grep "90[0-9][0-9][0-9]"
```

**Expected:** No specialization logs for isolated UIDs.

---

## Test 5: Netlink Communication

Verify the netlink channel is functional.

### Check kernel netlink socket:
```bash
# Should show YZ_NETLINK_PROTO (27) is registered
cat /proc/net/netlink
# Look for proto 27
```

### Check userspace can send:
```bash
# Launch app and immediately check logs
am start -n com.android.settings/.Settings & dmesg | tail -20
```

**Expected:**
```
zygote_nl: userspace report pid=X uid=Y
```

**If missing:** Netlink socket creation failed in userspace.

---

## Troubleshooting

### Issue: No logs appear at all

**Cause:** YukiZygisk not enabled or kernel module not loaded

**Fix:**
```bash
# Check module loaded
lsmod | grep ksu

# Check feature enabled
cat /sys/kernel/ksu/yukizygisk/enabled
# Should output: 1
```

### Issue: Only `[specialize-kernel]` appears, never `[specialize-userspace]`

**Cause:** Userspace reporting broken

**Debug:**
1. Check zygisk core is injected: `cat /proc/<app_pid>/maps | grep zygisk`
2. Check netlink socket creation: Add debug logs to `report_specialization_to_kernel()`
3. Check SELinux denials: `dmesg | grep avc | grep netlink`

### Issue: Both `[specialize-kernel]` and `[specialize-userspace]` appear for same event

**Cause:** Race condition or logic bug

**Fix:** This should not happen. If it does, there's a bug in the orchestrator logic.

---

## Performance Testing

### Measure reporting latency:
```bash
# Add timestamps to kernel logs
dmesg -T | grep zygote_orch
```

**Expected:** Userspace report should appear within ~1ms of app launch.

### Check overhead:
```bash
# Before: Launch 10 apps and time it
time for i in {1..10}; do
  am start -n com.android.settings/.Settings
  am force-stop com.android.settings
done
```

**Expected:** No noticeable overhead (<5ms per app).

---

## Success Criteria

✅ **All tests pass if:**
1. Userspace reporting (`[specialize-userspace]`) appears for normal apps
2. Kernel detection (`[specialize-kernel]`) appears when injection fails
3. Isolated processes are skipped
4. No duplicate detections for the same event
5. Netlink channel is functional
6. Negligible performance impact

---

## Quick Test Script

```bash
#!/system/bin/sh
# Save as /data/local/tmp/test_specialization.sh

echo "=== Testing Dual-Path Specialization Detection ==="
dmesg -c > /dev/null

echo "Launching test app..."
am start -n com.android.settings/.Settings
sleep 2

echo ""
echo "=== Kernel Logs ==="
dmesg | grep -E "zygote_orch|zygote_nl" | tail -5

echo ""
echo "=== Result ==="
if dmesg | grep -q "specialize-userspace"; then
  echo "✓ Userspace reporting: WORKING"
else
  echo "✗ Userspace reporting: FAILED"
fi

if dmesg | grep -q "specialize-kernel"; then
  echo "✓ Kernel detection: WORKING"
else
  echo "- Kernel detection: Not triggered (userspace worked)"
fi

am force-stop com.android.settings
```

**Run with:**
```bash
adb push test_specialization.sh /data/local/tmp/
adb shell "su -c 'sh /data/local/tmp/test_specialization.sh'"
```
