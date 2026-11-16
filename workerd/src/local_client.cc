#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include "tsugumi/workerd/local.hh"
#include "util/process.hh"

namespace tsugumi::workerd {
namespace L = Tsugumi::LocalWorker;

LocalWorkers::LocalWorkers(std::filesystem::path binary,
                           std::filesystem::path scratch)
    : binary_(std::move(binary)), scratch_(std::move(scratch)) {}

LocalWorkers::~LocalWorkers() {
  if (fd_ >= 0) ::close(fd_);  // EOF is the sole lifetime of the local pool.
  if (pid_ > 0) proc::wait(pid_);
}

auto LocalWorkers::exchange(L::MessageT message, LocalPacket& reply) -> void {
  std::lock_guard guard(mutex_);
  try {
    if (fd_ < 0) {
      if (pid_ > 0)
        throw std::runtime_error(
            "local workerd ended with this core's workers");
      int pair[2];
      if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        throw std::runtime_error("cannot create local workerd connection");
      for (int socket : pair) {
        ::fcntl(socket, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
        int one = 1;
        ::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
      }
      proc::Spec spec;
      spec.argv = {binary_.string(), "--local", "3", scratch_.string()};
      spec.fds = {{3, pair[1]}};
      spec.stderr_fd = STDERR_FILENO;
      for (const auto* variable : {"SSL_CERT_FILE", "NIX_SSL_CERT_FILE"})
        if (const auto* value = std::getenv(variable))
          spec.env.push_back(std::string(variable) + "=" + value);
      try {
        pid_ = proc::spawn(spec);
      } catch (...) {
        ::close(pair[0]);
        ::close(pair[1]);
        throw;
      }
      ::close(pair[1]);
      fd_ = pair[0];
    }
    send_local(fd_, message);
    if (!receive_local(fd_, reply))
      throw std::runtime_error("local workerd exited");
    if (!reply.message->error.empty())
      throw Failed(reply.message->error_kind, reply.message->error);
  } catch (const Failed&) {
    throw;
  } catch (const std::exception& error) {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    throw Failed(FailureKind::worker_failed, error.what());
  }
}

auto LocalWorkers::command(L::Action action, uint64_t lease, bool force)
    -> void {
  L::MessageT request;
  request.action = action;
  request.lease = lease;
  request.force = force;
  LocalPacket reply;
  exchange(std::move(request), reply);
}

auto LocalWorkers::acquire(const std::string& card, const std::string& key,
                           const WorkerSpawn& spawn, bool reuse)
    -> WorkerPool::Lease {
  L::MessageT request;
  request.action = L::Action::Acquire;
  request.card = card;
  request.key = key;
  request.reuse = reuse;
  auto& target = request.spawn = std::make_unique<L::SpawnT>();
  target->worker_binary = spawn.worker_binary;
  target->bash = spawn.bash;
  target->store_path = spawn.store_path;
  target->package = spawn.program.package.value_or("");
  target->script = spawn.program.bash.value_or("");
  target->has_script = spawn.program.bash.has_value();
  target->pre_run = spawn.program.pre_run;
  target->post_run = spawn.program.post_run;
  target->views = spawn.program.views;
  target->sandbox = spawn.sandbox;
  target->network = spawn.network;
  target->ro_paths = spawn.ro_paths;
  target->hidden_paths = spawn.hidden_paths;
  if (spawn.uid) target->uid = *spawn.uid;
  if (spawn.gid) target->gid = *spawn.gid;
  for (const auto& bind : spawn.binds) {
    auto value = std::make_unique<L::BindT>();
    value->source = bind.source;
    value->target = bind.target;
    value->write = bind.write;
    target->binds.push_back(std::move(value));
  }
  LocalPacket reply;
  exchange(std::move(request), reply);
  const auto& message = *reply.message;
  if (!message.lease || message.scratch.empty() || reply.descriptors[3] < 0)
    throw Failed(FailureKind::worker_failed,
                 "workerd did not hand over a worker connection");
  WorkerProc::Borrowed borrowed{
      reply.descriptors,
      message.scratch,
      message.call_id,
      message.lease,
      [this, id = message.lease] { command(L::Action::ClearScratch, id); },
      [this, id = message.lease](bool force) {
        command(L::Action::Terminate, id, force);
      },
      [this, id = message.lease] { command(L::Action::Release, id); }};
  std::unique_ptr<WorkerProc> worker;
  try {
    worker = std::make_unique<WorkerProc>(std::move(borrowed));
  } catch (...) {
    try {
      command(L::Action::Release, message.lease);
    } catch (...) {
    }
    throw;
  }
  reply.descriptors.fill(-1);
  return {std::move(worker), message.reused, message.replaced};
}

auto LocalWorkers::release(std::unique_ptr<WorkerProc> worker, bool keep,
                           const std::string& reason) -> void {
  if (!worker) return;
  L::MessageT request;
  request.action = L::Action::Release;
  request.lease = worker->lease_id();
  request.call_id = worker->call_id();
  request.keep = keep;
  request.reason = reason;
  worker->returning_lease();
  worker.reset();  // No concurrent log readers when workerd resumes ownership.
  LocalPacket reply;
  exchange(std::move(request), reply);
}

auto LocalWorkers::clear() -> void {
  // Clearing an unused pool must not start a service.
  {
    std::lock_guard guard(mutex_);
    if (fd_ < 0) return;
  }
  command(L::Action::Clear, 0);
}
}  // namespace tsugumi::workerd
