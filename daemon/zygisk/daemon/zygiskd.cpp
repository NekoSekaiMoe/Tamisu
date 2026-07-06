/* SPDX-License-Identifier: GPL-3.0 */
/*
 * Tamisu Zygisk daemon.
 *
 * Author: Anatdx
 */

#include "zygiskd.h"
#include "uapi/zygisk.h"

#include "core/json.h"
#include "core/restorecon.h"
#include "defs.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <poll.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <dlfcn.h>
#include <elf.h>
#include <pthread.h>

#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits.h>
#include <sstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace tamisu_daemon {
int tamisuctl(int request, void *arg);
bool uid_granted_root(uint32_t uid);
bool uid_should_umount(uint32_t uid);
} // namespace tamisu_daemon

namespace {

[[gnu::format(printf, 1, 2)]] void dlog(const char *fmt, ...) {
  static int kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
  if (kmsg < 0)
    return;

  char buf[256];
  int n = snprintf(buf, sizeof(buf), "<6>zygiskd: ");
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
  va_end(ap);
  if (m < 0)
    return;

  size_t len = static_cast<size_t>(n) + static_cast<size_t>(m);
  if (len >= sizeof(buf))
    len = sizeof(buf) - 1;
  ssize_t w = write(kmsg, buf, len);
  (void)w;
}
#define DLOGE(...) dlog(__VA_ARGS__)
#define DLOGI(...) dlog(__VA_ARGS__)

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif // #ifndef MFD_CLOEXEC

#if defined(__aarch64__)
constexpr char kAbi[] = "arm64-v8a";
#elif defined(__arm__)
constexpr char kAbi[] = "armeabi-v7a";
#elif defined(__x86_64__)
constexpr char kAbi[] = "x86_64";
#elif defined(__i386__)
constexpr char kAbi[] = "x86";
#else
constexpr char kAbi[] = "unknown";
#endif // #if defined(__aarch64__)

constexpr char kModulesDir[] = "/data/adb/modules";

struct Module {
  std::string name;
  std::string lib_path; // <id>/zygisk/<abi>.so
};

/* Per-module zygote grant whitelist, mirrored from yzconfig.json
 * ("zygote_modules": [...]). When grant_filter_active is set, only modules
 * whose dir-id appears here are loaded into zygote. The set is reparsed on
 * every read_yzconfig() call so manager edits apply to the next specialize. */
std::set<std::string> g_granted_modules;
bool g_grant_filter_active = false;

/* Per-module uid scope, mirrored from yzconfig.json
 * ("module_scopes": {"<dir-id>": [<uid>, ...] | "*"}). An empty entry means
 * "all uids" (back-compat with zygote_modules-only config). A non-empty entry
 * means the module loads only for the listed uids. Absent modules fall back to
 * the global g_granted_modules test (or are loaded unconditionally if the
 * grant filter is off). */
struct ModuleScope {
  bool wildcard = true;          // true = all uids
  std::set<uint32_t> uids;       // populated when wildcard == false
};
std::map<std::string, ModuleScope> g_module_scopes;

std::vector<Module> g_modules;

struct NativeModule {
  std::string module_id;
  uint8_t target_type = 0;
  std::string target;
  std::string lib_path;
  bool has_companion = false;
};

std::vector<NativeModule> g_native_modules;

int consume_ready_fd() {
  const char *env = getenv("YUKIZYGISK_READY_FD");
  if (env == nullptr || *env == '\0')
    return -1;

  errno = 0;
  char *end = nullptr;
  long fd = strtol(env, &end, 10);
  unsetenv("YUKIZYGISK_READY_FD");
  if (errno || end == env || *end != '\0' || fd < 0 || fd > INT32_MAX)
    return -1;
  return static_cast<int>(fd);
}

void notify_ready(int fd, bool ok) {
  if (fd < 0)
    return;
  const char byte = ok ? '1' : '0';
  ssize_t w = write(fd, &byte, 1);
  (void)w;
  close(fd);
}

/* Enabled zygisk modules for this ABI. */
std::vector<Module> scan_modules() {
  std::vector<Module> mods;
  DIR *d = opendir(kModulesDir);
  if (d == nullptr)
    return mods;

  while (dirent *e = readdir(d)) {
    if (e->d_name[0] == '.')
      continue;
    std::string base = std::string(kModulesDir) + "/" + e->d_name;
    if (access((base + "/disable").c_str(), F_OK) == 0)
      continue;
    std::string lib = base + "/zygisk/" + kAbi + ".so";
    if (access(lib.c_str(), F_OK) != 0)
      continue;
    mods.push_back(Module{e->d_name, std::move(lib)});
  }
  closedir(d);
  return mods;
}

std::string trim_copy(const std::string &s) {
  size_t first = 0;
  while (first < s.size() &&
         std::isspace(static_cast<unsigned char>(s[first])) != 0)
    ++first;
  size_t last = s.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(s[last - 1])) != 0)
    --last;
  return s.substr(first, last - first);
}

