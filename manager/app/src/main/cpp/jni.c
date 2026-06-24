#include "ksu.h"
#include "prelude.h"

#include <android/log.h>
#include <errno.h>
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

NativeBridgeNP(getVersion, jint) { return (jint)get_version(); }

NativeBridgeNP(getUapiVersion, jint) { return (jint)get_uapi_version(); }

// get VERSION FULL
NativeBridgeNP(getFullVersion, jstring) {
  char buff[255] = {0};
  get_full_version((char *)&buff);
  return GetEnvironment()->NewStringUTF(env, buff);
}

NativeBridgeNP(isSafeMode, jboolean) { return is_safe_mode(); }

// Get HOOK type
NativeBridgeNP(getHookType, jstring) {
  char hook_type[32] = {0};
  get_hook_type((char *)&hook_type);
  return GetEnvironment()->NewStringUTF(env, hook_type);
}

NativeBridgeNP(isKsuDriverPresent, jboolean) { return ksu_driver_present(); }

// --- YukiZygisk injection-status query ---
#define YZ_REQ_GET_STATUS 7 /* == zygiskd::Request::GetStatus (zygiskd.hpp) */
#define YZ_STATUS_MAX (1u << 20)

#if defined(__LP64__)
#define YZ_ZYGISKD_SOCKET "zygiskd64"
#else
#define YZ_ZYGISKD_SOCKET "zygiskd32"
#endif

static bool yz_read_full(int fd, void *buf, size_t n) {
  uint8_t *p = (uint8_t *)buf;
  while (n > 0) {
    ssize_t r = read(fd, p, n);
    if (r <= 0) {
      return false;
    }
    p += r;
    n -= (size_t)r;
  }
  return true;
}

NativeBridgeNP(yzQueryStatus, jstring) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return NULL;
  }

  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  size_t nlen = strlen(YZ_ZYGISKD_SOCKET);
  memcpy(addr.sun_path + 1, YZ_ZYGISKD_SOCKET, nlen); // abstract: leading NUL
  socklen_t alen =
      (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nlen);
  if (connect(fd, (struct sockaddr *)&addr, alen) != 0) {
    close(fd);
    return NULL;
  }

  uint8_t op = YZ_REQ_GET_STATUS;
  uint32_t len = 0;
  if (write(fd, &op, 1) != 1 || !yz_read_full(fd, &len, sizeof(len)) ||
      len == 0 || len > YZ_STATUS_MAX) {
    close(fd); // len 0 == denied (not the manager) or empty
    return NULL;
  }

  char *buf = (char *)malloc(len + 1);
  if (buf == NULL) {
    close(fd);
    return NULL;
  }
  jstring out = NULL;
  if (yz_read_full(fd, buf, len)) {
    buf[len] = '\0';
    out = GetEnvironment()->NewStringUTF(env, buf);
  }
  free(buf);
  close(fd);
  return out;
}
