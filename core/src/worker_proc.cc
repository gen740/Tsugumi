#include "worker_proc.hh"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#ifdef __APPLE__
#include <libproc.h>
#endif

#include "tsugumi/protocol/worker_generated.h"
#include "store.hh"
#include "util/fs.hh"
#include "util/text.hh"
#include "util/log.hh"
#include "util/process.hh"
#include "wire.hh"

#ifndef TSUGUMI_BWRAP
#define TSUGUMI_BWRAP ""
#endif

namespace tsugumi {

namespace W = Tsugumi::Worker;
namespace D = Tsugumi::Data;

std::string Utf8Decoder::feed(std::string_view bytes) {
  std::string in = pending_ + std::string(bytes);
  pending_.clear();
  std::string out;
  size_t i = 0;
  while (i < in.size()) {
    unsigned char c = in[i];
    size_t n;
    if (c < 0x80) { out += static_cast<char>(c); ++i; continue; }
    if ((c & 0xE0) == 0xC0) n = 2;
    else if ((c & 0xF0) == 0xE0) n = 3;
    else if ((c & 0xF8) == 0xF0) n = 4;
    else { out += "\xEF\xBF\xBD"; ++i; continue; }
    if (i + n > in.size()) {
      // Possibly an incomplete character at the chunk end: keep it if it could still be valid.
      bool ok = true;
      for (size_t k = 1; i + k < in.size(); ++k) ok &= (static_cast<unsigned char>(in[i + k]) & 0xC0) == 0x80;
      if (ok) { pending_ = in.substr(i); break; }
      out += "\xEF\xBF\xBD";
      ++i;
      continue;
    }
    std::string_view ch(in.data() + i, n);
    if (text::valid_utf8(ch)) {
      out += ch;
      i += n;
    } else {
      out += "\xEF\xBF\xBD";
      ++i;
    }
  }
  return out;
}

std::string Utf8Decoder::finish() {
  std::string out = pending_.empty() ? "" : "\xEF\xBF\xBD";
  pending_.clear();
  return out;
}

namespace {

void close_fd(int& fd) {
  if (fd >= 0) ::close(fd);
  fd = -1;
}

bool write_full(int fd, const char* p, size_t n) {
  while (n > 0) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

}  // namespace

WorkerProc::WorkerProc(const WorkerSpawn& sp) : scratch_(sp.scratch) {
  int data[2], control[2], out[2], err[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, data) != 0 || ::socketpair(AF_UNIX, SOCK_STREAM, 0, control) != 0 ||
      ::pipe(out) != 0 || ::pipe(err) != 0)
    throw Failed(FailureKind::worker_failed, "cannot create worker channels");
  // The worker's own folder; the step scripts are written once per worker.
  auto scripts = std::filesystem::path(scratch_) / "scripts";
  for (const char* d : {"work", "tmp", "scripts"}) std::filesystem::create_directories(std::filesystem::path(scratch_) / d);
  if (::geteuid() == 0 && sp.uid)
    for (const char* d : {"work", "tmp"}) ::chown((scratch_ + "/" + d).c_str(), *sp.uid, sp.gid.value_or(*sp.uid));
  std::vector<std::string> args;
  auto script = [&](const char* flag, const std::string& text, const char* name) {
    if (text.empty()) return;
    auto path = scripts / name;
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    args.push_back(flag);
    args.push_back(path.string());
  };
  script("--pre", sp.build.pre_run, "pre.sh");
  if (sp.build.bash) script("--run", *sp.build.bash, "run.sh");
  script("--post", sp.build.post_run, "post.sh");
  if (sp.build.package) {
    args.push_back("--package");
    args.push_back(*sp.build.package);
  }
  args.push_back("--bash");
  args.push_back(sp.bash);