void replace_all(std::string &s, const std::string &from,
                 const std::string &to) {
  if (from.empty())
    return;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

bool parse_native_module_line(const std::string &module_id,
                              const std::string &base, const std::string &line,
                              NativeModule *out) {
  std::string text = trim_copy(line);
  if (text.empty() || text[0] == '#')
    return false;

  std::istringstream iss(text);
  std::string head;
  if (!(iss >> head))
    return false;

  NativeModule m{};
  m.module_id = module_id;
  constexpr char kNamePrefix[] = "name=";
  constexpr char kPathPrefix[] = "path=";
  if (head.rfind(kNamePrefix, 0) == 0) {
    m.target_type = YZ_NATIVE_TARGET_NAME;
    m.target = head.substr(sizeof(kNamePrefix) - 1);
  } else if (head.rfind(kPathPrefix, 0) == 0) {
    m.target_type = YZ_NATIVE_TARGET_PATH;
    m.target = head.substr(sizeof(kPathPrefix) - 1);
  } else {
    return false;
  }
  if (m.target.empty() || m.target.size() >= YZ_NATIVE_TARGET_VALUE_MAX)
    return false;

  std::string token;
  std::string lib_rel;
  while (iss >> token) {
    if (token == "companion") {
      m.has_companion = true;
    } else if (lib_rel.empty()) {
      lib_rel = token;
    }
  }
  if (lib_rel.empty())
    return false;

  replace_all(lib_rel, "${moduleId}", module_id);
  replace_all(lib_rel, "$moduleId", module_id);
  if (!lib_rel.empty() && lib_rel[0] == '/')
    m.lib_path = lib_rel;
  else
    m.lib_path = base + "/" + lib_rel;

  if (access(m.lib_path.c_str(), F_OK) != 0)
    return false;
  *out = std::move(m);
  return true;
}

std::vector<NativeModule> scan_native_modules() {
  std::vector<NativeModule> mods;
  DIR *d = opendir(kModulesDir);
  if (d == nullptr)
    return mods;

  while (dirent *e = readdir(d)) {
    if (e->d_name[0] == '.')
      continue;
    std::string module_id = e->d_name;
    std::string base = std::string(kModulesDir) + "/" + module_id;
    if (access((base + "/disable").c_str(), F_OK) == 0)
      continue;

    std::ifstream f(base + "/zn_modules.txt");
    if (!f.is_open())
      continue;
    std::string line;
    while (std::getline(f, line)) {
      NativeModule m{};
      if (parse_native_module_line(module_id, base, line, &m)) {
        if (!tamisu_daemon::lsetfilecon(m.lib_path, tamisu_daemon::SYSTEM_LIB_CON))
          DLOGE("native module: failed to label lib=%s", m.lib_path.c_str());
        DLOGI("native module: id=%s target=%s%s lib=%s companion=%u",
              m.module_id.c_str(),
              m.target_type == YZ_NATIVE_TARGET_PATH ? "path=" : "name=",
              m.target.c_str(), m.lib_path.c_str(), m.has_companion ? 1 : 0);
        mods.push_back(std::move(m));
      } else if (!trim_copy(line).empty() && trim_copy(line)[0] != '#') {
        DLOGI("native module: ignored invalid line in %s: %s",
              module_id.c_str(), trim_copy(line).c_str());
      }
    }
  }
  closedir(d);
  return mods;
}

void publish_native_targets() {
  yz_native_targets_cmd cmd{};
  for (const auto &m : g_native_modules) {
    if (cmd.count >= YZ_NATIVE_TARGET_MAX)
      break;
    yz_native_target &t = cmd.targets[cmd.count++];
    t.type = m.target_type;
    snprintf(t.value, sizeof(t.value), "%s", m.target.c_str());
  }
  int ret = tamisu_daemon::tamisuctl(TAMISU_IOCTL_YZ_SET_NATIVE_TARGETS, &cmd);
  if (ret == 0) {
    DLOGI("native targets: %u module(s), kernel ret=0", cmd.count);
  } else {
    DLOGI("native targets: %u module(s), kernel ret=%d errno=%d (%s)",
          cmd.count, ret, errno, strerror(errno));
  }
}

void rescan_modules() {
  g_modules = scan_modules();
  g_native_modules = scan_native_modules();
  publish_native_targets();
  DLOGI("found %zu zygisk module(s), %zu native module(s) for %s",
        g_modules.size(), g_native_modules.size(), kAbi);
}

bool read_exact(int fd, void *buf, size_t n) {
  auto *p = static_cast<uint8_t *>(buf);
  while (n > 0) {
    ssize_t r = read(fd, p, n);
    if (r <= 0)
      return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

bool write_exact(int fd, const void *buf, size_t n) {
  const auto *p = static_cast<const uint8_t *>(buf);
  while (n > 0) {
    ssize_t r = write(fd, p, n);
    if (r <= 0)
      return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

/* Send one fd via SCM_RIGHTS. */
bool send_fd(int sock, int fd) {
  msghdr msg{};
  iovec io{};
  char dummy = '!';
  io.iov_base = &dummy;
  io.iov_len = 1;
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;

  char cbuf[CMSG_SPACE(sizeof(int))] = {};
  if (fd >= 0) {
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);
    cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
  }
  return sendmsg(sock, &msg, MSG_NOSIGNAL) >
         0; // EPIPE not SIGPIPE on dead client
}

int copy_file_to_memfd(const std::string &path) {
  int src = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (src < 0) {
    DLOGE("native module memfd: open failed path=%s err=%s", path.c_str(),
          strerror(errno));
    return -1;
  }

  struct stat st{};
  if (fstat(src, &st) != 0 || st.st_size <= 0 || !S_ISREG(st.st_mode)) {
    DLOGE("native module memfd: invalid source path=%s err=%s", path.c_str(),
          strerror(errno));
    close(src);
    return -1;
  }

  int mfd = static_cast<int>(
      syscall(__NR_memfd_create, "data-code-cache", MFD_CLOEXEC));
  if (mfd < 0) {
    DLOGE("native module memfd: memfd_create failed path=%s err=%s",
          path.c_str(), strerror(errno));
    close(src);
    return -1;
  }

  if (ftruncate(mfd, st.st_size) != 0) {
    DLOGE("native module memfd: ftruncate failed size=%lld err=%s",
          static_cast<long long>(st.st_size), strerror(errno));
    close(mfd);
    close(src);
    return -1;
  }

  std::vector<uint8_t> buf(64 * 1024);
  while (true) {
    ssize_t r = read(src, buf.data(), buf.size());
    if (r == 0)
      break;
    if (r < 0) {
      if (errno == EINTR)
        continue;
      DLOGE("native module memfd: read failed path=%s err=%s", path.c_str(),
            strerror(errno));
      close(mfd);
      close(src);
      return -1;
    }

    const uint8_t *p = buf.data();
    size_t left = static_cast<size_t>(r);
    while (left > 0) {
      ssize_t w = write(mfd, p, left);
      if (w < 0) {
        if (errno == EINTR)
          continue;
        DLOGE("native module memfd: write failed path=%s err=%s", path.c_str(),
              strerror(errno));
        close(mfd);
        close(src);
        return -1;
      }
      if (w == 0) {
        DLOGE("native module memfd: short write path=%s", path.c_str());
        close(mfd);
        close(src);
        return -1;
      }
      p += w;
      left -= static_cast<size_t>(w);
    }
  }

  close(src);
  if (lseek(mfd, 0, SEEK_SET) < 0) {
    DLOGE("native module memfd: rewind failed err=%s", strerror(errno));
    close(mfd);
    return -1;
  }

  DLOGI("native module memfd: staged size=%lld fd=%d",
        static_cast<long long>(st.st_size), mfd);
  return mfd;
}

/* Receive one fd via SCM_RIGHTS. */
int recv_fd(int sock) {
  char data = 0;
  char cbuf[CMSG_SPACE(sizeof(int))] = {};
  iovec io{&data, 1};
  msghdr msg{};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = cbuf;
  msg.msg_controllen = sizeof(cbuf);
  if (recvmsg(sock, &msg, 0) <= 0)
    return -1;
  for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c))
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int fd = -1;
      memcpy(&fd, CMSG_DATA(c), sizeof(fd));
      return fd;
    }
  return -1;
}

using companion_entry_fn = void (*)(int);

struct CompanionJob {
  companion_entry_fn fn;
  int client;
};

void *companion_thread(void *p) {
  auto *job = static_cast<CompanionJob *>(p);
  job->fn(job->client);
  close(job->client);
  delete job;
  return nullptr;
}

[[noreturn]] void companion_main(const std::string &lib_path, int ctrl) {
  // Drop daemon fds.
  if (DIR *fdd = opendir("/proc/self/fd")) {
    int dfd = dirfd(fdd);
    while (dirent *e = readdir(fdd)) {
      int fd = atoi(e->d_name);
      if (fd > 2 && fd != ctrl && fd != dfd)
        close(fd);
    }
    closedir(fdd);
  }
  void *h = dlopen(lib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  auto fn = h ? reinterpret_cast<companion_entry_fn>(
                    dlsym(h, "zygisk_companion_entry"))
              : nullptr;
  uint8_t ready = fn != nullptr ? 1 : 0;
  if (write(ctrl, &ready, 1) != 1)
    _exit(0);
  for (;;) {
    int client = recv_fd(ctrl);
    if (client < 0)
      _exit(0); // daemon gone
    if (fn == nullptr) {
      close(client);
      continue;
    }
    auto *job = new CompanionJob{fn, client};
    pthread_t t;
    if (pthread_create(&t, nullptr, companion_thread, job) == 0)
      pthread_detach(t);
    else {
      close(client);
      delete job;
    }
  }
}

struct Companion {
  pid_t pid = -1;
  int ctrl = -1;
  bool has_entry = false;
};
std::vector<Companion> g_companions; // indexed like g_modules, spawned lazily
constexpr int kCompanionReadyMs = 5000; // bound on a companion's startup

bool ensure_companion(uint32_t idx) {
  if (idx >= g_modules.size())
    return false;
  if (g_companions.size() != g_modules.size())
    g_companions.resize(g_modules.size());
  Companion &c = g_companions[idx];
  if (c.pid > 0)
    return c.has_entry;

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
    return false;
  pid_t pid = fork();
  if (pid < 0) {
    close(sv[0]);
    close(sv[1]);
    return false;
  }
  if (pid == 0) {
    close(sv[0]);
    companion_main(g_modules[idx].lib_path, sv[1]); // never returns
  }
  close(sv[1]);

  pollfd pfd{sv[0], POLLIN, 0};
  uint8_t ready = 0;
  if (poll(&pfd, 1, kCompanionReadyMs) != 1 || !(pfd.revents & POLLIN) ||
      !read_exact(sv[0], &ready, 1)) {
    DLOGE("companion for '%s' pid=%d not ready in %dms; killing",
          g_modules[idx].name.c_str(), pid, kCompanionReadyMs);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    close(sv[0]);
    return false;
  }

  c.pid = pid;
  c.ctrl = sv[0];
  c.has_entry = (ready == 1);
  DLOGI("companion for '%s' pid=%d entry=%d", g_modules[idx].name.c_str(), pid,
        c.has_entry);
  return c.has_entry;
}

struct ZygiskNextCompanionModule {
  int target_api_version;
  void (*onCompanionLoaded)();
  void (*onModuleConnected)(int fd);
};

struct NativeCompanionJob {
  void (*fn)(int);
  int client;
};

void *native_companion_thread(void *p) {
  auto *job = static_cast<NativeCompanionJob *>(p);
  job->fn(job->client);
  delete job;
  return nullptr;
}

[[noreturn]] void native_companion_main(const std::string &lib_path, int ctrl) {
  if (DIR *fdd = opendir("/proc/self/fd")) {
    int dfd = dirfd(fdd);
    while (dirent *e = readdir(fdd)) {
      int fd = atoi(e->d_name);
      if (fd > 2 && fd != ctrl && fd != dfd)
        close(fd);
    }
    closedir(fdd);
  }

  void *h = dlopen(lib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  auto *mod = h ? reinterpret_cast<ZygiskNextCompanionModule *>(
                      dlsym(h, "zn_companion_module"))
                : nullptr;
  bool valid = mod != nullptr && mod->target_api_version == 3;
  if (valid && mod->onCompanionLoaded != nullptr)
    mod->onCompanionLoaded();

  uint8_t ready = valid && mod->onModuleConnected != nullptr ? 1 : 0;
  if (write(ctrl, &ready, 1) != 1)
    _exit(0);
  for (;;) {
    int client = recv_fd(ctrl);
    if (client < 0)
      _exit(0);
    if (!ready) {
      close(client);
      continue;
    }
    auto *job = new NativeCompanionJob{mod->onModuleConnected, client};
    pthread_t t;
    if (pthread_create(&t, nullptr, native_companion_thread, job) == 0)
      pthread_detach(t);
    else {
      close(client);
      delete job;
    }
  }
}

std::vector<Companion> g_native_companions;

bool ensure_native_companion(uint32_t idx) {
  if (idx >= g_native_modules.size() || !g_native_modules[idx].has_companion)
    return false;
  if (g_native_companions.size() != g_native_modules.size())
    g_native_companions.resize(g_native_modules.size());
  Companion &c = g_native_companions[idx];
  if (c.pid > 0)
    return c.has_entry;

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
    return false;
  pid_t pid = fork();
  if (pid < 0) {
    close(sv[0]);
    close(sv[1]);
    return false;
  }
  if (pid == 0) {
    close(sv[0]);
    native_companion_main(g_native_modules[idx].lib_path, sv[1]);
  }
  close(sv[1]);

  pollfd pfd{sv[0], POLLIN, 0};
  uint8_t ready = 0;
  if (poll(&pfd, 1, kCompanionReadyMs) != 1 || !(pfd.revents & POLLIN) ||
      !read_exact(sv[0], &ready, 1)) {
    DLOGE("native companion for '%s' pid=%d not ready in %dms; killing",
          g_native_modules[idx].module_id.c_str(), pid, kCompanionReadyMs);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    close(sv[0]);
    return false;
  }

  c.pid = pid;
  c.ctrl = sv[0];
  c.has_entry = (ready == 1);
  DLOGI("native companion for '%s' pid=%d entry=%d",
        g_native_modules[idx].module_id.c_str(), pid, c.has_entry);
  return c.has_entry;
}

static bool yz_mi_parse(const std::string &line, std::string &root,
                        std::string &target, std::string &source) {
  std::istringstream iss(line);
  std::vector<std::string> tok;
  std::string t;
  while (iss >> t)
    tok.push_back(t);
  if (tok.size() < 7)
    return false;
  size_t dash = 0;
  bool found = false;
  for (size_t i = 5; i < tok.size(); ++i)
    if (tok[i] == "-") {
      dash = i;
      found = true;
      break;
    }
  if (!found || dash + 2 >= tok.size())
    return false;
  root = tok[3];
  target = tok[4];
  source = tok[dash + 2]; /* dash+1 = fstype, dash+2 = mount source */
  return true;
}

static void yz_umount_root_in_ns() {
  std::ifstream f("/proc/self/mountinfo");
  if (!f.is_open())
    return;
  std::vector<std::string> targets;
  std::string line;
  while (std::getline(f, line)) {
    std::string root, target, source;
    if (!yz_mi_parse(line, root, target, source))
      continue;
    bool should = source == "TAMISU" || source == "magisk" || source == "APatch" ||
                  target.rfind("/data/adb/modules", 0) == 0 ||
                  root.rfind("/adb/modules/", 0) == 0;
    if (should)
      targets.push_back(target);
  }
  for (auto it = targets.rbegin(); it != targets.rend(); ++it)
    umount2(it->c_str(), MNT_DETACH);
}

static bool yz_revert_app_mounts(pid_t app_pid) {
  if (app_pid <= 0)
    return false;
  pid_t child = fork();
  if (child < 0)
    return false;
  if (child == 0) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/ns/mnt", app_pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      _exit(1);
    if (setns(fd, CLONE_NEWNS) != 0)
      _exit(2);
    close(fd);
    yz_umount_root_in_ns();
    _exit(0);
  }
  int st = 0;
  waitpid(child, &st, 0);
  return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

uint32_t query_flags(uint32_t uid) {
  uint32_t flags = 0;
  if (tamisu_daemon::uid_granted_root(uid))
    flags |= 1u << 0;
  if (tamisu_daemon::uid_should_umount(uid))
    flags |= 1u << 1;
  return flags;
}

yz_config g_yz_config{1, 0, 0, 0};

/* Decide whether the module with the given dir-id should load into the zygote
 * at all (global grant filter). When the grant filter is off, every enabled
 * module passes. */
bool module_globally_granted(const std::string &dir_id) {
  if (!g_grant_filter_active)
    return true;
  return g_granted_modules.count(dir_id) != 0;
}

/* Decide whether the module with the given dir-id should run for a specific
 * uid. A scope entry of "*" (or absent entry) means all uids. A bare uid list
 * restricts it to those uids. */
bool module_granted_for_uid(const std::string &dir_id, uint32_t uid) {
  auto it = g_module_scopes.find(dir_id);
  if (it == g_module_scopes.end())
    return true;
  if (it->second.wildcard)
    return true;
  return it->second.uids.count(uid) != 0;
}

void read_yzconfig() {
  yz_config cfg{1, 0, 0, 0};
  std::set<std::string> granted;
  bool grant_filter_active = false;
  std::map<std::string, ModuleScope> scopes;
  int fd = open(tamisu_daemon::YZCONFIG_PATH, O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    std::string buf;
    char tmp[1024];
    for (ssize_t n; (n = read(fd, tmp, sizeof(tmp))) > 0;)
      buf.append(tmp, static_cast<size_t>(n));
    close(fd);
    json::Value root = json::parse(buf);
    if (root.type == json::Type::Object) {
      if (root.contains("zygisk_linker"))
        cfg.zygisk_linker = root.at("zygisk_linker").as_bool() ? 1 : 0;
      if (root.contains("denylist_mode"))
        cfg.denylist_mode =
            static_cast<__u8>(root.at("denylist_mode").as_number());
      if (root.contains("dmesg_log"))
        cfg.dmesg_log = root.at("dmesg_log").as_bool() ? 1 : 0;
      /* yzconfig.json schema:
       *   "zygote_modules": ["<dir-id>", ...]
       * Presence of the key enables the per-module grant filter; absence
       * loads every enabled zygisk module (back-compat). */
      if (root.contains("zygote_modules")) {
        grant_filter_active = true;
        const auto &arr = root.at("zygote_modules");
        if (arr.type == json::Type::Array) {
          for (const auto &v : arr.a) {
            if (v.type == json::Type::String && !v.s.empty())
              granted.insert(v.s);
          }
        }
      }
      /* yzconfig.json schema:
       *   "module_scopes": {
       *     "<dir-id>": [<uid>, ...] | "*"
       *   }
       * Optional per-app scope. Absent or "*" means all uids. A bare list
       * constrains the module to those uids only. Lets the user express
       * "LSPosed only for WeChat" without a denylist. */
      if (root.contains("module_scopes")) {
        const auto &obj = root.at("module_scopes");
        if (obj.type == json::Type::Object) {
          for (const auto &kv : obj.o) {
            ModuleScope ms;
            const auto &val = kv.second;
            if (val.type == json::Type::String && val.s == "*") {
              ms.wildcard = true;
            } else if (val.type == json::Type::Array) {
              for (const auto &v : val.a) {
                if (v.type == json::Type::String && v.s == "*") {
                  ms.wildcard = true;
                  ms.uids.clear();
                  break;
                } else if (v.type == json::Type::Number) {
                  ms.uids.insert(static_cast<uint32_t>(v.n));
                }
              }
              if (!ms.wildcard && !ms.uids.empty())
                ms.wildcard = false;
            }
            scopes[kv.first] = ms;
          }
        }
      }
    }
  }
  cfg.grant_filter_active = grant_filter_active ? 1 : 0;
  g_yz_config = cfg;
  g_granted_modules = std::move(granted);
  g_grant_filter_active = grant_filter_active;
  g_module_scopes = std::move(scopes);
  yz_zygisk_linker_cmd yc{};
  yc.enabled = cfg.zygisk_linker;
  tamisu_daemon::tamisuctl(TAMISU_IOCTL_YZ_SET_YUKILINKER, &yc);
  DLOGI("yzconfig: zygisk_linker=%u denylist_mode=%u dmesg_log=%u grant=%u "
        "(%zu module(s), %zu scoped)",
        cfg.zygisk_linker, cfg.denylist_mode, cfg.dmesg_log,
        cfg.grant_filter_active, g_granted_modules.size(),
        g_module_scopes.size());
}

bool g_safemode_active = false;
uint32_t g_safemode_crashes = 0;
std::string g_safemode_zygote;

bool g_safemode_active = false;
uint32_t g_safemode_crashes = 0;

uint64_t g_inject_count = 0;
std::deque<uint32_t> g_recent_appids;
constexpr size_t kRecentMax = 16;

struct ZygoteRecord {
  uint32_t pid;
  std::string name;
  std::string abi;
};

std::deque<ZygoteRecord> g_zygotes;
constexpr size_t kZygoteMax = 8;

struct ZygoteMonitorRecord {
  uint32_t pid;
  std::string name;
  std::string abi;
  std::string state;
};

struct NativeInjectionRecord {
  uint32_t pid;
  std::string process;
  std::string module_id;
  std::string target_type;
  std::string target;
  std::string abi;
  bool has_companion;
};

std::deque<NativeInjectionRecord> g_native_injections;
constexpr size_t kNativeInjectionMax = 16;

void record_injection(uint32_t appid) {
  ++g_inject_count;
  for (auto it = g_recent_appids.begin(); it != g_recent_appids.end(); ++it)
    if (*it == appid) {
      g_recent_appids.erase(it); // move-to-front: keep the list distinct
      break;
    }
  g_recent_appids.push_front(appid);
  if (g_recent_appids.size() > kRecentMax)
    g_recent_appids.pop_back();
}

bool parse_zygote_cmdline(pid_t pid, std::string *socket_name,
                          std::string *abi_list = nullptr) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;

  char buf[4096];
  ssize_t n = read(fd, buf, sizeof(buf));
  close(fd);
  if (n <= 0)
    return false;

  bool found = false;
  std::string sock;
  std::string abi;
  size_t off = 0;
  while (off < static_cast<size_t>(n)) {
    const char *arg = buf + off;
    size_t len = strnlen(arg, static_cast<size_t>(n) - off);
    if (len == 0) {
      ++off;
      continue;
    }
    if (strcmp(arg, "-Xzygote") == 0) {
      found = true;
    } else {
      constexpr char kSocketPrefix[] = "--socket-name=";
      constexpr size_t kSocketPrefixLen = sizeof(kSocketPrefix) - 1;
      if (len >= kSocketPrefixLen &&
          strncmp(arg, kSocketPrefix, kSocketPrefixLen) == 0)
        sock.assign(arg + kSocketPrefixLen, len - kSocketPrefixLen);
      constexpr char kAbiPrefix[] = "--abi-list=";
      constexpr size_t kAbiPrefixLen = sizeof(kAbiPrefix) - 1;
      if (len >= kAbiPrefixLen && strncmp(arg, kAbiPrefix, kAbiPrefixLen) == 0)
        abi.assign(arg + kAbiPrefixLen, len - kAbiPrefixLen);
    }
    off += len + 1;
  }

  if (socket_name != nullptr)
    *socket_name = sock.empty() ? "zygote" : sock;
  if (abi_list != nullptr)
    *abi_list = std::move(abi);
  return found;
}

std::string read_proc_exe(pid_t pid) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/exe", pid);
  char buf[PATH_MAX];
  ssize_t n = readlink(path, buf, sizeof(buf) - 1);
  if (n <= 0)
    return {};
  buf[n] = '\0';
  return buf;
}

std::string zygote_abi(pid_t pid, const std::string &abi_list) {
  if (!abi_list.empty())
    return abi_list;
  std::string exe = read_proc_exe(pid);
  if (exe.find("32") != std::string::npos)
    return "armeabi-v7a";
  if (exe.find("64") != std::string::npos)
    return kAbi;
  return "unknown";
}

bool is_32bit_abi(const std::string &abi) {
  return !abi.empty() && abi.find("64") == std::string::npos;
}

bool is_zygote_injected(uint32_t pid, const std::string &name,
                        const std::string &abi) {
  for (const auto &z : g_zygotes)
    if (z.pid == pid || (z.name == name && z.abi == abi))
      return true;
  return false;
}

std::vector<ZygoteMonitorRecord> scan_zygote_monitor() {
  std::vector<ZygoteMonitorRecord> out;
  DIR *d = opendir("/proc");
  if (d == nullptr)
    return out;

  while (dirent *e = readdir(d)) {
    if (!std::isdigit(static_cast<unsigned char>(e->d_name[0])))
      continue;
    char *end = nullptr;
    long pid_long = strtol(e->d_name, &end, 10);
    if (end == e->d_name || *end != '\0' || pid_long <= 0 ||
        pid_long > INT32_MAX)
      continue;
    pid_t pid = static_cast<pid_t>(pid_long);
    std::string name;
    std::string abi_list;
    if (!parse_zygote_cmdline(pid, &name, &abi_list))
      continue;
    std::string abi = zygote_abi(pid, abi_list);
    std::string state = "failed";
    if (is_32bit_abi(abi))
      state = "unsupported32";
    else if (is_zygote_injected(static_cast<uint32_t>(pid), name, abi))
      state = "injected";
    out.push_back(ZygoteMonitorRecord{
        static_cast<uint32_t>(pid),
        std::move(name),
        std::move(abi),
        std::move(state),
    });
  }
  closedir(d);
  return out;
}

void record_zygote(pid_t pid) {
  std::string name;
  if (!parse_zygote_cmdline(pid, &name))
    name = "zygote";

  for (auto it = g_zygotes.begin(); it != g_zygotes.end(); ++it) {
    if (it->pid == static_cast<uint32_t>(pid) ||
        (it->name == name && it->abi == kAbi)) {
      g_zygotes.erase(it);
      break;
    }
  }
  g_zygotes.push_front(
      ZygoteRecord{static_cast<uint32_t>(pid), std::move(name), kAbi});
  if (g_zygotes.size() > kZygoteMax)
    g_zygotes.pop_back();
  DLOGI("zygote injected: pid=%d name=%s abi=%s", pid,
        g_zygotes.front().name.c_str(), g_zygotes.front().abi.c_str());
}

std::string read_proc_comm(pid_t pid) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/comm", pid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return {};

  char buf[128];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return {};
  buf[n] = '\0';
  std::string comm = trim_copy(buf);
  return comm;
}

