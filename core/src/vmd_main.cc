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
//   Nix            the shared store through the one nix-daemon; build roots
//                  under /nix/var/nix/gcroots/tsugumi/projects/<id>
//
// The host talks to vsock ports:
//
//   1024  control, one request line per connection, one reply line:
//           ping                            -> ok
//           start <id> <cpus> <name> <slot> -> ok <rpc port>
//           stop <id>                       -> ok
//           forget <id>                     -> ok (stopped project's build
//                                              roots go; data deletion)
//           status                          -> ok <id>:<pid>:<port> ...
//           shell <id> <rows> <cols>        -> ok <session>, then the
//                                              connection is the terminal
//           resize <session> <rows> <cols>  -> ok
//           clock <unix ms>                 -> ok (the Mac's time; run times and
//                                              the app's clock then agree)
//           shutdown                        -> ok (then the VM powers off)
//   1025  RPC: the first line names the project id, then the bytes go to
//         and from that project's tsugumid (127.0.0.1:<port> in the VM).
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
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Options {
  std::string tsugumid, tini, bash;
  std::string path;       // PATH for project processes
  std::string cert_file;  // CA bundle
  std::string host = "/mnt/slots";
  std::string state = "/persist/tsugumi";
  int control_port = 1024, rpc_port = 1025;
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

bool valid_id(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  for (char c : id)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
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
  bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
  ::close(fd);
  return ok;
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
  (void)!::write(fd, line.data(), line.size());
}

// ---- projects ----

struct Project {
  std::string id;
  int slot = -1;
  uid_t uid = 0;
  int port = 0;
  pid_t pid = 0;  // tini, PID 1 of the project's PID namespace
  bool running = false;
};

std::mutex mu;
std::condition_variable exited;
std::map<std::string, Project> projects;
std::map<pid_t, int> exit_status;  // children reaped by the reaper thread

std::string cgroup_dir(const std::string& id) { return "/sys/fs/cgroup/tsugumi/" + id; }

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
    std::ofstream("/etc/passwd", std::ios::app) << name << ":x:" << uid << ":" << uid << ":Tsugumi project " << id
                                                << ":/tmp/home:/bin/sh\n";
  if (!has("/etc/group")) std::ofstream("/etc/group", std::ios::app) << name << ":x:" << uid << ":\n";
}

struct ChildArgs {
  std::string id, name;
  uid_t uid;
  int slot, port, cpus;
  int sync_fd;  // read end: the parent has put the child into its cgroup
  int log_fd;
};

