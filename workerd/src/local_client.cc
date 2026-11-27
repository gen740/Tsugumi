#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <utility>

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
  for (const auto& channel : channels_)
    if (channel.fd >= 0) ::close(channel.fd);
  if (pid_ > 0) proc::wait(pid_);
}

auto LocalWorkers::connect() -> void {
  std::lock_guard guard(start_mutex_);
  if (pid_ > 0) return;
  struct Sockets {
    std::array<std::array<int, 2>, kLocalWorkerChannels> pairs;
    Sockets() { pairs.fill({-1, -1}); }
    ~Sockets() {
      for (const auto& pair : pairs)
        for (const int fd : pair)
          if (fd >= 0) ::close(fd);
    }
  } sockets;
  proc::Spec spec;
  spec.argv = {binary_.string(), "--local", scratch_.string()};
  for (size_t i = 0; i < sockets.pairs.size(); ++i) {
    auto& pair = sockets.pairs[i];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()) != 0)
      throw std::runtime_error("cannot create local workerd connection");
    for (const int socket : pair) {
      ::fcntl(socket, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
      int one = 1;
      ::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    }
    const auto fd = static_cast<int>(3 + i);
    spec.argv.push_back(std::to_string(fd));
    spec.fds.emplace_back(fd, pair[1]);
  }
  spec.stderr_fd = STDERR_FILENO;
  for (const auto* name : {"DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR",
                           "TSUGUMI_CARD_CGROUP_ROOT"})
    if (const auto* value = std::getenv(name))
      spec.env.push_back(std::string(name) + "=" + value);
  if (trace::LaunchEnabled())
    spec.env.push_back(std::string(trace::kLaunchVariable) + "=1");
  pid_ = proc::spawn(spec);
  for (size_t i = 0; i < channels_.size(); ++i)
    channels_[i].fd = std::exchange(sockets.pairs[i][0], -1);
}

auto LocalWorkers::started() -> bool {
  std::lock_guard guard(start_mutex_);
  return pid_ > 0;
}

auto LocalWorkers::exchange(size_t index, L::MessageT message,
                            LocalPacket& reply) -> void {
  struct Wipe {
    L::MessageT& message;
    ~Wipe() { clear_spawn_secrets(message); }
  } wipe{message};
  try {
    connect();
  } catch (const std::exception& error) {
    throw Failed(FailureKind::worker_failed, error.what());
  }
  auto& channel = channels_[index];
  std::lock_guard guard(channel.mutex);
  if (channel.fd < 0)
    throw Failed(FailureKind::worker_failed, "local workerd connection ended");
  try {
    trace::Link link;
    send_local(channel.fd, message, nullptr, &link);
    clear_spawn_secrets(message);
    if (!receive_local(channel.fd, reply, &link))
      throw std::runtime_error("local workerd exited");
    if (!reply.message->error.empty())
      throw Failed(reply.message->error_kind, reply.message->error);
  } catch (const Failed&) {
    throw;
  } catch (const std::exception& error) {
    ::close(std::exchange(channel.fd, -1));
    throw Failed(FailureKind::worker_failed, error.what());
  }
}

auto LocalWorkers::command(size_t channel, L::Action action, uint64_t lease,
                           bool force) -> void {
  L::MessageT request;
  request.action = action;
  request.lease = lease;
  request.force = force;
  LocalPacket reply;
  exchange(channel, std::move(request), reply);
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
  target->closure = spawn.closure;
  target->shared_vm = spawn.shared_vm;
  target->packed = spawn.packed;
  target->packed_debug = spawn.packed_debug;
  target->card_probe = spawn.card_probe;
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
  target->artifact_store = spawn.artifact_store;
  target->artifact_staging = spawn.artifact_staging.string();
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
  const size_t channel =
      reuse ? 0
            : 1 + next_fresh_channel_.fetch_add(1, std::memory_order_relaxed) %
                      (kLocalWorkerChannels - 1);
  std::unique_lock leases(leases_mutex_, std::defer_lock);
  if (reuse) leases.lock();
  LocalPacket reply;
  exchange(channel, std::move(request), reply);
  if (reuse)
    std::erase_if(idle_,
                  [](const auto& entry) { return !entry.second->alive(); });
  const auto& message = *reply.message;
  if (!message.lease || (message.lease - 1) % kLocalWorkerChannels != channel ||
      message.scratch.empty() || reply.descriptors[3] < 0)
    throw Failed(FailureKind::worker_failed,
                 "workerd did not hand over a worker connection");
  WorkerProc::Borrowed borrowed{
      reply.descriptors,
      message.scratch,
      message.artifact_scratch,
      message.call_id,
      message.lease,
      [this, channel, id = message.lease] {
        command(channel, L::Action::ClearScratch, id);
      },
      [this, channel, id = message.lease](bool force) {
        command(channel, L::Action::Terminate, id, force);
      },
      [this, channel, id = message.lease] {
        command(channel, L::Action::Release, id);
      },
      spawn.packed};
  std::unique_ptr<WorkerProc> worker;
  try {
    if (reuse) {
      auto found = idle_.extract(message.scratch);
      if (found) {
        worker = std::move(found.mapped());
        worker->resume(std::move(borrowed));
      }
    }
    if (!worker) {
      worker = std::make_unique<WorkerProc>(std::move(borrowed));
      reply.descriptors.fill(-1);
    }
  } catch (...) {
    try {
      command(channel, L::Action::Release, message.lease);
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
  const size_t channel = (request.lease - 1) % kLocalWorkerChannels;
  request.keep = channel == 0 && keep;
  request.reason = reason;
  std::unique_lock leases(leases_mutex_, std::defer_lock);
  if (channel == 0) leases.lock();
  worker->idle();
  if (!request.keep) worker.reset();
  LocalPacket reply;
  exchange(channel, std::move(request), reply);
  if (channel != 0) return;
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
  if (!started()) return;
  command(0, L::Action::Clear, 0);
  idle_.clear();
}
auto LocalWorkers::invalidate(const std::string& card,
                              const std::string& reason) -> void {
  std::lock_guard leases(leases_mutex_);
  if (!started()) return;
  L::MessageT request;
  request.action = L::Action::Invalidate;
  request.card = card;
  request.reason = reason;
  LocalPacket reply;
  exchange(0, std::move(request), reply);
  std::erase_if(idle_,
                [](const auto& entry) { return !entry.second->alive(); });
}

}  // namespace tsugumi::workerd