  proc::Spec spec;
  auto own = [&](std::vector<std::string>& argv) {
    for (const auto& p : sp.ro_paths) argv.insert(argv.end(), {"--ro-bind", p, p});
    argv.insert(argv.end(), {"--bind", scratch_, scratch_, "--ro-bind", scripts.string(), scripts.string()});
    for (const auto& b : sp.binds) argv.insert(argv.end(), {b.write ? "--bind" : "--ro-bind", b.source, b.target});
    if (sp.network && sp.sandbox)
      // Name resolution and the CA bundle of the environment (a sandbox
      // starts without /etc; unsandboxed, the real one is there).
      for (const char* f : {"/etc/resolv.conf", "/etc/hosts", "/etc/nsswitch.conf", "/etc/ssl"})
        argv.insert(argv.end(), {"--ro-bind-try", f, f});
  };
  if (!sp.binds.empty() && std::string(TSUGUMI_BWRAP).empty())
    throw Failed(FailureKind::unsupported, "folders of the Mac need the shared VM (bubblewrap)");
  if (sp.sandbox) {
    std::string bwrap = TSUGUMI_BWRAP;
    if (bwrap.empty() || ::access(bwrap.c_str(), X_OK) != 0)
      throw Failed(FailureKind::permission_denied,
                   "sandbox: true needs bubblewrap, which this environment does not provide; refusing to run unsandboxed");
    spec.argv = {bwrap, "--unshare-all", "--die-with-parent", "--new-session", "--cap-drop", "ALL",
                 "--ro-bind", "/nix/store", "/nix/store", "--proc", "/proc", "--dev", "/dev",
                 "--tmpfs", "/tmp"};
    if (sp.network) spec.argv.push_back("--share-net");
    if (sp.uid) {
      spec.argv.insert(spec.argv.end(), {"--uid", std::to_string(*sp.uid), "--gid", std::to_string(sp.gid.value_or(*sp.uid))});
    }
    own(spec.argv);
    spec.argv.insert(spec.argv.end(), {"--", sp.worker_binary});
    passes_dirs_ = true;
  } else if (std::string bwrap = TSUGUMI_BWRAP; !bwrap.empty() && ::access(bwrap.c_str(), X_OK) == 0) {
    // sandbox: false lifts the file access limits only. The worker still gets
    // its own PID namespace, so every descendant ends with it, and its own
    // network namespace (no network) unless the Card allows the network.
    spec.argv = {bwrap, "--unshare-user", "--unshare-pid", "--unshare-ipc", "--die-with-parent",
                 "--new-session", "--dev-bind", "/", "/", "--proc", "/proc"};
    if (!sp.network) spec.argv.push_back("--unshare-net");
    if (sp.uid)
      spec.argv.insert(spec.argv.end(), {"--uid", std::to_string(*sp.uid), "--gid", std::to_string(sp.gid.value_or(*sp.uid))});
    // Shared folders (virtiofs) show every file as owned by whoever looks, so
    // file modes cannot keep the token from the Card: the folders are hidden.
    for (const auto& p : sp.hidden_paths) spec.argv.insert(spec.argv.end(), {"--tmpfs", p});
    own(spec.argv);
    spec.argv.insert(spec.argv.end(), {"--", sp.worker_binary});
    passes_dirs_ = true;
  } else {
    // Development hosts without bubblewrap (never the VM): paths as they are.
    spec.argv = {sp.worker_binary};
    spec.uid = sp.uid;
    spec.gid = sp.gid;
  }
  spec.argv.insert(spec.argv.end(), args.begin(), args.end());
  spec.env = {"PATH=/var/empty", "LANG=C.UTF-8"};
  if (sp.network)
    for (const char* v : {"SSL_CERT_FILE", "NIX_SSL_CERT_FILE"})
      if (const char* x = std::getenv(v)) spec.env.push_back(std::string(v) + "=" + x);
  spec.fds = {{3, data[1]}, {4, control[1]}};
  spec.stdout_fd = out[1];
  spec.stderr_fd = err[1];
  try {
    pid_ = proc::spawn(spec);
  } catch (const std::exception& e) {
    for (int fd : {data[0], data[1], control[0], control[1], out[0], out[1], err[0], err[1]}) ::close(fd);
    throw Failed(FailureKind::worker_failed, e.what());
  }
  ::close(data[1]);
  ::close(control[1]);
  ::close(out[1]);
  ::close(err[1]);
  data_fd_ = data[0];
  control_fd_ = control[0];
  out_fd_ = out[0];
  err_fd_ = err[0];
  ::fcntl(out_fd_, F_SETFL, O_NONBLOCK);
  ::fcntl(err_fd_, F_SETFL, O_NONBLOCK);
  for (int fd : {data_fd_, control_fd_, out_fd_, err_fd_}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
  int one = 1;
  ::setsockopt(data_fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
  ::setsockopt(control_fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  alive_ = true;
  log_thread_ = std::thread([this] {
    while (!stop_logs_) {
      pollfd fds[2] = {{out_fd_, POLLIN, 0}, {err_fd_, POLLIN, 0}};
      ::poll(fds, 2, 100);
      read_logs(false);
    }
  });
}

WorkerProc::~WorkerProc() {
  terminate();
  std::error_code ec;
  if (!scratch_.empty()) std::filesystem::remove_all(scratch_, ec);
}

bool WorkerProc::alive() const {
  if (!alive_ || pid_ <= 0) return false;
  siginfo_t si{};
  if (::waitid(P_PID, static_cast<id_t>(pid_), &si, WEXITED | WNOHANG | WNOWAIT) == 0 && si.si_pid == pid_) return false;
  return true;
}

void WorkerProc::set_logs(LogSink logs) {
  read_logs(false);  // what the previous call left goes to its own sink
  std::lock_guard<std::mutex> l(log_mu_);
  logs_ = std::move(logs);
}

void WorkerProc::clear_scratch() {
  std::error_code ec;
  for (const auto& d : {work_dir(), tmp_dir()})
    for (auto it = std::filesystem::directory_iterator(d, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
      std::filesystem::remove_all(it->path(), ec);
}

std::optional<uint64_t> WorkerProc::rss_bytes() const {
  if (pid_ <= 0) return std::nullopt;
#ifdef __APPLE__
  rusage_info_v2 ri{};
  if (proc_pid_rusage(pid_, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&ri)) != 0) return std::nullopt;
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
    std::ifstream ch("/proc/" + std::to_string(p) + "/task/" + std::to_string(p) + "/children");
    for (pid_t c; ch >> c;) todo.push_back(c);
  }
  if (!any) return std::nullopt;
  return total;
#endif
}

void WorkerProc::terminate() {
  if (pid_ > 0) {
    close_fd(data_fd_);
    close_fd(control_fd_);
    // Give a cooperative worker a moment to exit after its data socket closed.
    for (int i = 0; i < 50 && !proc::try_wait(pid_); ++i) ::usleep(10000);
    proc::kill_group(pid_, SIGKILL);
    proc::wait(pid_);
    pid_ = -1;
  }
  stop_logs_ = true;
  if (log_thread_.joinable()) log_thread_.join();
  read_logs(true);
  close_fd(out_fd_);
  close_fd(err_fd_);
  alive_ = false;
}

void WorkerProc::read_logs(bool drain) {
  std::lock_guard<std::mutex> l(log_mu_);
  char buf[16384];
  for (auto [fd, stream] : {std::pair{out_fd_, LogStream::stdout}, std::pair{err_fd_, LogStream::stderr}}) {
    if (fd < 0) continue;
    auto& dec = stream == LogStream::stdout ? out_dec_ : err_dec_;
    while (true) {
      ssize_t r = ::read(fd, buf, sizeof buf);
      if (r <= 0) break;
      std::string text = dec.feed(std::string_view(buf, static_cast<size_t>(r)));
      if (!text.empty() && logs_) logs_(stream, text);
    }
    if (drain) {
      std::string rest = dec.finish();
      if (!rest.empty() && logs_) logs_(stream, rest);
    }
  }
}

void WorkerProc::send(int fd, const std::string& frame, const std::vector<int>& fds) {
  uint32_t len = static_cast<uint32_t>(frame.size());
  std::string buf(reinterpret_cast<const char*>(&len), 4);
  buf += frame;
  size_t off = 0;
  if (!fds.empty()) {
    // The descriptors travel with the frame's first bytes.
    iovec iov{buf.data(), buf.size()};
    std::vector<char> ctrl(CMSG_SPACE(sizeof(int) * fds.size()));
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl.data();
    msg.msg_controllen = static_cast<socklen_t>(ctrl.size());
    cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
    std::memcpy(CMSG_DATA(c), fds.data(), sizeof(int) * fds.size());
    ssize_t n;
    do n = ::sendmsg(fd, &msg, 0);
    while (n < 0 && errno == EINTR);
    if (n <= 0) throw Failed(FailureKind::worker_failed, "worker connection broken");
    off = static_cast<size_t>(n);
  }
  if (!write_full(fd, buf.data() + off, buf.size() - off))
    throw Failed(FailureKind::worker_failed, "worker connection broken");
}

WorkerOutcome WorkerProc::call(const std::string& frame, uint64_t call_id, const std::vector<int>& fds,
                               const std::atomic<bool>* cancel,
                               std::optional<std::chrono::steady_clock::time_point> deadline,
                               const DisplaySink& display) {
  WorkerOutcome outcome;
  try {
    send(data_fd_, frame, fds);
  } catch (const Failed&) {
    terminate();
    outcome.failure = FailureData::make(FailureKind::worker_failed, "the worker exited before the call");
    return outcome;
  }
  std::string rbuf;
  bool cancel_sent = false;
  std::optional<std::chrono::steady_clock::time_point> kill_at;
  bool timed_out = false;
  while (true) {
    pollfd p{data_fd_, POLLIN, 0};
    ::poll(&p, 1, 20);
    auto now = std::chrono::steady_clock::now();
    if (!cancel_sent && ((cancel && cancel->load()) || (deadline && now >= *deadline))) {
      timed_out = !(cancel && cancel->load());
      flatbuffers::FlatBufferBuilder b;
      auto wc = W::CreateWorkerCancel(b);
      W::WorkerMessageBuilder mb(b);
      mb.add_call_id(call_id);
      mb.add_value_type(W::WorkerPayload::WorkerCancel);
      mb.add_value(wc.Union());
      W::FinishWorkerMessageBuffer(b, mb.Finish());
      try {
        send(control_fd_, std::string(reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()));
      } catch (const Failed&) {
      }
      cancel_sent = true;
      kill_at = now + std::chrono::seconds(1);
    }
    if (kill_at && now >= *kill_at) {
      proc::kill_group(pid_, SIGKILL);
      terminate();
      read_logs(true);
      outcome.failure = timed_out ? FailureData::make(FailureKind::timeout, "the Card exceeded timeoutSeconds")
                                  : FailureData::make(FailureKind::cancelled, "the invocation was cancelled");
      return outcome;
    }
    if (!(p.revents & (POLLIN | POLLHUP | POLLERR))) continue;
    char buf[65536];
    ssize_t r = ::read(data_fd_, buf, sizeof buf);
    if (r <= 0) {
      terminate();
      if (cancel_sent)
        outcome.failure = timed_out ? FailureData::make(FailureKind::timeout, "the Card exceeded timeoutSeconds")
                                    : FailureData::make(FailureKind::cancelled, "the invocation was cancelled");
      else outcome.failure = FailureData::make(FailureKind::worker_failed, "the worker exited before returning a result");
      return outcome;
    }
    rbuf.append(buf, static_cast<size_t>(r));
    while (rbuf.size() >= 4) {
      uint32_t len;
      std::memcpy(&len, rbuf.data(), 4);
      if (len == 0 || len > kMaxFrameBytes) {
        terminate();
        outcome.failure = FailureData::make(FailureKind::worker_failed, "corrupt frame from worker");
        return outcome;
      }
      if (rbuf.size() < 4 + size_t(len)) break;
      std::string msg = rbuf.substr(4, len);
      rbuf.erase(0, 4 + size_t(len));
      flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
      if (!W::VerifyWorkerMessageBuffer(v)) {
        terminate();
        outcome.failure = FailureData::make(FailureKind::worker_failed, "worker sent a buffer that does not verify");
        return outcome;
      }
      const auto* m = W::GetWorkerMessage(msg.data());
      if (!m->call_id() || *m->call_id() != call_id) {
        terminate();
        outcome.failure = FailureData::make(FailureKind::worker_failed, "worker answered an unknown call_id");
        return outcome;
      }
      switch (m->value_type()) {
        case W::WorkerPayload::WorkerDisplay:
          try {
            if (display) display(wire::display(m->value_as_WorkerDisplay()->display()));
          } catch (const Failed& f) {
            outcome.failure = f.failure;
          }
          break;
        case W::WorkerPayload::WorkerResult: {
          for (const auto* o : *m->value_as_WorkerResult()->outputs())
            outcome.outputs.emplace_back(o->name()->str(),
                                         std::string(reinterpret_cast<const char*>(o->bytes()->data()), o->bytes()->size()));
          read_logs(false);
          if (cancel_sent && !outcome.failure)
            outcome.failure = timed_out ? FailureData::make(FailureKind::timeout, "the Card exceeded timeoutSeconds")
                                        : FailureData::make(FailureKind::cancelled, "the invocation was cancelled");
          return outcome;
        }
        case W::WorkerPayload::WorkerFailure:
          if (!outcome.failure) outcome.failure = wire::failure(m->value_as_WorkerFailure()->failure());
          read_logs(false);
          return outcome;
        default:
          terminate();
          outcome.failure = FailureData::make(FailureKind::worker_failed, "unexpected message from worker");
          return outcome;
      }
    }
  }
}

std::string worker_call_frame(uint64_t call_id, const std::string& run_id, const CardPath& card,
                              uint64_t invocation, const std::vector<Value>& inputs,
                              const std::string& params_bfbs, const std::string& params_root,
                              const std::string& params, const std::vector<types::PortType>& outputs,
                              const std::string& work, const std::string& tmp, const std::string& out,
                              const std::vector<std::pair<std::string, std::string>>& env,
                              std::optional<uint64_t> deadline_ms, const std::vector<int32_t>& passed_fds) {
  flatbuffers::FlatBufferBuilder b(64 * 1024);
  auto bytes = [&](const std::string& s) { return b.CreateVector(reinterpret_cast<const uint8_t*>(s.data()), s.size()); };
  std::vector<flatbuffers::Offset<W::WorkerInput>> ins;
  for (const auto& v : inputs) {
    auto data = D::CreateFlatBufferData(b, bytes(v.type->bfbs), b.CreateString(v.type->root_type), bytes(v.bytes));
    ins.push_back(W::CreateWorkerInput(b, b.CreateString(v.name), v.kind, data));
  }
  auto insv = b.CreateVector(ins);
  auto pdata = D::CreateFlatBufferData(b, bytes(params_bfbs), b.CreateString(params_root), bytes(params));
  std::vector<flatbuffers::Offset<D::Port>> outs;
  for (const auto& p : outputs) outs.push_back(wire::port(b, p));
  auto outsv = b.CreateVector(outs);
  std::vector<flatbuffers::Offset<W::EnvVar>> ev;
  for (const auto& [k, v] : env) ev.push_back(W::CreateEnvVar(b, b.CreateString(k), b.CreateString(v)));
  auto evv = b.CreateVector(ev);
  auto run = b.CreateString(run_id);
  auto cp = wire::path(b, card);
  auto w = b.CreateString(work), t = b.CreateString(tmp);
  flatbuffers::Offset<flatbuffers::String> o;
  if (!out.empty()) o = b.CreateString(out);
  flatbuffers::Offset<flatbuffers::Vector<int32_t>> pf;
  if (!passed_fds.empty()) pf = b.CreateVector(passed_fds);
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
  if (!passed_fds.empty()) cb.add_passed_fds(pf);
  cb.add_environment(evv);
  if (deadline_ms) cb.add_deadline_ms(*deadline_ms);
  auto call = cb.Finish();
  W::WorkerMessageBuilder mb(b);
  mb.add_call_id(call_id);
  mb.add_value_type(W::WorkerPayload::WorkerCall);
  mb.add_value(call.Union());
  W::FinishWorkerMessageBuffer(b, mb.Finish());
  if (b.GetSize() > kMaxFrameBytes)
    throw Failed(FailureKind::resource_limit, "the worker call exceeds 16 MiB");
  return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize());
}

}  // namespace tsugumi
