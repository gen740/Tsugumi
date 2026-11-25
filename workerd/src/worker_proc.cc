#include "tsugumi/workerd/worker_proc.hh"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "tsugumi/card_abi.h"
#include "tsugumi/workerd/artifact_id.hh"
#ifdef __APPLE__
#include <libproc.h>
#else
#include <sys/vfs.h>
#endif

#include "display_channel.hh"
#include "tsugumi/protocol/trace_codec.hh"
#include "tsugumi/protocol/worker_generated.h"
#include "tsugumi/rpc/frame.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/log.hh"
#include "tsugumi/util/process.hh"
#include "tsugumi/workerd/frame.hh"
#include "tsugumi/workerd/protocol.hh"
#include "tsugumi/workerd/staging.hh"
#include "tsugumi/workerd/value_wire.hh"

#ifndef TSUGUMI_BWRAP
#define TSUGUMI_BWRAP ""
#endif
#ifndef TSUGUMI_CACERT
#define TSUGUMI_CACERT ""
#endif

namespace tsugumi {

namespace W = Tsugumi::Worker;
namespace C = Tsugumi::Card;

namespace {

void close_fd(int& fd) {
  if (fd >= 0) ::close(fd);
  fd = -1;
}

auto send_frame(trace::Link& link, int fd, const std::string& frame,
                const std::atomic<bool>* cancel,
                std::optional<std::chrono::steady_clock::time_point> deadline,
                bool observe = true) -> void {
  rpc::frame::CheckSize(frame.size(), workerd::kMaxWorkerFrameBytes);
  const auto prefix =
      rpc::frame::EncodePrefix(static_cast<uint32_t>(frame.size()));
  std::string buf(prefix.data(), prefix.size());
  buf += frame;
  struct Wipe {
    std::string& data;
    ~Wipe() { workerd::clear_secret(data); }
  } wipe{buf};
  size_t offset = 0;
  while (offset < buf.size()) {
    if (cancel && cancel->load())
      throw Failed(FailureKind::cancelled, "the invocation was cancelled");
    if (deadline && std::chrono::steady_clock::now() >= *deadline)
      throw Failed(FailureKind::timeout, "the Card exceeded timeoutSeconds");
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL | MSG_DONTWAIT;
#else
    constexpr int flags = MSG_DONTWAIT;  // SO_NOSIGPIPE on macOS
#endif
    const auto count =
        ::send(fd, buf.data() + offset, buf.size() - offset, flags);
    if (count > 0) {
      offset += static_cast<size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd ready{fd, POLLOUT, 0};
      if (::poll(&ready, 1, 20) >= 0 || errno == EINTR) continue;
    }
    throw Failed(FailureKind::worker_failed, "worker connection broken");
  }
  if (observe)
    trace::Observe(link, "project core", "Card worker", true, frame,
                   &trace::kFlatBufferCodec);
}

#ifdef __APPLE__
// Seatbelt checks host paths after symlink resolution (/tmp -> /private/tmp).
auto seatbelt_profile(const WorkerSpawn& sp, const std::string& scratch,
                      const std::filesystem::path& artifacts) -> std::string {
  const bool restrict_files = sp.sandbox.filesystem || sp.sandbox.identity;
  const bool restrict_processes = sp.sandbox.pid || sp.sandbox.identity;
  const bool restrict_services =
      restrict_files || restrict_processes || sp.sandbox.ipc;
  if (!restrict_services && sp.network) return {};

  std::string profile = "(version 1)\n";
  if (restrict_files) {
    profile +=
        "(deny default)\n"
        "(allow process-fork sysctl-read file-read-metadata)\n"
        "(allow signal (target same-sandbox))\n"
        "(allow ipc-posix-shm)\n";
  } else {
    profile += "(allow default)\n";
  }
  if (!sp.network) {
    profile += "(deny network*)\n";
  } else if (restrict_services) {
    profile += "(allow network*)\n";
    if (restrict_files)
      profile += "(deny network-outbound (remote ip \"localhost:*\"))\n";
  }
  if (restrict_services) {
    profile += "(deny mach-lookup mach-register)\n";
    if (sp.network)
      profile +=
          "(allow mach-lookup (global-name \"com.apple.trustd.agent\"))\n";
  }
  if (restrict_files || sp.sandbox.ipc) {
    profile += "(deny network-outbound (remote unix-socket))\n";
    // The worker's data and control sockets are inherited socketpairs.
    if (sp.sandbox.ipc)
      profile += "(deny network-bind (local unix-socket))\n";
    // System DNS uses this socket even when IPC isolation is enabled.
    if (sp.network)
      profile +=
          "(allow network-outbound (remote unix-socket "
          "(path-literal \"/private/var/run/mDNSResponder\")))\n";
  }
  if (restrict_processes) {
    profile +=
        "(deny process-info* signal mach-task* system-socket)\n"
        "(allow process-info* signal (target self) (target children))\n"
        "(deny process-info-listpids)\n"
        "(deny sysctl-read (sysctl-name-prefix \"kern.proc\" \"net.local\" "
        "\"net.inet.tcp.pcblist\" \"net.inet.udp.pcblist\"))\n"
        "(allow syscall-unix)\n"
        "(deny syscall-unix (syscall-number SYS_getppid SYS_getpgid "
        "SYS_getsid))\n";
  }
  if (sp.sandbox.ipc)
    profile +=
        "(deny ipc-posix-shm* ipc-posix-sem* "
        "ipc-sysv-shm ipc-sysv-sem ipc-sysv-msg)\n";
  if (!restrict_files) return profile;

  auto real = [](const std::string& p) {
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(p, ec);
    return ec ? p : c.string();
  };
  auto quote = [&](const std::string& p) {
    std::string out = "\"";
    for (char ch : real(p)) {
      if (ch == '"' || ch == '\\') out += '\\';
      out += ch;
    }
    return out + "\"";
  };
  // No /bin or /usr/bin: the Card gets no implicit shell or system tools.
  // What it may run are store paths (the tools it declared), the worker, and
  // files in its own folders and grants.
  std::string read =
      "(literal \"/\") (subpath \"/nix/store\") (subpath \"/usr/lib\") "
      "(subpath \"/usr/share\") (subpath \"/System/Library\")"
      " (subpath \"/private/var/db/timezone\")";
  std::string devices;
  for (const auto* p :
       {"/dev/null", "/dev/zero", "/dev/random", "/dev/urandom"})
    devices += " (literal " + quote(p) + ")";
  read += devices;
  for (const auto* p :
       {"/etc/hosts", "/etc/resolv.conf", "/etc/protocols", "/etc/services",
        "/etc/group", "/etc/passwd", "/etc/localtime"})
    read += " (literal " + quote(p) + ")";
  std::string exec = "(subpath \"/nix/store\") (literal " +
                     quote(sp.worker_binary) + ") (subpath " + quote(scratch) +
                     ")";
  if (std::string_view(TSUGUMI_CACERT).size())
    read += " (literal " + quote(TSUGUMI_CACERT) + ")";
  read += " (literal " + quote(sp.worker_binary) + ") (subpath " +
          quote(scratch) + ")";
  std::string write =
      "(literal \"/dev/null\") (subpath " + quote(scratch) + ")";
  for (const auto& p : sp.ro_paths) {
    read += " (subpath " + quote(p) + ")";
    exec += " (subpath " + quote(p) + ")";
  }
  if (!artifacts.empty()) {
    const auto rule = " (subpath " + quote(artifacts.string()) + ")";
    read += rule;
    write += " (subpath " + quote((artifacts / "outputs").string()) + ")";
  }
  std::string read_only_grants;
  if (!artifacts.empty())
    read_only_grants = " (require-all (subpath " + quote(artifacts.string()) +
                       ") (require-not (subpath " +
                       quote((artifacts / "outputs").string()) + ")))";
  std::string private_paths;
  if (!sp.artifact_store.empty()) {
    private_paths =
        "(deny file-read* file-write* (subpath " + quote(sp.artifact_store) +
        ")"
        " (require-all (subpath " +
        quote(sp.artifact_staging.string()) + ") (require-not (subpath " +
        quote(artifacts.string()) + "))))\n";
  }
  for (const auto& bind : sp.binds) {
    const auto rule = " (subpath " + quote(bind.source) + ")";
    read += rule;
    exec += rule;
    if (bind.write)
      write += rule;
    else
      read_only_grants += rule;
  }
  return profile + "(allow process-exec " + exec + ")\n(allow file-read* " +
         read + ")\n(allow file-write* " + write + ")\n(allow file-ioctl " +
         devices + ")\n" +
         (read_only_grants.empty()
              ? ""
              : "(deny file-write* " + read_only_grants + ")\n") +
         private_paths;
}
#else
auto delegated_root() -> std::filesystem::path {
  const auto* root = ::getenv("TSUGUMI_CARD_CGROUP_ROOT");
  return root ? std::filesystem::path(root) : std::filesystem::path{};
}

auto cgroup_write(const std::filesystem::path& path, const std::string& value)
    -> void {
  std::ofstream file(path);
  file << value;
  file.close();
  if (!file)
    throw Failed(FailureKind::unsupported,
                 "cannot configure delegated Card cgroup " + path.string());
}

struct GuiSocket {
  std::string variable;
  std::string value;
  std::string source;
};
auto gui_socket() -> std::optional<GuiSocket> {
  namespace fs = std::filesystem;
  if (const auto* display = ::getenv("WAYLAND_DISPLAY"); display && *display) {
    fs::path socket = display;
    if (!socket.is_absolute()) {
      const auto* root = ::getenv("XDG_RUNTIME_DIR");
      if (root && fs::path(root).is_absolute() && socket == socket.filename())
        socket = fs::path(root) / socket;
    }
    std::error_code error;
    if (socket.is_absolute() && fs::is_socket(socket, error))
      return GuiSocket{"WAYLAND_DISPLAY", socket.string(), socket.string()};
  }
  if (const auto* display = ::getenv("DISPLAY"); display && display[0] == ':') {
    std::string number = display + 1;
    number.erase(number.find('.') == std::string::npos ? number.size()
                                                       : number.find('.'));
    std::error_code error;
    if (!number.empty() &&
        number.find_first_not_of("0123456789") == std::string::npos &&
        fs::is_socket("/tmp/.X11-unix/X" + number, error))
      return GuiSocket{"DISPLAY", display, "/tmp/.X11-unix"};
  }
  return std::nullopt;
}
#endif

}  // namespace

WorkerSecrets::~WorkerSecrets() {
  for (auto& [name, value] : values) workerd::clear_secret(value);
}

auto worker_has_bwrap() -> bool {
#ifdef __linux__
  return *TSUGUMI_BWRAP && ::access(TSUGUMI_BWRAP, X_OK) == 0;
#else
  return false;
#endif
}

#ifdef __APPLE__
auto worker_has_seatbelt() -> bool {
  return ::access("/usr/bin/sandbox-exec", X_OK) == 0;
}
#endif

auto worker_limits_available() -> bool {
#ifdef __linux__
  const auto root = delegated_root();
  struct statfs kind{};
  if (!worker_has_bwrap() || !root.is_absolute() ||
      ::statfs(root.c_str(), &kind) != 0 || kind.f_type != 0x63677270 ||
      ::access(root.c_str(), W_OK | X_OK) != 0 ||
      ::access((root / "cgroup.procs").c_str(), W_OK) != 0)
    return false;
  const auto enabled = fs::read_file(root / "cgroup.subtree_control");
  return enabled && enabled->find("cpu") != std::string::npos &&
         enabled->find("memory") != std::string::npos;
#else
  return false;
#endif
}

auto worker_gui_available() -> bool {
#ifdef __linux__
  return worker_has_bwrap() && gui_socket().has_value();
#else
  return false;
#endif
}

auto remove_card_cgroup(const std::filesystem::path& path) -> void {
#ifdef __linux__
  if (path.empty()) return;
  std::error_code error;
  if (!std::filesystem::exists(path, error)) return;
  try {
    cgroup_write(path / "cgroup.kill", "1");
  } catch (const Failed&) {
  }
  for (int i = 0; i < 50; ++i) {
    if (::rmdir(path.c_str()) == 0 || errno == ENOENT) return;
    if (errno != EBUSY) break;
    ::usleep(10000);
  }
  log::warn("could not remove Card cgroup " + path.string());
#else
  (void)path;
#endif
}

bool worker_uid_on_host() { return !worker_has_bwrap(); }

WorkerProc::WorkerProc(const WorkerSpawn& sp) : scratch_(sp.scratch) {
  if (!sp.artifact_staging.empty())
    artifact_scratch_ = sp.artifact_staging / crypto::uuid4();
  // Construction failures clean up private artifact staging before the
  // partially constructed worker can be handed to the pool.
  struct StagingGuard {
    std::filesystem::path path;
    ~StagingGuard() {
      if (path.empty()) return;
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  } artifact_guard{artifact_scratch_};
#ifdef __APPLE__
  // O_EVTONLY still needs read permission on Darwin. Repair an unreadable
  // directory with exactly the Card's identity and sandbox, then open it with
  // O_NOFOLLOW. Even a surviving child swapping in a hard link cannot enlist
  // the core's broader authority to chmod a protected host inode.
  const auto profile = seatbelt_profile(sp, scratch_, artifact_scratch_);
  recover_permissions_ = [profile, uid = sp.uid, gid = sp.gid](
                             int parent, const char* name) {
    std::filesystem::path target(name);
    if (parent != AT_FDCWD) {
      char path[PATH_MAX];
      if (::fcntl(parent, F_GETPATH, path) != 0)
        throw std::system_error(errno, std::generic_category(),
                                "cannot locate staging directory");
      target = std::filesystem::path(path) / name;
    }
    proc::Spec repair;
    repair.cwd = "/";
    // The Card's own profile, plus exactly the one system tool the repair
    // runs; the Card itself never gets /bin.
    if (!profile.empty())
      repair.argv = {"/usr/bin/sandbox-exec", "-p",
                     profile +
                         "(allow file-read* process-exec (literal "
                         "\"/bin/chmod\"))\n"};
    repair.argv.insert(repair.argv.end(),
                       {"/bin/chmod", "-h", "u+rwx", target.string()});
    repair.uid = uid;
    repair.gid = gid;
    const auto result = proc::run(repair, nullptr, std::nullopt, {});
    if (!proc::exited_ok(result.status))
      throw std::system_error(
          EACCES, std::generic_category(),
          "cannot restore staging directory permissions: " + target.string());
  };
#endif
  for (const char* d : {"work", "tmp"})
    std::filesystem::create_directories(std::filesystem::path(scratch_) / d);
  if (::geteuid() == 0 && sp.uid && worker_uid_on_host())
    for (const char* d : {"work", "tmp"}) {
      const auto path = scratch_ + "/" + d;
      if (::chown(path.c_str(), *sp.uid, sp.gid.value_or(*sp.uid)) != 0) {
        const int error = errno;
        throw Failed(FailureKind::worker_failed,
                     "chown " + path + ": " + std::strerror(error));
      }
    }
  if (!artifact_scratch_.empty()) {
    std::filesystem::create_directories(artifact_scratch_ / "outputs");
    std::filesystem::create_directory(artifact_scratch_ / "inputs");
  }
  proc::Spec spec;
  std::string certificate = TSUGUMI_CACERT;
  std::string temporary = tmp_dir();
  std::vector<std::string> gui_env;
#ifdef __APPLE__
  if (sp.gui || sp.sandbox.limits.limited())
    throw Failed(FailureKind::unsupported,
                 "native macOS does not support GUI grants or sandbox limits");
  if (!profile.empty()) {
    if (!worker_has_seatbelt())
      throw Failed(FailureKind::permission_denied, "Seatbelt is unavailable");
    spec.argv = {"/usr/bin/sandbox-exec", "-p", profile};
  }
  spec.argv.push_back(sp.worker_binary);
  spec.uid = sp.uid;
  spec.gid = sp.gid;
#else
  const bool bwrap = worker_has_bwrap();
  const bool filesystem = sp.sandbox.filesystem;
  // Never the plain worker in place of isolation or a grant it cannot enforce.
  if (!bwrap && (filesystem || !sp.hidden_paths.empty() || sp.gui ||
                 sp.sandbox.limits.limited() || !sp.network ||
                 sp.sandbox.pid || sp.sandbox.ipc || sp.sandbox.identity))
    throw Failed(FailureKind::unsupported,
                 "the requested Card isolation requires bubblewrap");
  if (bwrap) {
    auto& argv = spec.argv;
    argv = {TSUGUMI_BWRAP, "--unshare-user", "--die-with-parent"};
    if (sp.sandbox.pid)
      argv.insert(argv.end(), {"--unshare-pid", "--new-session"});
    if (sp.sandbox.ipc) argv.push_back("--unshare-ipc");
    if (!sp.network) argv.push_back("--unshare-net");
    if (sp.sandbox.identity) {
      argv.insert(argv.end(),
                  {"--uid", "65534", "--gid", "65534", "--cap-drop", "ALL"});
    }
    if (filesystem) {
      argv.insert(argv.end(), {"--dir", "/nix/store", "--dev", "/dev"});
      for (const auto& item : std::set(sp.closure.begin(), sp.closure.end())) {
        const std::filesystem::path member(item);
        if (member.parent_path() != "/nix/store" || member.filename() == "." ||
            member.filename() == "..")
          throw Failed(FailureKind::worker_failed,
                       "invalid realized closure path");
        const auto source =
            (std::filesystem::path(sp.store_path) / member.filename()).string();
        argv.insert(argv.end(), {"--ro-bind", source, item});
      }
      if (!sp.worker_binary.starts_with("/nix/store/"))
        argv.insert(argv.end(),
                    {"--ro-bind", sp.worker_binary, sp.worker_binary});
    } else {
      argv.insert(argv.end(), {"--dev-bind", "/", "/"});
      for (const auto& path : sp.hidden_paths)
        argv.insert(argv.end(), {"--tmpfs", path});
      if (!delegated_root().empty())
        argv.insert(argv.end(), {"--tmpfs", "/sys/fs/cgroup"});
    }
    if (sp.sandbox.pid)
      argv.insert(argv.end(), {"--proc", "/proc"});
    else if (!sp.hidden_paths.empty() && !filesystem)
      // Keep the project mount namespace inaccessible through /proc/PID/root.
      argv.insert(argv.end(), {"--tmpfs", "/proc"});
    if (sp.sandbox.ipc && !filesystem) {
      if (::access("/dev/shm", F_OK) == 0)
        argv.insert(argv.end(), {"--tmpfs", "/dev/shm"});
      if (::access("/dev/mqueue", F_OK) == 0)
        argv.insert(argv.end(), {"--tmpfs", "/dev/mqueue"});
    } else if (sp.sandbox.ipc) {
      argv.insert(argv.end(), {"--tmpfs", "/dev/shm"});
    }
    const auto gui = sp.gui && !sp.shared_vm ? gui_socket() : std::nullopt;
    // Paths under the original /tmp must remain visible after mounting scratch.
    const auto under_tmp = [](const std::string& path) {
      return path == "/tmp" || path.starts_with("/tmp/");
    };
    const bool preserve_tmp =
        !filesystem &&
        (std::ranges::any_of(
             sp.binds,
             [&](const auto& grant) { return under_tmp(grant.source); }) ||
         (gui && gui->variable == "WAYLAND_DISPLAY" && under_tmp(gui->source)));
    if (!preserve_tmp && !under_tmp(scratch_) && !under_tmp(sp.worker_binary) &&
        !std::ranges::any_of(sp.ro_paths, under_tmp)) {
      argv.insert(argv.end(), {"--bind", tmp_dir(), "/tmp"});
      temporary = "/tmp";
    }
    for (const auto& path : sp.ro_paths)
      argv.insert(argv.end(), {"--ro-bind", path, path});
    if (filesystem || !sp.hidden_paths.empty())
      argv.insert(argv.end(), {"--bind", scratch_, scratch_});
    if (!filesystem && !sp.hidden_paths.empty() && !artifact_scratch_.empty()) {
      const auto root = artifact_scratch_.string();
      argv.insert(argv.end(), {"--bind", root, root});
    }
    if (filesystem) {
      for (const auto& bind : sp.binds) {
        std::vector<std::filesystem::path> hidden;
        if (!sp.artifact_store.empty()) {
          const auto source = std::filesystem::canonical(bind.source);
          for (const auto& private_path :
               {std::filesystem::path(sp.artifact_store),
                sp.artifact_staging}) {
            const auto root = std::filesystem::weakly_canonical(private_path);
            const auto relative = source.lexically_relative(root);
            if (!relative.empty() && *relative.begin() != "..")
              throw Failed(FailureKind::permission_denied,
                           "host grant points into private artifact storage");
            const auto child = root.lexically_relative(source);
            if (!child.empty() && *child.begin() != ".." &&
                std::filesystem::exists(root))
              hidden.push_back(std::filesystem::path(bind.target) / child);
          }
        }
        argv.insert(argv.end(), {bind.write ? "--bind" : "--ro-bind",
                                 bind.source, bind.target});
        for (const auto& path : hidden)
          argv.insert(argv.end(), {"--tmpfs", path.string()});
      }
      if (!artifact_scratch_.empty()) {
        const auto root = artifact_scratch_.string();
        argv.insert(argv.end(), {"--ro-bind", root, root});
        const auto outputs = artifact_scratch_ / "outputs";
        argv.insert(argv.end(), {"--bind", outputs.string(), outputs.string()});
      }
      if (sp.network)
        for (const char* path :
             {"/etc/resolv.conf", "/etc/hosts", "/etc/nsswitch.conf"})
          argv.insert(argv.end(), {"--ro-bind-try", path, path});
      if (!certificate.empty()) {
        argv.insert(argv.end(),
                    {"--ro-bind", certificate, "/tsugumi-ca-bundle.crt"});
        certificate = "/tsugumi-ca-bundle.crt";
      }
    }
    if (sp.gui) {
      const auto& socket = gui;
      if (!socket)
        throw Failed(FailureKind::unsupported,
                     "grants.gui needs a native Linux GUI socket");
      auto value = socket->value;
      if (socket->variable == "WAYLAND_DISPLAY" && filesystem) {
        value = "/run/tsugumi-wayland";
        argv.insert(argv.end(), {"--bind", socket->source, value});
      }
      if (socket->variable == "DISPLAY" &&
          (filesystem || temporary == "/tmp")) {
        argv.insert(argv.end(), {"--bind", socket->source, socket->source});
        x11_mounted_ = temporary == "/tmp";
      }
      if (filesystem)
        argv.insert(argv.end(), {"--dev-bind-try", "/dev/dri", "/dev/dri"});
      gui_env.push_back(socket->variable + "=" + value);
    }
    argv.insert(argv.end(), {"--chdir", work_dir(), "--", sp.worker_binary});
  } else {
    spec.argv = {sp.worker_binary};
    spec.uid = sp.uid;
    spec.gid = sp.gid;
  }
  if (sp.sandbox.limits.limited()) {
    auto group = sp.cgroup;
    if (group.empty()) {
      if (!worker_limits_available())
        throw Failed(FailureKind::unsupported,
                     "sandbox.limits requires a delegated cgroup v2 subtree");
      group = delegated_root() / ("tsugumi-card-" + crypto::uuid4());
    }
    if (!std::filesystem::create_directory(group))
      throw Failed(FailureKind::unsupported,
                   "cannot create a private Card cgroup");
    try {
      const auto& limits = sp.sandbox.limits;
      if (limits.cpu_millis)
        cgroup_write(group / "cpu.max",
                     std::to_string(limits.cpu_millis * 1000ull) + " 1000000");
      if (limits.memory_bytes)
        cgroup_write(group / "memory.max", std::to_string(limits.memory_bytes));
      spec.cgroup_procs = (group / "cgroup.procs").string();
    } catch (...) {
      remove_card_cgroup(group);
      throw;
    }
    cgroup_ = std::move(group);
  }
#endif
  spec.argv.insert(spec.argv.end(), {"--package", sp.package});
  spec.env = {"LANG=C.UTF-8",
              "HOME=" + work_dir(),
              "XDG_CONFIG_HOME=" + work_dir() + "/.config",
              "XDG_CACHE_HOME=" + work_dir() + "/.cache",
              "TMPDIR=" + temporary};
  // No implicit PATH: only the tools the Card declared to its SDK builder,
  // which the builder installs in the package's bin directory.
  {
    constexpr std::string_view store = "/nix/store";
    auto on_host = sp.package;
    if (on_host.starts_with(store))
      on_host = sp.store_path + on_host.substr(store.size());
    std::error_code ec;
    if (std::filesystem::is_directory(on_host + "/bin", ec))
      spec.env.push_back("PATH=" + sp.package + "/bin");
  }
  for (const auto& bind : sp.binds) {
    auto path = bind.source;
#ifdef __linux__
    if (sp.sandbox.filesystem) path = bind.target;
#endif
    if (!bind.name.empty())
      spec.env.push_back(grant_environment(bind.name) + "=" + path);
  }
  spec.env.insert(spec.env.end(), gui_env.begin(), gui_env.end());
  if (trace::LaunchEnabled())
    spec.env.push_back(std::string(trace::kLaunchVariable) + "=1");
  if (!certificate.empty()) {
    spec.env.push_back("SSL_CERT_FILE=" + certificate);
    spec.env.push_back("NIX_SSL_CERT_FILE=" + certificate);
  }
  // Not where tsugumid runs, which the sandbox does not let it read (getcwd
  // fails): libraries ask for the current directory while they are imported.
  spec.cwd = work_dir();
  int data[2]{-1, -1}, control[2]{-1, -1}, out[2]{-1, -1}, err[2]{-1, -1};
  const auto close_channels = [&] {
    for (const int fd : {data[0], data[1], control[0], control[1], out[0],
                         out[1], err[0], err[1]})
      if (fd >= 0) ::close(fd);
  };
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, data) != 0 ||
      ::socketpair(AF_UNIX, SOCK_STREAM, 0, control) != 0 || ::pipe(out) != 0 ||
      ::pipe(err) != 0) {
    close_channels();
    remove_card_cgroup(cgroup_);
    throw Failed(FailureKind::worker_failed, "cannot create worker channels");
  }
  spec.fds = {{3, data[1]}, {4, control[1]}};
  spec.stdout_fd = out[1];
  spec.stderr_fd = err[1];
  try {
    pid_ = proc::spawn(spec);
  } catch (const std::exception& e) {
    close_channels();
    remove_card_cgroup(cgroup_);
    throw Failed(sp.sandbox.limits.limited() ? FailureKind::unsupported
                                             : FailureKind::worker_failed,
                 e.what());
  }
  ::close(data[1]);
  ::close(control[1]);
  ::close(out[1]);
  ::close(err[1]);
  data_fd_ = data[0];
  control_fd_ = control[0];
  out_fd_ = out[0];
  err_fd_ = err[0];
  ::fcntl(data_fd_, F_SETFL, O_NONBLOCK);
  ::fcntl(control_fd_, F_SETFL, O_NONBLOCK);
  ::fcntl(out_fd_, F_SETFL, O_NONBLOCK);
  ::fcntl(err_fd_, F_SETFL, O_NONBLOCK);
  for (int fd : {data_fd_, control_fd_, out_fd_, err_fd_})
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
  int one = 1;
  ::setsockopt(data_fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
  ::setsockopt(control_fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  alive_ = true;
  try {
    if (sp.secrets) {
      flatbuffers::FlatBufferBuilder builder;
      std::vector<flatbuffers::Offset<W::EnvVar>> environment;
      for (const auto& [name, value] : sp.secrets->values)
        environment.push_back(W::CreateEnvVar(
            builder, builder.CreateString(name), builder.CreateString(value)));
      const auto start =
          W::CreateWorkerStart(builder, builder.CreateVector(environment));
      W::FinishWorkerMessageBuffer(
          builder,
          W::CreateWorkerMessage(builder, flatbuffers::nullopt,
                                 W::WorkerPayload::WorkerStart, start.Union()));
      struct Wipe {
        flatbuffers::FlatBufferBuilder& builder;
        std::string frame;
        ~Wipe() {
          workerd::wipe_secret(
              {reinterpret_cast<char*>(builder.GetBufferPointer()),
               builder.GetSize()});
          workerd::clear_secret(frame);
        }
      } wipe{builder,
             {reinterpret_cast<const char*>(builder.GetBufferPointer()),
              builder.GetSize()}};
      send_frame(trace_link_, data_fd_, wipe.frame, nullptr,
                 std::chrono::steady_clock::now() + std::chrono::seconds(10));
    }
    if (!sp.channels_borrowed) start_data_reader();
    resume_logs();
    artifact_guard.path.clear();
  } catch (...) {
    terminate(true);
    throw;
  }
}

WorkerProc::WorkerProc(Borrowed borrowed)
    : data_fd_(borrowed.descriptors[0]),
      control_fd_(borrowed.descriptors[1]),
      out_fd_(borrowed.descriptors[2]),
      err_fd_(borrowed.descriptors[3]),
      alive_(true),
      scratch_(std::move(borrowed.scratch)),
      artifact_scratch_(std::move(borrowed.artifact_scratch)),
      call_id_(borrowed.call_id),
      borrowed_(true),
      lease_id_(borrowed.lease),
      borrowed_clear_(std::move(borrowed.clear)),
      borrowed_terminate_(std::move(borrowed.terminate)),
      borrowed_abandon_(std::move(borrowed.abandon)) {
  try {
    start_data_reader();
    resume_logs();
  } catch (...) {
    stop_data_reader();
    stop_log_reader();
    throw;
  }
}

auto WorkerProc::resume(Borrowed borrowed) -> void {
  resume_logs();
  lease_id_ = borrowed.lease;
  call_id_ = borrowed.call_id;
  borrowed_clear_ = std::move(borrowed.clear);
  borrowed_terminate_ = std::move(borrowed.terminate);
  borrowed_abandon_ = std::move(borrowed.abandon);
}

auto WorkerProc::idle() -> void {
  stop_log_reader();
  returning_lease();
}

auto WorkerProc::resume_logs() -> void {
  if (log_thread_.joinable()) return;
  if (::pipe(log_wake_.data()) != 0)
    throw std::runtime_error("cannot create log wake pipe");
  for (int fd : log_wake_) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  stop_logs_ = false;
  log_thread_ = std::thread([this] {
    while (!stop_logs_) {
      pollfd fds[3] = {{out_fd_, POLLIN, 0},
                       {err_fd_, POLLIN, 0},
                       {log_wake_[0], POLLIN, 0}};
      ::poll(fds, 3, -1);
      read_logs(false);
    }
  });
}

auto WorkerProc::stop_log_reader() -> void {
  stop_logs_ = true;
  if (log_thread_.joinable()) {
    const char byte = 0;
    const auto written = ::write(log_wake_[1], &byte, 1);
    if (written < 0 && errno != EPIPE)
      log::warn("cannot wake worker log reader");
    log_thread_.join();
  }
  for (auto& fd : log_wake_) close_fd(fd);
}

auto WorkerProc::hand_over() -> std::array<int, 4> {
  stop_log_reader();
  return {data_fd_, control_fd_, out_fd_, err_fd_};
}

WorkerProc::~WorkerProc() {
  if (borrowed_) {
    if (view_receiver_) view_receiver_->close();
    stop_data_reader();
    stop_log_reader();
    for (auto* fd : {&data_fd_, &control_fd_, &out_fd_, &err_fd_})
      close_fd(*fd);
    // Exceptions during cleanup must not leave an active lease in workerd.
    if (borrowed_abandon_) {
      try {
        borrowed_abandon_();
      } catch (const std::exception& error) {
        log::warn(error.what());
      }
    }
    return;
  }
  terminate();
  std::error_code ec;
  for (const auto& directory :
       {std::filesystem::path(scratch_), artifact_scratch_}) {
    if (directory.empty()) continue;
    try {
      // Also covers failed/dead and remote workers, and lifetime home data.
      fs::clear_staging(directory, recover_permissions_);
      std::filesystem::remove(directory, ec);
      if (ec)
        log::warn("cannot remove worker scratch " + directory.string() +
                  " (errno " + std::to_string(ec.value()) +
                  "): " + ec.message());
    } catch (const std::system_error& error) {
      log::warn("cannot clear worker scratch: " + std::string(error.what()));
    }
  }
}

bool WorkerProc::alive() const {
  if (borrowed_) {
    if (!alive_ || data_fd_ < 0) return false;
    pollfd state{data_fd_, 0, 0};
    return ::poll(&state, 1, 0) >= 0 &&
           !(state.revents & (POLLHUP | POLLERR | POLLNVAL));
  }
  if (!alive_ || pid_ <= 0) return false;
  // Darwin can return CLD_STOPPED from this waitid query despite WEXITED.
  // A stopped resident still owns its connection and can be cancelled.
  siginfo_t si{};
  if (::waitid(P_PID, static_cast<id_t>(pid_), &si,
               WEXITED | WNOHANG | WNOWAIT) == 0 &&
      si.si_pid == pid_ &&
      (si.si_code == CLD_EXITED || si.si_code == CLD_KILLED ||
       si.si_code == CLD_DUMPED))
    return false;
  return true;
}

void WorkerProc::set_logs(LogSink logs) {
  read_logs(false);  // what the previous call left goes to its own sink
  std::lock_guard<std::mutex> l(log_mu_);
  logs_ = std::move(logs);
}

void WorkerProc::clear_scratch() {
  if (borrowed_) {
    borrowed_clear_();
    return;
  }
  try {
    if (!artifact_scratch_.empty()) {
      fs::clear_staging(artifact_scratch_ / "outputs", recover_permissions_);
      fs::clear_staging(artifact_scratch_ / "inputs", recover_permissions_);
    }
    fs::clear_staging(tmp_dir(), recover_permissions_,
                      x11_mounted_ ? ".X11-unix" : "");
  } catch (const std::system_error& error) {
    throw Failed(FailureKind::worker_failed,
                 "cannot clear the worker's invocation staging: " +
                     std::string(error.what()));
  }
}

std::optional<uint64_t> WorkerProc::rss_bytes() const {
  if (pid_ <= 0) return std::nullopt;
#ifdef __APPLE__
  rusage_info_v2 ri{};
  if (proc_pid_rusage(pid_, RUSAGE_INFO_V2,
                      reinterpret_cast<rusage_info_t*>(&ri)) != 0)
    return std::nullopt;
  return ri.ri_resident_size;
#else
  // Under bubblewrap the worker is a descendant of pid_: sum the whole tree.
  uint64_t total = 0;
  bool any = false;
  std::vector<pid_t> todo = {pid_};
  while (!todo.empty()) {
    pid_t p = todo.back();
    todo.pop_back();
    std::ifstream st("/proc/" + std::to_string(p) + "/status");
    for (std::string line; std::getline(st, line);)
      if (line.starts_with("VmRSS:")) {
        total += std::stoull(line.substr(6)) * 1024;
        any = true;
      }
    std::ifstream ch("/proc/" + std::to_string(p) + "/task/" +
                     std::to_string(p) + "/children");
    for (pid_t c; ch >> c;) todo.push_back(c);
  }
  if (!any) return std::nullopt;
  return total;
#endif
}

void WorkerProc::terminate(bool force) {
  if (view_receiver_) view_receiver_->close();
  stop_data_reader();
  if (borrowed_) {
    if (alive_) {
      alive_ = false;
      borrowed_terminate_(force);
    }
    return;
  }
  if (pid_ > 0) {
    if (force) proc::kill_group(pid_, SIGTERM);
    ::shutdown(data_fd_, SHUT_RDWR);
    close_fd(data_fd_);
    close_fd(control_fd_);
    // Give a cooperative worker a moment to exit after its data socket closed.
    for (int i = 0; i < 50 && !proc::try_wait(pid_); ++i) ::usleep(10000);
    proc::kill_group(pid_, SIGKILL);
    proc::wait(pid_);
    pid_ = -1;
  }
  remove_card_cgroup(cgroup_);
  cgroup_.clear();
  stop_log_reader();
  read_logs(true);
  close_fd(out_fd_);
  close_fd(err_fd_);
  alive_ = false;
}

void WorkerProc::read_logs(bool drain) {
  std::lock_guard<std::mutex> l(log_mu_);
  char buf[16384];
  for (auto [fd, stream] : {std::pair{out_fd_, LogStream::standard_output},
                            std::pair{err_fd_, LogStream::standard_error}}) {
    if (fd < 0) continue;
    auto& dec = stream == LogStream::standard_output ? out_dec_ : err_dec_;
    while (true) {
      ssize_t r = ::read(fd, buf, sizeof buf);
      if (r <= 0) break;
      std::string text =
          dec.feed(std::string_view(buf, static_cast<size_t>(r)));
      if (!text.empty() && logs_) logs_(stream, text);
    }
    if (drain) {
      std::string rest = dec.finish();
      if (!rest.empty() && logs_) logs_(stream, rest);
    }
  }
}

void WorkerProc::send(int fd, const std::string& frame, bool observe) {
  // A stopped worker may also stop draining control messages. Never let the
  // cancellation notification delay escalation to killing the process group.
  send_frame(trace_link_, fd, frame, nullptr,
             std::chrono::steady_clock::now() + std::chrono::milliseconds(100),
             observe);
}

auto WorkerProc::start_data_reader() -> void {
  if (::pipe(data_wake_.data()) != 0)
    throw std::runtime_error("cannot create worker data wake pipe");
  for (const int fd : data_wake_) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  view_receiver();
  display_writer_ = std::thread([this] { write_display(); });
  data_thread_ = std::thread([this] { read_data(); });
}

auto WorkerProc::stop_data_reader() -> void {
  {
    std::lock_guard lock(display_mutex_);
    data_stop_.request_stop();
  }
  display_ready_.notify_all();
  if (display_writer_.joinable()) display_writer_.join();
  if (data_thread_.joinable()) {
    const char wake = 0;
    ssize_t written;
    do {
      written = ::write(data_wake_[1], &wake, 1);
    } while (written < 0 && errno == EINTR);
    data_thread_.join();
  }
  for (auto& fd : data_wake_) close_fd(fd);
}

auto WorkerProc::set_display(DisplaySink display) -> void {
  auto sink =
      display ? std::make_shared<DisplaySink>(std::move(display)) : nullptr;
  std::lock_guard lock(data_mutex_);
  display_ = std::move(sink);
}

auto WorkerProc::set_view_control(ViewControl control) -> void {
  auto handler =
      control ? std::make_shared<ViewControl>(std::move(control)) : nullptr;
  std::lock_guard lock(data_mutex_);
  view_control_ = std::move(handler);
}

auto WorkerProc::ViewReceiver::send(const WorkerViewEvent& event,
                                    DisplayCredit complete) -> ViewDelivery {
  std::lock_guard lock(mutex);
  if (remote) return remote(event, std::move(complete));
  return worker ? worker->send_display(event, std::move(complete))
                : ViewDelivery::closed;
}

auto WorkerProc::ViewReceiver::close() -> void {
  std::function<void()> notify;
  {
    std::lock_guard lock(mutex);
    worker = nullptr;
    remote = nullptr;
    notify = std::exchange(on_close, {});
  }
  if (notify) notify();
}

auto WorkerProc::ViewReceiver::reply(const WorkerViewReply& reply) -> bool {
  std::lock_guard lock(mutex);
  return worker && worker->reply_view(reply);
}

auto WorkerProc::view_receiver() -> std::weak_ptr<ViewReceiver> {
  if (!view_receiver_) {
    view_receiver_ = std::make_shared<ViewReceiver>();
    view_receiver_->worker = this;
  }
  return view_receiver_;
}

auto WorkerProc::send_display(const WorkerViewEvent& event,
                              DisplayCredit complete) -> ViewDelivery {
  if (!workerd::valid_view_event(event)) return ViewDelivery::full;
  std::lock_guard lock(display_mutex_);
  if (data_stop_.stop_requested()) return ViewDelivery::closed;
  auto& route = control_views_[event.route];
  if (route.closed) return ViewDelivery::closed;
  if (event.kind == ViewEventKind::close) {
    route.closed = true;
  } else {
    if (route.pending_bytes > workerd::kViewBudget - TSUGUMI_MESSAGE_MAX)
      return ViewDelivery::full;
    route.pending_bytes += workerd::message_cost(event.bytes.size());
  }
  route.pending.emplace_back(event, std::move(complete));
  display_ready_.notify_one();
  return ViewDelivery::accepted;
}

auto WorkerProc::reply_view(const WorkerViewReply& reply) -> bool {
  std::lock_guard lock(display_mutex_);
  if (data_stop_.stop_requested() || !awaiting_view_reply_ ||
      view_request_id_ != reply.request_id || view_reply_)
    return false;
  view_reply_ = reply;
  display_ready_.notify_one();
  return true;
}

auto WorkerProc::return_display_credit(const ViewUuid& session, size_t bytes)
    -> void {
  std::lock_guard lock(display_mutex_);
  if (data_stop_.stop_requested()) return;
  forward_credits_[session] += bytes;
  display_ready_.notify_one();
}

auto WorkerProc::write_display() -> void {
  ViewUuid previous{};
  const auto has_capacity = [](const ControlView& route) {
    return route.reverse_credit <= workerd::kViewBudget - TSUGUMI_MESSAGE_MAX;
  };
  const auto ready = [&](const auto& entry) {
    const auto& route = entry.second;
    return !route.pending.empty() &&
           (route.pending.front().first.kind == ViewEventKind::close ||
            has_capacity(route));
  };
  try {
    for (;;) {
      flatbuffers::FlatBufferBuilder b;
      DisplayCredit complete;
      {
        std::unique_lock lock(display_mutex_);
        display_ready_.wait(lock, [&] {
          return data_stop_.stop_requested() || view_reply_ ||
                 !forward_credits_.empty() ||
                 std::ranges::any_of(control_views_, ready);
        });
        if (data_stop_.stop_requested()) break;
        if (view_reply_) {
          workerd::build_view_reply(b, *view_reply_);
          view_reply_.reset();
          awaiting_view_reply_ = false;
        } else if (!forward_credits_.empty()) {
          auto credit = forward_credits_.extract(forward_credits_.begin());
          workerd::build_display_credit(b, credit.key(), credit.mapped());
        } else {
          const auto end = control_views_.end();
          auto next =
              std::find_if(control_views_.upper_bound(previous), end, ready);
          if (next == end)
            next = std::find_if(control_views_.begin(), end, ready);
          previous = next->first;
          auto& route = next->second;
          auto [event, done] = std::move(route.pending.front());
          route.pending.pop_front();
          if (event.kind == ViewEventKind::message) {
            const auto charge = workerd::message_cost(event.bytes.size());
            route.pending_bytes -= charge;
            route.reverse_credit += charge;
          }
          complete = std::move(done);
          workerd::build_view_event(b, event);
        }
      }
      try {
        std::lock_guard write(control_mutex_);
        send(control_fd_,
             {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()},
             false);
      } catch (const std::exception&) {
        if (complete) complete();
        throw;
      }
      if (complete) complete();
    }
  } catch (const std::exception&) {
    {
      std::lock_guard lock(display_mutex_);
      data_stop_.request_stop();
    }
    ::shutdown(control_fd_, SHUT_RDWR);
    ::shutdown(data_fd_, SHUT_RDWR);
  }
  std::map<ViewUuid, ControlView> discarded;
  {
    std::lock_guard lock(display_mutex_);
    discarded.swap(control_views_);
  }
  for (auto& [address, route] : discarded)
    for (auto& [event, done] : route.pending)
      if (done) done();
}

auto WorkerProc::read_data() -> void {
  // Do not use a blocking exact read: teardown must interrupt partial frames
  // while another owner still holds a copy of the worker socket.
  const auto read = [&](char* bytes, size_t size) {
    size_t offset = 0;
    while (offset < size) {
      pollfd ready[] = {{data_fd_, POLLIN, 0}, {data_wake_[0], POLLIN, 0}};
      if (::poll(ready, 2, -1) < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (ready[1].revents) return false;
      const auto count = ::read(data_fd_, bytes + offset, size - offset);
      if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      if (count <= 0) return false;
      offset += static_cast<size_t>(count);
    }
    return true;
  };
  std::string diagnostic = "the worker exited before returning a result";
  try {
    std::array<char, rpc::frame::kPrefixBytes> prefix{};
    while (read(prefix.data(), prefix.size())) {
      const auto size = rpc::frame::DecodePrefix(prefix.data(),
                                                 workerd::kMaxWorkerFrameBytes);
      std::string frame(size, '\0');
      if (!read(frame.data(), frame.size())) break;
      flatbuffers::Verifier verifier(
          reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
      if (!W::VerifyWorkerMessageBuffer(verifier)) {
        diagnostic = "worker sent a buffer that does not verify";
        break;
      }
      const auto* message = W::GetWorkerMessage(frame.data());
      if (const auto* update = message->value_as_WorkerDisplay()) {
        if (message->call_id() != 0 ||
            workerd::view_uuid(update->session()) == ViewUuid{} ||
            update->bytes()->size() > TSUGUMI_MESSAGE_MAX)
          throw Failed(FailureKind::worker_failed, "invalid worker display message");
        std::shared_ptr<DisplaySink> sink;
        {
          std::lock_guard lock(data_mutex_);
          sink = display_;
        }
        auto credit = [receiver = std::weak_ptr(view_receiver_),
                       session = workerd::view_uuid(update->session()),
                       cost = workerd::message_cost(update->bytes()->size())] {
          if (const auto target = receiver.lock()) {
            std::lock_guard lock(target->mutex);
            if (target->worker)
              target->worker->return_display_credit(session, cost);
          }
        };
        if (!sink)
          credit();
        else
          (*sink)({workerd::view_uuid(update->session()),
                   workerd::view_bytes(update->bytes())},
                  std::move(credit));
        continue;
      }
      if (const auto* update = message->value_as_WorkerDisplayCredit()) {
        std::lock_guard lock(display_mutex_);
        const auto found =
            control_views_.find(workerd::view_uuid(update->address()));
        if (message->call_id() != 0 || found == control_views_.end() ||
            update->bytes() > found->second.reverse_credit)
          throw Failed(FailureKind::worker_failed, "invalid worker display credit");
        found->second.reverse_credit -= update->bytes();
        display_ready_.notify_one();
        continue;
      }
      if (const auto* raw = message->value_as_WorkerViewRequest()) {
        const auto request = workerd::view_request(*raw);
        {
          std::lock_guard lock(display_mutex_);
          if (message->call_id() != 0 || awaiting_view_reply_ ||
              request.request_id <= view_request_id_ ||
              raw->kind() > W::ViewControlKind::Close)
            throw Failed(FailureKind::worker_failed,
                         "invalid worker view request");
          view_request_id_ = request.request_id;
          awaiting_view_reply_ = true;
        }
        std::shared_ptr<ViewControl> handler;
        {
          std::lock_guard lock(data_mutex_);
          handler = view_control_;
        }
        auto reply = handler
                         ? (*handler)(request)
                         : std::optional{WorkerViewReply{request.request_id}};
        if (reply) {
          reply->request_id = request.request_id;
          reply_view(*reply);
        }
        continue;
      }
      if (message->value_as_WorkerViewEvent() ||
          message->value_as_WorkerViewReply())
        throw Failed(FailureKind::worker_failed,
                     "unexpected worker view relay direction");
      std::unique_lock lock(data_mutex_);
      if (!awaiting_ || message->call_id() != awaiting_ || pending_) {
        log::warn("discarding a worker reply for inactive call " +
                  std::to_string(message->call_id().value_or(0)));
        continue;
      }
      trace::Observe(trace_link_, "Card worker", "project core", false, frame,
                     &trace::kFlatBufferCodec);
      if (const auto* create = message->value_as_WorkerArtifactCreate()) {
        flatbuffers::FlatBufferBuilder b;
        const auto counter = created_artifacts_.size();
        std::optional<::tsugumi::ArtifactId> id;
        if (create_artifacts_ && !(create->flags() & ~TSUGUMI_ARTIFACT_KEEP)) {
          auto key = *parse_artifact_id(crypto::uuid4());
          id = wire::artifact_struct(key);
          created_artifacts_.push_back(
              {std::move(key), bool(create->flags() & TSUGUMI_ARTIFACT_KEEP)});
        }
        const auto response =
            W::CreateWorkerArtifactCreated(b, id ? &*id : nullptr, counter);
        W::FinishWorkerMessageBuffer(
            b, W::CreateWorkerMessage(b, *message->call_id(),
                                      W::WorkerPayload::WorkerArtifactCreated,
                                      response.Union()));
        send(data_fd_, {reinterpret_cast<const char*>(b.GetBufferPointer()),
                        b.GetSize()});
        continue;
      }
      if (const auto* open = message->value_as_WorkerArtifactOpen()) {
        const auto key = wire::artifact_id(*open->id());
        const bool unsealed =
            std::ranges::find(created_artifacts_, key, &CreatedArtifact::id) !=
            created_artifacts_.end();
        const auto open_artifact = open_artifact_;
        const auto call_id = *message->call_id();
        lock.unlock();
        const auto path = open_artifact && !unsealed
                              ? open_artifact(key, data_stop_.get_token())
                              : "";
        lock.lock();
        if (awaiting_ != call_id) continue;
        flatbuffers::FlatBufferBuilder b;
        const auto response = W::CreateWorkerArtifactOpened(
            b, path.empty() ? 0 : b.CreateString(path));
        W::FinishWorkerMessageBuffer(
            b, W::CreateWorkerMessage(b, call_id,
                                      W::WorkerPayload::WorkerArtifactOpened,
                                      response.Union()));
        send(data_fd_, {reinterpret_cast<const char*>(b.GetBufferPointer()),
                        b.GetSize()});
        continue;
      }
      WorkerOutcome outcome;
      if (const auto* result = message->value_as_WorkerResult()) {
        for (const auto* output : *result->outputs()) {
          const auto* bytes = output->bytes();
          outcome.outputs.push_back(
              {output->name()->str(),
               bytes ? std::string(bytes->begin(), bytes->end()) : ""});
        }
      } else if (const auto* failure = message->value_as_WorkerFailure()) {
        outcome.failure = wire::failure(failure->failure());
      } else {
        diagnostic = "unexpected message from worker";
        break;
      }
      outcome.created = std::move(created_artifacts_);
      pending_ = std::move(outcome);
      data_ready_.notify_one();
    }
  } catch (const std::exception& error) {
    diagnostic = error.what();
  }
  {
    std::lock_guard lock(data_mutex_);
    data_closed_ = true;
    display_.reset();
    view_control_.reset();
    if (awaiting_ && !pending_) {
      pending_.emplace();
      pending_->failure =
          FailureData::make(FailureKind::worker_failed, diagnostic);
    }
  }
  {
    std::lock_guard lock(display_mutex_);
    data_stop_.request_stop();
  }
  view_receiver_->close();
  display_ready_.notify_all();
  data_ready_.notify_one();
}

WorkerOutcome WorkerProc::call(
    std::string frame, uint64_t call_id, const std::atomic<bool>* cancel,
    std::optional<std::chrono::steady_clock::time_point> deadline,
    ArtifactOpen open_artifact, bool create_artifacts) {
  {
    std::lock_guard lock(data_mutex_);
    if (data_closed_)
      return {{},
              FailureData::make(FailureKind::worker_failed,
                                "worker data channel closed")};
    awaiting_ = call_id;
    open_artifact_ = std::move(open_artifact);
    create_artifacts_ = create_artifacts;
    created_artifacts_.clear();
  }
  try {
    send_frame(trace_link_, data_fd_, frame, cancel, deadline);
  } catch (const Failed& error) {
    workerd::clear_secret(frame);
    // A partially sent call cannot be resumed or reused.
    terminate();
    return {{}, error.failure};
  }
  workerd::clear_secret(frame);
  std::optional<std::chrono::steady_clock::time_point> kill_at;
  bool timed_out = false;
  const auto stopped = [&](std::string extra = {}) {
    return FailureData::make(
        timed_out ? FailureKind::timeout : FailureKind::cancelled,
        (timed_out ? "the Card exceeded timeoutSeconds"
                   : "the invocation was cancelled") +
            extra);
  };
  while (true) {
    {
      std::unique_lock lock(data_mutex_);
      if (data_ready_.wait_for(lock, std::chrono::milliseconds(20),
                               [&] { return pending_.has_value(); })) {
        auto outcome = std::move(*pending_);
        const bool disconnected = data_closed_;
        pending_.reset();
        awaiting_ = 0;
        open_artifact_ = {};
        lock.unlock();
        if (disconnected) terminate();
        read_logs(false);
        if (kill_at) {
          std::string extra;
          if (!disconnected && outcome.failure &&
              outcome.failure->kind != FailureKind::cancelled &&
              outcome.failure->kind != FailureKind::timeout) {
            const auto& reported = *outcome.failure;
            extra = std::string("; the worker then reported ") +
                    failure_kind_name(reported.kind);
            if (reported.diagnostic && !reported.diagnostic->empty())
              extra += ": " + *reported.diagnostic;
          }
          outcome.failure = stopped(std::move(extra));
        }
        return outcome;
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (!kill_at &&
        ((cancel && cancel->load()) || (deadline && now >= *deadline))) {
      timed_out = !(cancel && cancel->load());
      flatbuffers::FlatBufferBuilder b;
      const auto value = W::CreateWorkerCancel(b);
      W::FinishWorkerMessageBuffer(
          b, W::CreateWorkerMessage(b, call_id, W::WorkerPayload::WorkerCancel,
                                    value.Union()));
      try {
        std::lock_guard lock(control_mutex_);
        send(control_fd_, {reinterpret_cast<const char*>(b.GetBufferPointer()),
                           b.GetSize()});
      } catch (const Failed&) {
      }
      kill_at = now + std::chrono::seconds(1);
    }
    if (kill_at && now >= *kill_at) {
      terminate(true);
      return {{}, stopped()};
    }
  }
}

std::string worker_call_frame(uint64_t call_id, std::span<const Value> inputs,
                              const std::vector<types::PortType>& outputs,
                              std::optional<uint64_t> deadline_ms,
                              const std::filesystem::path& artifact_directory,
                              bool drain_display) {
  flatbuffers::FlatBufferBuilder b(64 * 1024);
  // The Card reads each nested message in place.
  auto message = [&](const std::string& s) {
    b.ForceVectorAlignment(s.size(), sizeof(uint8_t),
                           FLATBUFFERS_MAX_ALIGNMENT);
    return b.CreateVector(reinterpret_cast<const uint8_t*>(s.data()),
                          s.size());
  };
  std::vector<flatbuffers::Offset<C::CardPort>> ins;
  for (const auto& v : inputs)
    ins.push_back(C::CreateCardPort(
        b, b.CreateString(v.name), b.CreateString(v.schema), message(v.bytes)));
  auto insv = b.CreateVector(ins);
  std::vector<flatbuffers::Offset<W::WorkerPort>> outs;
  for (const auto& p : outputs)
    outs.push_back(W::CreateWorkerPort(b, b.CreateString(p.name), p.required));
  const auto artifact_root = artifact_directory.empty()
                                 ? flatbuffers::Offset<flatbuffers::String>{}
                                 : b.CreateString(artifact_directory.string());
  auto outsv = b.CreateVector(outs);
  W::WorkerCallBuilder cb(b);
  cb.add_inputs(insv);
  cb.add_artifact_directory(artifact_root);
  cb.add_drain_display(drain_display);
  cb.add_outputs(outsv);
  if (deadline_ms) cb.add_deadline_ms(*deadline_ms);
  auto call = cb.Finish();
  W::WorkerMessageBuilder mb(b);
  mb.add_call_id(call_id);
  mb.add_value_type(W::WorkerPayload::WorkerCall);
  mb.add_value(call.Union());
  W::FinishWorkerMessageBuffer(b, mb.Finish());
  if (b.GetSize() > workerd::kMaxWorkerFrameBytes)
    throw Failed(FailureKind::resource_limit, "the worker call exceeds the frame limit");
  return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()),
                     b.GetSize());
}

}  // namespace tsugumi
