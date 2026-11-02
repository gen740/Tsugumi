#include "worker_proc.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "tsugumi/protocol/worker_generated.h"
#include "store.hpp"
#include "util/fs.hpp"
#include "util/text.hpp"
#include "util/log.hpp"
#include "util/process.hpp"
#include "wire.hpp"

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

WorkerProc::WorkerProc(const WorkerSpawn& sp, LogSink logs) : logs_(std::move(logs)) {
  int data[2], control[2], out[2], err[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, data) != 0 || ::socketpair(AF_UNIX, SOCK_STREAM, 0, control) != 0 ||
      ::pipe(out) != 0 || ::pipe(err) != 0)
    throw Failed(FailureKind::worker_failed, "cannot create worker channels");
  // Script files for the steps.
  std::vector<std::string> args;
  auto script = [&](const char* flag, const std::string& text, const char* name) {
    if (text.empty()) return;
    auto path = std::filesystem::path(sp.script_dir) / name;
    fs::write_atomic(path, text, 0644);
    args.push_back(flag);
    args.push_back(path.string());
  };
  std::filesystem::create_directories(sp.script_dir);
  script("--pre", sp.build.pre_script, "pre.sh");
  script("--script", sp.build.script, "script.sh");
  script("--post", sp.build.post_script, "post.sh");
  if (sp.build.package) {
    args.push_back("--package");
    args.push_back(*sp.build.package);
  }
  args.push_back("--bash");
  args.push_back(sp.bash);

  proc::Spec spec;
  if (sp.sandbox) {
    std::string bwrap = TSUGUMI_BWRAP;
    if (bwrap.empty() || ::access(bwrap.c_str(), X_OK) != 0)
      throw Failed(FailureKind::permission_denied,
                   "sandbox: true needs bubblewrap, which this environment does not provide; refusing to run unsandboxed");
    spec.argv = {bwrap, "--unshare-all", "--die-with-parent", "--new-session", "--cap-drop", "ALL",
                 "--ro-bind", "/nix/store", "/nix/store", "--proc", "/proc", "--dev", "/dev",
                 "--tmpfs", "/tmp"};
    if (sp.uid) {
      spec.argv.insert(spec.argv.end(), {"--uid", std::to_string(*sp.uid), "--gid", std::to_string(sp.gid.value_or(*sp.uid))});
    }
    for (const auto& p : sp.ro_paths) spec.argv.insert(spec.argv.end(), {"--ro-bind", p, p});
    for (const auto& p : sp.rw_paths) spec.argv.insert(spec.argv.end(), {"--bind", p, p});
    spec.argv.insert(spec.argv.end(), {"--ro-bind", sp.script_dir, sp.script_dir, "--", sp.worker_binary});
  } else if (std::string bwrap = TSUGUMI_BWRAP; !bwrap.empty() && ::access(bwrap.c_str(), X_OK) == 0) {
    // sandbox: false lifts the file access limits only. The worker still gets
    // its own PID and network namespaces, so every descendant ends with it
    // and the Card has no network.
    spec.argv = {bwrap, "--unshare-user", "--unshare-pid", "--unshare-net", "--unshare-ipc", "--die-with-parent",
                 "--new-session", "--dev-bind", "/", "/", "--proc", "/proc"};
    if (sp.uid)
      spec.argv.insert(spec.argv.end(), {"--uid", std::to_string(*sp.uid), "--gid", std::to_string(sp.gid.value_or(*sp.uid))});
    spec.argv.insert(spec.argv.end(), {"--", sp.worker_binary});
  } else {
    // Development hosts without bubblewrap (never the container image).
    spec.argv = {sp.worker_binary};
    spec.uid = sp.uid;
    spec.gid = sp.gid;
  }
  spec.argv.insert(spec.argv.end(), args.begin(), args.end());
  spec.env = {"PATH=/var/empty", "LANG=C.UTF-8"};
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

WorkerProc::~WorkerProc() { terminate(); }

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

void WorkerProc::send(int fd, const std::string& frame) {
  uint32_t len = static_cast<uint32_t>(frame.size());
  if (!write_full(fd, reinterpret_cast<const char*>(&len), 4) || !write_full(fd, frame.data(), frame.size()))
    throw Failed(FailureKind::worker_failed, "worker connection broken");
}

WorkerOutcome WorkerProc::call(const std::string& frame, uint64_t call_id, const std::atomic<bool>* cancel,
                               std::optional<std::chrono::steady_clock::time_point> deadline,
                               const DisplaySink& display) {
  WorkerOutcome outcome;
  send(data_fd_, frame);
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
                              std::optional<uint64_t> deadline_ms) {
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
  auto w = b.CreateString(work), t = b.CreateString(tmp), o = b.CreateString(out);
  W::WorkerCallBuilder cb(b);
  cb.add_run_id(run);
  cb.add_card(cp);
  cb.add_invocation(invocation);
  cb.add_inputs(insv);
  cb.add_params(pdata);
  cb.add_outputs(outsv);
  cb.add_working_directory(w);
  cb.add_temporary_directory(t);
  cb.add_output_directory(o);
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