const char *native_target_type_name(uint8_t type) {
  return type == YZ_NATIVE_TARGET_PATH ? "path" : "name";
}

void record_native_injection(pid_t pid, uint32_t idx) {
  if (idx >= g_native_modules.size())
    return;

  const NativeModule &m = g_native_modules[idx];
  std::string process = read_proc_comm(pid);
  if (process.empty())
    process = m.target;
  std::string target_type = native_target_type_name(m.target_type);

  for (auto it = g_native_injections.begin(); it != g_native_injections.end();
       ++it) {
    if (it->pid == static_cast<uint32_t>(pid) && it->module_id == m.module_id) {
      g_native_injections.erase(it);
      break;
    }
  }
  g_native_injections.push_front(NativeInjectionRecord{
      static_cast<uint32_t>(pid),
      std::move(process),
      m.module_id,
      std::move(target_type),
      m.target,
      kAbi,
      m.has_companion,
  });
  if (g_native_injections.size() > kNativeInjectionMax)
    g_native_injections.pop_back();
  DLOGI("native injection: pid=%d process=%s module=%s target=%s=%s", pid,
        g_native_injections.front().process.c_str(), m.module_id.c_str(),
        native_target_type_name(m.target_type), m.target.c_str());
}

void json_append_escaped(std::string &out, const std::string &s) {
  for (char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char b[8];
        snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned char>(c));
        out += b;
      } else {
        out += c;
      }
    }
  }
}

