#include <sys/socket.h>

#include <cstdio>
#include <map>
#include <stdexcept>
#include <thread>

#include "tsugumi/workerd/local.hh"

namespace tsugumi::workerd {
namespace L = Tsugumi::LocalWorker;

namespace {
auto serve_connection(int fd, WorkerPool& pool, size_t channel) -> void {
  struct Held {
    std::string card;
    uint64_t generation;
    std::unique_ptr<WorkerProc> worker;
  };
  std::map<uint64_t, Held> held;
  // The residue of a lease identifies its connection, so later commands
  // reach the same held lease even when the caller moves between threads.
  uint64_t next_lease = channel + 1;
  for (;;) {
    LocalPacket packet;
    if (!receive_local(fd, packet)) return;
    const auto& request = *packet.message;
    L::MessageT reply;
    std::optional<WorkerProc::Descriptors> descriptors;
    try {
      switch (request.action) {
        case L::Action::Acquire: {
          if (!request.spawn)
            throw std::runtime_error("missing worker spawn specification");
          auto& source = *request.spawn;
          WorkerSpawn spawn;
          spawn.worker_binary = source.worker_binary;
          spawn.closure = source.closure;
          spawn.shared_vm = source.shared_vm;
          spawn.packed = source.packed;
          spawn.packed_debug = source.packed_debug;
          spawn.card_probe = source.card_probe;
          if (!source.secrets.empty()) {
            spawn.secrets = std::make_shared<WorkerSecrets>();
            spawn.secrets->values.reserve(source.secrets.size());
            for (auto& entry : source.secrets)
              spawn.secrets->values.emplace_back(std::move(entry->name),
                                                 entry->value);
            clear_spawn_secrets(*packet.message);
          }
          spawn.store_path = source.store_path;
          spawn.package = source.package;
          spawn.channels_borrowed = true;
          spawn.sandbox = {
              source.filesystem,
              source.pid,
              source.ipc,
              source.identity,
              {static_cast<uint32_t>(source.cpu_millis), source.memory_bytes}};
          spawn.gui = source.gui;
          spawn.network = source.network;
          spawn.ro_paths = source.ro_paths;
          spawn.artifact_store = source.artifact_store;
          spawn.artifact_staging = source.artifact_staging;
          spawn.hidden_paths = source.hidden_paths;
          if (source.uid) spawn.uid = *source.uid;
          if (source.gid) spawn.gid = *source.gid;
          for (const auto& bind : source.binds)
            spawn.binds.push_back(
                {bind->source, bind->target, bind->write, bind->name});
          auto lease =
              pool.acquire(request.card, request.key, spawn, request.reuse);
          reply.lease = next_lease;
          next_lease += kLocalWorkerChannels;
          reply.reused = lease.reused;
          reply.replaced = lease.replaced;
          reply.scratch = std::filesystem::path(lease.worker->work_dir())
                              .parent_path()
                              .string();
          reply.call_id = lease.worker->call_id();
          reply.artifact_scratch = lease.worker->artifact_scratch().string();
          try {
            descriptors = lease.worker->hand_over();
            held.emplace(reply.lease, Held{request.card, lease.generation,
                                           std::move(lease.worker)});
          } catch (...) {
            pool.release(request.card, lease.generation,
                         std::move(lease.worker), false);
            throw;
          }
          break;
        }
        case L::Action::Invalidate:
          pool.invalidate(request.card, request.reason);
          break;
        case L::Action::Clear:
          pool.clear();
          break;
        case L::Action::Release:
        case L::Action::ClearScratch:
        case L::Action::Terminate: {
          const auto it = held.find(request.lease);
          if (it == held.end())
            throw std::runtime_error("unknown worker lease");
          auto& worker = it->second.worker;
          if (request.action == L::Action::ClearScratch)
            worker->clear_scratch();
          else if (request.action == L::Action::Terminate)
            worker->terminate(request.force);
          else {
            worker->set_call_id(request.call_id);
            if (request.keep) worker->resume_logs();
            pool.release(it->second.card, it->second.generation,
                         std::move(worker), request.keep, request.reason);
            held.erase(it);
          }
          break;
        }
      }
    } catch (const Failed& error) {
      reply.error = error.what();
      reply.error_kind = error.failure.kind;
    } catch (const std::exception& error) {
      reply.error = error.what();
    }
    send_local(fd, reply, descriptors ? &*descriptors : nullptr);
  }
}
}  // namespace

auto serve_local(std::span<const int> fds, const std::filesystem::path& scratch)
    -> void {
  WorkerPool pool(scratch, {}, fds.size() > 1 ? fds.size() : 0);
  std::vector<std::jthread> receivers;
  for (size_t channel = 1; channel < fds.size(); ++channel) {
    const int fd = fds[channel];
    receivers.emplace_back([&pool, fd, channel](std::stop_token stop) {
      const auto disconnect = [fd] { ::shutdown(fd, SHUT_RDWR); };
      std::stop_callback wake(stop, disconnect);
      try {
        serve_connection(fd, pool, channel);
      } catch (const std::exception& error) {
        std::fprintf(stderr, "tsugumi-workerd: %s\n", error.what());
      }
      disconnect();
    });
  }
  serve_connection(fds.front(), pool, 0);
}
}  // namespace tsugumi::workerd
