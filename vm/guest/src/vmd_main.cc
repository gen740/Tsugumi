// tsugumi-vmd: the supervisor of Tsugumi's shared VM (one VM per app).
//
// The app boots one Linux VM with Virtualization.framework. Each open
// project has its own virtiofs device (slot n, tag tsugumi-<n>, with the
// folders `project` and `control`), mounted here at /mnt/slots/<n> while the
// project runs; the app only ever replaces the share of a free slot.
// Every project gets its own tsugumid, started here:
//
//   uid            a fixed uid per project (kept in /persist/tsugumi/uids)
//   namespaces     mount, PID, IPC and UTS; only /project and /control of
//                  this project are visible (the share, the store disk and
//                  other projects are not); a private /tmp
//   cgroup         /sys/fs/cgroup/tsugumi/<id>: stopping a project ends
//                  exactly its processes and never another project's
//   run history    the project database at /project/.tsugumi/runs.sqlite
//   Nix            the shared store through the one nix-daemon; build roots
//                  under /nix/var/nix/gcroots/tsugumi/projects/<id>, a folder
//                  only vmd creates (the parent is not writable by projects);
//                  roots are removed only by an explicit user action
//
// The guest opens the persistent control stream to host port 1024. The
// host connects to 1025 for project RPC and 1026 for raw terminal streams.

#include <arpa/inet.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/sched.h>
#include <linux/vm_sockets.h>
#include <netinet/in.h>
#include <poll.h>
#include <pty.h>
#include <sched.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "control_wire.hh"
#include "tsugumi/runtime_support/cache_usage.hh"
#include "tsugumi/workerd/store.hh"

namespace fs = std::filesystem;
namespace control_wire = tsugumi::vm_guest;
namespace G = control_wire::wire;

namespace {

struct Options {
  std::string tsugumid;
  std::string tini;
  std::string bash;
  std::string path;       // PATH for project processes
  std::string cert_file;  // CA bundle
  std::string host = "/mnt/slots";
  std::string state = "/persist/tsugumi";
  int control_port = 1024;
  int rpc_port = 1025;
  int terminal_port = 1026;
};
Options opt;
pid_t rosettad_pid = -1;
int rosettad_pidfd = -1;
bool rosettad_listening = false;

int control_fd = -1;
int ready_events = -1;
std::mutex control_write;
bool control_writable = false;

template <class T>
auto send_control(T value, uint64_t request_id = 0) -> bool {
  std::lock_guard lock(control_write);
  if (!control_writable) return false;
  if (control_wire::Send(control_fd, std::move(value), request_id)) return true;
  std::fprintf(stderr, "vmd: cannot write the control stream: %s\n",
               std::strerror(errno));
  control_writable = false;
  ::shutdown(control_fd, SHUT_RDWR);
  return false;
}

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  G::ConsoleLineT line;
  line.text = std::string("vmd: ") + buf;
  if (!send_control(line)) std::fprintf(stderr, "%s\n", line.text.c_str());
}

auto failure(std::string error) -> G::ReplyT {
  G::ReplyT result;
  result.error = std::move(error);
  return result;
}

bool write_file(const std::string& path, const std::string& text) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  ssize_t count;
  do {
    count = ::write(fd, text.data(), text.size());
  } while (count < 0 && errno == EINTR);
  bool ok = count == static_cast<ssize_t>(text.size());
  const int error = count < 0 ? errno : EIO;
  ::close(fd);
  if (!ok) errno = error;
  return ok;
}

auto system_error(const char* operation, const std::string& path)
    -> std::string {
  return std::string(operation) + " " + path + ": " + std::strerror(errno);
}

// ---- projects ----

struct Project {
  std::string id;
  int slot = -1;
  uid_t uid = 0;
  pid_t pid = 0;  // tini, PID 1 of the project's PID namespace
  bool running = false;
  int ready_watch = -1;
};

std::mutex mu;
std::mutex lifecycle;
std::atomic<bool> powering_off{false};
std::condition_variable exited;
std::map<std::string, Project> projects;
std::map<pid_t, int> exit_status;  // children reaped by the reaper thread

auto publish_status(const Project& project, G::ProjectPhase phase,
                    int status = -1) -> void {
  G::ProjectStatusT event;
  event.key = project.id;
  event.pid = project.pid;
  event.phase = phase;
  event.exit_status = status;
  send_control(std::move(event));
}

auto read_memory() -> std::optional<G::MemoryT> {
  std::ifstream in("/proc/meminfo");
  G::MemoryT memory;
  std::string key, rest;
  uint64_t value = 0;
  while (in >> key >> value) {
    std::getline(in, rest);
    if (key == "MemTotal:") memory.total_kib = value;
    if (key == "MemAvailable:") memory.available_kib = value;
    if (key == "Cached:") memory.cached_kib = value;
  }
  if (memory.total_kib == 0) return std::nullopt;
  return memory;
}

auto publish_memory() -> void {
  if (auto memory = read_memory())
    send_control(std::move(*memory));
  else
    logf("cannot read guest memory information");
}

auto end_ready_watch(Project& project) -> void {
  if (project.ready_watch < 0) return;
  ::inotify_rm_watch(ready_events, project.ready_watch);
  project.ready_watch = -1;
}

auto monitor_readiness() -> void {
  alignas(inotify_event) std::array<char, 4096> buffer;
  for (;;) {
    const auto count = ::read(ready_events, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    for (ssize_t offset = 0; offset < count;) {
      const auto* event =
          reinterpret_cast<const inotify_event*>(buffer.data() + offset);
      offset += sizeof(inotify_event) + event->len;
      if (event->mask & IN_Q_OVERFLOW) {
        logf("project readiness events overflowed");
        ::shutdown(control_fd, SHUT_RDWR);
        return;
      }
      if (!(event->mask & IN_MOVED_TO) ||
          std::string_view(event->name) != "server.ready")
        continue;
      std::lock_guard lock(mu);
      for (auto& [_, project] : projects) {
        if (project.ready_watch != event->wd) continue;
        end_ready_watch(project);
        publish_status(project, G::ProjectPhase::Ready);
        publish_memory();
        break;
      }
    }
  }
  logf("cannot read project readiness events: %s", std::strerror(errno));
  ::shutdown(control_fd, SHUT_RDWR);
}

std::string cgroup_dir(const std::string& id) {
  return "/sys/fs/cgroup/tsugumi/" + id;
}

// The uid of a project, allocated once and kept on the store disk.
uid_t uid_for(const std::string& id) {
  fs::create_directories(opt.state);
  std::string file = opt.state + "/uids";
  std::map<std::string, uid_t> uids;
  uid_t next = 20001;
  {
    std::ifstream in(file);
    std::string pid;
    uid_t u;
    while (in >> pid >> u) {
      uids[pid] = u;
      next = std::max<uid_t>(next, u + 1);
    }
  }
  if (auto it = uids.find(id); it != uids.end()) return it->second;
  std::ofstream(file, std::ios::app) << id << " " << next << "\n";
  logf("uid %u for %s (%zu known)", next, id.c_str(), uids.size());
  return next;
}

// The project's user in /etc/passwd and /etc/group (a tmpfs copy): Nix
// needs a passwd entry and a home owned by the user.
void ensure_user(const std::string& id, uid_t uid) {
  std::string name = "p" + id.substr(0, 16);
  auto has = [&](const char* file) {
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);)
      if (line.rfind(name + ":", 0) == 0) return true;
    return false;
  };
  if (!has("/etc/passwd"))
    std::ofstream("/etc/passwd", std::ios::app)
        << name << ":x:" << uid << ":" << uid << ":Tsugumi project " << id
        << ":/tmp/home:/bin/sh\n";
  if (!has("/etc/group"))
    std::ofstream("/etc/group", std::ios::app) << name << ":x:" << uid << ":\n";
}