/* Compact status JSON for the manager. */
std::string build_status_json() {
  std::string s = "{\"count\":";
  s += std::to_string(g_inject_count);
  s += ",\"zygisk_linker\":";
  s += g_yz_config.zygisk_linker ? "true" : "false";
  s += ",\"denylist_mode\":";
  s += std::to_string(g_yz_config.denylist_mode);
  s += ",\"dmesg_log\":";
  s += g_yz_config.dmesg_log ? "true" : "false";
  s += ",\"grant_filter_active\":";
  s += g_yz_config.grant_filter_active ? "true" : "false";
  s += ",\"granted_modules\":[";
  {
    bool first = true;
    for (const auto &name : g_granted_modules) {
      if (!first)
        s += ',';
      first = false;
      s += '"';
      json_append_escaped(s, name);
      s += '"';
    }
  }
  s += "]";
  s += ",\"recent\":[";
  bool first = true;
  for (uint32_t a : g_recent_appids) {
    if (!first)
      s += ',';
    first = false;
    s += std::to_string(a);
  }
  s += "],\"zygotes\":[";
  first = true;
  for (const auto &z : g_zygotes) {
    if (!first)
      s += ',';
    first = false;
    s += "{\"pid\":";
    s += std::to_string(z.pid);
    s += ",\"name\":\"";
    json_append_escaped(s, z.name);
    s += "\",\"abi\":\"";
    json_append_escaped(s, z.abi);
    s += "\"}";
  }
  s += "],\"zygote_monitor\":[";
  first = true;
  for (const auto &z : scan_zygote_monitor()) {
    if (!first)
      s += ',';
    first = false;
    s += "{\"pid\":";
    s += std::to_string(z.pid);
    s += ",\"name\":\"";
    json_append_escaped(s, z.name);
    s += "\",\"abi\":\"";
    json_append_escaped(s, z.abi);
    s += "\",\"state\":\"";
    json_append_escaped(s, z.state);
    s += "\"}";
  }
  s += "],\"modules\":[";
  first = true;
  for (const auto &m : g_modules) {
    if (!first)
      s += ',';
    first = false;
    s += '"';
    json_append_escaped(s, m.name);
    s += '"';
  }
  s += "],\"native_modules\":[";
  first = true;
  for (const auto &m : g_native_modules) {
    if (!first)
      s += ',';
    first = false;
    s += "{\"id\":\"";
    json_append_escaped(s, m.module_id);
    s += "\",\"target_type\":\"";
    json_append_escaped(s, native_target_type_name(m.target_type));
    s += "\",\"target\":\"";
    json_append_escaped(s, m.target);
    s += "\",\"companion\":";
    s += m.has_companion ? "true" : "false";
    s += ",\"state\":\"";
    bool loaded = false;
    for (const auto &n : g_native_injections)
      if (n.module_id == m.module_id) {
        loaded = true;
        break;
      }
    s += loaded ? "injected" : "failed";
    s += "\"";
    s += "}";
  }
  s += "],\"safemode\":{\"active\":";
  s += g_safemode_active ? "true" : "false";
  s += ",\"crashes\":";
  s += std::to_string(g_safemode_crashes);
  s += "}";
  s += ",\"native_injections\":[";
  first = true;
  for (const auto &n : g_native_injections) {
    if (!first)
      s += ',';
    first = false;
    s += "{\"pid\":";
    s += std::to_string(n.pid);
    s += ",\"process\":\"";
    json_append_escaped(s, n.process);
    s += "\",\"module\":\"";
    json_append_escaped(s, n.module_id);
    s += "\",\"target_type\":\"";
    json_append_escaped(s, n.target_type);
    s += "\",\"target\":\"";
    json_append_escaped(s, n.target);
    s += "\",\"abi\":\"";
    json_append_escaped(s, n.abi);
    s += "\",\"companion\":";
    s += n.has_companion ? "true" : "false";
    s += ",\"state\":\"injected\"";
    s += "}";
  }
  s += "]}";
  return s;
}

