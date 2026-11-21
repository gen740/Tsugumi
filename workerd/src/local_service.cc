#include <map>
#include <stdexcept>

#include "tsugumi/workerd/local.hh"

namespace tsugumi::workerd {
namespace L = Tsugumi::LocalWorker;

auto serve_local(int fd, const std::filesystem::path& scratch) -> void {
  WorkerPool pool(scratch);
  struct Held {
    std::string card;
    std::string key;
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
          spawn.bash = source.bash;
          spawn.path = source.path;
          if (!source.secrets.empty()) {
            spawn.secrets = std::make_shared<WorkerSecrets>();
            for (auto& entry : source.secrets)
              spawn.secrets->values.emplace_back(std::move(entry->name),
                                                 std::move(entry->value));
            clear_spawn_secrets(*packet.message);
          }
          spawn.store_path = source.store_path;
          if (!source.package.empty()) spawn.program.package = source.package;
          if (source.has_script) spawn.program.bash = source.script;
          spawn.program.pre_run = source.pre_run;
          spawn.program.post_run = source.post_run;
          spawn.program.views = source.views;
          spawn.sandbox = {
              source.filesystem,
              source.pid,
              source.ipc,
              source.identity,
              {static_cast<uint32_t>(source.cpu_millis), source.memory_bytes}};
          spawn.gui = source.gui;
          spawn.network = source.network;
          spawn.ro_paths = source.ro_paths;
          spawn.hidden_paths = source.hidden_paths;
          spawn.card = request.card;
          if (source.uid) spawn.uid = *source.uid;
          if (source.gid) spawn.gid = *source.gid;
          for (const auto& bind : source.binds)
            spawn.binds.push_back({bind->source, bind->target, bind->write});
          auto lease =
              pool.acquire(request.card, request.key, spawn, request.reuse);
          reply.lease = ++sequence;
          reply.reused = lease.reused;
          reply.replaced = lease.replaced;
          reply.scratch = std::filesystem::path(lease.worker->work_dir())
                              .parent_path()
                              .string();
          reply.call_id = lease.worker->call_id();
          descriptors = lease.worker->hand_over();
          held.emplace(reply.lease, Held{request.card, request.key,
                                         std::move(lease.worker)});
          break;
        }
        case L::Action::Clear:
          pool.clear(false);
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
            pool.release(it->second.card, it->second.key, std::move(worker),
                         request.keep, request.reason);
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