// What a project's first process does, prepared by the parent: the child of
// this multithreaded process only makes system calls (no allocation).
struct ProjectView {
  std::string project;
  std::string control;
  std::string scratch;
  std::string rpc;
  std::string slots;
  std::vector<std::pair<std::string, std::string>>
      extra;  // folders of the Mac / of chats: source, target
};

struct ChildArgs {
  std::string name;
  uid_t uid;
  int sync_fd;  // read end: the parent has put the child into its cgroup
  int log_fd;
  ProjectView view;
  std::vector<std::string> argv;
  std::vector<std::string> env;
  std::vector<char*> av;
  std::vector<char*> ev;
};

// In a child of this multithreaded process: no stdio locks, no allocation.
void die(const char* what) {
  ::dprintf(2, "vmd child: %s: %s\n", what, std::strerror(errno));
  _exit(127);
}

const std::string kRoots = "/nix/var/nix/gcroots/tsugumi/projects";

// The supervisor owns the project build root directory.
void prepare_project_roots() {
  fs::create_directories(kRoots);
  if (::chown(kRoots.c_str(), 0, 0) != 0)
    throw std::runtime_error(system_error("chown", kRoots));
  ::chmod(kRoots.c_str(), 0755);
}

std::vector<std::string> project_env(const std::string& id) {
  return {"PATH=" + opt.path,
          "HOME=/tmp/home",
          "TMPDIR=/tmp",
          "USER=tsugumi",
          "LANG=C.UTF-8",
          "NIX_REMOTE=daemon",
          "NIX_SSL_CERT_FILE=" + opt.cert_file,
          "SSL_CERT_FILE=" + opt.cert_file,
          "TSUGUMI_PROJECT_ID=" + id};
}

// Disk-backed scratch, isolated by project uid and bind-mounted before /persist
// is hidden.
const std::string kScratch = "/persist/run-scratch";
const std::string kUsage = "/run/tsugumi-cache-usage";
std::unique_ptr<tsugumi::CacheUsage> cache_usage;
// Per project: the folder of its tsugumid's socket (0700, the project's user).
const std::string kRpc = "/run/tsugumi-rpc";
std::string rpc_dir(const std::string& id) { return kRpc + "/" + id; }
// The nix-daemon's binary caches, one URL per line (no keys, no credentials):
// written by /init, readable by every project.
const std::string kSubstituters = "/run/tsugumi-substituters";

// Mount namespace of one project: its folder and control folder at /project
// and /control, the folders of the Mac it was
// given (share entries host-<tag>) at /run/tsugumi/host/<tag> and those
// attached to its chats (chat-<tag>) at /run/tsugumi/chat/<tag>, nothing of the
// share, the store disk or other projects.
// host-<tag> / chat-<tag> folders: the tag is lowercase hex, like a project id.
bool valid_id(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  for (char c : id)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

ProjectView project_view(int slot, const std::string& id) {
  ProjectView v;
  std::string dir = opt.host + "/" + std::to_string(slot);
  v.project = dir + "/project";
  v.control = dir + "/control";
  v.scratch = kScratch + "/" + id;
  v.rpc = rpc_dir(id);
  v.slots = fs::path(opt.host).parent_path().string();
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    std::string name = e.path().filename().string();
    bool host = name.starts_with("host-");
    bool chat = name.starts_with("chat-");
    if (!(host || chat) || !valid_id(name.substr(5))) continue;
    v.extra.emplace_back(
        e.path().string(),
        std::string(host ? "/run/tsugumi/host/" : "/run/tsugumi/chat/") +
            name.substr(5));
    logf("%s: folder %s at %s", id.c_str(), name.c_str(),
         v.extra.back().second.c_str());
  }
  if (ec)
    logf("%s: cannot list %s: %s", id.c_str(), dir.c_str(),
         ec.message().c_str());
  return v;
}

void enter_project_view(const ProjectView& v) {
  if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0)
    die("make / private");
  if (::mount(v.project.c_str(), "/project", nullptr, MS_BIND, nullptr) != 0)
    die("bind /project");
  if (::mount(v.control.c_str(), "/control", nullptr, MS_BIND, nullptr) != 0)
    die("bind /control");
  if (::mount(v.scratch.c_str(), "/scratch", nullptr,
              MS_BIND | MS_NOSUID | MS_NODEV, nullptr) != 0)
    die("bind /scratch");
  if (::mount("tmpfs", "/run/tsugumi", "tmpfs", MS_NOSUID | MS_NODEV,
              "mode=0755") != 0)
    die("private /run/tsugumi");
  // tsugumid's socket: in a folder only this project's user (and vmd) enters.
  ::mkdir("/run/tsugumi/rpc", 0700);
  if (::mount(v.rpc.c_str(), "/run/tsugumi/rpc", nullptr,
              MS_BIND | MS_NOSUID | MS_NODEV, nullptr) != 0)
    die("bind the rpc folder");
  ::mkdir("/run/tsugumi/host", 0755);
  ::mkdir("/run/tsugumi/chat", 0755);
  for (const auto& [source, target] : v.extra) {
    ::mkdir(target.c_str(), 0755);
    if (::mount(source.c_str(), target.c_str(), nullptr,
                MS_BIND | MS_NOSUID | MS_NODEV, nullptr) != 0)
      ::dprintf(2, "vmd child: bind %s: %s\n", source.c_str(),
                std::strerror(errno));
  }
  // /mnt (a tmpfs holding every slot mount) goes, with all slots under it.
  if (::umount2(v.slots.c_str(), MNT_DETACH) != 0) die("hide the slots");
  ::umount2("/persist", MNT_DETACH);  // the store disk (the /nix overlay stays)
  if (::mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777") != 0)
    die("private /tmp");
  ::mkdir("/tmp/home", 0700);
}