void handle_client(int client) {
  uint8_t op = 0;
  if (!read_exact(client, &op, sizeof(op)))
    return;

  switch (static_cast<zygiskd::Request>(op)) {
  case zygiskd::Request::GetModuleCount: {
    uint32_t n = static_cast<uint32_t>(g_modules.size());
    write_exact(client, &n, sizeof(n));
    break;
  }
  case zygiskd::Request::GetModuleFd: {
    uint32_t idx = 0;
    if (!read_exact(client, &idx, sizeof(idx)) || idx >= g_modules.size()) {
      send_fd(client, -1);
      break;
    }
    int fd = open(g_modules[idx].lib_path.c_str(), O_RDONLY | O_CLOEXEC);
    send_fd(client, fd);
    if (fd >= 0)
      close(fd);
    break;
  }
  case zygiskd::Request::GetModuleName: {
    /* idx -> NUL-terminated module dir-id string. Empty string on bad idx so
     * the core can still read a fixed byte and skip the module. */
    uint32_t idx = 0;
    if (!read_exact(client, &idx, sizeof(idx)) || idx >= g_modules.size()) {
      char zero = 0;
      write_exact(client, &zero, 1);
      break;
    }
    const std::string &name = g_modules[idx].name;
    write_exact(client, name.c_str(), name.size() + 1);
    break;
  }
  case zygiskd::Request::GetGrantedModules: {
    /* uid -> u32 count followed by count NUL-terminated dir-id strings. The
     * core uses this to skip dlopen'ing modules that are out of scope for the
     * target app, and to short-circuit their pre/postSpecialize callbacks. */
    uint32_t uid = 0;
    if (!read_exact(client, &uid, sizeof(uid))) {
      uint32_t zero = 0;
      write_exact(client, &zero, sizeof(zero));
      break;
    }
    std::vector<std::string> picked;
    for (const auto &m : g_modules) {
      if (!module_globally_granted(m.name))
        continue;
      if (!module_granted_for_uid(m.name, uid))
        continue;
      picked.push_back(m.name);
    }
    uint32_t count = static_cast<uint32_t>(picked.size());
    write_exact(client, &count, sizeof(count));
    for (const auto &n : picked)
      write_exact(client, n.c_str(), n.size() + 1);
    break;
  }
  case zygiskd::Request::ConnectCompanion: {
    uint32_t idx = 0;
    if (!read_exact(client, &idx, sizeof(idx)) || !ensure_companion(idx)) {
      send_fd(client, -1);
      break;
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
      send_fd(client, -1);
      break;
    }
    // companion services sv[1] on a thread; caller talks over sv[0]
    if (!send_fd(g_companions[idx].ctrl, sv[1])) {
      close(sv[0]);
      close(sv[1]);
      send_fd(client, -1);
      break;
    }
    close(sv[1]);
    send_fd(client, sv[0]);
    close(sv[0]);
    break;
  }
  case zygiskd::Request::GetModuleDir: {
    uint32_t idx = 0;
    if (!read_exact(client, &idx, sizeof(idx)) || idx >= g_modules.size()) {
      send_fd(client, -1);
      break;
    }
    std::string dir = std::string(kModulesDir) + "/" + g_modules[idx].name;
    int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    send_fd(client, fd);
    if (fd >= 0)
      close(fd);
    break;
  }
  case zygiskd::Request::GetProcessFlags: {
    uint32_t uid = 0;
    if (!read_exact(client, &uid, sizeof(uid)))
      break;
    uint32_t flags = query_flags(uid);
    write_exact(client, &flags, sizeof(flags));
    break;
  }
  case zygiskd::Request::GetConfig: {
    write_exact(client, &g_yz_config, sizeof(g_yz_config));
    break;
  }
  case zygiskd::Request::GetStatus: {
    // SO_PEERCRED: the kernel stamps the connecting process's real uid onto the
    // socket -- a caller cannot forge it (unlike a package name or class). Only
    // the kernel-authenticated manager (preset/superkey full uid) may read the
    // telemetry; anyone else gets an empty reply (len 0). The manager itself
    // connects, so the peer uid is its own app uid.
    // Tamisu has no manager-app concept: the only legitimate caller of
    // GetStatus is a root-owned process (tamisu_daemon, manager app holding root).
    // Gate on peer uid == 0 directly, no manager-uid lookup needed.
    std::string js;
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.uid == 0) {
      js = build_status_json();
    } else {
      DLOGI("GetStatus denied: peer uid=%d (root only)",
            static_cast<int>(cr.uid));
    }
    uint32_t n = static_cast<uint32_t>(js.size());
    write_exact(client, &n, sizeof(n));
    if (n != 0)
      write_exact(client, js.data(), n);
    break;
  }
  case zygiskd::Request::RevertMount: {
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    uint8_t ok = 0;
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.pid > 0)
      ok = yz_revert_app_mounts(static_cast<pid_t>(cr.pid)) ? 1 : 0;
    write_exact(client, &ok, sizeof(ok));
    break;
  }
  case zygiskd::Request::SelfDestruct: {
    uint8_t n = 0;
    if (!read_exact(client, &n, sizeof(n)) || n == 0 || n > YZ_MAX_UNMAP_SEGS)
      break;
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    uint8_t ok = 0;
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.pid > 0) {
      yz_unmap_pid_cmd ucmd{};
      ucmd.pid = static_cast<uint32_t>(cr.pid);
      ucmd.n_segs = n;
      bool good = true;
      for (uint8_t i = 0; i < n; ++i)
        if (!read_exact(client, &ucmd.addr[i], sizeof(ucmd.addr[i])) ||
            !read_exact(client, &ucmd.size[i], sizeof(ucmd.size[i]))) {
          good = false;
          break;
        }
      if (good) {
        // Revert the app's module mounts in-process (ReZygisk-style:
        // fork + setns into the target's mount namespace + umount2 by
        // source-name predicate). This is the sole umount path --
        // tamisu no longer ships the kernel YZ_UMOUNT_PID ioctl.
        //
        // The core munmaps its OWN segments synchronously (tail-call
        // munmap in zygisk_self_destruct); we do NOT kernel-munmap them
        // here -- the async task_work raced the core's own execution
        // and crashed it (libc returned into the just-unmapped core).
        // ucmd.addr/size are read for ABI compat but otherwise unused.
        ok = yz_revert_app_mounts(ucmd.pid) ? 1 : 0;
      }
    }
    write_exact(client, &ok, sizeof(ok));
    break;
  }
  case zygiskd::Request::PatchText: {
    uint64_t addr = 0;
    uint32_t len = 0;
    if (!read_exact(client, &addr, sizeof(addr)) ||
        !read_exact(client, &len, sizeof(len)) || len == 0 ||
        len > YZ_PATCH_TEXT_MAX)
      break;
    uint8_t bytes[YZ_PATCH_TEXT_MAX];
    if (!read_exact(client, bytes, len))
      break;
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    uint8_t ok = 0;
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.pid > 0) {
      yz_patch_text_cmd pcmd{};
      pcmd.pid = static_cast<uint32_t>(cr.pid);
      pcmd.len = len;
      pcmd.addr = addr;
      memcpy(pcmd.bytes, bytes, len);
      ok = tamisu_daemon::tamisuctl(TAMISU_IOCTL_YZ_PATCH_TEXT, &pcmd) == 0 ? 1 : 0;
    }
    write_exact(client, &ok, sizeof(ok));
    break;
  }
  case zygiskd::Request::Log: {
    uint16_t len = 0;
    if (!read_exact(client, &len, sizeof(len)) || len == 0 || len > 256)
      break;
    char buf[257];
    if (!read_exact(client, buf, len))
      break;
    buf[len] = '\0';
    dlog("core: %s", buf);
    break;
  }
  case zygiskd::Request::ReportZygote: {
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    uint8_t ok = 0;
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.pid > 0) {
      record_zygote(static_cast<pid_t>(cr.pid));
      ok = 1;
    }
    write_exact(client, &ok, sizeof(ok));
    break;
  }
  case zygiskd::Request::GetNativeModuleCount: {
    uint32_t n = static_cast<uint32_t>(g_native_modules.size());
    write_exact(client, &n, sizeof(n));
    break;
  }
  case zygiskd::Request::GetNativeModuleInfo: {
    uint32_t idx = 0;
    zygiskd::NativeModuleInfo info{};
    if (read_exact(client, &idx, sizeof(idx)) &&
        idx < g_native_modules.size()) {
      const NativeModule &m = g_native_modules[idx];
      info.target_type = m.target_type;
      info.has_companion = m.has_companion ? 1 : 0;
      snprintf(info.module_id, sizeof(info.module_id), "%s",
               m.module_id.c_str());
      snprintf(info.target, sizeof(info.target), "%s", m.target.c_str());
      snprintf(info.lib_path, sizeof(info.lib_path), "%s", m.lib_path.c_str());
    }
    write_exact(client, &info, sizeof(info));
    break;
  }
  case zygiskd::Request::GetNativeModuleFd: {
    uint32_t idx = 0;
    if (!read_exact(client, &idx, sizeof(idx)) ||
        idx >= g_native_modules.size()) {
      send_fd(client, -1);
      break;
    }
    const std::string &path = g_native_modules[idx].lib_path;
    int fd = copy_file_to_memfd(path);
    if (fd < 0)
      fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    send_fd(client, fd);
    if (fd >= 0)
      close(fd);
    break;
  }
  case zygiskd::Request::ConnectNativeCompanion: {
    uint32_t idx = 0;
    if (!read_exact(client, &idx, sizeof(idx)) ||
        !ensure_native_companion(idx)) {
      send_fd(client, -1);
      break;
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
      send_fd(client, -1);
      break;
    }
    if (!send_fd(g_native_companions[idx].ctrl, sv[1])) {
      close(sv[0]);
      close(sv[1]);
      send_fd(client, -1);
      break;
    }
    close(sv[1]);
    send_fd(client, sv[0]);
    close(sv[0]);
    break;
  }
  case zygiskd::Request::RestoreNativeLoadPolicy: {
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    uint8_t ok = 0;
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.pid > 0) {
      yz_native_load_policy_cmd cmd{};
      cmd.pid = static_cast<uint32_t>(cr.pid);
      int ret = tamisu_daemon::tamisuctl(TAMISU_IOCTL_YZ_RESTORE_NATIVE_LOAD_POLICY, &cmd);
      DLOGI("native load policy restore: pid=%d ret=%d", cr.pid, ret);
      ok = ret == 0 ? 1 : 0;
    }
    write_exact(client, &ok, sizeof(ok));
    break;
  }
  case zygiskd::Request::ReportNativeInjection: {
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    uint32_t idx = 0;
    uint8_t ok = 0;
    if (read_exact(client, &idx, sizeof(idx)) &&
        getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.pid > 0 && idx < g_native_modules.size()) {
      record_native_injection(static_cast<pid_t>(cr.pid), idx);
      ok = 1;
    }
    write_exact(client, &ok, sizeof(ok));
    break;
  }
  default:
    break;
  }
}

