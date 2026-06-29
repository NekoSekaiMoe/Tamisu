/* SPDX-License-Identifier: GPL-3.0 */
/*
 * Tamisu - logging.
 *
 * NEVER logcat: app-side detectors read logcat and would see our tags. Logs go
 * to dmesg (/dev/kmsg) -- but only via zygiskd (the root daemon), since the
 * zygote/app domain can't write the kernel ring buffer (an avc denial would
 * itself be a tell). Gated on tamisu_config's dmesg_log; the default is a
 * silent no-op with zero cost.
 *
 * tamisu_klog's strong definition is in core.cpp (formats + forwards to
 * zygiskd). It is declared weak so the loader build of solist.cpp -- which has
 * no zygiskd channel -- links it as null; the macros then skip the call.
 *
 * Author: Anatdx
 */
#pragma once

extern "C" __attribute__((weak, format(printf, 1, 2))) void
tamisu_klog(const char *fmt, ...);

#define TAMISU_LOG(...)                                                         \
  do {                                                                         \
    if (tamisu_klog != nullptr)                                                \
      tamisu_klog(__VA_ARGS__);                                                \
  } while (0)

#define ZLOGE(...) TAMISU_LOG(__VA_ARGS__)
#define ZLOGW(...) TAMISU_LOG(__VA_ARGS__)
#define ZLOGI(...) TAMISU_LOG(__VA_ARGS__)
#define ZLOGD(...) TAMISU_LOG(__VA_ARGS__)
