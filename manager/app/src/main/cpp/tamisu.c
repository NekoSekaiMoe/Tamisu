//
//

#include <android/log.h>
#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tamisu.h"
#include "prelude.h"

static int fd = -1;

static inline int scan_driver_fd() {
  const char *kName = "[tamisu]";
  DIR *fd_dir = opendir("/proc/self/fd");
  if (!fd_dir) {
    return -1;
  }

  int found = -1;
  struct dirent *de;
  char path[64];
  char target[PATH_MAX];

  while ((de = readdir(fd_dir)) != NULL) {
    if (de->d_name[0] == '.') {
      continue;
    }

    char *endptr = nullptr;
    long fd_long = strtol(de->d_name, &endptr, 10);
    if (!de->d_name[0] || *endptr != '\0' || fd_long < 0 || fd_long > INT_MAX) {
      continue;
    }

    snprintf(path, sizeof(path), "/proc/self/fd/%s", de->d_name);
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n < 0) {
      continue;
    }
    target[n] = '\0';

    const char *base = strrchr(target, '/');
    base = base ? base + 1 : target;

    if (strstr(base, kName)) {
      found = (int)fd_long;
      break;
    }
  }

  closedir(fd_dir);
  return found;
}

static int tamisuctl(unsigned long op, void *arg) {
  if (fd < 0) {
    fd = scan_driver_fd();
  }
  return ioctl(fd, op, arg);
}

static struct tamisu_get_info_cmd g_version = {0};

struct tamisu_get_info_cmd get_info() {
  if (!g_version.version) {
    tamisuctl(TAMISU_IOCTL_GET_INFO, &g_version);
  }
  return g_version;
}

uint32_t get_version() {
  auto info = get_info();
  return info.version;
}

uint32_t get_uapi_version() {
  uint32_t v = 0;
  tamisuctl(TAMISU_IOCTL_GET_UAPI_VERSION, &v);
  return v;
}

bool is_safe_mode() {
  struct tamisu_check_safemode_cmd cmd = {};
  if (tamisuctl(TAMISU_IOCTL_CHECK_SAFEMODE, &cmd) == 0) {
    return cmd.in_safe_mode;
  }
  return false;
}

void get_full_version(char *buff) {
  struct tamisu_get_full_version_cmd cmd = {};
  if (tamisuctl(TAMISU_IOCTL_GET_FULL_VERSION, &cmd) == 0) {
    strcpy(buff, cmd.version_full);
  } else {
    strcpy(buff, "unknown");
  }
}

void get_hook_type(char *hook_type) {
  struct tamisu_hook_type_cmd cmd = {};
  if (tamisuctl(TAMISU_IOCTL_HOOK_TYPE, &cmd) == 0) {
    strcpy(hook_type, cmd.hook_type);
  } else {
    strcpy(hook_type, "unknown");
  }
}

bool tamisu_driver_present(void) {
  if (fd < 0) {
    fd = scan_driver_fd();
  }
  return fd >= 0;
}
