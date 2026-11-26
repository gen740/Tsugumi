#include <map>
#include <stdexcept>

#include "tsugumi/workerd/local.hh"

namespace tsugumi::workerd {
namespace L = Tsugumi::LocalWorker;

auto serve_local(int fd, const std::filesystem::path& scratch) -> void {
  WorkerPool pool(scratch);
  struct Held {
    std::string card;
    uint64_t generation;
    std::unique_ptr<WorkerProc> worker;
  };
  std::map<uint64_t, Held> held;
  uint64_t sequence = 0;
  for (;;) {
    LocalPacket packet;
    if (!receive_local(fd, packet)) return;
    const auto& request = *packet.message;
    L::MessageT reply;
    std::optional<std::array<int, 4>> descriptors;
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
          reply.lease = ++sequence;
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
}  // namespace tsugumi::workerd