void die(const char* what) {
  std::fprintf(stderr, "vmd child: %s: %s\n", what, std::strerror(errno));
  _exit(127);
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

// Mount namespace of one project: its folder and control folder at /project
// and /control, nothing of the share, the store disk or other projects.
void enter_project_view(int slot) {
  if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) die("make / private");
  std::string dir = opt.host + "/" + std::to_string(slot);
  std::string p = dir + "/project", c = dir + "/control";
  if (::mount(p.c_str(), "/project", nullptr, MS_BIND, nullptr) != 0) die("bind /project");
  if (::mount(c.c_str(), "/control", nullptr, MS_BIND, nullptr) != 0) die("bind /control");
  // /mnt (a tmpfs holding every slot mount) goes, with all slots under it.
  if (::umount2(fs::path(opt.host).parent_path().c_str(), MNT_DETACH) != 0) die("hide the slots");
  ::umount2("/persist", MNT_DETACH);  // the store disk (the /nix overlay stays)
  if (::mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777") != 0) die("private /tmp");
  ::mkdir("/tmp/home", 0700);
  if (::mount("tmpfs", "/run/tsugumi", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755") != 0) die("private /run/tsugumi");
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

int project_main(void* arg) {
  auto* a = static_cast<ChildArgs*>(arg);
  char b;
  if (::read(a->sync_fd, &b, 1) != 1) die("wait for the cgroup");
  ::close(a->sync_fd);
  ::dup2(a->log_fd, 1);
  ::dup2(a->log_fd, 2);
  enter_project_view(a->slot);
  // A fresh /proc for this PID namespace.
  if (::mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) != 0) die("mount /proc");
  ::sethostname(a->name.c_str(), a->name.size());
  std::string roots = "/nix/var/nix/gcroots/tsugumi/projects/" + a->id;
  ::chown("/tmp/home", a->uid, a->uid);
  drop_to(a->uid);
  std::vector<std::string> argv = {opt.tini, "-g", "--", opt.tsugumid, "--project", "/project",
                                   "--listen", "127.0.0.1:" + std::to_string(a->port), "--control-dir", "/control",
                                   "--cpus", std::to_string(a->cpus), "--own-nix-store", "--allow-missing-origin",
                                   "--gcroots-dir", roots};
  std::vector<std::string> env = project_env(a->id);
  auto av = cstrings(argv), ev = cstrings(env);
  ::chdir("/project");
  ::execve(av[0], av.data(), ev.data());
  die("exec tsugumid");
  return 127;
}

std::string sanitize_name(const std::string& s) {
  std::string out;
  for (char c : s)
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') out += c;
  if (out.empty()) out = "project";
  return out.substr(0, 63);
}

std::string slot_dir(int slot) { return opt.host + "/" + std::to_string(slot); }

// Mounts the slot's virtiofs device afresh (the app has just set its share).
std::string mount_slot(int slot) {
  std::string dir = slot_dir(slot);
  ::umount2(dir.c_str(), MNT_DETACH);
  fs::create_directories(dir);
  std::string tag = "tsugumi-" + std::to_string(slot);
  if (::mount(tag.c_str(), dir.c_str(), "virtiofs", MS_NOSUID | MS_NODEV, nullptr) != 0)
    return std::string("error cannot mount ") + tag + ": " + std::strerror(errno);
  return "";
}

std::string start_project(const std::string& id, int cpus, const std::string& name, int slot) {
  std::unique_lock<std::mutex> l(mu);
  if (auto it = projects.find(id); it != projects.end() && it->second.running)
    return "ok " + std::to_string(it->second.port);
  if (slot < 0 || slot > 63) return "error invalid slot";
  for (const auto& [other, op] : projects)
    if (op.running && op.slot == slot) return "error slot " + std::to_string(slot) + " is in use";
  if (auto err = mount_slot(slot); !err.empty()) return err;
  std::string p = slot_dir(slot) + "/project", c = slot_dir(slot) + "/control";
  if (!fs::is_regular_file(p + "/tsugumi.json")) return "error the project folder is not shared";
  if (!fs::is_directory(c)) return "error the control folder is not shared";
  Project pr;
  pr.id = id;
  pr.slot = slot;
  pr.uid = uid_for(id);
  pr.port = 7700 + static_cast<int>(pr.uid - 20000);
  ensure_user(id, pr.uid);
  // Build roots of this project, kept while the project exists.
  std::string roots = "/nix/var/nix/gcroots/tsugumi/projects/" + id;
  fs::create_directories(roots);
  ::chown(roots.c_str(), pr.uid, pr.uid);
  ::chmod(roots.c_str(), 0755);
  std::string cg = cgroup_dir(id);
  fs::create_directories(cg);
  std::string log_path = c + "/tsugumid.log";
  int log_fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (log_fd < 0) return std::string("error cannot open tsugumid.log: ") + std::strerror(errno);
  int sync[2];
  if (::pipe2(sync, O_CLOEXEC) != 0) {
    ::close(log_fd);
    return "error pipe";
  }
  ChildArgs args{id, sanitize_name(name), pr.uid, slot, pr.port, cpus, sync[0], log_fd};
  // The child's fds must survive exec only where dup2'ed, so CLOEXEC is fine.
  static thread_local std::vector<char> stack(1 << 20);
  pid_t pid = ::clone(project_main, stack.data() + stack.size(),
                      CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS | SIGCHLD, &args);
  ::close(sync[0]);
  ::close(log_fd);
  if (pid < 0) {
    ::close(sync[1]);
    return std::string("error clone: ") + std::strerror(errno);
  }
  write_file(cg + "/cgroup.procs", std::to_string(pid));
  (void)!::write(sync[1], "x", 1);
  ::close(sync[1]);
  pr.pid = pid;
  pr.running = true;
  projects[id] = pr;
  logf("started %s (uid %u, pid %d, port %d)", id.c_str(), pr.uid, pid, pr.port);
  return "ok " + std::to_string(pr.port);
}

bool cgroup_empty(const std::string& id) {
  std::ifstream in(cgroup_dir(id) + "/cgroup.procs");
  std::string line;
  return !std::getline(in, line) || line.empty();
}

std::string stop_project(const std::string& id) {
  std::unique_lock<std::mutex> l(mu);
  auto it = projects.find(id);
  if (it == projects.end() || !it->second.running) return "ok";
  pid_t pid = it->second.pid;
  ::kill(pid, SIGTERM);
  // tsugumid records interrupted runs on SIGTERM; give it time.
  exited.wait_for(l, std::chrono::seconds(20), [&] { return !it->second.running; });
  // Whatever is left of exactly this project ends now.
  write_file(cgroup_dir(id) + "/cgroup.kill", "1");
  for (int i = 0; i < 100 && !cgroup_empty(id); ++i) {
    l.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    l.lock();
  }
  bool empty = cgroup_empty(id);
  it->second.running = false;
  // The slot goes back to the app (which then empties its share).
  ::umount2(slot_dir(it->second.slot).c_str(), MNT_DETACH);
  logf("stopped %s%s", id.c_str(), empty ? "" : " (processes remain)");
  return empty ? "ok" : "error processes of the project remain";
}

std::string forget_project(const std::string& id) {
  std::string r = stop_project(id);
  if (r != "ok") return r;
  std::error_code ec;
  fs::remove_all("/nix/var/nix/gcroots/tsugumi/projects/" + id, ec);
  ::rmdir(cgroup_dir(id).c_str());
  std::lock_guard<std::mutex> l(mu);
  projects.erase(id);
  return ec ? "error " + ec.message() : "ok";
}

std::string status() {
  std::lock_guard<std::mutex> l(mu);
  std::string out = "ok";
  for (const auto& [id, p] : projects)
    if (p.running) out += " " + id + ":" + std::to_string(p.pid) + ":" + std::to_string(p.port);
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
        logf("project %s exited (%s %d)", id.c_str(), WIFEXITED(st) ? "status" : "signal",
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
  int master, slave;
  winsize ws{static_cast<unsigned short>(rows), static_cast<unsigned short>(cols), 0, 0};
  if (::openpty(&master, &slave, nullptr, nullptr, &ws) != 0) {
    reply(conn, "error openpty");
    return;
  }
  ::fcntl(master, F_SETFD, FD_CLOEXEC);
  pid_t pid = ::fork();
  if (pid == 0) {
    // Join the project's namespaces (the PID namespace applies to the next
    // child). All are opened first: /proc changes with the mount namespace.
    std::vector<int> nsfd;
    for (const char* ns : {"ipc", "uts", "pid", "mnt"}) {
      std::string path = "/proc/" + std::to_string(target) + "/ns/" + ns;
      int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0) die(ns);
      nsfd.push_back(fd);
    }
    for (int fd : nsfd)
      if (::setns(fd, 0) != 0) die("setns");
    // Into the project's cgroup, so stopping the project ends its terminals too.
    write_file(cgroup_dir(id) + "/cgroup.procs", std::to_string(::getpid()));
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
    ::chdir("/project");
    std::vector<std::string> env = project_env(id);
    env.push_back("TERM=xterm-256color");
    env.push_back("PS1=\\[\\e[1;32m\\]\\h\\[\\e[0m\\]:\\[\\e[1;34m\\]\\w\\[\\e[0m\\]\\$ ");
    std::vector<std::string> argv = {opt.bash, "--noprofile", "--norc", "-i"};
    auto av = cstrings(argv), ev = cstrings(env);
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
  logf("terminal %llu for %s", static_cast<unsigned long long>(sid), id.c_str());
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
  winsize ws{static_cast<unsigned short>(rows), static_cast<unsigned short>(cols), 0, 0};
  return ::ioctl(it->second.master, TIOCSWINSZ, &ws) == 0 ? "ok" : "error resize";
}

// ---- connections ----

int listen_vsock(int port) {
  int fd = ::socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  sockaddr_vm a{};
  a.svm_family = AF_VSOCK;
  a.svm_cid = VMADDR_CID_ANY;
  a.svm_port = static_cast<unsigned>(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 64) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

void shutdown_vm() {
  std::vector<std::string> ids;
  {
    std::lock_guard<std::mutex> l(mu);
    for (const auto& [id, p] : projects)
      if (p.running) ids.push_back(id);
  }
  for (const auto& id : ids) stop_project(id);
  logf("powering off");
  ::sync();
  ::reboot(RB_POWER_OFF);
}

void control(int conn) {
  auto w = split(read_line(conn));
  std::string r;
  if (w.empty()) r = "error empty request";
  else if (w[0] == "ping") r = "ok";
  else if (w[0] == "status") r = status();
  else if (w[0] == "start" && w.size() == 5 && valid_id(w[1]))
    r = start_project(w[1], std::max(1, std::atoi(w[2].c_str())), w[3], std::atoi(w[4].c_str()));
  else if (w[0] == "stop" && w.size() == 2 && valid_id(w[1])) r = stop_project(w[1]);
  else if (w[0] == "forget" && w.size() == 2 && valid_id(w[1])) r = forget_project(w[1]);
  else if (w[0] == "shell" && w.size() == 4 && valid_id(w[1])) {
    shell(conn, w[1], std::atoi(w[2].c_str()), std::atoi(w[3].c_str()));
    ::close(conn);
    return;
  } else if (w[0] == "resize" && w.size() == 4) r = resize(std::strtoull(w[1].c_str(), nullptr, 10), std::atoi(w[2].c_str()), std::atoi(w[3].c_str()));
  else if (w[0] == "clock" && w.size() == 2) {
    long long ms = std::atoll(w[1].c_str());
    timespec ts{static_cast<time_t>(ms / 1000), static_cast<long>((ms % 1000) * 1000000)};
    r = ms > 0 && ::clock_settime(CLOCK_REALTIME, &ts) == 0 ? "ok" : std::string("error clock: ") + std::strerror(errno);
  } else if (w[0] == "shutdown") {
    reply(conn, "ok");
    ::close(conn);
    shutdown_vm();
    return;
  } else r = "error unknown request";
  reply(conn, r);
  ::close(conn);
}

void rpc(int conn) {
  std::string id = read_line(conn);
  int port = 0;
  {
    std::lock_guard<std::mutex> l(mu);
    if (auto it = projects.find(id); it != projects.end() && it->second.running) port = it->second.port;
  }
  if (!port) {
    ::close(conn);
    return;
  }
  int up = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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

void serve(int port, void (*handler)(int)) {
  int fd = listen_vsock(port);
  if (fd < 0) {
    logf("cannot listen on vsock port %d: %s", port, std::strerror(errno));
    std::exit(1);
  }
  for (;;) {
    int c = ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (c < 0) continue;
    std::thread(handler, c).detach();
  }
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i], v = argv[i + 1];
    if (k == "--tsugumid") opt.tsugumid = v;
    else if (k == "--tini") opt.tini = v;
    else if (k == "--bash") opt.bash = v;
    else if (k == "--path") opt.path = v;
    else if (k == "--cert-file") opt.cert_file = v;
    else if (k == "--host") opt.host = v;
    else if (k == "--state") opt.state = v;
    else {
      std::fprintf(stderr, "tsugumi-vmd: unknown option %s\n", k.c_str());
      return 2;
    }
  }
  if (opt.tsugumid.empty() || opt.tini.empty() || opt.bash.empty()) {
    std::fprintf(stderr, "usage: tsugumi-vmd --tsugumid PATH --tini PATH --bash PATH [--path PATH] [--cert-file FILE]\n");
    return 2;
  }
  ::signal(SIGPIPE, SIG_IGN);
  fs::create_directories("/sys/fs/cgroup/tsugumi");
  fs::create_directories("/run/tsugumi");
  fs::create_directories(opt.host);
  std::thread(reaper).detach();
  std::thread(serve, opt.rpc_port, rpc).detach();
  logf("ready (control vsock %d, rpc vsock %d)", opt.control_port, opt.rpc_port);
  // The host waits for this line on the console before connecting.
  std::fprintf(stderr, "TSUGUMI-STAGE ready\n");
  serve(opt.control_port, control);
}