void drop_to(uid_t uid) {
  if (::setgroups(0, nullptr) != 0) die("setgroups");
  if (::setgid(uid) != 0) die("setgid");
  if (::setuid(uid) != 0) die("setuid");
  ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
}

std::vector<char*> cstrings(std::vector<std::string>& v) {
  std::vector<char*> out;
  for (auto& s : v) out.push_back(s.data());
  out.push_back(nullptr);
  return out;
}

// The shared store's cleanup: deletes what no GC root keeps (an app setting,
// never a project's cache clearing). Runs as root against the local store.
auto collect_garbage(bool verify) -> G::ReplyT {
  if (verify) {
    std::lock_guard lock(mu);
    for (const auto& [_, project] : projects)
      if (project.running)
        return failure("stop all projects before checking the store");
  }
  int out[2];
  if (::pipe2(out, O_CLOEXEC) != 0)
    return failure("cannot open store output pipe");
  std::vector<std::string> argv = {
      opt.bash, "-c",
      verify ? "exec /run/tsugumi/store-health check /persist $(cat "
               "/persist/store-health/boot-closure) maintenance"
             : "exec nix-store --gc"};
  std::vector<std::string> env = {"PATH=" + opt.path};
  auto av = cstrings(argv);
  auto ev = cstrings(env);
  pid_t pid = ::fork();
  if (pid == 0) {
    ::dup2(out[1], 1);
    ::dup2(out[1], 2);
    sigset_t empty;
    sigemptyset(&empty);
    ::sigprocmask(SIG_SETMASK, &empty, nullptr);
    ::execve(av[0], av.data(), ev.data());
    ::_exit(127);
  }
  ::close(out[1]);
  std::string text;
  char buf[4096];
  for (ssize_t n; (n = ::read(out[0], buf, sizeof buf)) > 0;)
    text.append(buf, static_cast<size_t>(n));
  ::close(out[0]);
  if (pid < 0) return failure("cannot start store maintenance");
  int st;
  {
    std::unique_lock lock(mu);
    exited.wait(lock, [&] { return exit_status.contains(pid); });
    st = exit_status.at(pid);
    exit_status.erase(pid);
  }
  logf("store %s: %s", verify ? "check/repair" : "cleanup", text.c_str());
  const bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
  if (verify) {
    // Keep the latter part of the diagnostic in the reply. The full check
    // is recorded in the guest's check.log.
    if (text.size() > 65536) text.erase(0, text.size() - 65536);
    G::ReplyT result;
    if (ok)
      result.message = std::move(text);
    else
      result.error = "store check failed:\n" + text;
    return result;
  }
  return ok ? G::ReplyT{} : failure("nix-store --gc failed");
}

int project_main(void* arg) {
  auto* a = static_cast<ChildArgs*>(arg);
  char b;
  if (::read(a->sync_fd, &b, 1) != 1) die("wait for the cgroup");
  ::close(a->sync_fd);
  ::dup2(a->log_fd, 1);
  ::dup2(a->log_fd, 2);
  enter_project_view(a->view);
  // A fresh /proc for this PID namespace.
  if (::mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC,
              nullptr) != 0)
    die("mount /proc");
  // The emulators the VM registered (binfmt_misc, global): seen read-only, so
  // tsugumid can name the one a Card built for another system runs with.
  if (::mount("binfmt_misc", "/proc/sys/fs/binfmt_misc", "binfmt_misc",
              MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RDONLY, nullptr) != 0)
    ::dprintf(2, "vmd child: binfmt_misc: %s\n", std::strerror(errno));
  if (::sethostname(a->name.c_str(), a->name.size()) != 0) die("sethostname");
  if (::chown("/tmp/home", a->uid, a->uid) != 0) die("chown /tmp/home");
  drop_to(a->uid);
  if (::chdir("/project") != 0) die("chdir /project");
  sigset_t empty;
  sigemptyset(&empty);
  ::sigprocmask(SIG_SETMASK, &empty, nullptr);
  ::execve(a->av[0], a->av.data(), a->ev.data());
  die("exec tsugumid");
  return 127;
}

std::string slot_dir(int slot) { return opt.host + "/" + std::to_string(slot); }

// Mounts the slot's virtiofs device afresh (the app has just set its share).
std::string mount_slot(int slot) {
  std::string dir = slot_dir(slot);
  ::umount2(dir.c_str(), MNT_DETACH);
  fs::create_directories(dir);
  std::string tag = "tsugumi-" + std::to_string(slot);
  if (::mount(tag.c_str(), dir.c_str(), "virtiofs", MS_NOSUID | MS_NODEV,
              nullptr) != 0)
    return std::string("cannot mount ") + tag + ": " + std::strerror(errno);
  return "";
}

