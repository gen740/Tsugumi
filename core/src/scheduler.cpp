#include "scheduler.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <deque>
#include <flatbuffers/reflection.h>
#include <set>

#include "history.hpp"
#include "schema.hpp"
#include "nix.hpp"
#include "service.hpp"
#include "util/crypto.hpp"
#include "util/fs.hpp"
#include "util/log.hpp"
#include "wire.hpp"
#include "worker_proc.hpp"

#ifndef TSUGUMI_CARD_PATH
#define TSUGUMI_CARD_PATH "/usr/bin:/bin"
#endif
#ifndef TSUGUMI_BASH
#define TSUGUMI_BASH "/bin/bash"
#endif

namespace tsugumi {

namespace R = Tsugumi::RPC;
namespace D = Tsugumi::Data;
namespace stdfs = std::filesystem;
using FBB = flatbuffers::FlatBufferBuilder;

namespace {

constexpr size_t kQueueCapacity = 16;
constexpr uid_t kCardUid = 65534;  // unprivileged identity for Card processes

Note run_event_note(const std::string& run_id,
                    std::function<std::pair<R::RunEventValue, flatbuffers::Offset<void>>(FBB&)> body) {
  Note n;
  n.kind = Note::Kind::Run;
  n.approx_bytes = 512;
  n.build = std::make_shared<const Note::Build>([run_id, body](FBB& b) {
    auto [type, value] = body(b);
    auto ev = R::CreateRunEvent(b, b.CreateString(run_id), type, value);
    return std::make_pair(R::NotificationPayload::RunEvent, ev.Union());
  });
  return n;
}

Note status_note(const RunStatusData& s, bool accepted) {
  return run_event_note(*s.run_id, [s, accepted](FBB& b) {
    auto st = wire::run_status(b, s);
    if (accepted) return std::make_pair(R::RunEventValue::RunAcceptedEvent, R::CreateRunAcceptedEvent(b, st).Union());
    return std::make_pair(R::RunEventValue::RunStatusChanged, R::CreateRunStatusChanged(b, st).Union());
  });
}

Note card_note(const InvocationSummaryData& s) {
  bool final = s.state == CardState::succeeded || s.state == CardState::failed || s.state == CardState::cancelled ||
               s.state == CardState::skipped || s.state == CardState::blocked;
  Note n = run_event_note(s.run_id, [s](FBB& b) {
    auto inv = wire::invocation(b, s);
    return std::make_pair(R::RunEventValue::RunCardChanged, R::CreateRunCardChanged(b, inv).Union());
  });
  // Progress of one invocation may be collapsed to its latest state; final states never are.
  if (!final) n.coalesce_key = "card|" + s.run_id + "|" + path_key(s.card) + "|" + std::to_string(s.invocation);
  return n;
}

Note timeline_note(const std::string& run_id, const CardPath& card, uint64_t inv, Phase phase, uint32_t lane,
                   int64_t start, std::optional<int64_t> end) {
  Note n = run_event_note(run_id, [=](FBB& b) {
    auto cp = wire::path(b, card);
    R::RunTimelineBuilder tb(b);
    tb.add_card(cp);
    tb.add_invocation(inv);
    tb.add_phase(phase);
    tb.add_lane(lane);
    tb.add_start(start);
    if (end) tb.add_end(*end);
    return std::make_pair(R::RunEventValue::RunTimeline, tb.Finish().Union());
  });
  n.coalesce_key = "timeline|" + run_id + "|" + path_key(card) + "|" + std::to_string(int(phase));
  return n;
}

Note finished_note(const RunSummaryData& s) {
  return run_event_note(*s.status.run_id, [s](FBB& b) {
    auto sum = wire::run_summary(b, s);
    return std::make_pair(R::RunEventValue::RunFinished, R::CreateRunFinished(b, sum).Union());
  });
}

Note log_note(const LogEnd& e) {
  Note n;
  n.kind = Note::Kind::Log;
  n.coalesce_key = "log|" + e.run_id + "|" + (e.card ? path_key(*e.card) : "") + "|" +
                   (e.invocation ? std::to_string(*e.invocation) : "") + "|" + std::to_string(int(e.stream));
  n.approx_bytes = 256;
  n.build = std::make_shared<const Note::Build>([e](FBB& b) {
    auto run = b.CreateString(e.run_id);
    auto card = e.card ? wire::path(b, *e.card) : flatbuffers::Offset<D::CardPath>();
    R::LogAvailableBuilder lb(b);
    lb.add_run_id(run);
    if (e.card) lb.add_card(card);
    if (e.invocation) lb.add_invocation(*e.invocation);
    lb.add_stream(e.stream);
    lb.add_end_offset(e.end_offset);
    return std::make_pair(R::NotificationPayload::LogAvailable, lb.Finish().Union());
  });
  return n;
}

std::string journal_line(const char* phase, const char* event, const std::optional<CardPath>& card,
                         std::optional<uint64_t> inv, const std::string& message) {
  if (card) {
    schema::CardJournalLine l;
    *l.time_ms = now_ms();
    *l.phase = phase;
    *l.event = event;
    *l.card = *card;
    *l.invocation = inv.value_or(0);
    *l.message = message;
    return schema::encode(l) + "\n";
  }
  schema::RunJournalLine l;
  *l.time_ms = now_ms();
  *l.phase = phase;
  *l.event = event;
  *l.message = message;
  return schema::encode(l) + "\n";
}

const char* phase_name(Phase p) {
  switch (p) {
    case Phase::evaluate: return "evaluate";
    case Phase::build: return "build";
    case Phase::execute: return "execute";
    case Phase::validate: return "validate";
    default: return "run";
  }
}

// Reads the string `value` field of a File/Directory message.
std::optional<std::string> path_value(const Value& v) {
  const auto* root = v.type->root();
  const auto* f = root->fields()->LookupByKey("value");
  if (!f) return std::nullopt;
  const auto* t = flatbuffers::GetAnyRoot(reinterpret_cast<const uint8_t*>(v.bytes.data()));
  const auto* s = flatbuffers::GetFieldS(*t, *f);
  if (!s) return std::nullopt;
  return s->str();
}

}  // namespace

Note display_note(const DisplayData& d) {
  auto shared = std::make_shared<DisplayData>(d);
  Note n;
  n.kind = Note::Kind::Display;
  n.card_key = path_key(d.card);
  n.coalesce_key = "display|" + n.card_key;
  n.approx_bytes = 1024;
  if (const auto* c = std::get_if<DisplayData::LineChart>(&d.value))
    for (const auto& s : c->series) n.approx_bytes += s.x.size() * 16;
  n.build = std::make_shared<const Note::Build>([shared](FBB& b) {
    auto dv = wire::display(b, *shared);
    return std::make_pair(R::NotificationPayload::DisplayUpdated, R::CreateDisplayUpdated(b, dv).Union());
  });
  return n;
}

// ---------------------------------------------------------------------------

class RunExec : public std::enable_shared_from_this<RunExec> {
 public:
  RunExec(Service& svc, Scheduler& sched, DocumentPtr doc, RunParams p, RunSummaryData summary)
      : svc_(svc), sched_(sched), doc_(std::move(doc)), p_(std::move(p)), summary_(std::move(summary)) {
    run_id_ = *summary_.status.run_id;
    run_dir_ = svc_.store().state_dir() / "runs" / run_id_;
  }

