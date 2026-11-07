#include "platform/Platform.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace app {
namespace {
struct Fd {
  int value = -1;
  explicit Fd(int fd = -1) : value(fd) {}
  ~Fd() { close(); }
  void close() { if (value >= 0) ::close(value); value = -1; }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
};
std::string error_text(int code) { return std::string(std::strerror(code)) + " (" + std::to_string(code) + ")"; }

struct Spawned {
  pid_t pid = -1;
  std::string error;
};
// The child gets its own process group, so cancellation reaches helpers it
// starts. Daemons that call setsid() (the ADB server) leave the group and keep
// running for other clients, matching the Windows job breakaway behaviour.
Spawned spawn(const std::string &exe, const std::vector<std::string> &args, int output_fd,
              const std::filesystem::path &directory, bool new_session) {
  Spawned spawned;
  std::vector<char *> argv;
  argv.push_back(const_cast<char *>(exe.c_str()));
  for (const auto &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attributes;
  posix_spawn_file_actions_init(&actions);
  posix_spawnattr_init(&attributes);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (output_fd >= 0) {
    posix_spawn_file_actions_adddup2(&actions, output_fd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output_fd, STDERR_FILENO);
  } else {
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  }
  auto dir = directory.string();
  if (!dir.empty()) posix_spawn_file_actions_addchdir_np(&actions, dir.c_str());
  sigset_t defaults;
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGPIPE);
  sigaddset(&defaults, SIGCHLD);
  sigset_t no_mask;
  sigemptyset(&no_mask);
  posix_spawnattr_setsigdefault(&attributes, &defaults);
  posix_spawnattr_setsigmask(&attributes, &no_mask);
  short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
  if (new_session) flags |= POSIX_SPAWN_SETSID;
  else { flags |= POSIX_SPAWN_SETPGROUP; posix_spawnattr_setpgroup(&attributes, 0); }
  posix_spawnattr_setflags(&attributes, flags);
  // A bare name (the "adb" fallback) is looked up on PATH; paths are used as is.
  bool search = exe.find('/') == std::string::npos;
  int code = search ? posix_spawnp(&spawned.pid, exe.c_str(), &actions, &attributes, argv.data(), environ)
                    : posix_spawn(&spawned.pid, exe.c_str(), &actions, &attributes, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  if (code) { spawned.pid = -1; spawned.error = error_text(code); }
  return spawned;
}
}

std::string last_error() { return error_text(errno); }
unsigned long process_id() { return (unsigned long)getpid(); }
std::string environment(const char *name) {
  const char *value = std::getenv(name);
  return value ? value : "";
}
std::filesystem::path config_directory() {
  auto base = environment("XDG_CONFIG_HOME");
  if (!base.empty() && base.front() == '/') return std::filesystem::path(base) / "scrcpy-tray";
  auto home = environment("HOME");
  if (home.empty()) throw std::runtime_error("HOME is unavailable.");
  return std::filesystem::path(home) / ".config" / "scrcpy-tray";
}
std::filesystem::path executable_directory() {
  std::error_code error;
  auto path = std::filesystem::read_symlink("/proc/self/exe", error);
  return error ? std::filesystem::current_path() : path.parent_path();
}
bool is_file(const std::filesystem::path &path) {
  struct stat info {};
  return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}
std::filesystem::path find_executable(const std::string &name) {
  auto path = environment("PATH");
  size_t start = 0;
  while (start <= path.size()) {
    auto end = path.find(':', start);
    auto dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!dir.empty()) {
      auto candidate = std::filesystem::path(dir) / name;
      if (is_file(candidate) && access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return {};
}
std::string create_new_file(const std::filesystem::path &path, bool &exists) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  exists = fd < 0 && errno == EEXIST;
  if (fd < 0) return exists ? "File exists." : last_error();
  ::close(fd);
  return {};
}
std::string replace_file(const std::filesystem::path &from, const std::filesystem::path &to) {
  return rename(from.c_str(), to.c_str()) == 0 ? std::string{} : last_error();
}
bool open_url(const std::string &url, std::string &error) {
  return launch_detached("xdg-open", {url}, error);
}

ProcessResult run_process(const std::string &executable, const std::vector<std::string> &args,
                          Output output, unsigned timeout_ms, std::shared_ptr<Cancel> cancel,
                          const std::filesystem::path &directory) {
  ProcessResult result;
  auto fail = [&](const std::string &message) { result.text += message + "\n"; if (output) output(message + "\n"); };
  int pipes[2];
  if (pipe2(pipes, O_CLOEXEC) != 0) { fail(last_error()); return result; }
  Fd reader(pipes[0]), writer(pipes[1]);
  auto child = spawn(executable, args, writer.value, directory, false);
  writer.close();
  if (child.pid < 0) { fail("Could not start " + executable + ": " + child.error); return result; }
  fcntl(reader.value, F_SETFL, fcntl(reader.value, F_GETFL) | O_NONBLOCK);

  auto began = milliseconds();
  std::string pending;
  bool terminated = false, exited = false, eof = false;
  uint64_t killed_at = 0;
  int status = 0;
  auto consume = [&](const char *buffer, size_t count) {
    if (result.text.size() < 1024 * 1024) result.text.append(buffer, count);
    pending.append(buffer, count);
    auto end = pending.find_last_of('\n');
    if (end != std::string::npos) { if (output) output(pending.substr(0, end + 1)); pending.erase(0, end + 1); }
    // Bound output from tools that never write a newline without splitting a
    // UTF-8 character across callbacks.
    if (pending.size() > 8192) {
      size_t cut = pending.size() - 4;
      while (cut && (static_cast<unsigned char>(pending[cut]) & 0xc0) == 0x80) --cut;
      if (output) output(pending.substr(0, cut));
      pending.erase(0, cut);
    }
  };
  for (;;) {
    if (!terminated && ((cancel && cancel->requested) || (timeout_ms && milliseconds() - began > timeout_ms))) {
      result.timed_out = !(cancel && cancel->requested);
      // Ask politely first so scrcpy can restore the device, then force it.
      kill(-child.pid, SIGTERM);
      terminated = true;
      killed_at = milliseconds();
      if (result.timed_out) fail("Command timed out.");
    }
    if (terminated && !exited && milliseconds() - killed_at > 2000) kill(-child.pid, SIGKILL);
    if (!eof) {
      pollfd fd{reader.value, POLLIN, 0};
      if (poll(&fd, 1, 20) > 0) {
        char buffer[4096];
        ssize_t count = read(reader.value, buffer, sizeof(buffer));
        if (count > 0) { consume(buffer, (size_t)count); continue; }
        if (count == 0) eof = true;
        else if (errno != EAGAIN && errno != EINTR) eof = true;
      }
    } else if (!exited)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!exited && waitpid(child.pid, &status, WNOHANG) == child.pid) exited = true;
    // A grandchild may keep the pipe open after the child exits; stop once the
    // child is gone and no output is immediately available.
    if (exited) {
      char buffer[4096];
      ssize_t count;
      while (!eof && (count = read(reader.value, buffer, sizeof(buffer))) > 0) consume(buffer, (size_t)count);
      break;
    }
  }
  if (output && !pending.empty()) output(pending + "\n");
  if (WIFEXITED(status)) result.code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) result.code = 128 + WTERMSIG(status);
  return result;
}

bool launch_detached(const std::string &executable, const std::vector<std::string> &args, std::string &error) {
  auto child = spawn(executable, args, -1, {}, true);
  if (child.pid < 0) { error = child.error; return false; }
  // Reap it in the background so it never lingers as a zombie.
  std::thread([pid = child.pid] { int status; waitpid(pid, &status, 0); }).detach();
  return true;
}
}
