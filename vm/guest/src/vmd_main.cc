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
// The host talks to vsock ports:
//
//   1024  control, one request line per connection, one reply line:
//           ping                            -> ok
//           start <id> <name> <slot> [origins] -> ok <id>
//           stop <id>                       -> ok
//           forget <id>                     -> ok (stop and delete build
//                                              roots and scratch)
//           forget-stopped <id>             -> ok (delete for a stopped project)
//           gc                              -> ok (remove store paths without
//                                              GC roots)
//           status                          -> ok <id>:<pid>:<port> ...
//           shell <id> <rows> <cols>        -> ok <session>, then the
//                                              connection is the terminal
//           resize <session> <rows> <cols>  -> ok
//           clock <unix ms>                 -> ok (the Mac's time; run times
//           and
//                                              the app's clock then agree)
//           shutdown                        -> ok (then the VM powers off)
//   1025  RPC: the first line names the project id, then the bytes go to
//         and from that project's tsugumid (its Unix socket, under kRpc).
//
// Everything is logged to stderr, which is the VM console.

#include <arpa/inet.h>
#include <fcntl.h>
#include <grp.h>
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

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "tsugumi/runtime_support/cache_usage.hh"
#include "tsugumi/workerd/store.hh"

namespace fs = std::filesystem;

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
};
Options opt;

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  std::fprintf(stderr, "vmd: %s\n", buf);
}

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  for (std::string w; in >> w;) out.push_back(w);
  return out;
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
  return std::string("error ") + operation + " " + path + ": " +
         std::strerror(errno);
}

std::string read_line(int fd) {
  std::string line;
  char c;
  while (line.size() < 4096) {
    ssize_t n = ::read(fd, &c, 1);
    if (n <= 0 || c == '\n') break;
    line += c;
  }
  return line;
}

void reply(int fd, const std::string& text) {
  std::string line = text + "\n";
  size_t offset = 0;
  while (offset < line.size()) {
    const auto count = ::write(fd, line.data() + offset, line.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      // A disconnected caller cannot receive another error reply. The request
      // has already run; report the transport failure on the VM console.
      logf("cannot write reply: %s", std::strerror(count < 0 ? errno : EIO));
      return;
    }
    offset += static_cast<size_t>(count);
  }
}

// ---- projects ----

struct Project {
  std::string id;
  int slot = -1;
  uid_t uid = 0;
  pid_t pid = 0;  // tini, PID 1 of the project's PID namespace
  bool running = false;
};

std::mutex mu;
std::mutex lifecycle;
std::atomic<bool> powering_off{false};
std::condition_variable exited;
std::map<std::string, Project> projects;
std::map<pid_t, int> exit_status;  // children reaped by the reaper thread

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
std::string collect_garbage(bool verify = false) {
  if (verify) {
    std::lock_guard lock(mu);
    for (const auto& [_, project] : projects)
      if (project.running)
        return "error stop all projects before checking the store";
  }
  int out[2];
  if (::pipe2(out, O_CLOEXEC) != 0) return "error pipe";
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
  if (pid < 0) return "error cannot start store maintenance";
  int st;
  {
    std::unique_lock lock(mu);
    exited.wait(lock, [&] { return exit_status.contains(pid); });
    st = exit_status.at(pid);
    exit_status.erase(pid);
  }
  logf("store %s: %s", verify ? "check/repair" : "cleanup", text.c_str());
  if (verify) {
    // Full diagnostics stay in check.log and on the VM console. Bound the RPC
    // reply, retaining its last lines (failed paths and final result).
    if (text.size() > 65536) text.erase(0, text.size() - 65536);
    for (auto& ch : text)
      if (ch == '\n' || ch == '\r') ch = ' ';
    return std::string(WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "ok "
                                                             : "error ") +
           text;
  }
  if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0))
    return "error nix-store --gc failed";
  return "ok";
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
    return std::string("error cannot mount ") + tag + ": " +
           std::strerror(errno);
  return "";
}