int bind_listen() {
  int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (srv < 0) {
    DLOGE("socket failed: %s", strerror(errno));
    return -1;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  /* abstract namespace */
  const size_t name_len = strlen(zygiskd::kSocketName);
  memcpy(addr.sun_path + 1, zygiskd::kSocketName, name_len);
  socklen_t len =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name_len);

  if (bind(srv, reinterpret_cast<sockaddr *>(&addr), len) < 0) {
    DLOGE("bind @%s failed: %s", zygiskd::kSocketName, strerror(errno));
    close(srv);
    return -1;
  }
  if (listen(srv, 32) < 0) {
    DLOGE("listen failed: %s", strerror(errno));
    close(srv);
    return -1;
  }
  return srv;
}

int nl_listen() {
  int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, YZ_NETLINK_PROTO);
  if (fd < 0) {
    DLOGE("netlink socket: %s", strerror(errno));
    return -1;
  }
  sockaddr_nl addr{};
  addr.nl_family = AF_NETLINK;
  addr.nl_groups = 1u << (YZ_NL_GROUP_EVENTS - 1);
  if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    DLOGE("netlink bind: %s", strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

void on_specialize(uint32_t /*pid*/, uint32_t /*appid*/) {}

void nl_drain(int fd) {
  char buf[4096];
  ssize_t got = recv(fd, buf, sizeof(buf), 0);
  if (got <= 0)
    return;

  int len = static_cast<int>(got);
  for (nlmsghdr *nlh = reinterpret_cast<nlmsghdr *>(buf); NLMSG_OK(nlh, len);
       nlh = NLMSG_NEXT(nlh, len)) {
    if (nlh->nlmsg_type != YZ_NL_MSG_EVENT)
      continue;
    if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(yz_event)))
      continue;
    auto *ev = static_cast<yz_event *>(NLMSG_DATA(nlh));
    DLOGI("event type=%u pid=%u appid=%u", ev->type, ev->pid, ev->appid);
    if (ev->type == YZ_EV_SPECIALIZE) {
      record_injection(ev->appid);
      on_specialize(ev->pid, ev->appid);
    } else if (ev->type == YZ_EV_RELOAD) {
      rescan_modules();
      read_yzconfig();
    } else if (ev->type == YZ_EV_SAFEMODE) {
      g_safemode_active = true;
      g_safemode_crashes = ev->appid;
      DLOGE("safemode activated: pid=%u crashes=%u", ev->pid, ev->appid);
    }
  }
}