  void start() {
    auto self = shared_from_this();
    thread_ = std::thread([self] { self->main(); });
  }
  void join() {
    if (thread_.joinable()) thread_.join();
  }

  RunStatusData status() {
    std::lock_guard<std::mutex> l(mu_);
    return summary_.status;
  }

  RunStatusData request_cancel(bool interrupted) {
    std::lock_guard<std::mutex> l(mu_);
    if (!summary_.status.terminal() && !cancel_) {
      cancel_ = true;
      interrupted_ = interrupted;
      summary_.status.state = RunState::cancelling;
      persist_status_locked();
    }
    cv_.notify_all();
    return summary_.status;
  }

 private:
  struct Token {
    Value value;
    CardPath from;
    uint64_t from_inv = 0;
    uint64_t seq = 0;
  };
  struct NodeRt {
    const Node* node = nullptr;
    size_t idx = 0;
    bool in_targets = false;
    bool start = false;
    bool start_fired = false;
    std::map<std::string, std::deque<Token>> queues;  // connected inputs only
    uint64_t invocations = 0;
    bool running = false;
    bool halted = false;  // failed hard (e.g. maxRuns) and no longer scheduled
    CardState last = CardState::not_started;
    std::optional<FailureData> last_failure;
    std::optional<CardBuild> build;
    std::unique_ptr<WorkerProc> worker;
    uint64_t call_id = 0;
    uint64_t display_rev = 0;
    std::optional<DisplayData> display;
  };
  struct Work {
    size_t node;
    uint64_t invocation;
    std::vector<Token> inputs;
  };

  // ---- persistence helpers -------------------------------------------------

  void persist_status_locked() {
    auto s = summary_.status;
    svc_.history().submit([s](sql::Db& db) { History::update_run(db, s); },
                          [this, s](bool) { svc_.server()->broadcast(status_note(s, false)); });
  }

  void set_state(RunState st) {
    std::lock_guard<std::mutex> l(mu_);
    if (summary_.status.state == st || summary_.status.state == RunState::cancelling) return;
    summary_.status.state = st;
    if (st == RunState::running && !summary_.status.started_at) summary_.status.started_at = now_ms();
    persist_status_locked();
  }

  void card_status(const InvocationSummaryData& s) {
    svc_.history().submit([s](sql::Db& db) { History::put_status(db, s); },
                          [this, s](bool) { svc_.server()->broadcast(card_note(s)); });
  }

  void journal(const char* phase, const char* event, const std::optional<CardPath>& card,
               std::optional<uint64_t> inv, const std::string& message) {
    std::string line = journal_line(phase, event, card, inv, message);
    auto ends = std::make_shared<std::vector<LogEnd>>();
    std::string run = run_id_;
    svc_.history().submit(
        [this, line, card, inv, run, ends](sql::Db& db) {
          if (card) *ends = svc_.history().append_log(db, run, *card, *inv, LogStream::journal, line);
          *ends = card ? *ends : svc_.history().append_run_journal(db, run, line);
        },
        [this, ends](bool ok) {
          if (ok)
            for (const auto& e : *ends) svc_.server()->broadcast(log_note(e));
        });
  }

  void card_log(const CardPath& card, uint64_t inv, LogStream stream, const std::string& text) {
    auto ends = std::make_shared<std::vector<LogEnd>>();
    std::string run = run_id_;
    svc_.history().submit(
        [this, card, inv, stream, text, run, ends](sql::Db& db) {
          *ends = svc_.history().append_log(db, run, card, inv, stream, text);
        },
        [this, ends](bool ok) {
          if (ok)
            for (const auto& e : *ends) svc_.server()->broadcast(log_note(e));
        });
  }

  // ---- setup ----------------------------------------------------------------

  void prepare_graph() {
    const auto& res = *doc_->resolution;
    nodes_.resize(res.nodes.size());
    for (size_t i = 0; i < res.nodes.size(); ++i) {
      nodes_[i].node = &res.nodes[i];
      nodes_[i].idx = i;
    }
    for (const auto& e : res.edges) {
      out_edges_[{e.from, e.from_port}].push_back({e.to, e.to_port});
      nodes_[e.to].queues[e.to_port];
    }
    auto plan = Scheduler::plan(res, p_.card, p_.propagate);
    for (const auto& t : plan.targets) nodes_[res.index.at(path_key(t))].in_targets = true;
    for (const auto& s : plan.starts) nodes_[res.index.at(path_key(s))].start = true;
  }

  // ---- scheduling -------------------------------------------------------------

  bool downstream_has_space(const NodeRt& n) {
    for (const auto& out : n.node->outputs) {
      auto it = out_edges_.find({n.idx, out.name});
      if (it == out_edges_.end()) continue;
      for (const auto& [to, port] : it->second)
        if (nodes_[to].in_targets && nodes_[to].queues[port].size() >= kQueueCapacity) return false;
    }
    return true;
  }