auto start_project(const G::StartT& request) -> G::ReplyT {
  const auto& id = request.key;
  const int slot = request.slot;
  if (!valid_id(id) || slot >= 16 || request.host_name.empty() ||
      request.host_name.size() > 64)
    return failure("invalid project start");
  std::unique_lock<std::mutex> l(mu);
  if (auto it = projects.find(id); it != projects.end() && it->second.running)
    return failure("the project is already running");
  if (auto err = mount_slot(slot); !err.empty()) return failure(err);
  struct MountedSlot {
    std::string path;
    bool started = false;
    ~MountedSlot() {
      if (!started && ::umount2(path.c_str(), MNT_DETACH) != 0)
        logf("cannot unmount %s: %s", path.c_str(), std::strerror(errno));
    }
  } slot_mount{slot_dir(slot)};
  std::string p = slot_dir(slot) + "/project";
  std::string c = slot_dir(slot) + "/control";
  Project pr;
  pr.id = id;
  pr.slot = slot;
  pr.uid = uid_for(id);
  ensure_user(id, pr.uid);
  const auto prepare = [&](const std::string& path, mode_t mode) {
    fs::create_directories(path);
    if (::chown(path.c_str(), pr.uid, pr.uid) != 0)
      return system_error("chown", path);
    ::chmod(path.c_str(), mode);
    return std::string{};
  };
  std::string roots = kRoots + "/" + id;
  if (auto error = prepare(roots, 0755); !error.empty()) return failure(error);
  ::utimensat(AT_FDCWD, roots.c_str(), nullptr, AT_SYMLINK_NOFOLLOW);
  std::string scratch = kScratch + "/" + id;
  if (auto error = prepare(scratch, 0700); !error.empty())
    return failure(error);
  std::string rpc = rpc_dir(id);
  if (auto error = prepare(rpc, 0700); !error.empty()) return failure(error);
  std::string cg = cgroup_dir(id);
  fs::create_directories(cg + "/core");
  // The core and terminals run in a leaf. The empty parent delegates the
  // controllers and process migration to the project for its Card leaves.
  if (!write_file(cg + "/cgroup.subtree_control", "+cpu +memory") ||
      ::chown(cg.c_str(), pr.uid, pr.uid) != 0 ||
      ::chown((cg + "/cgroup.procs").c_str(), pr.uid, pr.uid) != 0)
    return failure(system_error("delegate Card cgroups", cg));
  // The control folder is the project's: a link there is never followed by
  // root.
  std::string log_path = c + "/tsugumid.log";
  int log_fd =
      ::open(log_path.c_str(),
             O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (log_fd < 0) return failure(system_error("open", log_path));
  int sync[2];
  if (::pipe2(sync, O_CLOEXEC) != 0) {
    ::close(log_fd);
    return failure("cannot open project start pipe");
  }
  ChildArgs args{request.host_name,
                 pr.uid,
                 sync[0],
                 log_fd,
                 project_view(slot, id),
                 {},
                 {},
                 {},
                 {}};
  args.argv = {opt.tini,
               "-g",
               "--",
               opt.tsugumid,
               "--project",
               "/project",
               "--listen-unix-ws",
               "/run/tsugumi/rpc/s",
               "--control-dir",
               "/control",
               "--cpus",
               std::to_string(::get_nprocs()),
               "--allow-missing-origin",
               "--gcroots-dir",
               kRoots + "/" + id,
               "--run-scratch-dir",
               "/scratch",
               "--usage-file",
               kUsage,
               "--host-folders",
               "/run/tsugumi/host",
               "--project-shell"};
  // The binary caches of this VM's nix-daemon, for the run records. A project
  // cannot read the daemon's settings (they sit beside the caches' credentials
  // under /run/tsugumi, which its view replaces); /init wrote this list.
  args.argv.insert(args.argv.end(), {"--substituters-file", kSubstituters});
  for (const auto& origin : request.origins)
    args.argv.insert(args.argv.end(), {"--allow-origin", origin});
  args.env = project_env(id);
  // The shared daemon enforces the build settings in vm-boot.nix.
  args.env.emplace_back("TSUGUMI_GUEST_NIX_SANDBOX=1");
  args.env.push_back("TSUGUMI_CARD_CGROUP_ROOT=" + cg);
  if (request.rpc_trace) args.env.emplace_back("TSUGUMI_RPC_TRACE=1");
  args.env.emplace_back("TSUGUMI_EXECUTION_BROKER=vsock:1027");
  args.env.emplace_back(
      "TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE=/control/"
      "execution-broker-capability");
  args.av = cstrings(args.argv);
  args.ev = cstrings(args.env);
  // The daemon renames server.ready into place from inside the guest, so
  // inotify sees the rename even though the host's vnode API does not.
  pr.ready_watch = ::inotify_add_watch(ready_events, c.c_str(), IN_MOVED_TO);
  if (pr.ready_watch < 0) {
    const auto error = system_error("watch", c);
    ::close(sync[0]);
    ::close(sync[1]);
    ::close(log_fd);
    return failure(error);
  }
  // The child's fds must survive exec only where dup2'ed, so CLOEXEC is fine.
  static thread_local std::vector<char> stack(1 << 20);
  pid_t pid = ::clone(
      project_main, stack.data() + stack.size(),
      CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS | SIGCHLD,
      &args);
  ::close(sync[0]);
  ::close(log_fd);
  if (pid < 0) {
    ::close(sync[1]);
    end_ready_watch(pr);
    return failure(system_error("clone", id));
  }
  if (!write_file(cg + "/core/cgroup.procs", std::to_string(pid))) {
    const auto error = system_error("write", cg + "/core/cgroup.procs");
    // The child inherited both pipe ends, so closing ours alone cannot wake
    // its read. Stop it without releasing it into an unconfigured cgroup.
    ::kill(pid, SIGKILL);
    ::close(sync[1]);
    end_ready_watch(pr);
    return failure(error);
  }
  ssize_t count;
  do {
    count = ::write(sync[1], "x", 1);
  } while (count < 0 && errno == EINTR);
  if (count != 1) {
    if (count == 0) errno = EIO;
    const auto error = system_error("write cgroup release pipe for", id);
    ::kill(pid, SIGKILL);
    ::close(sync[1]);
    end_ready_watch(pr);
    return failure(error);
  }
  ::close(sync[1]);
  pr.pid = pid;
  pr.running = true;
  projects[id] = pr;
  slot_mount.started = true;
  logf("started %s (uid %u, pid %d)", id.c_str(), pr.uid, pid);
  publish_status(pr, G::ProjectPhase::Starting);
  G::ReplyT reply;
  reply.pid = pid;
  return reply;
}

auto stop_project(const std::string& id) -> void {
  std::unique_lock<std::mutex> l(mu);
  auto it = projects.find(id);
  if (it == projects.end() || !it->second.running) return;
  pid_t pid = it->second.pid;
  ::kill(pid, SIGTERM);
  // tsugumid records interrupted runs on SIGTERM before exiting.
  exited.wait(l, [&] { return !it->second.running; });
}

auto clear_build_roots(const std::string& id) -> G::ReplyT {
  std::lock_guard lock(mu);
  if (auto it = projects.find(id); it != projects.end() && it->second.running)
    return failure("the project is running; retry when it is stopped");
  const auto folder = fs::path(kRoots) / id;
  const auto status = fs::symlink_status(folder);
  if (status.type() == fs::file_type::not_found) return {};
  if (!fs::is_directory(status)) return failure("invalid build root directory");
  G::ReplyT reply;
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (fs::is_symlink(entry.symlink_status()) && fs::remove(entry.path()))
      reply.build_roots_removed = true;
  }
  return reply;
}