uint64_t resolve_linker_sym(const char *path, const char *want) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 0;
  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
    close(fd);
    return 0;
  }
  void *map = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (map == MAP_FAILED)
    return 0;

  auto *base = static_cast<const uint8_t *>(map);
  auto *eh = reinterpret_cast<const Elf64_Ehdr *>(base);
  uint64_t result = 0;
  if (memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0 &&
      eh->e_ident[EI_CLASS] == ELFCLASS64) {
    auto *sh = reinterpret_cast<const Elf64_Shdr *>(base + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum && !result; i++) {
      if (sh[i].sh_type != SHT_DYNSYM)
        continue;
      auto *syms = reinterpret_cast<const Elf64_Sym *>(base + sh[i].sh_offset);
      const char *strs =
          reinterpret_cast<const char *>(base + sh[sh[i].sh_link].sh_offset);
      size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
      for (size_t j = 0; j < n; j++) {
        if (strcmp(strs + syms[j].st_name, want) == 0) {
          result = syms[j].st_value;
          break;
        }
      }
    }
  }
  munmap(map, st.st_size);
  return result;
}

uint64_t resolve_first(const char *const *cands, size_t n, const char **hit) {
  for (size_t i = 0; i < n; ++i) {
    uint64_t off = resolve_linker_sym("/system/bin/linker64", cands[i]);
    if (off) {
      if (hit)
        *hit = cands[i];
      return off;
    }
  }
  return 0;
}