auto start_project(const std::string& id, const std::string& name, int slot,
                   const std::string& origins) -> std::string {
  std::unique_lock<std::mutex> l(mu);
  if (auto it = projects.find(id); it != projects.end() && it->second.running)
    return "ok " + id;
  if (auto err = mount_slot(slot); !err.empty()) return err;
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
  if (auto error = prepare(roots, 0755); !error.empty()) return error;
  ::utimensat(AT_FDCWD, roots.c_str(), nullptr, AT_SYMLINK_NOFOLLOW);
  std::string scratch = kScratch + "/" + id;
  if (auto error = prepare(scratch, 0700); !error.empty()) return error;
  std::string rpc = rpc_dir(id);
  if (auto error = prepare(rpc, 0700); !error.empty()) return error;
  std::string cg = cgroup_dir(id);
  fs::create_directories(cg);
  // The control folder is the project's: a link there is never followed by
  // root.
  std::string log_path = c + "/tsugumid.log";
  int log_fd =
      ::open(log_path.c_str(),
             O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (log_fd < 0)
    return std::string("error cannot open tsugumid.log: ") +
           std::strerror(errno);
  int sync[2];
  if (::pipe2(sync, O_CLOEXEC) != 0) {
    ::close(log_fd);
    return "error pipe";
  }
  ChildArgs args{name,
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
  // The pages that may connect (the Mac's tsugumi command): comma-separated.
  // Without any, as for the app, a browser's request is refused.
  for (size_t at = 0; at < origins.size();) {
    size_t end = std::min(origins.find(',', at), origins.size());
    args.argv.insert(args.argv.end(),
                     {"--allow-origin", origins.substr(at, end - at)});
    at = end + 1;
  }
  args.env = project_env(id);
  args.env.emplace_back("TSUGUMI_EXECUTION_BROKER=vsock:1027");
  args.env.emplace_back(
      "TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE=/control/"
      "execution-broker-capability");
  args.av = cstrings(args.argv);
  args.ev = cstrings(args.env);
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
    return std::string("error clone: ") + std::strerror(errno);
  }
  if (!write_file(cg + "/cgroup.procs", std::to_string(pid))) {
    const auto error = system_error("write", cg + "/cgroup.procs");
    // The child inherited both pipe ends, so closing ours alone cannot wake
    // its read. Stop it without releasing it into an unconfigured cgroup.
    ::kill(pid, SIGKILL);
    ::close(sync[1]);
    return error;
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
    return error;
  }
  ::close(sync[1]);
  pr.pid = pid;
  pr.running = true;
  projects[id] = pr;
  logf("started %s (uid %u, pid %d)", id.c_str(), pr.uid, pid);
  return "ok " + id;
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

auto forget_project(const std::string& id, bool stop) -> std::string {
  if (stop) {
    stop_project(id);
  } else {
    std::lock_guard lock(mu);
    if (auto it = projects.find(id); it != projects.end() && it->second.running)
      return "error the project is running; stop it first";
  }
  std::error_code ec;
  fs::remove_all(kRoots + "/" + id, ec);
  fs::remove_all(kScratch + "/" + id, ec);
  ::rmdir(cgroup_dir(id).c_str());
  std::lock_guard<std::mutex> l(mu);
  projects.erase(id);
  return ec ? "error " + ec.message() : "ok";
}

std::string status() {
  std::lock_guard<std::mutex> l(mu);
  std::string out = "ok";
  for (const auto& [id, p] : projects)
    if (p.running) out += " " + id + ":" + std::to_string(p.pid);
  return out;
}

void reaper() {
  for (;;) {
    int st = 0;
    pid_t pid = ::waitpid(-1, &st, 0);
    if (pid < 0) {
      if (errno == ECHILD) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      continue;
    }
    std::lock_guard<std::mutex> l(mu);
    exit_status[pid] = st;
    for (auto& [id, p] : projects) {
      if (p.pid == pid && p.running) {
        p.running = false;
        write_file(cgroup_dir(id) + "/cgroup.kill", "1");
        ::umount2(slot_dir(p.slot).c_str(), MNT_DETACH);
        logf("project %s exited (%s %d)", id.c_str(),
             WIFEXITED(st) ? "status" : "signal",
             WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
      }
    }
    exited.notify_all();
  }
}

// ---- terminals ----

struct Session {
  int master = -1;
  pid_t pid = 0;
};
std::map<uint64_t, Session> sessions;
std::atomic<uint64_t> next_session{1};

void shell(int conn, const std::string& id, int rows, int cols) {
  pid_t target;
  uid_t uid;
  {
    std::lock_guard<std::mutex> l(mu);
    auto it = projects.find(id);
    if (it == projects.end() || !it->second.running) {
      reply(conn, "error the project is not running");
      return;
    }
    target = it->second.pid;
    uid = it->second.uid;
  }
  int master;
  int slave;
  winsize ws{static_cast<unsigned short>(rows),
             static_cast<unsigned short>(cols), 0, 0};
  if (::openpty(&master, &slave, nullptr, nullptr, &ws) != 0) {
    reply(conn, "error openpty");
    return;
  }
  // Neither end leaks into processes other threads start meanwhile.
  ::fcntl(master, F_SETFD, FD_CLOEXEC);
  ::fcntl(slave, F_SETFD, FD_CLOEXEC);
  // The child of this multithreaded process allocates nothing: prepared here.
  std::vector<std::string> ns_paths;
  for (const char* ns : {"ipc", "uts", "pid", "mnt"})
    ns_paths.push_back("/proc/" + std::to_string(target) + "/ns/" + ns);
  std::string procs = cgroup_dir(id) + "/cgroup.procs";
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
    reply(conn, "error fork");
    return;
  }
  uint64_t sid = next_session++;
  {
    std::lock_guard<std::mutex> l(mu);
    sessions[sid] = {master, pid};
  }
  reply(conn, "ok " + std::to_string(sid));
  logf("terminal %llu for %s", static_cast<unsigned long long>(sid),
       id.c_str());
  char buf[16384];
  pollfd fds[2] = {{conn, POLLIN, 0}, {master, POLLIN, 0}};
  for (;;) {
    if (::poll(fds, 2, -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      ssize_t n = ::read(conn, buf, sizeof buf);
      if (n <= 0 || ::write(master, buf, n) != n) break;
    }
    if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      ssize_t n = ::read(master, buf, sizeof buf);
      if (n <= 0 || ::write(conn, buf, n) != n) break;
    }
  }
  ::kill(pid, SIGHUP);
  {
    std::lock_guard<std::mutex> l(mu);
    sessions.erase(sid);
  }
  ::close(master);
}

std::string resize(uint64_t sid, int rows, int cols) {
  std::lock_guard<std::mutex> l(mu);
  auto it = sessions.find(sid);
  if (it == sessions.end()) return "error no such terminal";
  winsize ws{static_cast<unsigned short>(rows),
             static_cast<unsigned short>(cols), 0, 0};
  return ::ioctl(it->second.master, TIOCSWINSZ, &ws) == 0 ? "ok"
                                                          : "error resize";
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
    if (std::string_view(state, static_cast<size_t>(size)).starts_with("populated 0\n")) {
      empty = true;
      break;
    }
    pollfd changed{fd, POLLPRI, 0};
    if (::poll(&changed, 1, -1) < 0 && errno != EINTR) break;
  }
  ::close(fd);
  return empty;
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
  logf("syncing and remounting persistent disks read-only");
  // Run with the boot image's tools, independent of a cached vm-system.
  // The reaper may collect this child; the marker is managed by the script.
  bool drained = true;
  for (const auto& id : ids) drained = wait_for_empty_cgroup(id) && drained;
  if (!drained) logf("cannot verify that project cgroups drained; keeping dirty marker");
  const auto child = drained ? ::fork() : -1;
  if (child == 0) {
    ::execl("/run/tsugumi/store-health", "store-health", "poweroff", "/persist",
            "/run/tsugumi/vmd", nullptr);
    ::_exit(127);
  }
  if (child > 0) {
    std::unique_lock lock(mu);
    exited.wait(lock, [&] { return exit_status.contains(child); });
    if (auto it = exit_status.find(child); it != exit_status.end()) {
      if (it->second != 0)
        logf("read-only remount failed; next boot will check the store");
      exit_status.erase(it);
    }
  }
  logf("powering off");
  ::sync();
  ::reboot(RB_POWER_OFF);
}

// "ok <MemTotal> <MemAvailable> <Cached>" in KiB: the host's balloon follows
// how much of its memory the guest has (MemTotal shrinks as it inflates).
std::string memory() {
  std::ifstream in("/proc/meminfo");
  std::map<std::string, std::string> values;
  std::string key, value, unit;
  while (in >> key >> value) {
    std::getline(in, unit);
    values[key] = value;
  }
  return "ok " + values["MemTotal:"] + " " + values["MemAvailable:"] + " " +
         values["Cached:"];
}

void control(int conn) {
  auto w = split(read_line(conn));
  std::unique_lock operation(lifecycle);
  if (powering_off) {
    reply(conn, "error VM is shutting down");
    ::close(conn);
    return;
  }
  std::string r;
  if (w.empty())
    r = "error empty request";
  else if (w[0] == "ping")
    r = "ok";
  else if (w[0] == "status")
    r = status();
  else if (w[0] == "memory")
    r = memory();
  else if (w[0] == "cache-usage") {
    cache_usage->snapshot();
    r = "ok";
  } else if (w[0] == "start" && (w.size() == 4 || w.size() == 5))
    r = start_project(w[1], w[2], std::atoi(w[3].c_str()),
                      w.size() == 5 ? w[4] : "");
  else if (w[0] == "stop" && w.size() == 2) {
    stop_project(w[1]);
    r = "ok";
  } else if ((w[0] == "forget" || w[0] == "forget-stopped") && w.size() == 2)
    r = forget_project(w[1], w[0] == "forget");
  else if (w[0] == "gc" && w.size() == 1)
    r = collect_garbage();
  else if (w[0] == "verify-store" && w.size() == 1)
    r = collect_garbage(true);
  else if (w[0] == "shell" && w.size() == 4) {
    operation.unlock();
    shell(conn, w[1], std::atoi(w[2].c_str()), std::atoi(w[3].c_str()));
    ::close(conn);
    return;
  } else if (w[0] == "resize" && w.size() == 4)
    r = resize(std::strtoull(w[1].c_str(), nullptr, 10),
               std::atoi(w[2].c_str()), std::atoi(w[3].c_str()));
  else if (w[0] == "clock" && w.size() == 2) {
    long long ms = std::atoll(w[1].c_str());
    timespec ts{static_cast<time_t>(ms / 1000),
                static_cast<long>((ms % 1000) * 1000000)};
    r = ms > 0 && ::clock_settime(CLOCK_REALTIME, &ts) == 0
            ? "ok"
            : std::string("error clock: ") + std::strerror(errno);
  } else if (w[0] == "shutdown") {
    reply(conn, "ok");
    ::close(conn);
    shutdown_vm();
    return;
  } else
    r = "error unknown request";
  reply(conn, r);
  ::close(conn);
}

void rpc(int conn) {
  std::string id = read_line(conn);
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
      ssize_t n = ::read(fds[i].fd, buf, sizeof buf);
      if (n <= 0) {
        done = true;
        break;
      }
      for (ssize_t off = 0; off < n;) {
        ssize_t m = ::write(fds[1 - i].fd, buf + off, n - off);
        if (m <= 0) {
          done = true;
          break;
        }
        off += m;
      }
    }
    if (done) break;
  }
  ::close(up);
  ::close(conn);
}