  std::optional<Work> take_ready(NodeRt& n) {
    if (!n.in_targets || n.running || n.halted || n.node->is_virtual) return std::nullopt;
    Work w{n.idx, 0, {}};
    if (n.start && !n.start_fired) {
      if (!downstream_has_space(n)) return std::nullopt;
      n.start_fired = true;
      if (!n.queues.empty() || !n.node->inputs.empty()) {
        // A start Card with inputs runs on its last saved inputs.
        auto last = svc_.history().read([&](sql::Db& db) { return History::last_input(db, n.node->path); });
        if (!last) {
          start_failure_ = FailureData::make(FailureKind::input_missing,
                                             "no saved inputs for " + path_key(n.node->path), {{"card", path_key(n.node->path)}});
          n.halted = true;
          n.last = CardState::failed;
          n.last_failure = start_failure_;
          return std::nullopt;
        }
        for (auto& v : last->inputs) {
          const auto* port = n.node->input(v.name);
          if (!port || !port->type || !types::compatible(*port->type, *v.type)) {
            start_failure_ = FailureData::make(FailureKind::type_mismatch,
                                               "saved input " + v.name + " no longer matches the port type",
                                               {{"card", path_key(n.node->path)}, {"port", v.name}});
            n.halted = true;
            n.last = CardState::failed;
            n.last_failure = start_failure_;
            return std::nullopt;
          }
          w.inputs.push_back({v, {}, last->invocation, 0});
        }
      }
      return w;
    }
    if (n.queues.empty()) return std::nullopt;
    if (n.node->settings.any_mode) {
      std::deque<Token>* best = nullptr;
      for (auto& [_, q] : n.queues)
        if (!q.empty() && (!best || q.front().seq < best->front().seq)) best = &q;
      if (!best || !downstream_has_space(n)) return std::nullopt;
      w.inputs.push_back(std::move(best->front()));
      best->pop_front();
    } else {
      for (auto& [_, q] : n.queues)
        if (q.empty()) return std::nullopt;
      if (!downstream_has_space(n)) return std::nullopt;
      for (auto& [_, q] : n.queues) {
        w.inputs.push_back(std::move(q.front()));
        q.pop_front();
      }
    }
    return w;
  }

  void main() {
    stdfs::create_directories(run_dir_);
    prepare_graph();
    set_state(RunState::preparing);
    journal("run", "started", std::nullopt, std::nullopt, "run started");
    // Execution pool.
    for (uint32_t lane = 0; lane < p_.max_concurrency; ++lane)
      pool_.emplace_back([this, lane] { pool_loop(lane); });
    {
      std::unique_lock<std::mutex> l(mu_);
      while (true) {
        if (!cancel_) {
          bool progressed = true;
          while (progressed && running_ < p_.max_concurrency) {
            progressed = false;
            for (auto& n : nodes_) {
              if (running_ >= p_.max_concurrency) break;
              auto w = take_ready(n);
              if (!w) continue;
              n.running = true;
              w->invocation = ++n.invocations;
              ++running_;
              tasks_.push_back(std::move(*w));
              task_cv_.notify_one();
              progressed = true;
            }
          }
        }
        if (running_ == 0 && (cancel_ || !any_ready_locked())) break;
        cv_.wait_for(l, std::chrono::milliseconds(200));
      }
      pool_stop_ = true;
    }
    task_cv_.notify_all();
    for (auto& t : pool_) t.join();
    for (auto& n : nodes_)
      if (n.worker) n.worker->terminate();
    finalize();
  }

  bool any_ready_locked() {
    for (auto& n : nodes_) {
      if (!n.in_targets || n.running || n.halted || n.node->is_virtual) continue;
      if (n.start && !n.start_fired) return true;
      if (n.queues.empty()) continue;
      if (n.node->settings.any_mode) {
        for (auto& [_, q] : n.queues)
          if (!q.empty()) return true;
      } else {
        bool all = true;
        for (auto& [_, q] : n.queues) all &= !q.empty();
        if (all) return true;
      }
    }
    return false;
  }

  void pool_loop(uint32_t lane) {
    while (true) {
      Work w;
      {
        std::unique_lock<std::mutex> l(mu_);
        task_cv_.wait(l, [&] { return pool_stop_ || !tasks_.empty(); });
        if (tasks_.empty()) return;
        w = std::move(tasks_.front());
        tasks_.pop_front();
      }
      invoke(w, lane);
      {
        std::lock_guard<std::mutex> l(mu_);
        nodes_[w.node].running = false;
        --running_;
      }
      cv_.notify_all();
    }
  }

  // ---- invocation -----------------------------------------------------------

  struct Produced {
    std::vector<Value> outputs;
    std::optional<FailureData> failure;
  };