bool send_dlopen_offset() {
  static const char *const kDlopen[] = {
      "__loader_android_dlopen_ext",
      "android_dlopen_ext",
  };
  static const char *const kDlsym[] = {
      "__loader_dlsym",
      "dlsym",
  };

  const char *dlopen_name = nullptr;
  const char *dlsym_name = nullptr;
  yz_dlopen_cmd cmd{};
  cmd.dlopen_offset = resolve_first(kDlopen, 2, &dlopen_name);
  cmd.dlsym_offset = resolve_first(kDlsym, 2, &dlsym_name);

  if (!cmd.dlopen_offset || !cmd.dlsym_offset) {
    DLOGI("linker resolve incomplete: dlopen=%s dlsym=%s",
          dlopen_name ? dlopen_name : "(none)",
          dlsym_name ? dlsym_name : "(none)");
    return false;
  }

  int ret = tamisu_daemon::tamisuctl(TAMISU_IOCTL_YZ_SET_DLOPEN, &cmd);
  DLOGI("linker dlopen '%s'=0x%llx dlsym '%s'=0x%llx -> kernel ret=%d",
        dlopen_name, (unsigned long long)cmd.dlopen_offset, dlsym_name,
        (unsigned long long)cmd.dlsym_offset, ret);
  return ret == 0;
}

int run_daemon() {
  int ready_fd = consume_ready_fd();

  signal(SIGPIPE, SIG_IGN);

  int srv = bind_listen();
  if (srv < 0) {
    DLOGI("@%s already owned by another zygiskd; exiting",
          zygiskd::kSocketName);
    notify_ready(ready_fd, true);
    return 0;
  }

  rescan_modules();
  read_yzconfig();
  bool offsets_ready = send_dlopen_offset();

  int nlfd = nl_listen();
  DLOGI("zygiskd up: unix @%s, netlink proto=%d", zygiskd::kSocketName,
        YZ_NETLINK_PROTO);
  notify_ready(ready_fd, offsets_ready);

  pollfd pfds[2] = {{srv, POLLIN, 0}, {nlfd, POLLIN, 0}};
  nfds_t nfds = (nlfd >= 0) ? 2 : 1;
  for (;;) {
    if (poll(pfds, nfds, -1) < 0)
      continue;
    if (pfds[0].revents & POLLIN) {
      int client = accept4(srv, nullptr, nullptr, SOCK_CLOEXEC);
      if (client >= 0) {
        handle_client(client); // TODO: concurrency once companions exist
        close(client);
      }
    }
    if (nlfd >= 0 && (pfds[1].revents & POLLIN))
      nl_drain(nlfd);
  }
}

} // namespace

extern "C" int zygiskd_main(int /*argc*/, char ** /*argv*/) {
  return run_daemon();
}