// Boot waits for the daemon's socket or for the process to exit.
int wait_daemon(pid_t pid, const fs::path& socket) {
  const int child = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
  const int events = ::inotify_init1(IN_CLOEXEC);
  if (child < 0 || events < 0 ||
      ::inotify_add_watch(events, socket.parent_path().c_str(),
                          IN_CREATE | IN_MOVED_TO | IN_DELETE_SELF) < 0)
    return 1;
  for (;;) {
    struct stat st{};
    if (::stat(socket.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) return 0;
    pollfd fds[] = {{child, POLLIN, 0}, {events, POLLIN, 0}};
    const auto ready = ::poll(fds, 2, -1);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0 || fds[0].revents || !(fds[1].revents & POLLIN)) return 1;
    std::array<char, 4096> buffer;
    if (::read(events, buffer.data(), buffer.size()) <= 0) return 1;
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
      if (::syscall(SYS_pidfd_send_signal, fd, SIGTERM, nullptr, 0) < 0 &&
          errno != ESRCH)
        return 1;
      daemons.push_back(process);
    }
    if (daemons.empty()) return 0;
    for (auto& process : daemons) {
      int ready;
      do {
        ready = ::poll(&process, 1, -1);
      } while (ready < 0 && errno == EINTR);
      ::close(process.fd);
      if (ready < 0) return 1;
    }
  }
}