  void invoke(Work& w, uint32_t lane) {
    NodeRt& rt = nodes_[w.node];
    const Node& node = *rt.node;
    InvocationSummaryData s;
    s.run_id = run_id_;
    s.card = node.path;
    s.invocation = w.invocation;
    s.started_at = now_ms();
    std::vector<Value> inputs;
    std::vector<InputSourceData> sources;
    for (const auto& t : w.inputs) {
      inputs.push_back(t.value);
      if (!t.from.empty()) sources.push_back({t.value.name, t.from, t.from_inv});
    }
    Produced out;
    if (w.invocation > node.settings.max_runs) {
      out.failure = FailureData::make(FailureKind::execution_limit,
                                      "maxRuns " + std::to_string(node.settings.max_runs) + " exceeded",
                                      {{"card", path_key(node.path)}, {"max_runs", std::to_string(node.settings.max_runs)}});
    } else {
      s.state = CardState::running;
      s.phase = node.kind == Template::Code ? Phase::evaluate : Phase::execute;
      card_status(s);
      if (node.kind == Template::Code || !inputs.empty()) {
        auto inputs_copy = inputs;
        auto card = node.path;
        auto run = run_id_;
        auto inv = w.invocation;
        svc_.history().submit([card, run, inv, inputs_copy](sql::Db& db) {
          History::put_last_input(db, card, run, inv, inputs_copy);
        });
      }
      journal(phase_name(*s.phase), "started", node.path, w.invocation, "invocation started");
      try {
        if (node.kind == Template::Code) out = run_code(rt, s, inputs, lane);
        else out = run_builtin(rt, s, inputs);
      } catch (const Failed& f) {
        out.failure = f.failure;
      } catch (const std::exception& e) {
        out.failure = FailureData::make(FailureKind::internal_error, e.what());
      }
      if (!out.failure) {
        s.phase = Phase::validate;
        int64_t vs = now_ms();
        try {
          validate_outputs(rt, w.invocation, out.outputs);
        } catch (const Failed& f) {
          out.failure = f.failure;
          out.outputs.clear();
        }
        svc_.server()->broadcast(timeline_note(run_id_, node.path, w.invocation, Phase::validate, lane, vs, now_ms()));
      }
    }
    s.ended_at = now_ms();
    s.duration_ms = static_cast<uint64_t>(*s.ended_at - *s.started_at);
    bool was_cancel = out.failure && out.failure->kind == FailureKind::cancelled;
    if (out.failure && !out.failure->args.empty() == false) out.failure->args.emplace_back("card", path_key(node.path));
    s.state = out.failure ? (was_cancel ? CardState::cancelled : CardState::failed) : CardState::succeeded;
    s.failure = out.failure;
    if (out.failure && out.failure->diagnostic)
      card_log(node.path, w.invocation, LogStream::stderr, "tsugumi: " + std::string(failure_kind_name(out.failure->kind)) +
                                                              ": " + *out.failure->diagnostic + "\n");
    InvocationRecordData rec;
    rec.summary = s;
    rec.record_io = node.settings.record_io;
    rec.input_sources = sources;
    if (rec.record_io) {
      rec.inputs = inputs;
      rec.outputs = out.outputs;
    }
    {
      std::lock_guard<std::mutex> l(mu_);
      rec.display = rt.display && rt.display->invocation == w.invocation ? rt.display : std::nullopt;
    }
    // Artifacts are copied before the completion becomes visible.
    if (!out.failure && node.settings.retain_artifact && node.settings.artifact_output) {
      for (auto& v : out.outputs)
        if (v.name == *node.settings.artifact_output) {
          try {
            retain_artifact(node, w.invocation, v);
          } catch (const Failed& f) {
            s.state = CardState::failed;
            s.failure = f.failure;
            rec.summary = s;
            out.failure = f.failure;
          }
        }
      if (rec.record_io) rec.outputs = out.outputs;
    }
    svc_.history().submit([rec](sql::Db& db) { History::complete_invocation(db, rec); },
                          [this, s](bool) { svc_.server()->broadcast(card_note(s)); });
    journal(phase_name(s.phase.value_or(Phase::execute)),
            s.state == CardState::succeeded ? "succeeded" : s.state == CardState::cancelled ? "cancelled" : "failed",
            node.path, w.invocation,
            out.failure ? std::string(failure_kind_name(out.failure->kind)) + ": " + out.failure->diagnostic.value_or("") : "ok");
    std::lock_guard<std::mutex> l(mu_);
    rt.last = s.state;
    if (out.failure) {
      rt.last_failure = out.failure;
      if (!first_failure_ && !was_cancel) first_failure_ = out.failure;
      if (out.failure->kind == FailureKind::execution_limit) rt.halted = true;
      return;
    }
    // Route outputs downstream. Never forward invalid output.
    for (auto& v : out.outputs) {
      auto it = out_edges_.find({rt.idx, v.name});
      if (it == out_edges_.end()) continue;
      for (const auto& [to, port] : it->second) {
        if (!nodes_[to].in_targets) continue;
        Token t;
        t.value = v;
        t.value.name = port;
        t.from = node.path;
        t.from_inv = w.invocation;
        t.seq = ++token_seq_;
        nodes_[to].queues[port].push_back(std::move(t));
      }
    }
  }

  void validate_outputs(NodeRt& rt, uint64_t inv, std::vector<Value>& outputs) {
    const Node& node = *rt.node;
    std::set<std::string> seen;
    for (auto& v : outputs) {
      const auto* port = node.output(v.name);
      if (!port || !port->type)
        throw Failed(FailureKind::output_invalid, "unknown output " + v.name, {{"port", v.name}});
      if (!seen.insert(v.name).second)
        throw Failed(FailureKind::output_invalid, "output " + v.name + " produced twice", {{"port", v.name}});
      v.kind = port->kind;
      v.type = port->type;
      if (!types::verify(*v.type, v.bytes))
        throw Failed(FailureKind::output_invalid, "output " + v.name + " is not a valid " + v.type->root_type,
                     {{"port", v.name}});
      if (v.kind != PortKind::FlatBuffer) {
        auto p = path_value(v);
        auto out_dir = invocation_dir(node, inv) / "out";
        std::error_code ec;
        if (!p || !stdfs::path(*p).is_absolute() || !fs::resolves_inside(out_dir, *p))
          throw Failed(FailureKind::output_invalid, "output " + v.name + " must point inside the output directory",
                       {{"port", v.name}});
        auto st = stdfs::symlink_status(*p, ec);
        bool ok = v.kind == PortKind::File ? stdfs::is_regular_file(st) : stdfs::is_directory(st);
        if (!ok)
          throw Failed(FailureKind::output_invalid,
                       "output " + v.name + " does not exist as a " + (v.kind == PortKind::File ? "file" : "directory"),
                       {{"port", v.name}});
      }
    }
    for (const auto& port : node.outputs)
      if (!port.optional && !seen.count(port.name) && node.kind == Template::Code)
        throw Failed(FailureKind::output_invalid, "required output " + port.name + " is missing", {{"port", port.name}});
  }

  stdfs::path invocation_dir(const Node& node, uint64_t inv) {
    return run_dir_ / "work" / path_key(node.path) / std::to_string(inv);
  }

  void retain_artifact(const Node& node, uint64_t inv, Value& v) {
    auto src = path_value(v);
    if (!src) throw Failed(FailureKind::output_invalid, "artifact output has no path");
    ArtifactData a;
    a.artifact_id = crypto::uuid4();
    a.run_id = run_id_;
    a.card = node.path;
    a.invocation = inv;
    a.output = v.name;
    a.kind = v.kind;
    a.name = stdfs::path(*src).filename().string();
    auto dest = run_dir_ / "artifacts" / a.artifact_id;
    if (v.kind == PortKind::File) {
      auto data = fs::read_file_or_throw(*src);
      fs::write_atomic(dest / a.name, data, 0644);
      a.members.push_back({a.name, data.size(), crypto::sha256_hex(data)});
    } else {
      auto listing = fs::list_tree(*src);
      if (!listing.links.empty())
        throw Failed(FailureKind::output_invalid, "artifact directory contains symlink " + listing.links.front());
      if (listing.files.size() > 1000)
        throw Failed(FailureKind::resource_limit, "artifact directory exceeds 1,000 files");
      for (const auto& f : listing.files) {
        auto data = fs::read_file_or_throw(stdfs::path(*src) / f);
        fs::write_atomic(dest / f, data, 0644);
        a.members.push_back({f, data.size(), crypto::sha256_hex(data)});
      }
    }
    v.artifact_id = a.artifact_id;
    svc_.history().write_sync([a](sql::Db& db) { History::put_artifact(db, a); });
  }

  // ---- builtins --------------------------------------------------------------

  const Value* input(const std::vector<Value>& in, const std::string& name) {
    for (const auto& v : in)
      if (v.name == name) return &v;
    return nullptr;
  }