auto forget_project(const std::string& id) -> G::ReplyT {
  std::lock_guard lock(mu);
  if (auto it = projects.find(id); it != projects.end() && it->second.running)
    return failure("the project is running; stop it first");
  std::error_code error;
  fs::remove_all(kRoots + "/" + id, error);
  if (error) return failure(error.message());
  fs::remove_all(kScratch + "/" + id, error);
  if (error) return failure(error.message());
  const auto cg = cgroup_dir(id);
  if (fs::exists(cg))
    for (const auto& leaf : fs::directory_iterator(cg))
      if (leaf.is_directory() && ::rmdir(leaf.path().c_str()) != 0)
        return failure(system_error("rmdir", leaf.path().string()));
  if (::rmdir(cg.c_str()) != 0 && errno != ENOENT)
    return failure(system_error("rmdir", cgroup_dir(id)));
  projects.erase(id);
  return {};
}

auto reap_children(sigset_t signals) -> void {
  for (;;) {
    int number = 0;
    ::sigwait(&signals, &number);
    for (;;) {
      int status = 0;
      const auto pid = ::waitpid(-1, &status, WNOHANG);
      if (pid < 0 && errno == EINTR) continue;
      if (pid <= 0) break;
      if (pid == rosettad_pid) {
        rosettad_pid = -1;
        if (rosettad_listening && !powering_off)
          logf(
              "Rosetta AOT: rosettad exited (%s %d); translating without the "
              "cache",
              WIFEXITED(status) ? "status" : "signal",
              WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
        continue;
      }
      std::lock_guard lock(mu);
      const auto project = std::find_if(
          projects.begin(), projects.end(), [pid](const auto& entry) {
            return entry.second.running && entry.second.pid == pid;
          });
      if (project == projects.end()) {
        exit_status[pid] = status;
      } else {
        auto& p = project->second;
        p.running = false;
        end_ready_watch(p);
        if (!write_file(cgroup_dir(p.id) + "/cgroup.kill", "1"))
          logf("cannot drain project %s: %s", p.id.c_str(),
               std::strerror(errno));
        if (::umount2(slot_dir(p.slot).c_str(), MNT_DETACH) != 0)
          logf("cannot unmount project %s: %s", p.id.c_str(),
               std::strerror(errno));
        logf("project %s exited (%s %d)", p.id.c_str(),
             WIFEXITED(status) ? "status" : "signal",
             WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
        publish_status(p, G::ProjectPhase::Exited, status);
        publish_memory();
      }
      exited.notify_all();
    }
  }
}

// ---- terminals ----

struct Session {
  int master = -1;
  pid_t pid = 0;
};
std::map<uint64_t, Session> sessions;
std::atomic<uint64_t> next_session{1};

void shell(int conn, uint64_t request_id, const G::OpenTerminalT& request) {
  const auto& id = request.key;
  const auto reject = [&](const char* reason) {
    control_wire::Send(conn, failure(reason), request_id);
  };
  pid_t target;
  uid_t uid;
  {
    std::lock_guard<std::mutex> l(mu);
    auto it = projects.find(id);
    if (it == projects.end() || !it->second.running) {
      reject("the project is not running");
      return;
    }
    target = it->second.pid;
    uid = it->second.uid;
  }
  int master;
  int slave;
  winsize ws{request.rows, request.cols, 0, 0};
  if (::openpty(&master, &slave, nullptr, nullptr, &ws) != 0) {
    reject("openpty");
    return;
  }
  // Neither end leaks into processes other threads start meanwhile.
  ::fcntl(master, F_SETFD, FD_CLOEXEC);
  ::fcntl(slave, F_SETFD, FD_CLOEXEC);
  // The child of this multithreaded process allocates nothing: prepared here.
  std::vector<std::string> ns_paths;
  for (const char* ns : {"ipc", "uts", "pid", "mnt"})
    ns_paths.push_back("/proc/" + std::to_string(target) + "/ns/" + ns);
  std::string procs = cgroup_dir(id) + "/core/cgroup.procs";
  std::vector<std::string> env = project_env(id);
  env.push_back("TERM=xterm-256color");
  env.push_back(
      "PS1=\\[\\e[1;32m\\]\\h\\[\\e[0m\\]:\\[\\e[1;34m\\]\\w\\[\\e[0m\\]\\$ ");
  std::vector<std::string> argv = {opt.bash, "--noprofile", "--norc", "-i"};
  auto av = cstrings(argv);
  auto ev = cstrings(env);
  pid_t pid = ::fork();
  if (pid == 0) {
    // This wrapper waits for the shell instead of execing. Close all inherited
    // sockets and PTYs except this shell's slave, even the CLOEXEC descriptors.
    // Otherwise another tab's wrapper can keep its PTY master alive after it
    // closes.
    const auto first = static_cast<unsigned int>(STDERR_FILENO + 1);
    const auto terminal = static_cast<unsigned int>(slave);
    if ((terminal > first &&
         ::syscall(SYS_close_range, first, terminal - 1, 0) != 0) ||
        ::syscall(SYS_close_range, terminal + 1, ~0u, 0) != 0)
      die("close inherited terminal descriptors");
    // Join the project's namespaces (the PID namespace applies to the next
    // child). All are opened first: /proc changes with the mount namespace.
    int nsfd[4];
    for (int i = 0; i < 4; ++i)
      if ((nsfd[i] = ::open(ns_paths[i].c_str(), O_RDONLY | O_CLOEXEC)) < 0)
        die("open a namespace");
    for (int fd : nsfd)
      if (::setns(fd, 0) != 0) die("setns");
    // Into the project's cgroup, so stopping the project ends its terminals
    // too.
    int fd = ::open(procs.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) die(procs.c_str());
    ssize_t count;
    do {
      count = ::write(fd, "0", 1);
    } while (count < 0 && errno == EINTR);
    if (count != 1) {
      if (count == 0) errno = EIO;
      die(procs.c_str());
    }
    ::close(fd);
    pid_t inner = ::fork();
    if (inner != 0) {
      int st = 0;
      ::waitpid(inner, &st, 0);
      _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 1);
    }
    ::setsid();
    ::ioctl(slave, TIOCSCTTY, 0);
    ::dup2(slave, 0);
    ::dup2(slave, 1);
    ::dup2(slave, 2);
    if (slave > 2) ::close(slave);
    drop_to(uid);
    if (::chdir("/project") != 0) die("chdir /project");
    sigset_t empty;
    sigemptyset(&empty);
    ::sigprocmask(SIG_SETMASK, &empty, nullptr);
    ::execve(av[0], av.data(), ev.data());
    die("exec bash");
  }
  ::close(slave);
  if (pid < 0) {
    ::close(master);
    reject("fork");
    return;
  }
  uint64_t sid = next_session++;
  {
    std::lock_guard<std::mutex> l(mu);
    sessions[sid] = {master, pid};
  }
  G::ReplyT reply;
  reply.session = sid;
  const bool connected = control_wire::Send(conn, reply, request_id);
  logf("terminal %llu for %s", static_cast<unsigned long long>(sid),
       id.c_str());
  char buf[16384];
  pollfd fds[2] = {{conn, POLLIN, 0}, {master, POLLIN, 0}};
  while (connected) {
    if (::poll(fds, 2, -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      ssize_t n = ::read(conn, buf, sizeof buf);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0 ||
          !control_wire::WriteAll(master, buf, static_cast<size_t>(n)))
        break;
    }
    if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      ssize_t n = ::read(master, buf, sizeof buf);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0 || !control_wire::WriteAll(conn, buf, static_cast<size_t>(n)))
        break;
    }
  }
  ::kill(pid, SIGHUP);
  {
    std::lock_guard<std::mutex> l(mu);
    sessions.erase(sid);
  }
  ::close(master);
}

