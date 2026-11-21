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
#ifdef __APPLE__
#include <libproc.h>
#else
#include <sys/vfs.h>
#endif

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
namespace D = Tsugumi::Data;

namespace {

void close_fd(int& fd) {
  if (fd >= 0) ::close(fd);
  fd = -1;
}

auto send_frame(trace::Link& link, int fd, const std::string& frame,
                const std::atomic<bool>* cancel,
                std::optional<std::chrono::steady_clock::time_point> deadline)
    -> void {
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
  trace::Observe(link, "project core", "Card worker", true, frame,
                 &trace::kFlatBufferCodec);
}

#ifdef __APPLE__
// Seatbelt profile for sandbox: true Cards when tsugumid runs directly on a
// macOS host (sandbox-exec is deprecated but still the system's process
// sandbox). As under bubblewrap: no network unless the Card allows it,
// /nix/store, staged invocation inputs and system libraries read-only, writes
// only to the worker's own folder (scripts excepted) and to the output
// staging folder of the current call (copied to the run afterwards). Paths
// are matched after symlink resolution (/tmp -> /private/tmp).
auto seatbelt_profile(const WorkerSpawn& sp, const std::string& scratch,
                      const std::string& scripts) -> std::string {
  if (!sp.sandbox.filesystem)
    return sp.network ? std::string{}
                      : "(version 1)\n(allow default)\n(deny network*)\n";
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
  std::string read =
      "(literal \"/\") (subpath \"/nix/store\") (subpath \"/usr/lib\") "
      "(subpath \"/usr/share\")"
      " (subpath \"/System\") (subpath \"/bin\") (subpath \"/usr/bin\") "
      "(subpath \"/private/etc\")"
      " (subpath \"/private/var/db/timezone\") (subpath \"/dev\")";
  if (std::string_view(TSUGUMI_CACERT).size())
    read += " (literal " + quote(TSUGUMI_CACERT) + ")";
  read += " (literal " + quote(sp.worker_binary) + ") (subpath " +
          quote(scratch) + ")";
  if (!sp.bash.empty()) read += " (literal " + quote(sp.bash) + ")";
  std::string write =
      "(literal \"/dev/null\") (literal \"/dev/tty\") (subpath " +
      quote(scratch) + ")";
  for (const auto& p : sp.ro_paths) {
    read += " (subpath " + quote(p) + ")";
  }
  std::string read_only_grants;
  for (const auto& bind : sp.binds) {
    const auto rule = " (subpath " + quote(bind.source) + ")";
    read += rule;
    if (bind.write)
      write += rule;
    else
      read_only_grants += rule;
  }
  return "(version 1)\n(deny default)\n"
         "(allow process-fork)\n(allow process-exec)\n(allow signal (target "
         "same-sandbox))\n"
         "(allow sysctl-read)\n(allow ipc-posix-shm)\n(allow "
         "file-read-metadata)\n"
         "(allow file-read* " +
         read + ")\n(allow file-write* " + write +
         ")\n"
         "(deny file-write* (subpath " +
         quote(scripts) + ") (subpath " + quote(scratch + "/inputs") + ")" +
         read_only_grants +
         ")\n"
         "(allow file-ioctl (subpath \"/dev\"))\n" +
         // With the network allowed, still nothing on this host: tsugumid's
         // own ports and sockets, the web server, other local services. For
         // Seatbelt "localhost" is every address of this host (LAN, VPN,
         // 0.0.0.0), not only loopback; it accepts no other host, so other
         // machines cannot be denied one by one. The one exception is
         // mDNSResponder: getaddrinfo asks its socket for every name not in
         // /etc/hosts. The last matching rule wins, so it comes after the
         // denial. Of the Mach services only trustd: Security.framework (Go,
         // NSURLSession) verifies certificates through it. The others are the
         // user's session: the clipboard, LaunchServices, Apple events.
         (sp.network ? "(allow network*)\n(allow mach-lookup (global-name "
                       "\"com.apple.trustd.agent\"))\n"
                       "(deny network-outbound (remote ip \"localhost:*\"))\n"
                       "(deny network-outbound (remote unix-socket))\n"
                       "(allow network-outbound (remote unix-socket "
                       "(path-literal \"/private/var/run/mDNSResponder\")))\n"
                     : "");
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
  // The worker's own folder; the step scripts are written once per worker.
  auto scripts = std::filesystem::path(scratch_) / "scripts";
#ifdef __APPLE__
  // O_EVTONLY still needs read permission on Darwin. Repair an unreadable
  // directory with exactly the Card's identity and sandbox, then open it with
  // O_NOFOLLOW. Even a surviving child swapping in a hard link cannot enlist
  // the core's broader authority to chmod a protected host inode.
  const auto profile = seatbelt_profile(sp, scratch_, scripts.string());
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
    if (!profile.empty())
      repair.argv = {"/usr/bin/sandbox-exec", "-p", profile};
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
  for (const char* d : {"work", "tmp", "out", "scripts", "inputs"})
    std::filesystem::create_directories(std::filesystem::path(scratch_) / d);
  if (::geteuid() == 0 && sp.uid && worker_uid_on_host())
    for (const char* d : {"work", "tmp", "out"}) {
      const auto path = scratch_ + "/" + d;
      if (::chown(path.c_str(), *sp.uid, sp.gid.value_or(*sp.uid)) != 0) {
        const int error = errno;
        throw Failed(FailureKind::worker_failed,
                     "chown " + path + ": " + std::strerror(error));
      }
    }
  std::vector<std::string> args;
  auto script = [&](const char* flag, const std::string& text,
                    const char* name) {
    if (text.empty()) return;
    auto path = scripts / name;
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    args.push_back(flag);
    args.push_back(path.string());
  };
  script("--pre", sp.program.pre_run, "pre.sh");
  if (sp.program.bash) script("--run", *sp.program.bash, "run.sh");
  script("--post", sp.program.post_run, "post.sh");
  if (!sp.program.views.empty())
    args.insert(args.end(), {"--views", sp.program.views});
  if (sp.program.package) {
    args.push_back("--package");
    args.push_back(*sp.program.package);
  }
  args.push_back("--bash");
  args.push_back(sp.bash);

  proc::Spec spec;
  std::string certificate = TSUGUMI_CACERT;
  std::string temporary = tmp_dir();
  std::vector<std::string> gui_env;
#ifdef __APPLE__
  if (sp.gui || sp.sandbox.limits.limited())
    throw Failed(FailureKind::unsupported,
                 "native macOS does not support GUI grants or sandbox limits");
  if (!profile.empty()) {
    if (::access("/usr/bin/sandbox-exec", X_OK) != 0)
      throw Failed(FailureKind::permission_denied, "Seatbelt is unavailable");
    spec.argv = {"/usr/bin/sandbox-exec", "-p", profile};
  }
  spec.argv.push_back(sp.worker_binary);
  spec.uid = sp.uid;
  spec.gid = sp.gid;
#else
  const bool bwrap = worker_has_bwrap();
  const bool filesystem = sp.sandbox.filesystem;
  if (!bwrap && (filesystem || !sp.hidden_paths.empty() || sp.gui ||
                 sp.sandbox.limits.limited()))
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
      if (!sp.bash.empty())
        argv.insert(argv.end(), {"--symlink", sp.bash, "/bin/sh"});
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
    } else if (!sp.sandbox.ipc && filesystem) {
      for (const char* path : {"/dev/shm", "/dev/mqueue"})
        argv.insert(argv.end(), {"--bind-try", path, path});
    }
    // Paths under the original /tmp must remain visible after mounting scratch.
    const auto under_tmp = [](const std::string& path) {
      return path == "/tmp" || path.starts_with("/tmp/");
    };
    if (!under_tmp(scratch_) && !under_tmp(sp.worker_binary) &&
        !std::ranges::any_of(sp.ro_paths, under_tmp)) {
      argv.insert(argv.end(), {"--bind", tmp_dir(), "/tmp"});
      temporary = "/tmp";
    }
    for (const auto& path : sp.ro_paths)
      argv.insert(argv.end(), {"--ro-bind", path, path});
    if (filesystem || !sp.hidden_paths.empty())
      argv.insert(argv.end(), {"--bind", scratch_, scratch_, "--ro-bind",
                               scripts.string(), scripts.string()});
    if (filesystem) {
      argv.insert(argv.end(), {"--ro-bind", inputs_dir(), inputs_dir()});
      read_only_inputs_ =
          std::ranges::none_of(sp.binds, &WorkerSpawn::Bind::write);
      for (const auto& bind : sp.binds)
        argv.insert(argv.end(), {bind.write ? "--bind" : "--ro-bind",
                                 bind.source, bind.target});
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
      const auto socket = sp.shared_vm ? std::nullopt : gui_socket();
      if (!socket)
        throw Failed(FailureKind::unsupported,
                     "grants.gui needs a native Linux GUI socket");
      auto value = socket->value;
      if (socket->variable == "WAYLAND_DISPLAY" && filesystem) {
        value = "/run/tsugumi-wayland";
        argv.insert(argv.end(), {"--bind", socket->source, value});
      }
      if (socket->variable == "DISPLAY" && (filesystem || temporary == "/tmp"))
        argv.insert(argv.end(), {"--bind", socket->source, socket->source});
      if (filesystem)
        argv.insert(argv.end(), {"--dev-bind-try", "/dev/dri", "/dev/dri"});
      gui_env.push_back(socket->variable + "=" + value);
    }
    argv.insert(argv.end(), {"--chdir", work_dir(), "--", sp.worker_binary});
  } else {
    spec.argv = {sp.worker_binary};
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
  spec.argv.insert(spec.argv.end(), args.begin(), args.end());
  spec.env = {"PATH=" + sp.path,
              "LANG=C.UTF-8",
              "HOME=" + work_dir(),
              "TMPDIR=" + temporary,
              "TSUGUMI_WORK_DIR=" + work_dir(),
              "TSUGUMI_TMP_DIR=" + temporary,
              "TSUGUMI_OUTPUT_DIR=" + out_dir(),
              "TSUGUMI_WORKER=" + sp.worker_binary};
  for (const auto& bind : sp.binds) {
    auto path = bind.source;
#ifdef __linux__
    if (sp.sandbox.filesystem) path = bind.target;
#endif
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
    resume_logs();
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
      read_only_inputs_(borrowed.read_only_inputs),
      call_id_(borrowed.call_id),
      borrowed_(true),
      lease_id_(borrowed.lease),
      borrowed_clear_(std::move(borrowed.clear)),
      borrowed_terminate_(std::move(borrowed.terminate)),
      borrowed_abandon_(std::move(borrowed.abandon)) {
  resume_logs();
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
  if (!scratch_.empty()) {
    try {
      // Also covers failed/dead and remote workers, and lifetime home data.
      fs::clear_staging(scratch_, recover_permissions_);
      std::filesystem::remove(scratch_, ec);
      if (ec)
        log::warn("cannot remove worker scratch " + scratch_ + " (errno " +
                  std::to_string(ec.value()) + "): " + ec.message());
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
  siginfo_t si{};
  if (::waitid(P_PID, static_cast<id_t>(pid_), &si,
               WEXITED | WNOHANG | WNOWAIT) == 0 &&
      si.si_pid == pid_)
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
    for (const auto& d : {work_dir(), tmp_dir(), out_dir(), inputs_dir()})
      fs::clear_staging(d, recover_permissions_);
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
  if (borrowed_) {
    if (alive_) {
      alive_ = false;
      borrowed_terminate_(force);
    }
    return;
  }
  if (pid_ > 0) {
    if (force) proc::kill_group(pid_, SIGTERM);
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

void WorkerProc::send(int fd, const std::string& frame) {
  // A stopped worker may also stop draining control messages. Never let the
  // cancellation notification delay escalation to killing the process group.
  send_frame(trace_link_, fd, frame, nullptr,
             std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
}

WorkerOutcome WorkerProc::call(
    std::string frame, uint64_t call_id, const std::atomic<bool>* cancel,
    std::optional<std::chrono::steady_clock::time_point> deadline,
    const DisplaySink& display) {
  WorkerOutcome outcome;
  try {
    send_frame(trace_link_, data_fd_, frame, cancel, deadline);
  } catch (const Failed& error) {
    workerd::clear_secret(frame);
    // A partially sent call cannot be resumed or reused.
    terminate();
    outcome.failure = error.failure;
    return outcome;
  }
  workerd::clear_secret(frame);
  std::string rbuf;
  bool cancel_sent = false;
  std::optional<std::chrono::steady_clock::time_point> kill_at;
  bool timed_out = false;
  while (true) {
    pollfd p{data_fd_, POLLIN, 0};
    ::poll(&p, 1, 20);
    auto now = std::chrono::steady_clock::now();
    if (!cancel_sent &&
        ((cancel && cancel->load()) || (deadline && now >= *deadline))) {
      timed_out = !(cancel && cancel->load());
      flatbuffers::FlatBufferBuilder b;
      auto wc = W::CreateWorkerCancel(b);
      W::WorkerMessageBuilder mb(b);
      mb.add_call_id(call_id);
      mb.add_value_type(W::WorkerPayload::WorkerCancel);
      mb.add_value(wc.Union());
      W::FinishWorkerMessageBuffer(b, mb.Finish());
      try {
        send(control_fd_,
             std::string(reinterpret_cast<const char*>(b.GetBufferPointer()),
                         b.GetSize()));
      } catch (const Failed&) {
      }
      cancel_sent = true;
      // A worker can ignore cancellation and otherwise never exit.
      kill_at = now + std::chrono::seconds(1);
    }
    if (kill_at && now >= *kill_at) {
      terminate(true);
      read_logs(true);
      outcome.failure =
          timed_out ? FailureData::make(FailureKind::timeout,
                                        "the Card exceeded timeoutSeconds")
                    : FailureData::make(FailureKind::cancelled,
                                        "the invocation was cancelled");
      return outcome;
    }
    if (!(p.revents & (POLLIN | POLLHUP | POLLERR))) continue;
    char buf[65536];
    ssize_t r = ::read(data_fd_, buf, sizeof buf);
    if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    if (r <= 0) {
      terminate();
      if (cancel_sent)
        outcome.failure =
            timed_out ? FailureData::make(FailureKind::timeout,
                                          "the Card exceeded timeoutSeconds")
                      : FailureData::make(FailureKind::cancelled,
                                          "the invocation was cancelled");
      else
        outcome.failure =
            FailureData::make(FailureKind::worker_failed,
                              "the worker exited before returning a result");
      return outcome;
    }
    rbuf.append(buf, static_cast<size_t>(r));
    while (rbuf.size() >= rpc::frame::kPrefixBytes) {
      uint32_t len = 0;
      try {
        len = rpc::frame::DecodePrefix(rbuf.data(),
                                       workerd::kMaxWorkerFrameBytes);
      } catch (const rpc::frame::Error&) {
        terminate();
        outcome.failure = FailureData::make(FailureKind::worker_failed,
                                            "corrupt frame from worker");
        return outcome;
      }
      if (rbuf.size() < 4 + size_t(len)) break;
      std::string msg = rbuf.substr(4, len);
      rbuf.erase(0, 4 + size_t(len));
      flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(msg.data()),
                              msg.size());
      if (!W::VerifyWorkerMessageBuffer(v)) {
        terminate();
        outcome.failure =
            FailureData::make(FailureKind::worker_failed,
                              "worker sent a buffer that does not verify");
        return outcome;
      }
      trace::Observe(trace_link_, "Card worker", "project core", false, msg,
                     &trace::kFlatBufferCodec);
      const auto* m = W::GetWorkerMessage(msg.data());
      if (!m->call_id() || *m->call_id() != call_id) {
        terminate();
        outcome.failure = FailureData::make(
            FailureKind::worker_failed, "worker answered an unknown call_id");
        return outcome;
      }
      switch (m->value_type()) {
        case W::WorkerPayload::WorkerDisplay:
          if (display) {
            auto presentation =
                wire::display(m->value_as_WorkerDisplay()->display(), msg);
            try {
              display(presentation);
            } catch (const Failed&) {
              // Presentation sinks cannot fail the invocation.
            }
          }
          break;
        case W::WorkerPayload::WorkerResult: {
          for (const auto* o : *m->value_as_WorkerResult()->outputs())
            outcome.outputs.emplace_back(
                o->name()->str(),
                std::string(reinterpret_cast<const char*>(o->bytes()->data()),
                            o->bytes()->size()));
          read_logs(false);
          if (cancel_sent && !outcome.failure)
            outcome.failure =
                timed_out
                    ? FailureData::make(FailureKind::timeout,
                                        "the Card exceeded timeoutSeconds")
                    : FailureData::make(FailureKind::cancelled,
                                        "the invocation was cancelled");
          return outcome;
        }
        case W::WorkerPayload::WorkerFailure:
          if (!outcome.failure) {
            auto reported =
                wire::failure(m->value_as_WorkerFailure()->failure());
            if (cancel_sent) {
              // The host knows why it stopped the call, just as for
              // WorkerResult: a cooperative SDK reports "cancelled" for a
              // deadline too. A different failure that arrives after the
              // request keeps its text, so it is not lost behind the reason.
              std::string why = timed_out ? "the Card exceeded timeoutSeconds"
                                          : "the invocation was cancelled";
              if (reported.kind != FailureKind::cancelled &&
                  reported.kind != FailureKind::timeout)
                why += std::string("; the worker then reported ") +
                       failure_kind_name(reported.kind) +
                       (reported.diagnostic && !reported.diagnostic->empty()
                            ? ": " + *reported.diagnostic
                            : "");
              outcome.failure = FailureData::make(
                  timed_out ? FailureKind::timeout : FailureKind::cancelled,
                  why);
            } else {
              outcome.failure = std::move(reported);
            }
          }
          read_logs(false);
          return outcome;
        default:
          terminate();
          outcome.failure = FailureData::make(FailureKind::worker_failed,
                                              "unexpected message from worker");
          return outcome;
      }
    }
  }
}

std::string worker_call_frame(
    uint64_t call_id, const std::string& run_id, const CardPath& card,
    uint64_t invocation, std::span<const Value> inputs,
    const std::string& params_bfbs, const std::string& params_root,
    const std::string& params, const std::vector<types::PortType>& outputs,
    const std::string& work, const std::string& tmp, const std::string& out,
    const std::vector<std::pair<std::string, std::string>>& env,
    std::optional<uint64_t> deadline_ms) {
  flatbuffers::FlatBufferBuilder b(64 * 1024);
  auto bytes = [&](const std::string& s) {
    return b.CreateVector(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  };
  // The Card reads each nested message in place.
  auto message = [&](const std::string& s) {
    b.ForceVectorAlignment(s.size(), sizeof(uint8_t),
                           FLATBUFFERS_MAX_ALIGNMENT);
    return bytes(s);
  };
  std::vector<flatbuffers::Offset<W::WorkerInput>> ins;
  for (const auto& v : inputs) {
    auto payload = message(v.bytes);
    auto data = D::CreateFlatBufferData(
        b, bytes(v.type->bfbs), b.CreateString(v.type->root_type), payload);
    ins.push_back(
        W::CreateWorkerInput(b, b.CreateString(v.name), v.kind, data));
  }
  auto insv = b.CreateVector(ins);
  auto payload = message(params);
  auto pdata = D::CreateFlatBufferData(b, bytes(params_bfbs),
                                       b.CreateString(params_root), payload);
  std::vector<flatbuffers::Offset<D::Port>> outs;
  for (const auto& p : outputs) outs.push_back(wire::port(b, p));
  auto outsv = b.CreateVector(outs);
  std::vector<flatbuffers::Offset<W::EnvVar>> ev;
  for (const auto& [k, v] : env)
    ev.push_back(W::CreateEnvVar(b, b.CreateString(k), b.CreateString(v)));
  auto evv = b.CreateVector(ev);
  auto run = b.CreateString(run_id);
  auto cp = wire::path(b, card);
  auto w = b.CreateString(work);
  auto t = b.CreateString(tmp);
  flatbuffers::Offset<flatbuffers::String> o;
  if (!out.empty()) o = b.CreateString(out);
  W::WorkerCallBuilder cb(b);
  cb.add_run_id(run);
  cb.add_card(cp);
  cb.add_invocation(invocation);
  cb.add_inputs(insv);
  cb.add_params(pdata);
  cb.add_outputs(outsv);
  cb.add_working_directory(w);
  cb.add_temporary_directory(t);
  if (!out.empty()) cb.add_output_directory(o);
  cb.add_environment(evv);
  if (deadline_ms) cb.add_deadline_ms(*deadline_ms);
  auto call = cb.Finish();
  W::WorkerMessageBuilder mb(b);
  mb.add_call_id(call_id);
  mb.add_value_type(W::WorkerPayload::WorkerCall);
  mb.add_value(call.Union());
  W::FinishWorkerMessageBuffer(b, mb.Finish());
  if (b.GetSize() > workerd::kMaxWorkerFrameBytes)
    throw Failed(FailureKind::resource_limit, "the worker call exceeds 16 MiB");
  return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()),
                     b.GetSize());
}

}  // namespace tsugumi