  Produced run_builtin(NodeRt& rt, InvocationSummaryData& s, const std::vector<Value>& in) {
    const Node& node = *rt.node;
    Produced p;
    int64_t t0 = now_ms();
    switch (node.kind) {
      case Template::Trigger: {
        FBB b;
        b.Finish(flatbuffers::Offset<void>(b.EndTable(b.StartTable())));
        p.outputs.push_back({"out", PortKind::FlatBuffer, types::empty_type(),
                             std::string(reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()), {}});
        break;
      }
      case Template::Input: {
        for (const auto& port : node.outputs) {
          if (!port.type) throw Failed(FailureKind::validation_failed, "output " + port.name + " has no type");
          // Message fields are filled from params of the same name.
          const auto* root = port.type->root();
          std::vector<hosq::json::GenericDom<>> owned;
          owned.reserve(node.settings.params.size());
          std::vector<std::pair<std::string, const hosq::json::GenericDom<>*>> members;
          for (const auto& prm : node.settings.params) {
            if (!prm.data || !root->fields()->LookupByKey(prm.name.c_str())) continue;
            const hosq::json::GenericDom<>* val = &*prm.data;
            if (port.kind != PortKind::FlatBuffer && prm.name == "value") {
              // A selected project file is copied into the run before use.
              schema::v::String relv;
              if (schema::decode(*prm.data, relv)) throw Failed(FailureKind::invalid_path, "input file must be a path string");
              const std::string& rel = relv.value();
              if (!fs::valid_relative_path(rel) || !doc_->sources.count(rel))
                throw Failed(FailureKind::invalid_path, "input file " + rel + " is not a project source", {{"path", rel}});
              auto data = svc_.store().read_source(*doc_, rel);
              auto dest = invocation_dir(node, s.invocation) / "out" / stdfs::path(rel).filename();
              fs::write_atomic(dest, data, 0644);
              owned.push_back(schema::string_value(dest.string()));
              val = &owned.back();
            }
            members.emplace_back(prm.name, val);
          }
          std::string bytes, err;
          if (!types::json_to_flatbuffer(*port.type, schema::object_text(members), &bytes, &err))
            throw Failed(FailureKind::output_invalid, "input " + port.name + ": " + err);
          p.outputs.push_back({port.name, port.kind, port.type, std::move(bytes), {}});
        }
        break;
      }
      case Template::Fork: {
        const Value* v = input(in, "in");
        if (!v) throw Failed(FailureKind::input_missing, "fork input missing");
        for (const auto& port : node.outputs) p.outputs.push_back({port.name, v->kind, v->type, v->bytes, {}});
        break;
      }
      case Template::Branch: {
        const Value* v = input(in, "value");
        const Value* c = input(in, "condition");
        if (!v || !c) throw Failed(FailureKind::input_missing, "branch needs value and condition");
        const auto* f = c->type->root()->fields()->LookupByKey("value");
        const auto* t = flatbuffers::GetAnyRoot(reinterpret_cast<const uint8_t*>(c->bytes.data()));
        bool cond = flatbuffers::GetAnyFieldI(*t, *f) != 0;
        p.outputs.push_back({cond ? "a" : "b", v->kind, v->type, v->bytes, {}});
        card_log(node.path, s.invocation, LogStream::stdout, std::string("branch -> ") + (cond ? "a" : "b") + "\n");
        break;
      }
      case Template::Merge: {
        if (in.size() != 1) throw Failed(FailureKind::input_missing, "merge consumes exactly one arrival");
        const Value& v = in.front();
        std::string inner, err;
        if (!types::flatbuffer_to_json(*v.type, v.bytes, &inner, &err)) throw Failed(FailureKind::output_invalid, err);
        auto type_name = schema::string_value(v.name);
        auto value = schema::capture(inner);
        std::string doc = schema::object_text({{"value_type", &type_name}, {"value", &value}});
        std::string bytes;
        const auto& out = node.outputs.front();
        if (!out.type || !types::json_to_flatbuffer(*out.type, doc, &bytes, &err))
          throw Failed(FailureKind::output_invalid, "merge: " + err);
        p.outputs.push_back({"out", PortKind::FlatBuffer, out.type, std::move(bytes), {}});
        break;
      }
      case Template::Logger: {
        const Value* v = input(in, "in");
        if (!v) throw Failed(FailureKind::input_missing, "logger input missing");
        std::string text, err;
        if (!types::flatbuffer_to_json(*v->type, v->bytes, &text, &err)) throw Failed(FailureKind::output_invalid, err);
        card_log(node.path, s.invocation, LogStream::stdout, text + "\n");
        DisplayData d;
        d.card = node.path;
        d.run_id = run_id_;
        d.invocation = s.invocation;
        d.value = DisplayData::Text{text};
        on_display(rt, d);
        if (node.settings.logger_database) write_logger_db(node, s.invocation, text);
        break;
      }
      default:
        throw Failed(FailureKind::unsupported, std::string(template_name(node.kind)) + " cards cannot run");
    }
    svc_.server()->broadcast(timeline_note(run_id_, node.path, s.invocation, Phase::execute, 0, t0, now_ms()));
    return p;
  }

  void write_logger_db(const Node& node, uint64_t inv, const std::string& text) {
    std::string url;
    for (const auto& db : doc_->database_connections())
      if (db.name->value() == *node.settings.logger_database) url = db.url->value();
    if (!url.starts_with("sqlite:"))
      throw Failed(FailureKind::unsupported, "logger databases support sqlite:<name>.db URLs only");
    std::string name = url.substr(7);
    if (!valid_id(name)) throw Failed(FailureKind::invalid_path, "invalid logger database name " + name);
    auto path = svc_.store().state_dir() / "logger" / name;
    stdfs::create_directories(path.parent_path());
    sql::Db db(path.string());
    db.exec("CREATE TABLE IF NOT EXISTS records(time_ms INTEGER, run_id TEXT, card TEXT, invocation INTEGER, value TEXT)");
    db.prepare("INSERT INTO records VALUES(?,?,?,?,?)")
        .bind(1, now_ms()).bind(2, run_id_).bind(3, path_key(node.path)).bind(4, inv).bind(5, text).run();
  }

  void on_display(NodeRt& rt, DisplayData d) {
    if (d.card != rt.node->path || d.run_id != run_id_)
      throw Failed(FailureKind::output_invalid, "display addressed another card or run");
    if (auto err = d.validate()) throw Failed(FailureKind::output_invalid, *err);
    {
      std::lock_guard<std::mutex> l(mu_);
      d.revision = ++rt.display_rev;
      rt.display = d;
    }
    sched_.publish_display(d);
  }