void serve(int port, void (*handler)(int), bool announce = false) {
  int fd = listen_vsock(port);
  if (fd < 0) {
    logf("cannot listen on vsock port %d: %s", port, std::strerror(errno));
    std::exit(1);
  }
  if (announce) std::fprintf(stderr, "TSUGUMI-STAGE ready\n");
  for (;;) {
    int c = ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (c < 0) continue;
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
  ::pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);
  fs::create_directories("/sys/fs/cgroup/tsugumi");
  fs::create_directories("/run/tsugumi");
  fs::create_directories(opt.host);
  prepare_project_roots();
  fs::create_directories(kScratch);
  ::chmod(kScratch.c_str(), 0700);
  fs::create_directories(kRpc);
  ::chmod(kRpc.c_str(), 0711);
  tsugumi::NixStore::init_process();
  cache_usage = std::make_unique<tsugumi::CacheUsage>(
      kRoots, kScratch, fs::path{}, fs::path{}, "", kUsage);
  std::thread(reaper).detach();
  std::thread([stop_signals]() mutable {
    int number = 0;
    sigwait(&stop_signals, &number);
    std::unique_lock operation(lifecycle);
    shutdown_vm();
  }).detach();
  std::thread(serve, opt.rpc_port, rpc, false).detach();
  logf("ready (control vsock %d, rpc vsock %d)", opt.control_port,
       opt.rpc_port);
  // The host waits for this line on the console before connecting.
  serve(opt.control_port, control, true);
}
