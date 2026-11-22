#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include "tsugumi/rpc/trace.hh"
#include "tsugumi/util/process.hh"
#include "tsugumi/workerd/local.hh"

namespace tsugumi::workerd {
namespace L = Tsugumi::LocalWorker;

LocalWorkers::LocalWorkers(std::filesystem::path binary,
                           std::filesystem::path scratch)
    : binary_(std::move(binary)), scratch_(std::move(scratch)) {}

LocalWorkers::~LocalWorkers() {
  idle_.clear();
  if (fd_ >= 0) ::close(fd_);  // EOF is the sole lifetime of the local pool.
  if (pid_ > 0) proc::wait(pid_);
}

auto LocalWorkers::exchange(L::MessageT message, LocalPacket& reply) -> void {
  struct Wipe {
    L::MessageT& message;
    ~Wipe() { clear_spawn_secrets(message); }
  } wipe{message};
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
      for (const auto* name : {"DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR",
                               "TSUGUMI_CARD_CGROUP_ROOT"})
        if (const auto* value = std::getenv(name))
          spec.env.push_back(std::string(name) + "=" + value);
      if (trace::LaunchEnabled())
        spec.env.push_back(std::string(trace::kLaunchVariable) + "=1");
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
    trace::Link link;
    send_local(fd_, message, nullptr, &link);
    clear_spawn_secrets(message);
    if (!receive_local(fd_, reply, &link))
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
  target->path = spawn.path;
  target->closure = spawn.closure;
  target->shared_vm = spawn.shared_vm;
  if (spawn.secrets) {
    for (const auto& [name, value] : spawn.secrets->values) {
      auto entry = std::make_unique<Tsugumi::Worker::EnvVarT>();
      entry->name = name;
      entry->value = value;
      target->secrets.push_back(std::move(entry));
    }
  }
  target->store_path = spawn.store_path;
  target->package = spawn.package;
  target->filesystem = spawn.sandbox.filesystem;
  target->pid = spawn.sandbox.pid;
  target->ipc = spawn.sandbox.ipc;
  target->identity = spawn.sandbox.identity;
  target->cpu_millis = spawn.sandbox.limits.cpu_millis;
  target->memory_bytes = spawn.sandbox.limits.memory_bytes;
  target->gui = spawn.gui;
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
    value->name = bind.name;
    target->binds.push_back(std::move(value));
  }
  std::lock_guard leases(leases_mutex_);
  LocalPacket reply;
  exchange(std::move(request), reply);
  std::erase_if(idle_,
                [](const auto& entry) { return !entry.second->alive(); });
  const auto& message = *reply.message;
  if (!message.lease || message.scratch.empty() || reply.descriptors[3] < 0)
    throw Failed(FailureKind::worker_failed,
                 "workerd did not hand over a worker connection");
  WorkerProc::Borrowed borrowed{
      reply.descriptors,
      message.scratch,
      message.call_id,
      message.lease,
      message.read_only_inputs,
      [this, id = message.lease] { command(L::Action::ClearScratch, id); },
      [this, id = message.lease](bool force) {
        command(L::Action::Terminate, id, force);
      },
      [this, id = message.lease] { command(L::Action::Release, id); }};
  std::unique_ptr<WorkerProc> worker;
  try {
    if (auto found = idle_.extract(message.scratch); found) {
      worker = std::move(found.mapped());
      worker->resume(std::move(borrowed));
    } else {
      worker = std::make_unique<WorkerProc>(std::move(borrowed));
      reply.descriptors.fill(-1);
    }
  } catch (...) {
    try {
      command(L::Action::Release, message.lease);
    } catch (...) {
    }
    throw;
  }
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
  std::lock_guard leases(leases_mutex_);
  worker->idle();
  if (!keep) worker.reset();
  LocalPacket reply;
  exchange(std::move(request), reply);
  std::erase_if(idle_,
                [](const auto& entry) { return !entry.second->alive(); });
  if (worker && worker->alive()) {
    const auto scratch = worker->scratch();
    idle_.emplace(scratch, std::move(worker));
  }
}

auto LocalWorkers::clear() -> void {
  std::lock_guard leases(leases_mutex_);
  // Clearing an unused pool must not start a service.
  {
    std::lock_guard guard(mutex_);
    if (fd_ < 0) return;
  }
  command(L::Action::Clear, 0);
  idle_.clear();
}
auto LocalWorkers::invalidate(const std::string& card,
                              const std::string& reason) -> void {
  std::lock_guard leases(leases_mutex_);
  {
    std::lock_guard guard(mutex_);
    if (fd_ < 0) return;
  }
  L::MessageT request;
  request.action = L::Action::Invalidate;
  request.card = card;
  request.reason = reason;
  LocalPacket reply;
  exchange(std::move(request), reply);
  std::erase_if(idle_,
                [](const auto& entry) { return !entry.second->alive(); });
}

}  // namespace tsugumi::workerd