  // ---- code Cards -------------------------------------------------------------

  Produced run_code(NodeRt& rt, InvocationSummaryData& s, const std::vector<Value>& in, uint32_t lane) {
    const Node& node = *rt.node;
    if (!rt.build) {
      s.phase = Phase::build;
      card_status(s);
      set_state(RunState::preparing);
      int64_t t0 = now_ms();
      BuildRequest br;
      br.run_id = run_id_;
      br.node = &node;
      br.doc = doc_;
      br.run_dir = run_dir_;
      br.jobs = p_.nix_jobs;
      br.cores = p_.nix_cores;
      br.timeout_seconds = node.settings.build_timeout_seconds;
      br.cancel = &cancel_;
      uint64_t inv = s.invocation;
      CardPath card = node.path;
      br.log = [this, card, inv](const std::string& line) { card_log(card, inv, LogStream::stderr, line); };
      rt.build = svc_.nix().build(br);
      svc_.server()->broadcast(timeline_note(run_id_, node.path, s.invocation, Phase::build, lane, t0, now_ms()));
      journal("build", "output", node.path, s.invocation, rt.build->cached ? "build reused from cache" : "built");
    }
    s.phase = Phase::execute;
    card_status(s);
    set_state(RunState::running);
    auto dir = invocation_dir(node, s.invocation);
    auto work = dir / "work", tmp = dir / "tmp", out = dir / "out";
    for (const auto& d : {work, tmp, out}) {
      stdfs::create_directories(d);
      if (::geteuid() == 0) ::chown(d.c_str(), kCardUid, kCardUid);
    }
    std::optional<uid_t> uid;
    if (::geteuid() == 0) uid = kCardUid;
    if (!rt.worker || !rt.worker->alive() || !rt.build->reuse_worker) {
      if (rt.worker) rt.worker->terminate();
      WorkerSpawn sp;
      sp.worker_binary = svc_.options().worker_binary.string();
      sp.bash = TSUGUMI_BASH;
      sp.build = *rt.build;
      sp.script_dir = (run_dir_ / "build" / path_key(node.path) / "scripts").string();
      sp.sandbox = node.settings.sandbox;
      sp.uid = uid;
      sp.gid = uid;
      // Reused workers keep the whole Card work tree; single calls get exactly their dirs.
      if (rt.build->reuse_worker) sp.rw_paths = {(run_dir_ / "work" / path_key(node.path)).string()};
      else sp.rw_paths = {work.string(), tmp.string(), out.string()};
      for (const auto& v : in)
        if (v.kind != PortKind::FlatBuffer)
          if (auto p = path_value(v)) sp.ro_paths.push_back(*p);
      CardPath card = node.path;
      auto inv_ptr = std::make_shared<std::atomic<uint64_t>>(s.invocation);
      current_inv_[rt.idx] = inv_ptr;
      sp.ro_paths.erase(std::unique(sp.ro_paths.begin(), sp.ro_paths.end()), sp.ro_paths.end());
      rt.worker = std::make_unique<WorkerProc>(sp, [this, card, inv_ptr](LogStream stream, const std::string& text) {
        card_log(card, inv_ptr->load(), stream, text);
      });
    }
    if (auto it = current_inv_.find(rt.idx); it != current_inv_.end()) it->second->store(s.invocation);
    auto ptype = NixRunner::params_type(node);
    std::string params = NixRunner::params_buffer(node);
    std::vector<std::pair<std::string, std::string>> env = {
        {"PATH", TSUGUMI_CARD_PATH},
        {"HOME", work.string()},
        {"LANG", "C.UTF-8"},
        {"TSUGUMI_RUN_ID", run_id_},
        {"TSUGUMI_CARD", path_key(node.path)},
        {"TSUGUMI_INVOCATION", std::to_string(s.invocation)},
        {"TSUGUMI_PARAMS_JSON", NixRunner::params_json(node, false)},
        {"TSUGUMI_WORKER", svc_.options().worker_binary.string()},
    };
    std::optional<uint64_t> deadline_ms;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (node.settings.timeout_seconds) {
      deadline_ms = *node.settings.timeout_seconds * 1000;
      deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(*deadline_ms + 500);
    }
    std::string frame = worker_call_frame(++rt.call_id, run_id_, node.path, s.invocation, in, ptype->bfbs,
                                          ptype->root_type, params, node.outputs, work.string(), tmp.string(),
                                          out.string(), env, deadline_ms);
    int64_t t0 = now_ms();
    auto outcome = rt.worker->call(frame, rt.call_id, &cancel_, deadline, [this, &rt](const DisplayData& d) {
      on_display(rt, d);
    });
    svc_.server()->broadcast(timeline_note(run_id_, node.path, s.invocation, Phase::execute, lane, t0, now_ms()));
    if (!rt.build->reuse_worker || !rt.worker->alive()) {
      rt.worker->terminate();
      rt.worker.reset();
    }
    if (!node.settings.filesystem) {
      std::error_code ec;
      stdfs::remove_all(work, ec);
      stdfs::remove_all(tmp, ec);
    }
    Produced p;
    p.failure = outcome.failure;
    if (!p.failure)
      for (auto& [name, bytes] : outcome.outputs) p.outputs.push_back({name, PortKind::FlatBuffer, nullptr, std::move(bytes), {}});
    return p;
  }

  // ---- convergence ------------------------------------------------------------