auto resize(const G::ResizeT& request) -> G::ReplyT {
  std::lock_guard<std::mutex> l(mu);
  auto it = sessions.find(request.session);
  if (it == sessions.end()) return failure("no such terminal");
  winsize ws{request.rows, request.cols, 0, 0};
  return ::ioctl(it->second.master, TIOCSWINSZ, &ws) == 0
             ? G::ReplyT{}
             : failure(std::strerror(errno));
}

// ---- connections ----

int listen_vsock(int port) {
  int fd = ::socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  sockaddr_vm a{};
  a.svm_family = AF_VSOCK;
  a.svm_cid = VMADDR_CID_ANY;
  a.svm_port = static_cast<unsigned>(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 ||
      ::listen(fd, 64) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool wait_for_empty_cgroup(const std::string& id) {
  const auto path = cgroup_dir(id) + "/cgroup.events";
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return errno == ENOENT;
  bool empty = false;
  for (;;) {
    char state[256];
    const auto size = ::pread(fd, state, sizeof state, 0);
    if (size < 0 && errno == EINTR) continue;
    if (size <= 0) break;
    if (std::string_view(state, static_cast<size_t>(size))
            .starts_with("populated 0\n")) {
      empty = true;
      break;
    }
    pollfd changed{fd, POLLPRI, 0};
    if (::poll(&changed, 1, -1) < 0 && errno != EINTR) break;
  }
  ::close(fd);
  return empty;
}

auto signal_daemon(int fd, int sig) -> bool {
  return ::syscall(SYS_pidfd_send_signal, fd, sig, nullptr, 0) == 0 ||
         errno == ESRCH;
}

auto wait_for_daemon_exit(pollfd& process) -> bool {
  int ready;
  do {
    ready = ::poll(&process, 1, -1);
  } while (ready < 0 && errno == EINTR);
  return ready > 0 && (process.revents & (POLLIN | POLLHUP));
}

auto stop_rosetta_cache() -> bool {
  if (rosettad_pidfd < 0) return true;
  if (!signal_daemon(rosettad_pidfd, SIGTERM)) {
    logf("Rosetta AOT: cannot terminate rosettad: %s", std::strerror(errno));
    return false;
  }
  pollfd process{rosettad_pidfd, POLLIN, 0};
  if (!wait_for_daemon_exit(process)) {
    logf("Rosetta AOT: cannot verify that rosettad exited");
    return false;
  }
  ::close(rosettad_pidfd);
  return true;
}

void shutdown_vm() {
  if (powering_off.exchange(true)) return;
  std::vector<std::string> ids;
  {
    std::lock_guard<std::mutex> l(mu);
    for (const auto& [id, _] : projects) ids.push_back(id);
  }
  std::vector<std::jthread> stops;
  for (const auto& id : ids) stops.emplace_back([id] { stop_project(id); });
  stops.clear();
  // Run with the boot image's tools, independent of a cached vm-system.
  // The reaper may collect this child; the marker is managed by the script.
  bool drained = true;
  for (const auto& id : ids) drained = wait_for_empty_cgroup(id) && drained;
  if (!drained)
    logf("cannot verify that project cgroups drained; keeping dirty marker");
  const bool cache_stopped = stop_rosetta_cache();
  if (!cache_stopped) logf("Rosetta AOT: keeping the dirty marker");
  const auto child = drained && cache_stopped ? ::fork() : -1;
  if (child < 0 && drained && cache_stopped)
    logf("cannot start store-health: %s", std::strerror(errno));
  if (child == 0) {
    ::execl("/run/tsugumi/store-health", "store-health", "poweroff", "/persist",
            "/run/tsugumi/vmd", nullptr);
    ::_exit(127);
  }
  if (child > 0) {
    logf("syncing persistent disks");
    std::unique_lock lock(mu);
    exited.wait(lock, [&] { return exit_status.contains(child); });
    if (auto it = exit_status.find(child); it != exit_status.end()) {
      if (it->second != 0)
        logf("store shutdown failed; next boot will check the store");
      exit_status.erase(it);
    }
  }
  logf("powering off");
  ::sync();
  if (::reboot(RB_POWER_OFF) != 0)
    logf("cannot power off: %s", std::strerror(errno));
}

auto command(const G::CommandUnion& request) -> G::ReplyT {
  if (request.AsReadMemory()) {
    auto memory = read_memory();
    if (!memory) return failure("cannot read guest memory information");
    G::ReplyT reply;
    reply.memory = std::make_unique<G::MemoryT>(std::move(*memory));
    return reply;
  }
  if (const auto* start = request.AsStart()) return start_project(*start);
  if (const auto* stop = request.AsStop()) {
    if (!valid_id(stop->key)) return failure("invalid project key");
    stop_project(stop->key);
    return {};
  }
  if (const auto* clear = request.AsClearBuildRoots()) {
    if (!valid_id(clear->key)) return failure("invalid project key");
    return clear_build_roots(clear->key);
  }
  if (const auto* forget = request.AsForget()) {
    if (!valid_id(forget->key)) return failure("invalid project key");
    return forget_project(forget->key);
  }
  if (const auto* size = request.AsResize()) return resize(*size);
  if (const auto* clock = request.AsClock()) {
    const auto ms = clock->unix_ms;
    if (ms <= 0) return failure("invalid VM clock");
    const timespec ts{static_cast<time_t>(ms / 1000),
                      static_cast<long>((ms % 1000) * 1000000)};
    return ::clock_settime(CLOCK_REALTIME, &ts) == 0
               ? G::ReplyT{}
               : failure(std::string("clock: ") + std::strerror(errno));
  }
  if (const auto* maintenance = request.AsMaintenance()) {
    switch (maintenance->action) {
      case G::MaintenanceAction::SnapshotCacheUsage:
        cache_usage->snapshot();
        return {};
      case G::MaintenanceAction::CollectStore:
        return collect_garbage(false);
      case G::MaintenanceAction::VerifyStore:
        return collect_garbage(true);
      case G::MaintenanceAction::Shutdown:
        return {};
    }
  }
  return failure("unknown control command");
}

auto control() -> void {
  send_control(G::ReadyT{});
  publish_memory();
  try {
    while (auto message = control_wire::ReadMessage(control_fd)) {
      const auto* request = message->payload.AsRequest();
      if (!request || message->request_id == 0)
        throw std::runtime_error("expected a request with an ID");
      std::unique_lock operation(lifecycle);
      if (powering_off) return;
      const auto* maintenance = request->command.AsMaintenance();
      const bool shutdown =
          maintenance && maintenance->action == G::MaintenanceAction::Shutdown;
      G::ReplyT result;
      try {
        result = command(request->command);
      } catch (const std::exception& error) {
        result = failure(error.what());
      }
      if (!send_control(std::move(result), message->request_id)) break;
      if (shutdown) {
        shutdown_vm();
        return;
      }
      if (maintenance &&
          maintenance->action != G::MaintenanceAction::SnapshotCacheUsage)
        publish_memory();
    }
    logf("control stream closed");
  } catch (const std::exception& error) {
    logf("control stream: %s", error.what());
  }
  std::unique_lock operation(lifecycle);
  shutdown_vm();
}

void terminal(int conn) {
  try {
    if (auto message = control_wire::ReadMessage(conn)) {
      const auto* request = message->payload.AsRequest();
      const auto* open = request ? request->command.AsOpenTerminal() : nullptr;
      if (open && message->request_id != 0 && valid_id(open->key))
        shell(conn, message->request_id, *open);
      else
        control_wire::Send(conn, failure("invalid terminal request"),
                           message->request_id);
    }
  } catch (const std::exception& error) {
    logf("terminal stream: %s", error.what());
  }
  ::close(conn);
}

void rpc(int conn) {
  std::string id;
  try {
    const auto route = control_wire::ReadFrame(conn);
    id.assign(route.begin(), route.end());
  } catch (const std::exception& error) {
    logf("RPC route: %s", error.what());
  }
  bool running = false;
  {
    std::lock_guard<std::mutex> l(mu);
    if (auto it = projects.find(id); it != projects.end() && it->second.running)
      running = true;
  }
  if (!running) {
    ::close(conn);
    return;
  }
  int up = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::string sock = rpc_dir(id) + "/s";
  std::strncpy(a.sun_path, sock.c_str(), sizeof a.sun_path - 1);
  if (up < 0 || ::connect(up, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    if (up >= 0) ::close(up);
    ::close(conn);
    return;
  }
  char buf[65536];
  pollfd fds[2] = {{conn, POLLIN, 0}, {up, POLLIN, 0}};
  for (;;) {
    if (::poll(fds, 2, -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    bool done = false;
    for (int i = 0; i < 2 && !done; ++i) {
      if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      const auto count = ::read(fds[i].fd, buf, sizeof buf);
      if (count < 0 && errno == EINTR) continue;
      done = count <= 0 || !control_wire::WriteAll(fds[1 - i].fd, buf,
                                                   static_cast<size_t>(count));
    }
    if (done) break;
  }
  ::close(up);
  ::close(conn);
}

// Boot waits for the daemon's socket or for the process to exit.
auto wait_for_daemon_socket(
    int child, const fs::path& socket,
    std::optional<std::chrono::steady_clock::time_point> deadline = {})
    -> bool {
  const int events = ::inotify_init1(IN_CLOEXEC);
  if (events < 0) return false;
  bool listening = false;
  if (::inotify_add_watch(events, socket.parent_path().c_str(),
                          IN_CREATE | IN_MOVED_TO | IN_DELETE_SELF) >= 0) {
    for (;;) {
      struct stat st{};
      if (::stat(socket.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) {
        listening = true;
        break;
      }
      int timeout = -1;
      if (deadline) {
        const auto remaining = *deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) break;
        timeout = static_cast<int>(
            std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
      }
      pollfd fds[] = {{child, POLLIN, 0}, {events, POLLIN, 0}};
      const auto ready = ::poll(fds, 2, timeout);
      if (ready < 0 && errno == EINTR) continue;
      if (ready <= 0 || fds[0].revents || !(fds[1].revents & POLLIN)) break;
      std::array<char, 4096> buffer;
      if (::read(events, buffer.data(), buffer.size()) <= 0) break;
    }
  }
  ::close(events);
  return listening;
}

int wait_daemon(pid_t pid, const fs::path& socket) {
  const int child = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
  if (child < 0) return 1;
  const bool ready = wait_for_daemon_socket(child, socket);
  ::close(child);
  return ready ? 0 : 1;
}

auto start_rosetta_cache() -> void {
  if (::access("/run/rosetta/rosetta", X_OK) != 0) return;
  constexpr auto executable = "/run/rosetta/rosettad";
  constexpr auto cache = "/persist/rosettad";
  const fs::path socket = fs::path(cache) / "uds" / "rosetta.sock";
  const fs::path visible = "/run/rosettad/rosetta.sock";
  try {
    if (::access(executable, X_OK) != 0)
      throw std::runtime_error(system_error("access", executable));
    fs::create_directories(socket.parent_path());
    if (::chmod(cache, 0700) != 0)
      throw std::runtime_error(system_error("chmod", cache));
    fs::remove(socket);
    fs::create_directories(visible.parent_path());
    // Acquire the lifetime handle when creating the child.
    struct clone_args args{};
    args.flags = CLONE_PIDFD;
    args.pidfd = reinterpret_cast<uintptr_t>(&rosettad_pidfd);
    args.exit_signal = SIGCHLD;
    const auto child =
        static_cast<pid_t>(::syscall(SYS_clone3, &args, sizeof args));
    if (child < 0) throw std::runtime_error(system_error("start", executable));
    if (child == 0) {
      sigset_t mask;
      ::sigemptyset(&mask);
      ::sigprocmask(SIG_SETMASK, &mask, nullptr);
      ::signal(SIGPIPE, SIG_DFL);
      ::execl(executable, executable, "daemon", cache, nullptr);
      std::perror(executable);
      ::_exit(127);
    }
    rosettad_pid = child;
    // This optional service must not block project startup if it wedges.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    if (!wait_for_daemon_socket(rosettad_pidfd, socket, deadline))
      throw std::runtime_error(
          "rosettad exited or did not provide a usable socket");
    if (::chmod(socket.c_str(), 0666) != 0)
      throw std::runtime_error(system_error("chmod", socket));
    const int mountpoint =
        ::open(visible.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (mountpoint < 0)
      throw std::runtime_error(system_error("create", visible));
    ::close(mountpoint);
    if (::mount(socket.c_str(), visible.c_str(), nullptr, MS_BIND, nullptr) !=
        0) {
      const auto error = system_error("bind", visible);
      fs::remove(visible);
      throw std::runtime_error(error);
    }
    rosettad_listening = true;
    logf("Rosetta AOT: cache socket ready at %s", visible.c_str());
  } catch (const std::exception& error) {
    if (rosettad_pidfd >= 0 && !signal_daemon(rosettad_pidfd, SIGKILL))
      logf("Rosetta AOT: cannot kill rosettad: %s", std::strerror(errno));
    logf("Rosetta AOT: %s; translating without the cache", error.what());
  }
}

// End every writer before the caller flushes the store and clears its marker.
int stop_nix() {
  for (;;) {
    std::vector<pollfd> daemons;
    for (const auto& proc : fs::directory_iterator("/proc")) {
      const auto name = proc.path().filename().string();
      char* end = nullptr;
      const auto pid = std::strtol(name.c_str(), &end, 10);
      if (*end || pid <= 0) continue;
      const int fd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
      if (fd < 0) {
        if (errno == ESRCH) continue;
        return 1;
      }
      std::string comm;
      std::ifstream status(proc.path() / "comm");
      pollfd process{fd, POLLIN, 0};
      if (!std::getline(status, comm) || comm != "nix-daemon" ||
          ::poll(&process, 1, 0) > 0) {
        ::close(fd);
        continue;
      }
      if (!signal_daemon(fd, SIGTERM)) return 1;
      daemons.push_back(process);
    }
    if (daemons.empty()) return 0;
    for (auto& process : daemons) {
      const bool stopped = wait_for_daemon_exit(process);
      ::close(process.fd);
      if (!stopped) return 1;
    }
  }
}

void serve(int fd, void (*handler)(int)) {
  for (;;) {
    sockaddr_vm peer{};
    socklen_t size = sizeof(peer);
    const int c =
        ::accept4(fd, reinterpret_cast<sockaddr*>(&peer), &size, SOCK_CLOEXEC);
    if (c < 0 && errno == EINTR) continue;
    if (c < 0) {
      logf("cannot accept a vsock connection: %s", std::strerror(errno));
      ::shutdown(control_fd, SHUT_RDWR);
      return;
    }
    // A project cannot use the host's data routes to enter another project.
    if (peer.svm_cid != VMADDR_CID_HOST) {
      ::close(c);
      continue;
    }
    std::thread(handler, c).detach();
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--stop-nix") return stop_nix();
  if (argc == 4 && std::string_view(argv[1]) == "--wait-daemon")
    return wait_daemon(static_cast<pid_t>(std::atoi(argv[2])), argv[3]);
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    std::string v = argv[i + 1];
    if (k == "--tsugumid")
      opt.tsugumid = v;
    else if (k == "--tini")
      opt.tini = v;
    else if (k == "--bash")
      opt.bash = v;
    else if (k == "--path")
      opt.path = v;
    else if (k == "--cert-file")
      opt.cert_file = v;
    else if (k == "--host")
      opt.host = v;
    else if (k == "--state")
      opt.state = v;
    else {
      std::fprintf(stderr, "tsugumi-vmd: unknown option %s\n", k.c_str());
      return 2;
    }
  }
  if (opt.tsugumid.empty() || opt.tini.empty() || opt.bash.empty()) {
    std::fprintf(stderr,
                 "usage: tsugumi-vmd --tsugumid PATH --tini PATH "
                 "--bash PATH [--path PATH] [--cert-file FILE]\n");
    return 2;
  }
  ::signal(SIGPIPE, SIG_IGN);
  sigset_t stop_signals;
  sigemptyset(&stop_signals);
  for (const auto number : {SIGTERM, SIGINT, SIGHUP})
    sigaddset(&stop_signals, number);
  sigset_t child_signal, blocked = stop_signals;
  sigemptyset(&child_signal);
  sigaddset(&child_signal, SIGCHLD);
  sigaddset(&blocked, SIGCHLD);
  ::pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
  fs::create_directories("/sys/fs/cgroup/tsugumi");
  if (!write_file("/sys/fs/cgroup/cgroup.subtree_control", "+cpu +memory") ||
      !write_file("/sys/fs/cgroup/tsugumi/cgroup.subtree_control",
                  "+cpu +memory"))
    die("enable CPU and memory isolation for Cards");
  fs::create_directories("/run/tsugumi");
  fs::create_directories(opt.host);
  prepare_project_roots();
  fs::create_directories(kScratch);
  ::chmod(kScratch.c_str(), 0700);
  fs::create_directories(kRpc);
  ::chmod(kRpc.c_str(), 0711);
  start_rosetta_cache();
  tsugumi::NixStore::init_process();
  cache_usage = std::make_unique<tsugumi::CacheUsage>(
      kRoots, kScratch, fs::path{}, fs::path{}, "", kUsage);
  std::thread(reap_children, child_signal).detach();
  const int rpc_listener = listen_vsock(opt.rpc_port);
  const int terminal_listener = listen_vsock(opt.terminal_port);
  ready_events = ::inotify_init1(IN_CLOEXEC);
  control_fd = ::socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_vm host{};
  host.svm_family = AF_VSOCK;
  host.svm_cid = VMADDR_CID_HOST;
  host.svm_port = static_cast<unsigned>(opt.control_port);
  if (rpc_listener < 0 || terminal_listener < 0 || ready_events < 0 ||
      control_fd < 0 ||
      ::connect(control_fd, reinterpret_cast<sockaddr*>(&host), sizeof host) !=
          0) {
    std::fprintf(stderr,
                 "TSUGUMI-FAILED cannot establish VM control and listeners\n");
    shutdown_vm();
    return 1;
  }
  {
    std::lock_guard lock(control_write);
    control_writable = true;
  }
  std::thread(monitor_readiness).detach();
  std::thread([stop_signals]() mutable {
    int number = 0;
    sigwait(&stop_signals, &number);
    std::unique_lock operation(lifecycle);
    shutdown_vm();
  }).detach();
  std::thread(serve, rpc_listener, rpc).detach();
  std::thread(serve, terminal_listener, terminal).detach();
  control();
}