  void finalize() {
    const auto& res = *doc_->resolution;
    std::vector<CardState> final(nodes_.size(), CardState::not_started);
    std::vector<std::vector<WaitingInputData>> waiting(nodes_.size());
    for (auto& n : nodes_) {
      if (!n.in_targets || n.node->is_virtual) continue;
      final[n.idx] = n.invocations > 0 || n.halted ? n.last : CardState::skipped;
    }
    // Never invoked: blocked when an upstream failed/blocked, or partial inputs arrived.
    for (bool changed = true; changed;) {
      changed = false;
      for (auto& n : nodes_) {
        if (!n.in_targets || n.node->is_virtual || n.invocations > 0 || n.halted) continue;
        if (final[n.idx] == CardState::blocked) continue;
        bool partial = false;
        for (auto& [_, q] : n.queues) partial |= !q.empty();
        bool upstream_bad = false;
        for (const auto& e : res.edges)
          if (e.to == n.idx && nodes_[e.from].in_targets &&
              (final[e.from] == CardState::failed || final[e.from] == CardState::blocked ||
               final[e.from] == CardState::cancelled))
            upstream_bad = true;
        if (partial || upstream_bad) {
          final[n.idx] = cancel_ ? CardState::cancelled : CardState::blocked;
          changed = true;
        }
      }
    }
    std::optional<FailureData> blocked_failure;
    for (auto& n : nodes_) {
      if (!n.in_targets || n.node->is_virtual || n.invocations > 0 || n.halted) continue;
      CardState st = final[n.idx];
      if (st == CardState::blocked) {
        for (auto& [port, q] : n.queues) {
          if (!q.empty()) continue;
          WaitingInputData wi;
          wi.port = port;
          for (const auto& e : res.edges)
            if (e.to == n.idx && e.to_port == port) wi.upstream.push_back({res.nodes[e.from].path, e.from_port});
          waiting[n.idx].push_back(std::move(wi));
        }
        if (!blocked_failure)
          blocked_failure = FailureData::make(FailureKind::input_missing, path_key(n.node->path) + " is missing inputs",
                                              {{"card", path_key(n.node->path)}});
      }
      if (cancel_ && st == CardState::skipped) continue;  // never reached before the cancel
      InvocationRecordData rec;
      rec.summary.run_id = run_id_;
      rec.summary.card = n.node->path;
      rec.summary.invocation = n.invocations + 1;
      rec.summary.state = st;
      rec.summary.waiting = waiting[n.idx];
      svc_.history().submit([rec](sql::Db& db) { History::complete_invocation(db, rec); },
                            [this, s = rec.summary](bool) { svc_.server()->broadcast(card_note(s)); });
    }
    RunSummaryData sum;
    {
      std::lock_guard<std::mutex> l(mu_);
      auto& st = summary_.status;
      if (cancel_ && interrupted_) {
        st.state = RunState::failed;
        st.failure = FailureData::make(FailureKind::interrupted, "the environment stopped while the run was active");
      } else if (cancel_) {
        st.state = RunState::cancelled;
        st.failure = FailureData::make(FailureKind::cancelled, "the run was cancelled");
      } else if (start_failure_ || first_failure_ || blocked_failure) {
        st.state = RunState::failed;
        st.failure = start_failure_ ? start_failure_ : first_failure_ ? first_failure_ : blocked_failure;
      } else {
        st.state = RunState::succeeded;
        st.failure.reset();
      }
      st.ended_at = now_ms();
      sum = summary_;
    }
    journal("run", sum.status.state == RunState::succeeded ? "succeeded"
                   : sum.status.state == RunState::cancelled ? "cancelled" : "failed",
            std::nullopt, std::nullopt, sum.status.failure ? sum.status.failure->diagnostic.value_or("") : "run finished");
    // Final displays are persisted and queued before Finished.
    std::vector<DisplayData> displays;
    for (auto& n : nodes_)
      if (n.display) displays.push_back(*n.display);
    for (const auto& d : displays) svc_.server()->broadcast(display_note(d));
    auto self = shared_from_this();
    svc_.history().submit(
        [sum, displays](sql::Db& db) {
          for (const auto& d : displays) History::put_display(db, d);
          History::update_run(db, sum.status);
        },
        [self, sum](bool) {
          self->svc_.server()->broadcast(status_note(sum.status, false));
          self->svc_.server()->broadcast(finished_note(sum));
          self->sched_.finished(self.get());
        });
  }

  Service& svc_;
  Scheduler& sched_;
  DocumentPtr doc_;
  RunParams p_;
  RunSummaryData summary_;
  std::string run_id_;
  stdfs::path run_dir_;
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_, task_cv_;
  std::vector<NodeRt> nodes_;
  std::map<std::pair<size_t, std::string>, std::vector<std::pair<size_t, std::string>>> out_edges_;
  std::deque<Work> tasks_;
  std::vector<std::thread> pool_;
  bool pool_stop_ = false;
  uint32_t running_ = 0;
  uint64_t token_seq_ = 0;
  std::atomic<bool> cancel_{false};
  bool interrupted_ = false;
  std::optional<FailureData> first_failure_, start_failure_;
  std::map<size_t, std::shared_ptr<std::atomic<uint64_t>>> current_inv_;
};

// ---------------------------------------------------------------------------

Scheduler::Scheduler(Service& svc) : svc_(svc) {}

Scheduler::~Scheduler() { shutdown(); }

void Scheduler::recover_interrupted() {
  auto runs = svc_.history().read([](sql::Db& db) { return History::unfinished_runs(db); });
  for (const auto& run_id : runs) {
    svc_.history().write_sync([&](sql::Db& db) {
      auto r = History::run(db, run_id);
      if (!r) return;
      r->status.state = RunState::failed;
      r->status.failure = FailureData::make(FailureKind::interrupted, "tsugumid restarted while the run was active");
      r->status.ended_at = now_ms();
      History::update_run(db, r->status);
      // In-progress invocations become interrupted failures; nothing is re-run.
      auto st = db.prepare("SELECT card, invocation, started_at FROM statuses WHERE run_id=?");
      st.bind(1, run_id);
      std::vector<InvocationRecordData> recs;
      while (st.step()) {
        InvocationRecordData rec;
        rec.summary.run_id = run_id;
        CardPath p;
        std::string key = st.text(0);
        size_t s = 0;
        while (true) {
          size_t e = key.find('/', s);
          p.push_back(key.substr(s, e == std::string::npos ? std::string::npos : e - s));
          if (e == std::string::npos) break;
          s = e + 1;
        }
        rec.summary.card = p;
        rec.summary.invocation = st.u64(1);
        rec.summary.started_at = st.opt_i64(2);
        rec.summary.state = CardState::failed;
        rec.summary.failure = r->status.failure;
        recs.push_back(std::move(rec));
      }
      for (const auto& rec : recs) History::complete_invocation(db, rec);
    });
    log::warn("run " + run_id + " was interrupted by a restart");
  }
}

RunPlanData Scheduler::plan(const Resolution& res, const CardPath& card, bool propagate) {
  RunPlanData out;
  const Node* target = res.find(card);
  if (!target) throw Failed(FailureKind::not_found, "unknown card " + path_key(card), {{"card", path_key(card)}});
  if (target->is_virtual) throw Failed(FailureKind::invalid_request, "subgraph containers cannot be run directly; run a Card inside");
  size_t t = res.index.at(path_key(card));
  std::set<size_t> targets;
  std::vector<size_t> starts;
  if (!propagate) {
    targets.insert(t);
    starts.push_back(t);
  } else if (target->kind == Template::Trigger) {
    // Whole run: every executable Card; Start Cards are the trigger and all
    // sources without connected inputs.
    for (size_t i = 0; i < res.nodes.size(); ++i) {
      if (res.nodes[i].is_virtual || res.nodes[i].kind == Template::Draft) continue;
      targets.insert(i);
      bool has_in = false;
      for (const auto& e : res.edges) has_in |= e.to == i;
      if (!has_in) starts.push_back(i);
    }
  } else {
    std::vector<size_t> stack = {t};
    while (!stack.empty()) {
      size_t i = stack.back();
      stack.pop_back();
      if (!targets.insert(i).second) continue;
      for (const auto& e : res.edges)
        if (e.from == i) stack.push_back(e.to);
    }
    starts.push_back(t);
  }
  for (size_t i : targets) {
    out.targets.push_back(res.nodes[i].path);
    if (res.nodes[i].kind == Template::Code) out.build_candidates.push_back(res.nodes[i].path);
    out.diagnostics.insert(out.diagnostics.end(), res.nodes[i].diags.begin(), res.nodes[i].diags.end());
    out.diagnostics.insert(out.diagnostics.end(), res.nodes[i].exec_diags.begin(), res.nodes[i].exec_diags.end());
  }
  // Inputs fed from outside a partial run are not missing for the plan's start Card.
  for (size_t s : starts) out.starts.push_back(res.nodes[s].path);
  return out;
}

std::string Scheduler::accept(DocumentPtr doc, const RunParams& p) {
  std::lock_guard<std::mutex> l(mu_);
  if (active_) throw Failed(FailureKind::busy, "another run is active", {{"run_id", *active_->status().run_id}});
  auto plan = Scheduler::plan(*doc->resolution, p.card, p.propagate);
  std::vector<std::string> errors;
  for (const auto& d : plan.diagnostics) {
    if (!d.error) continue;
    // A partial run's start Card gets its inputs from the saved inputs.
    if (d.code == "input_missing" && d.card && *d.card == p.card) continue;
    errors.push_back((d.card ? path_key(*d.card) + ": " : "") + d.message);
  }
  if (!errors.empty()) {
    std::string msg = "the run plan has errors: " + errors.front();
    if (errors.size() > 1) msg += " (and " + std::to_string(errors.size() - 1) + " more)";
    throw Failed(FailureKind::validation_failed, msg, {{"count", std::to_string(errors.size())}});
  }
  RunSummaryData s;
  s.status.run_id = crypto::uuid4();
  s.status.state = RunState::queued;
  s.status.rev = doc->rev;
  s.status.execution_rev = doc->execution_rev;
  s.status.accepted_at = now_ms();
  s.source_hash = doc->execution_fingerprint;
  s.target = p.card;
  s.propagate = p.propagate;
  FBB b;
  auto st = wire::run_status(b, s.status);
  auto ra = R::CreateRunAccepted(b, b.CreateString(p.op_id), st, false);
  auto rc = R::CreateOperationReceipt(b, b.CreateString(p.op_id), R::ReceiptValue::RunAccepted, ra.Union());
  b.Finish(rc);
  std::string receipt(reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize());
  // The run id and the receipt are durable before any processing starts.
  svc_.history().write_sync([&](sql::Db& db) {
    History::insert_run(db, s, p.op_id);
    History::put_receipt(db, p.op_id, "run", p.digest, receipt);
  });
  svc_.server()->broadcast(status_note(s.status, true));
  active_ = std::make_shared<RunExec>(svc_, *this, doc, p, s);
  active_->start();
  return receipt;
}

std::optional<RunStatusData> Scheduler::active_status() {
  std::lock_guard<std::mutex> l(mu_);
  if (!active_) return std::nullopt;
  return active_->status();
}

RunStatusData Scheduler::cancel(const std::string& run_id) {
  std::shared_ptr<RunExec> exec;
  {
    std::lock_guard<std::mutex> l(mu_);
    if (active_ && active_->status().run_id == run_id) exec = active_;
  }
  if (exec) return exec->request_cancel(false);
  auto r = svc_.history().read([&](sql::Db& db) { return History::run(db, run_id); });
  if (!r) throw Failed(FailureKind::not_found, "unknown run " + run_id, {{"run_id", run_id}});
  return r->status;
}

void Scheduler::publish_display(const DisplayData& d) {
  {
    std::lock_guard<std::mutex> l(mu_);
    displays_[path_key(d.card)] = d;
  }
  svc_.server()->broadcast(display_note(d));
}

std::optional<Note> Scheduler::latest_display_note(const std::string& card_key) {
  std::lock_guard<std::mutex> l(mu_);
  auto it = displays_.find(card_key);
  if (it != displays_.end()) return display_note(it->second);
  // After a restart the latest display comes from the last run's history.
  auto last = svc_.history().read([](sql::Db& db) { return History::latest_run(db); });
  if (!last) return std::nullopt;
  CardPath p;
  size_t s = 0;
  while (true) {
    size_t e = card_key.find('/', s);
    p.push_back(card_key.substr(s, e == std::string::npos ? std::string::npos : e - s));
    if (e == std::string::npos) break;
    s = e + 1;
  }
  auto d = svc_.history().read([&](sql::Db& db) { return History::display(db, *last->status.run_id, p); });
  if (!d) return std::nullopt;
  return display_note(*d);
}

void Scheduler::finished(RunExec* exec) {
  std::shared_ptr<RunExec> done;
  {
    std::lock_guard<std::mutex> l(mu_);
    if (active_.get() == exec) done = std::move(active_);
  }
  if (done) {
    // The run thread finishes on its own; reap it off the callback thread.
    std::lock_guard<std::mutex> l(mu_);
    reapers_.emplace_back([done] { done->join(); });
  }
}

void Scheduler::shutdown() {
  std::shared_ptr<RunExec> exec;
  {
    std::lock_guard<std::mutex> l(mu_);
    exec = active_;
  }
  if (exec) {
    exec->request_cancel(true);
    exec->join();
  }
  std::vector<std::thread> reapers;
  {
    std::lock_guard<std::mutex> l(mu_);
    reapers.swap(reapers_);
  }
  for (auto& t : reapers)
    if (t.joinable()) t.join();
}

}  // namespace tsugumi
