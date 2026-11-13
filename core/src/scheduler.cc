#include "scheduler.hh"

#include <algorithm>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <flatbuffers/reflection.h>
#include <set>

#include "execution_service.hh"
#include "collect_value.hh"
#include "history.hh"
#include "remote_execution_service.hh"
#include "schema.hh"
#include "nix.hh"
#include "service.hh"
#include "util/crypto.hh"
#include "util/fs.hh"
#include "util/log.hh"
#include "wire.hh"

namespace tsugumi {

namespace R = Tsugumi::RPC;
namespace D = Tsugumi::Data;
namespace stdfs = std::filesystem;
using FBB = flatbuffers::FlatBufferBuilder;

namespace {

constexpr size_t kQueueCapacity = 16;
constexpr uint32_t kMaxRunWorkers = 4096;
constexpr uint32_t kMaxParallelWorkers = 8;
constexpr size_t kMaxBatchBytes = 32u << 20;

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

// Opens stage `p` of an invocation, closing a still open previous stage as
// done. A stage with another outcome (cached, reused) is closed at once.
void stage_begin(InvocationSummaryData& s, Phase p, StageOutcome o = StageOutcome::running) {
  int64_t t = now_ms();
  if (!s.stages.empty() && s.stages.back().outcome == StageOutcome::running) {
    s.stages.back().outcome = StageOutcome::done;
    s.stages.back().end = t;
  }
  StageTimeData st;
  st.phase = p;
  st.outcome = o;
  st.start = t;
  if (o != StageOutcome::running) st.end = t;
  s.stages.push_back(st);
  s.phase = p;
}

// Closes the open stage (if any) with outcome `o`.
void stage_close(InvocationSummaryData& s, StageOutcome o) {
  if (!s.stages.empty() && s.stages.back().outcome == StageOutcome::running) {
    s.stages.back().outcome = o;
    s.stages.back().end = now_ms();
  }
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
  if (const auto* t = std::get_if<DisplayData::Table>(&d.value))
    for (const auto& r : t->rows)
      for (const auto& c : r) n.approx_bytes += c.size() + 8;
  if (const auto* im = std::get_if<DisplayData::Image>(&d.value)) n.approx_bytes += im->data.size();
  if (const auto* p = std::get_if<DisplayData::PointCloud>(&d.value)) n.approx_bytes += p->x.size() * 16;
  if (const auto* m = std::get_if<DisplayData::Module>(&d.value)) n.approx_bytes += m->data_json.size();
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

  std::string note_summary() {
    std::lock_guard<std::mutex> l(mu_);
    return note_summary_;
  }

  RunStatusData request_cancel(bool interrupted) {
    std::lock_guard<std::mutex> l(mu_);
    // Once finalize() has taken the final status it is on its way to the history:
    // a "cancelling" written now would land after it and stay as the run's state.
    if (interrupted) detaching_.store(true);
    if (!interrupted && !summary_.status.terminal() && !finishing_) explicit_cancel_.store(true);
    if (!summary_.status.terminal() && !cancel_ && !finishing_) {
      cancel_ = true;
      interrupted_ = interrupted;
      for (auto& node : nodes_)
        if (node.cancellation) node.cancellation->stop.store(true);
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
  };
  struct TokenGroup {
    std::string port;
    bool execution_list = false;
    std::vector<Token> members;
    uint64_t seq = 0;
    int64_t arrived_at = now_ms();
  };
  struct BatchCancellation {
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
  };
  struct NodeRt {
    const Node* node = nullptr;
    size_t idx = 0;
    bool in_targets = false;
    bool start = false;
    bool start_fired = false;
    std::map<std::string, std::deque<TokenGroup>> queues;  // connected inputs only
    uint64_t invocations = 0;
    uint64_t recorded_invocations = 0;
    uint64_t activations = 0;
    bool running = false;
    bool halted = false;  // failed hard (e.g. maxRuns) and no longer scheduled
    bool saved_inputs = false;  // some of its inputs are saved values, not this run's
    CardState last = CardState::not_started;
    std::optional<FailureData> last_failure;
    std::optional<CardBuildPlan> build;
    std::shared_ptr<BatchCancellation> cancellation;
    uint64_t display_rev = 0;
    std::optional<DisplayData> display;
    // Groups of the latest activation, saved at the end of the run.
    std::optional<std::pair<uint64_t, std::vector<SavedInputGroup>>> last_inputs;
    std::map<std::string, std::pair<uint64_t, SavedInputGroup>> last_port_inputs;
  };
  struct Work {
    size_t node = 0;
    uint64_t invocation = 0;
    std::vector<Token> inputs;
    bool from_saved = false;  // single-Card run on the saved inputs
    int64_t ready_at = now_ms();  // all inputs there (or the start fired)
    uint64_t activation = 0;
    std::vector<TokenGroup> groups;
    std::shared_ptr<BatchCancellation> cancellation;
    std::optional<InvocationSummaryData> prepared;
    const CardBuild* implementation = nullptr;
    PreparedExecution* execution = nullptr;
    std::optional<uint32_t> batch_index, batch_size;
    bool execution_list = false;
    std::optional<std::pair<std::string, std::string>> out_alias;
    std::optional<DisplayData> display;
    std::optional<FailureData> preparation_failure;
  };

  // ---- persistence helpers -------------------------------------------------

  // A history, log or artifact write that did not commit: the run cannot
  // succeed, because its records would be incomplete.
  void persistence_lost(const std::string& what) {
    std::lock_guard<std::mutex> l(persist_mu_);
    if (!persist_failure_)
      persist_failure_ = FailureData::make(FailureKind::persistence_failed, "saving the run history failed: " + what);
  }

  std::optional<FailureData> persist_failure() {
    std::lock_guard<std::mutex> l(persist_mu_);
    return persist_failure_;
  }

  void persist_status_locked() {
    auto s = summary_.status;
    svc_.history().submit([s](sql::Db& db) { History::update_run(db, s); },
                          [self = shared_from_this(), s](bool ok) {
                            if (!ok) self->persistence_lost("run status");
                            self->svc_.server()->broadcast(status_note(s, false));
                          });
  }

  void set_state(RunState st) {
    std::lock_guard<std::mutex> l(mu_);
    if (summary_.status.state == st || summary_.status.state == RunState::cancelling) return;
    summary_.status.state = st;
    if (st == RunState::running && !summary_.status.started_at) summary_.status.started_at = now_ms();
    persist_status_locked();
  }

  // The start of an invocation is persisted (it becomes an interrupted
  // failure after a crash); later stage changes are kept in memory for
  // queries and announced in the same order as the start.
  void card_status(const InvocationSummaryData& s, bool persist = false) {
    svc_.history().set_live(s);
    svc_.history().submit([s, persist](sql::Db& db) { if (persist) History::put_status(db, s); },
                          [self = shared_from_this(), s, persist](bool ok) {
                            if (!ok && persist) self->persistence_lost("progress of " + path_key(s.card));
                            self->svc_.server()->broadcast(card_note(s));
                          });
  }

  void journal(const char* phase, const char* event, const std::optional<CardPath>& card,
               std::optional<uint64_t> inv, const std::string& message) {
    std::string line = journal_line(phase, event, card, inv, message);
    auto ends = std::make_shared<std::vector<LogEnd>>();
    std::string run = run_id_;
    svc_.history().submit(
        [&h = svc_.history(), line, card, inv, run, ends](sql::Db& db) {
          if (card) *ends = h.append_log(db, run, *card, *inv, LogStream::journal, line);
          *ends = card ? *ends : h.append_run_journal(db, run, line);
        },
        [self = shared_from_this(), ends](bool ok) {
          if (!ok) self->persistence_lost("journal");
          if (ok)
            for (const auto& e : *ends) self->svc_.server()->broadcast(log_note(e));
        });
  }

  void card_log(const CardPath& card, uint64_t inv, LogStream stream, const std::string& text) {
    auto ends = std::make_shared<std::vector<LogEnd>>();
    std::string run = run_id_;
    svc_.history().submit(
        [&h = svc_.history(), card, inv, stream, text, run, ends](sql::Db& db) {
          *ends = h.append_log(db, run, card, inv, stream, text);
        },
        [self = shared_from_this(), ends](bool ok) {
          if (!ok) self->persistence_lost("log");
          if (ok)
            for (const auto& e : *ends) self->svc_.server()->broadcast(log_note(e));
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
    std::set<std::string> seeded;
    for (const auto& s : plan.seeds) seeded.insert(path_key(s));
    for (const auto& s : plan.starts)
      if (!seeded.count(path_key(s))) nodes_[res.index.at(path_key(s))].start = true;
    for (const auto& s : plan.seeds) {
      NodeRt& n = nodes_[res.index.at(path_key(s))];
      std::set<std::string> outside;
      for (const auto& e : res.edges)
        if (e.to == n.idx && !nodes_[e.from].in_targets) outside.insert(e.to_port);
      seed_saved(n, outside);
    }
    for (const auto& j : plan.joins) seed_saved(nodes_[res.index.at(path_key(j.card))], j.saved_ports);
  }

  auto saved_group(const NodeRt& node, const SavedInputGroup& saved, uint64_t activation) -> TokenGroup {
    const auto* port = node.node->input(saved.port);
    if (!port || !port->type) throw Failed(FailureKind::type_mismatch, "saved input no longer matches " + saved.port);
    TokenGroup group{saved.port, saved.execution_list, {}, ++token_seq_};
    for (const auto& value : saved.values) {
      if (value.kind != port->kind || !value.type || !types::compatible(*port->type, *value.type))
        throw Failed(FailureKind::type_mismatch, "saved input no longer matches " + saved.port);
      if (value.kind != PortKind::FlatBuffer) {
        const auto path = path_value(value);
        std::error_code ec;
        if (!path || !stdfs::exists(*path, ec))
          throw Failed(FailureKind::input_missing, "the saved file for " + saved.port + " no longer exists");
      }
      group.members.push_back({value, {}, activation});
    }
    return group;
  }

  auto missing_saved_inputs(const Node& node) -> Failed {
    const auto card = path_key(node.path);
    return Failed(FailureKind::input_missing, "no saved inputs for " + card +
                  "; run the workflow from its trigger Card once first (it saves every Card's inputs)", {{"card", card}});
  }

  auto reject_start(NodeRt& node, const FailureData& failure) -> void {
    start_failure_ = failure;
    node.halted = true;
    node.last = CardState::failed;
    node.last_failure = failure;
  }

  // Queue one complete saved group per external port. Required groups are
  // from the same saved activation, unless eachInput consumes them separately.
  void seed_saved(NodeRt& node, const std::set<std::string>& outside) {
    try {
      const auto last = svc_.history().read([&](sql::Db& db) { return History::last_input(db, node.node->path); });
      if (!last) throw missing_saved_inputs(*node.node);
      for (const auto& port : outside) {
        std::optional<History::LastInput> one;
        if (node.node->settings.any_mode)
          one = svc_.history().read([&](sql::Db& db) { return History::last_port_input(db, node.node->path, port); });
        const auto& prior = one ? *one : *last;
        const auto group = std::ranges::find(prior.groups, port, &SavedInputGroup::port);
        if (group == prior.groups.end())
          throw Failed(FailureKind::input_missing, "the saved inputs have no " + port);
        node.queues[port].push_back(saved_group(node, *group, prior.invocation));
        node.saved_inputs = true;
        seed_notes_.push_back(path_key(node.node->path) + "." + port + " uses the saved values from run " +
                              prior.run_id + ", activation " + std::to_string(prior.invocation));
      }
    } catch (const Failed& failure) {
      reject_start(node, failure.failure);
    }
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
    Work w;
    w.node = n.idx;
    if (n.start && !n.start_fired) {
      if (!downstream_has_space(n)) return std::nullopt;
      n.start_fired = true;
      if (!n.queues.empty()) {
        try {
          const auto last = svc_.history().read([&](sql::Db& db) { return History::last_input(db, n.node->path); });
          if (!last) throw missing_saved_inputs(*n.node);
          for (const auto& port : n.node->inputs)
            if (!port.optional && std::ranges::find(last->groups, port.name, &SavedInputGroup::port) == last->groups.end())
              throw Failed(FailureKind::input_missing, "the saved inputs have no value for " + port.name);
          for (const auto& group : last->groups) w.groups.push_back(saved_group(n, group, last->invocation));
          w.from_saved = true;
          n.saved_inputs = true;
        } catch (const Failed& failure) {
          reject_start(n, failure.failure);
          return std::nullopt;
        }
      }
      return w;
    }
    if (n.queues.empty()) return std::nullopt;
    if (n.node->settings.any_mode) {
      std::deque<TokenGroup>* best = nullptr;
      for (auto& [_, queue] : n.queues)
        if (!queue.empty() && (!best || queue.front().seq < best->front().seq)) best = &queue;
      if (!best || !downstream_has_space(n)) return std::nullopt;
      w.groups.push_back(std::move(best->front()));
      best->pop_front();
    } else {
      for (auto& [_, queue] : n.queues)
        if (queue.empty()) return std::nullopt;
      if (!downstream_has_space(n)) return std::nullopt;
      for (auto& [_, queue] : n.queues) {
        w.groups.push_back(std::move(queue.front()));
        queue.pop_front();
      }
    }
    w.ready_at = 0;
    for (const auto& group : w.groups) w.ready_at = std::max(w.ready_at, group.arrived_at);
    return w;
  }

  void main() {
    stdfs::create_directories(run_dir_);
    {
      std::lock_guard lock(mu_);
      prepare_graph();
    }
    set_state(RunState::preparing);
    journal("run", "started", std::nullopt, std::nullopt, "run started");
    for (const auto& note : seed_notes_) journal("run", "seeded", std::nullopt, std::nullopt, note);
    for (uint32_t lane = 0; lane < std::min(p_.max_concurrency, kMaxParallelWorkers); ++lane)
      workers_.emplace_back([this, lane] { worker_loop(lane); });
    // Coordinators wait for complete activations without occupying a worker slot.
    for (uint32_t lane = 0; lane < p_.max_concurrency; ++lane)
      pool_.emplace_back([this, lane] { pool_loop(lane); });
    {
      std::unique_lock<std::mutex> l(mu_);
      while (true) {
        bool dispatched = false;
        if (!cancel_) {
          bool progressed = true;
          while (progressed && running_ < p_.max_concurrency) {
            progressed = false;
            for (auto& n : nodes_) {
              if (running_ >= p_.max_concurrency) break;
              auto w = take_ready(n);
              if (!w) continue;
              n.running = true;
              w->activation = ++n.activations;
              w->cancellation = std::make_shared<BatchCancellation>();
              n.cancellation = w->cancellation;
              ++running_;
              tasks_.push_back(std::move(*w));
              task_cv_.notify_one();
              progressed = true;
              dispatched = true;
            }
          }
        }
        if (running_ == 0 && (cancel_ || !any_ready_locked())) break;
        if (running_ == 0 && !dispatched) {
          // Inputs are waiting but nothing runs and nothing can start: only a
          // full downstream queue holds them back, and with no invocation in
          // flight it can never drain. This is a stall, not a temporary wait.
          stall_failure_ = stall_reason_locked();
          break;
        }
        cv_.wait_for(l, std::chrono::milliseconds(200));
      }
      pool_stop_ = true;
    }
    task_cv_.notify_all();
    for (auto& t : pool_) t.join();
    worker_cv_.notify_all();
    for (auto& t : workers_) t.join();
    // Workers stay in the project's pool (resident across runs).
    finalize();
  }

  FailureData stall_reason_locked() {
    for (auto& n : nodes_) {
      if (!n.in_targets || n.running || n.halted || n.node->is_virtual) continue;
      for (const auto& out : n.node->outputs) {
        auto it = out_edges_.find({n.idx, out.name});
        if (it == out_edges_.end()) continue;
        for (const auto& [to, port] : it->second)
          if (nodes_[to].in_targets && nodes_[to].queues[port].size() >= kQueueCapacity)
            return FailureData::make(
                FailureKind::execution_limit,
                "the run cannot progress: " + path_key(nodes_[to].node->path) + "." + port + " holds " +
                    std::to_string(kQueueCapacity) + " unconsumed values while its other inputs never arrive",
                {{"card", path_key(nodes_[to].node->path)}, {"port", port}});
      }
    }
    return FailureData::make(FailureKind::execution_limit, "the run cannot progress");
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
      invoke_batch(w, lane);
      {
        std::lock_guard<std::mutex> l(mu_);
        nodes_[w.node].running = false;
        nodes_[w.node].cancellation.reset();
        --running_;
      }
      cv_.notify_all();
    }
  }

  // ---- invocation -----------------------------------------------------------

  struct Produced {
    std::vector<Value> outputs;
    std::optional<FailureData> failure;
    std::vector<std::pair<LogStream, std::string>> remote_logs;
    bool detached = false;
    std::optional<InvocationSummaryData> unpublished;
  };

  auto worker_loop(uint32_t lane) -> void {
    while (true) {
      std::function<void(uint32_t)> task;
      {
        std::unique_lock lock(mu_);
        worker_cv_.wait(lock, [&] { return pool_stop_ || !worker_tasks_.empty(); });
        if (worker_tasks_.empty()) return;
        task = std::move(worker_tasks_.front());
        worker_tasks_.pop_front();
      }
      task(lane);
    }
  }

  auto publish_batch(const RunBatchData& batch, const std::vector<Produced>& members = {}) -> void {
    svc_.history().write_sync([&](sql::Db& db) {
      History::put_batch(db, batch);
      if (batch.failed) return;
      for (const auto& member : members) {
        if (!member.unpublished) continue;
        auto summary = *member.unpublished;
        for (const auto& output : member.outputs) summary.outputs_sent.push_back(output.name);
        History::update_detail(db, summary);
      }
    });
    svc_.server()->broadcast(run_event_note(run_id_, [batch](FBB& b) {
      const auto data = wire::run_batch(b, batch);
      return std::make_pair(R::RunEventValue::RunBatchChanged, R::CreateRunBatchChanged(b, data).Union());
    }));
  }

  auto prepare_code(NodeRt& rt, Work& work, uint32_t max_implementations, uint32_t lane,
                    std::unique_ptr<PreparedExecution>& prepared) -> const CardBuildPlan& {
    const auto& node = *rt.node;
    auto& s = *work.prepared;
    auto& execution = svc_.execution_service(node.settings.execution_service);
    BuildRequest request;
    request.run_id = run_id_;
    request.node = &node;
    request.doc = doc_;
    request.run_dir = run_dir_;
    request.jobs = p_.nix_jobs;
    request.cores = p_.nix_cores;
    request.timeout_seconds = node.settings.build_timeout_seconds;
    request.max_implementations = max_implementations;
    request.cancel = &work.cancellation->stop;
    // Resolve worker kind and system before any Card or source evaluation.
    prepared = execution.prepare(node, invocation_dir(node, work.invocation), request);
    if (rt.build && execution.reuses_builds()) {
      stage_begin(s, Phase::evaluate, StageOutcome::reused);
      stage_begin(s, Phase::build, StageOutcome::reused);
      return *rt.build;
    }
    request.log = [&](const std::string& line) { card_log(node.path, s.invocation, LogStream::build, line); };
    auto started = now_ms();
    const auto trace_phase = [&] {
      svc_.server()->broadcast(timeline_note(run_id_, node.path, s.invocation, *s.phase, lane, started, now_ms()));
    };
    request.on_phase = [&](Phase phase) {
      trace_phase();
      started = now_ms();
      stage_begin(s, phase);
      card_status(s);
    };
    set_state(RunState::preparing);
    rt.build = svc_.nix().build(request);
    trace_phase();
    if (execution.reuses_builds()) {
      journal("build", "output", node.path, s.invocation,
              rt.build->common.cached ? "evaluated; the store already had the result" : "built");
      if (rt.build->common.cached) stage_begin(s, Phase::build, StageOutcome::cached);
    }
    return *rt.build;
  }

  auto batch_limit(const Node& node, std::string message) -> Failed {
    return Failed(FailureKind::execution_limit, std::move(message), {{"card", path_key(node.path)}});
  }

  auto input_combinations(const Node& node, const std::vector<TokenGroup>& groups) -> uint32_t {
    size_t bytes = 0;
    for (const auto& group : groups) {
      for (const auto& member : group.members) {
        if (member.value.bytes.size() > kMaxBatchBytes - bytes)
          throw batch_limit(node, "input batch exceeds 32 MiB");
        bytes += member.value.bytes.size();
      }
    }
    if (node.kind == Template::Collect) return 1;
    if (std::ranges::any_of(groups, [](const auto& group) { return group.members.empty(); })) return 0;
    uint32_t count = 1;
    for (const auto& group : groups) {
      if (group.members.size() > kMaxExecutionBatchSize / count)
        throw batch_limit(node, "input and run list expansion exceeds 256 combinations");
      count *= static_cast<uint32_t>(group.members.size());
    }
    return count;
  }

  auto batch_inputs(const Node& node, const std::vector<TokenGroup>& groups, uint32_t index) -> std::vector<Token> {
    std::vector<Token> inputs;
    if (node.kind == Template::Collect) {
      if (!groups.empty()) inputs = groups.front().members;
      return inputs;
    }
    for (auto it = groups.rbegin(); it != groups.rend(); ++it) {
      inputs.push_back(it->members.at(index % it->members.size()));
      index /= static_cast<uint32_t>(it->members.size());
    }
    std::ranges::reverse(inputs);
    return inputs;
  }

  auto remember_inputs(NodeRt& rt, const Work& batch) -> void {
    if (batch.from_saved || (rt.node->kind != Template::Code && batch.groups.empty())) return;
    std::vector<SavedInputGroup> saved;
    for (const auto& group : batch.groups) {
      SavedInputGroup values{group.port, group.execution_list, {}};
      for (const auto& member : group.members) values.values.push_back(member.value);
      rt.last_port_inputs[group.port] = std::make_pair(batch.activation, values);
      saved.push_back(std::move(values));
    }
    rt.last_inputs = std::make_pair(batch.activation, std::move(saved));
  }

  auto route_batch(NodeRt& rt, const RunBatchData& batch, const std::vector<Produced>& members, uint64_t first) -> void {
    for (const auto& output : batch.outputs) {
      if (!output.execution_list && output.count == 0) continue;
      const auto edge = out_edges_.find({rt.idx, output.port});
      if (edge == out_edges_.end()) continue;
      TokenGroup group{output.port, output.execution_list, {}};
      for (size_t index = 0; index < members.size(); ++index) {
        const auto value = std::ranges::find(members[index].outputs, output.port, &Value::name);
        if (value != members[index].outputs.end())
          group.members.push_back({*value, rt.node->path, first + index});
      }
      for (const auto& [to, port] : edge->second) {
        if (!nodes_[to].in_targets) continue;
        group.port = port;
        group.seq = ++token_seq_;
        for (auto& member : group.members) member.value.name = port;
        nodes_[to].queues.at(port).push_back(group);
      }
    }
  }

  auto invoke_batch(Work& work, uint32_t lane) -> void {
    auto& rt = nodes_[work.node];
    const auto& node = *rt.node;
    const auto first = rt.invocations + 1;
    work.invocation = first;
    RunBatchData batch;
    batch.run_id = run_id_;
    batch.card = node.path;
    batch.activation = work.activation;
    batch.execution_list = node.kind != Template::Collect &&
        std::ranges::any_of(work.groups, &TokenGroup::execution_list);
    std::unique_ptr<PreparedExecution> prepared;
    std::vector<CardBuild> implementations;
    uint32_t count = 0;
    bool reserved = false;
    const bool code = node.kind == Template::Code;
    std::optional<FailureData> failure;
    try {
      if (work.activation > node.settings.max_runs)
        throw Failed(FailureKind::execution_limit, "maxRuns " + std::to_string(node.settings.max_runs) + " exceeded",
                     {{"card", path_key(node.path)}, {"max_runs", std::to_string(node.settings.max_runs)}});
      count = input_combinations(node, work.groups);
      remember_inputs(rt, work);
      if (code && count != 0) {
        work.prepared = new_invocation(work);
        auto& s = *work.prepared;
        s.state = CardState::running;
        stage_begin(s, Phase::prepare);
        card_status(s, true);
        journal("prepare", "started", node.path, s.invocation, "invocation started");
        const auto& plan = prepare_code(rt, work, kMaxExecutionBatchSize / count, lane, prepared);
        batch.execution_list |= plan.execution_list;
        if (plan.implementations.size() > kMaxExecutionBatchSize / count)
          throw batch_limit(node, "input and run list expansion exceeds 256 combinations");
        for (size_t index = 0; index < plan.implementations.size(); ++index)
          implementations.push_back(plan.implementation(index));
        count *= static_cast<uint32_t>(implementations.size());
      }
      batch.invocation_count = count;
      if (code) {
        std::lock_guard lock(mu_);
        if (count > kMaxRunWorkers - worker_budget_)
          throw batch_limit(node, "run exceeds the limit of 4096 worker invocations");
        worker_budget_ += count;
        reserved = true;
      }
      publish_batch(batch);
      if (count != 0 && prepared) prepared->stage(work.cancellation->stop);
    } catch (const Failed& error) {
      failure = error.failure;
    } catch (const std::exception& error) {
      failure = FailureData::make(FailureKind::internal_error, error.what());
    }

    std::vector<Produced> members(failure ? 0 : count);
    std::atomic<uint32_t> started{0};
    std::mutex outcome_mu;
    size_t bytes = 0;
    auto execute = [&](uint32_t index, uint32_t worker_lane) {
      // The provisional preparation record belongs to the first member.
      if (index != 0 && work.cancellation->stop.load()) return;
      started.fetch_add(1);
      Work member;
      member.node = work.node;
      member.activation = work.activation;
      member.invocation = first + index;
      member.ready_at = work.ready_at;
      member.cancellation = work.cancellation;
      member.batch_index = index + 1;
      member.batch_size = count;
      member.execution = prepared.get();
      member.execution_list = batch.execution_list;
      if (index == 0) member.prepared = std::move(work.prepared);
      if (code) member.implementation = &implementations.at(index % implementations.size());
      const auto inputs_index = code ? index / static_cast<uint32_t>(implementations.size()) : index;
      member.inputs = batch_inputs(node, work.groups, inputs_index);
      auto produced = invoke(member, worker_lane);
      bool stop_siblings = false;
      {
        std::lock_guard lock(outcome_mu);
        if (!produced.failure && !produced.detached) {
          for (const auto& output : produced.outputs) {
            if (output.bytes.size() > kMaxBatchBytes - bytes) {
              produced.failure = FailureData::make(FailureKind::execution_limit, "output batch exceeds 32 MiB",
                                                   {{"card", path_key(node.path)}});
              break;
            }
            bytes += output.bytes.size();
          }
        }
        if ((produced.failure || produced.detached) &&
            (!failure || (failure->kind == FailureKind::cancelled && produced.failure &&
                          produced.failure->kind != FailureKind::cancelled))) {
          failure = produced.failure.value_or(FailureData::make(FailureKind::interrupted, "batch interrupted"));
          work.cancellation->failed.store(!produced.detached && !detaching_.load());
          work.cancellation->stop.store(true);
          stop_siblings = work.cancellation->failed.load();
        }
        if (failure) produced.outputs.clear();
        members[index] = std::move(produced);
      }
      if (stop_siblings && prepared) {
        try {
          svc_.history().write_sync([&](sql::Db& db) {
            prepared->cancel_batch(db, run_id_, node.path, work.activation);
          });
        } catch (const Failed&) {
          persistence_lost("cancelling batch of " + path_key(node.path));
        }
      }
    };

    if (failure) {
      work.preparation_failure = failure;
      invoke(work, lane);
      ++rt.invocations;
      ++rt.recorded_invocations;
    } else if (count == 0) {
      if (work.prepared) {
        // Build logs for an empty plan keep their own ID.
        ++rt.invocations;
        const auto& s = *work.prepared;
        svc_.history().clear_live(s);
        try {
          svc_.history().write_sync([&](sql::Db& db) {
            db.prepare("DELETE FROM statuses WHERE run_id=? AND card=? AND invocation=?")
                .bind(1, s.run_id).bind(2, path_key(s.card)).bind(3, s.invocation).run();
          });
        } catch (const Failed&) {
          persistence_lost("clearing preparation of " + path_key(node.path));
        }
      }
    } else if (!code) {
      for (uint32_t index = 0; index < count && !work.cancellation->stop.load(); ++index) execute(index, lane);
      rt.invocations += started.load();
    } else {
      std::vector<std::future<void>> pending;
      {
        std::lock_guard lock(mu_);
        const bool serial = node.settings.reuse_worker;
        const auto jobs = serial ? 1u : count;
        for (uint32_t index = 0; index < jobs; ++index) {
          auto task = std::make_shared<std::packaged_task<void(uint32_t)>>([&, index, serial](uint32_t worker_lane) {
            if (serial) {
              for (uint32_t member = 0; member < count && !work.cancellation->stop.load(); ++member)
                execute(member, worker_lane);
              if (started.load() == 0) execute(0, worker_lane);
            } else {
              execute(index, worker_lane);
            }
          });
          pending.push_back(task->get_future());
          worker_tasks_.push_back([task](uint32_t worker_lane) { (*task)(worker_lane); });
        }
      }
      worker_cv_.notify_all();
      for (auto& future : pending) {
        try {
          future.get();
        } catch (const std::exception& error) {
          std::lock_guard lock(outcome_mu);
          if (!failure) failure = FailureData::make(FailureKind::internal_error, error.what());
        }
      }
      // Assigned IDs are stable even when cancellation skipped queued members.
      rt.invocations += count;
    }
    rt.recorded_invocations += started.load();
    if (reserved) {
      std::lock_guard lock(mu_);
      worker_budget_ -= count - started.load();
    }
    // Cancellation and publication are ordered by the same mutex. A
    // committed batch is complete and routes all its output groups.
    std::lock_guard lock(mu_);
    if (cancel_.load()) failure = FailureData::make(FailureKind::cancelled, "run cancelled");

    batch.complete = true;
    batch.failed = failure.has_value();
    if (!batch.failed) {
      for (const auto& port : node.outputs) {
        uint32_t sent = 0;
        for (const auto& member : members)
          if (std::ranges::find(member.outputs, port.name, &Value::name) != member.outputs.end()) ++sent;
        batch.outputs.push_back({port.name, sent, batch.execution_list});
      }
    }
    try {
      publish_batch(batch, members);
    } catch (const Failed&) {
      persistence_lost("batch of " + path_key(node.path));
      failure = persist_failure();
    }
    if (failure) {
      const bool cancelled = failure->kind == FailureKind::cancelled;
      rt.last = cancelled ? CardState::cancelled : CardState::failed;
      rt.last_failure = failure;
      if (!first_failure_ && !cancelled) first_failure_ = failure;
      if (failure->kind == FailureKind::execution_limit) rt.halted = true;
    } else {
      rt.last = CardState::succeeded;
      route_batch(rt, batch, members, first);
    }
  }

  auto new_invocation(const Work& w) -> InvocationSummaryData {
    const auto& node = *nodes_[w.node].node;
    InvocationSummaryData s;
    s.run_id = run_id_;
    s.card = node.path;
    s.params_json = NixRunner::params_json(node, false);
    s.invocation = w.invocation;
    s.batch_activation = w.activation;
    s.started_at = now_ms();
    s.ready_at = w.ready_at;
    return s;
  }

  auto invoke(Work& w, uint32_t lane) -> Produced {
    NodeRt& rt = nodes_[w.node];
    const Node& node = *rt.node;
    const bool prepared = w.prepared.has_value();
    auto s = prepared ? std::move(*w.prepared) : new_invocation(w);
    s.batch_index = w.batch_index;
    s.batch_size = w.batch_size;
    if (w.implementation) s.implementation_index = w.implementation->implementation_index;
    std::vector<Value> inputs;
    std::vector<InputSourceData> sources;
    for (const auto& t : w.inputs) {
      inputs.push_back(t.value);
      if (!t.from.empty()) sources.push_back({t.value.name, t.from, t.from_inv});
    }
    Produced out;
    if (w.preparation_failure) {
      out.failure = w.preparation_failure;
    } else {
      s.state = CardState::running;
      if (!prepared) {
        if (node.kind == Template::Code) {
          stage_begin(s, Phase::prepare, StageOutcome::reused);
          stage_begin(s, Phase::evaluate, StageOutcome::reused);
          stage_begin(s, Phase::build, StageOutcome::reused);
        } else {
          stage_begin(s, Phase::execute);
        }
        card_status(s, true);
        journal(phase_name(*s.phase), "started", node.path, w.invocation, "invocation started");
      }
      try {
        if (w.cancellation->stop.load()) throw Failed(FailureKind::cancelled, "batch cancelled before execution");
        if (node.kind == Template::Code) out = run_code(rt, w, s, inputs, sources, lane);
        else out = run_builtin(rt, w, s, inputs);
      } catch (const ExecutionDetached&) {
        svc_.history().clear_live(s);
        out.detached = true;
        return out;
      } catch (const Failed& f) {
        out.failure = f.failure;
      } catch (const std::exception& e) {
        out.failure = FailureData::make(FailureKind::internal_error, e.what());
      }
      if (!out.failure) {
        stage_begin(s, Phase::validate);
        const auto started = now_ms();
        try {
          validate_outputs(node, w, out.outputs, inputs);
        } catch (const Failed& f) {
          out.failure = f.failure;
          out.outputs.clear();
        }
        svc_.server()->broadcast(timeline_note(run_id_, node.path, w.invocation, Phase::validate, lane, started, now_ms()));
      }
    }
    bool was_cancel = out.failure && out.failure->kind == FailureKind::cancelled;
    if (out.failure) stage_close(s, was_cancel ? StageOutcome::cancelled : StageOutcome::failed);
    else if (!w.execution_list)
      for (const auto& v : out.outputs) s.outputs_sent.push_back(v.name);
    s.ended_at = now_ms();
    s.duration_ms = static_cast<uint64_t>(*s.ended_at - *s.started_at);
    if (out.failure && out.failure->args.empty()) out.failure->args.emplace_back("card", path_key(node.path));
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
      if (node.kind != Template::Collect || inputs.size() <= 1) rec.inputs = inputs;
      rec.outputs = out.outputs;
    }
    rec.display = w.display;
    if (s.state == CardState::succeeded) {
      // Records, artifacts and saved values are written in the save stage.
      stage_begin(s, Phase::save);
      s.state = CardState::running;
      card_status(s);
      s.state = CardState::succeeded;
    }
    // Artifacts are copied before the completion becomes visible.
    std::optional<ArtifactData> remote_artifact;
    if (!out.failure && node.settings.retain_artifact && node.settings.artifact_output) {
      for (auto& v : out.outputs)
        if (v.name == *node.settings.artifact_output) {
          try {
            if (s.execution_job_id.empty()) retain_artifact(node, w.invocation, v, w.cancellation->stop);
            else remote_artifact = prepare_remote_artifact(run_dir_, invocation_dir(node, w.invocation) / "out", s, v);
          } catch (const Failed& f) {
            was_cancel = f.failure.kind == FailureKind::cancelled;
            s.state = was_cancel ? CardState::cancelled : CardState::failed;
            s.failure = f.failure;
            rec.summary = s;
            out.failure = f.failure;
          } catch (const std::exception& error) {
            s.state = CardState::failed;
            s.failure = FailureData::make(FailureKind::output_invalid, error.what());
            rec.summary = s;
            out.failure = s.failure;
          }
        }
      if (out.failure) {
        s.outputs_sent.clear();
        out.outputs.clear();
      }
      if (rec.record_io) rec.outputs = out.outputs;
    }
    // The completion is durable before it is announced or its outputs move on.
    // The record holds the save stage as done; a failed write reports it failed.
    {
      auto done = s;
      stage_close(done, was_cancel ? StageOutcome::cancelled :
                                      out.failure ? StageOutcome::failed : StageOutcome::done);
      rec.summary.stages = done.stages;
      rec.summary.outputs_sent = done.outputs_sent;
      rec.summary.worker_reused = done.worker_reused;
      rec.summary.emulator = done.emulator;
      try {
        History& h = svc_.history();
        std::map<LogStream, std::vector<LogEnd>> remote_log_ends;
        const bool failed_batch = out.failure && w.execution && !detaching_.load();
        if (failed_batch) {
          w.cancellation->failed.store(true);
          w.cancellation->stop.store(true);
        }
        h.write_sync([&](sql::Db& db) {
          if (failed_batch) w.execution->cancel_batch(db, run_id_, node.path, w.activation);
          if (remote_artifact && !out.failure) History::put_artifact(db, *remote_artifact);
          for (const auto& [stream, text] : out.remote_logs)
            remote_log_ends[stream] = h.append_log(db, s.run_id, s.card, s.invocation, stream, text);
          h.complete_invocation(db, rec);
        });
        for (const auto& [_, ends] : remote_log_ends)
          for (const auto& end : ends) svc_.server()->broadcast(log_note(end));
        // The record holds the save stage as closed just before its commit.
        s.stages = done.stages;
        h.clear_live(s);
      } catch (const Failed&) {
        svc_.history().clear_live(s);
        persistence_lost("invocation " + std::to_string(w.invocation) + " of " + path_key(node.path));
        stage_close(s, StageOutcome::failed);
        s.state = CardState::failed;
        s.failure = persist_failure();
        s.outputs_sent.clear();
        out.failure = s.failure;
        out.outputs.clear();
      }
    }
    svc_.server()->broadcast(card_note(s));
    journal(phase_name(s.phase.value_or(Phase::execute)),
            s.state == CardState::succeeded ? "succeeded" : s.state == CardState::cancelled ? "cancelled" : "failed",
            node.path, w.invocation,
            out.failure ? std::string(failure_kind_name(out.failure->kind)) + ": " + out.failure->diagnostic.value_or("") : "ok");
    if (w.execution_list && !out.failure) out.unpublished = std::move(s);
    return out;
  }

  // Save all groups from the latest activation. A per-port snapshot lets an
  // eachInput join reuse the last arrival on each outside port independently.
  auto save_last_inputs(const Node& node, uint64_t inv, const std::vector<SavedInputGroup>& inputs,
                        const std::map<std::string, std::pair<uint64_t, SavedInputGroup>>& ports) -> void {
    if (cancel_.load()) return;
    const auto base = svc_.store().state_dir() / "inputs" / path_key(node.path);
    const auto snapshot = run_id_ + "-" + std::to_string(inv);
    const auto directory = base / snapshot;
    std::vector<SavedInputGroup> saved;
    std::vector<std::pair<uint64_t, SavedInputGroup>> per_port;
    std::vector<stdfs::path> port_copies;
    const auto remove_candidates = [](const stdfs::path& main, const std::vector<stdfs::path>& candidates) {
      std::error_code error;
      stdfs::remove_all(main, error);
      for (const auto& candidate : candidates) stdfs::remove_all(candidate, error);
    };
    const auto check_cancel = [&] {
      if (cancel_.load()) throw Failed(FailureKind::cancelled, "saving inputs was cancelled");
    };
    const auto copy_into = [&](const Value& value, const stdfs::path& where) -> Value {
      check_cancel();
      if (value.kind == PortKind::FlatBuffer) return value;
      const auto source = path_value(value);
      if (!source) throw Failed(FailureKind::output_invalid, "input " + value.name + " has no path");
      const auto [root, rel] = beneath(*source);
      const auto destination = where / rel.filename();
      const auto copy_file = [&](const stdfs::path& file, const stdfs::path& to) {
        check_cancel();
        fs::copy_beneath(root, file, to, [&](std::string_view) { check_cancel(); });
      };
      if (value.kind == PortKind::File) {
        copy_file(rel, destination);
      } else {
        const auto listing = fs::list_beneath(root, rel);
        stdfs::create_directories(destination);
        for (const auto& child : listing.directories) {
          check_cancel();
          stdfs::create_directories(destination / child);
        }
        for (const auto& child : listing.files) copy_file(rel / child, destination / child);
      }
      return with_path(value, destination.string());
    };
    const auto copy_group = [&](const SavedInputGroup& group, const stdfs::path& where) -> SavedInputGroup {
      SavedInputGroup result{group.port, group.execution_list, {}};
      for (size_t index = 0; index < group.values.size(); ++index)
        result.values.push_back(copy_into(group.values[index], where / std::to_string(index)));
      return result;
    };
    try {
      for (const auto& group : inputs) saved.push_back(copy_group(group, directory / group.port));
      for (const auto& [name, p] : ports) {
        auto& candidate = port_copies.emplace_back(base / ("port-" + name) / snapshot);
        per_port.emplace_back(p.first, copy_group(p.second, candidate));
      }
      check_cancel();
    } catch (const std::exception& error) {
      remove_candidates(directory, port_copies);
      if (!cancel_.load()) persistence_lost("saved inputs of " + path_key(node.path) + ": " + error.what());
      return;
    }
    const auto card = node.path;
    const auto run = run_id_;
    svc_.history().submit(
        [card, run, inv, saved = std::move(saved), per_port = std::move(per_port)](sql::Db& db) {
          History::put_last_input(db, card, run, inv, saved);
          for (const auto& [activation, group] : per_port) History::put_last_port_input(db, card, run, activation, group);
        },
        [self = shared_from_this(), card, base, directory, port_copies = std::move(port_copies), remove_candidates](bool ok) {
          if (!ok) {
            remove_candidates(directory, port_copies);
            self->persistence_lost("saved inputs of " + path_key(card));
            return;
          }
          // Prior snapshots may still be read until the history commits.
          std::error_code error;
          for (const auto& entry : stdfs::directory_iterator(base, error))
            if (entry.path() != directory && !entry.path().filename().string().starts_with("port-"))
              stdfs::remove_all(entry.path(), error);
          for (const auto& candidate : port_copies)
            for (const auto& entry : stdfs::directory_iterator(candidate.parent_path(), error))
              if (entry.path() != candidate) stdfs::remove_all(entry.path(), error);
        });
  }

  auto validate_outputs(const Node& node, const Work& w, std::vector<Value>& outputs,
                        const std::vector<Value>& inputs) -> void {
    // Files received as inputs may be passed on unchanged (fork, branch, a
    // Card forwarding its input); anything else must be newly produced in the
    // invocation's own output directory.
    std::set<std::string> forwarded;
    for (const auto& in : inputs)
      if (in.kind != PortKind::FlatBuffer)
        if (auto p = path_value(in)) forwarded.insert(stdfs::path(*p).lexically_normal().string());
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
        // A path under the passed output descriptor is the output folder's real path.
        if (p && w.out_alias) {
          const auto& [alias, real] = *w.out_alias;
          if (*p == alias || p->starts_with(alias + "/")) {
            v = with_path(v, real + p->substr(alias.size()));
            p = path_value(v);
          }
        }
        auto out_dir = invocation_dir(node, w.invocation) / "out";
        std::error_code ec;
        bool is_forward = p && forwarded.count(stdfs::path(*p).lexically_normal().string());
        if (!p || !stdfs::path(*p).is_absolute() || (!is_forward && !fs::resolves_inside(out_dir, *p)))
          throw Failed(FailureKind::output_invalid,
                       "output " + v.name + " must point inside the output directory or forward an input",
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

  // A File / Directory value's path below the trees tsugumid keeps them in
  // (the runs, the saved inputs): read from there without following links,
  // since the worker that wrote it may still be running.
  std::pair<stdfs::path, stdfs::path> beneath(const std::string& p) const {
    auto state = svc_.store().state_dir();
    for (const auto& root : {state / "runs", state / "inputs"}) {
      auto rel = stdfs::path(p).lexically_normal().lexically_relative(root);
      if (!rel.empty() && *rel.begin() != ".." && rel != ".") return {root, rel};
    }
    throw Failed(FailureKind::output_invalid, "a file value outside the run's folders: " + p);
  }

  stdfs::path invocation_dir(const Node& node, uint64_t inv) {
    return run_dir_ / "work" / path_key(node.path) / std::to_string(inv);
  }

  auto retain_artifact(const Node& node, uint64_t inv, Value& v, const std::atomic<bool>& stop) -> void {
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
    const auto destination = run_dir_ / "artifacts" / a.artifact_id;
    const auto [root, rel] = beneath(*src);
    const auto check_cancel = [&] {
      if (stop.load()) throw Failed(FailureKind::cancelled, "retaining artifact was cancelled");
    };
    const auto retain_file = [&](const stdfs::path& file, const std::string& name) {
      check_cancel();
      crypto::Sha256 hash;
      const auto size = fs::copy_beneath(root, file, destination / name, [&](std::string_view chunk) {
        check_cancel();
        hash.update(chunk);
      });
      a.members.push_back({name, size, hash.finish_hex()});
    };
    try {
      if (v.kind == PortKind::File) {
        retain_file(rel, a.name);
      } else {
        const auto listing = fs::list_beneath(root, rel);
        if (!listing.links.empty())
          throw Failed(FailureKind::output_invalid, "artifact directory contains symlink " + listing.links.front());
        if (listing.files.size() > 1000)
          throw Failed(FailureKind::resource_limit, "artifact directory exceeds 1,000 files");
        stdfs::create_directories(destination);
        for (const auto& child : listing.directories) {
          check_cancel();
          stdfs::create_directories(destination / child);
        }
        for (const auto& child : listing.files) retain_file(rel / child, child);
      }
      check_cancel();
      svc_.history().write_sync([&](sql::Db& db) { History::put_artifact(db, a); });
    } catch (...) {
      std::error_code error;
      stdfs::remove_all(destination, error);
      throw;
    }
    v.artifact_id = a.artifact_id;
  }

  // ---- builtins --------------------------------------------------------------

  const Value* input(const std::vector<Value>& in, const std::string& name) {
    for (const auto& v : in)
      if (v.name == name) return &v;
    return nullptr;
  }

  auto run_builtin(NodeRt& rt, const Work& w, InvocationSummaryData& s, const std::vector<Value>& in) -> Produced {
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
      case Template::Input:
      case Template::Switch: {
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
              // Card-local by default; project: explicitly selects the Card's
              // own project, including when that project is an external.
              const bool project_relative = relv.value().starts_with("project:");
              const std::string file = project_relative ? relv.value().substr(8) : relv.value();
              const std::string rel = (project_relative ? node.project_dir : node.def_dir) + file;
              if (!fs::valid_relative_path(file) || !doc_->sources.count(rel))
                throw Failed(FailureKind::invalid_path,
                             "input file " + rel + " is not a project source; use a path relative to the Card's "
                             "definition folder, or project:<path> for its project root", {{"path", rel}});
              auto data = svc_.store().read_source(*doc_, rel);
              auto dest = invocation_dir(node, s.invocation) / "out" / stdfs::path(rel).filename();
              fs::write_atomic(dest, data, 0644);
              owned.push_back(schema::string_value(dest.string()));
              val = &owned.back();
            }
            members.emplace_back(prm.name, val);
          }
          std::string bytes;
          std::string err;
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
      case Template::Collect:
        p.outputs.push_back(collect_values(*node.input("in"), node.outputs.front(), in, svc_.store().state_dir(),
                                           invocation_dir(node, s.invocation) / "out", w.cancellation->stop));
        break;
      case Template::Merge: {
        if (in.size() != 1) throw Failed(FailureKind::input_missing, "merge consumes exactly one arrival");
        const Value& v = in.front();
        const auto& out = node.outputs.front();
        if (!out.type) throw Failed(FailureKind::output_invalid, "merge: the output type is not resolved");
        std::string err;
        auto bytes = types::wrap_in_union(*out.type, v.name, *v.type, v.bytes, &err);
        if (!bytes) throw Failed(FailureKind::output_invalid, "merge: " + err);
        p.outputs.push_back({"out", PortKind::FlatBuffer, out.type, std::move(*bytes), {}});
        break;
      }
      default:
        throw Failed(FailureKind::unsupported, std::string(template_name(node.kind)) + " cards cannot run");
    }
    svc_.server()->broadcast(timeline_note(run_id_, node.path, s.invocation, Phase::execute, 0, t0, now_ms()));
    return p;
  }

  auto on_display(NodeRt& rt, Work& w, DisplayData d) -> void {
    if (d.card != rt.node->path || d.run_id != run_id_)
      throw Failed(FailureKind::output_invalid, "display addressed another card or run");
    if (auto err = d.validate()) throw Failed(FailureKind::output_invalid, *err);
    {
      std::lock_guard<std::mutex> l(mu_);
      d.revision = ++rt.display_rev;
      rt.display = d;
      w.display = d;
    }
    sched_.publish_display(d);
  }

  // ---- code Cards -------------------------------------------------------------

  auto run_code(NodeRt& rt, Work& w, InvocationSummaryData& s, const std::vector<Value>& in,
                const std::vector<InputSourceData>& sources, uint32_t lane) -> Produced {
    const Node& node = *rt.node;
    const auto inv = s.invocation;
    auto result = w.execution->execute(
        {node, *w.implementation, in, sources, s, invocation_dir(node, inv), w.cancellation->stop,
         [&] { return explicit_cancel_.load() || w.cancellation->failed.load(); }, detaching_,
         [&](LogStream stream, const std::string& text) { card_log(node.path, inv, stream, text); },
         [&](const DisplayData& display) { on_display(rt, w, display); },
         [&](int64_t begin, int64_t end) {
           svc_.server()->broadcast(timeline_note(run_id_, node.path, inv, Phase::execute, lane, begin, end));
         },
         [&](const std::function<void(sql::Db&)>& records) {
           stage_begin(s, Phase::execute);
           svc_.history().write_sync([&](sql::Db& db) {
             History::put_status(db, s);
             if (records) records(db);
           });
           card_status(s);
           set_state(RunState::running);
         },
         [&] { card_status(s); }});
    w.out_alias = std::move(result.out_alias);
    Produced p;
    p.failure = result.outcome.failure;
    p.remote_logs = std::move(result.logs);
    if (!p.failure)
      for (auto& [name, bytes] : result.outcome.outputs)
        p.outputs.push_back({name, PortKind::FlatBuffer, nullptr, std::move(bytes), {}});
    return p;
  }

  // ---- convergence ------------------------------------------------------------

  // One line for the note of the run's commit, from what this run did rather
  // than what it planned. Failure messages and logs stay out of git, as the
  // outputs do: they are in the run's record.
  std::string describe(const std::vector<CardState>& final) const {
    const auto& res = *doc_->resolution;
    const Node* target = res.find(p_.card);
    const bool whole = p_.card.empty() || (p_.propagate && target && target->kind == Template::Trigger);
    std::vector<std::string> ran;
    std::vector<std::string> failed;
    std::vector<std::string> never;
    bool saved = false;
    const bool en = p_.english;
    for (const auto& n : nodes_) {
      if (!n.in_targets || n.node->is_virtual) continue;
      const std::string key = path_key(n.node->path);
      // The trigger of a whole run only starts it.
      if (n.recorded_invocations > 0 && !(whole && n.node->kind == Template::Trigger)) {
        ran.push_back(n.recorded_invocations > 1 ? key + " ×" + std::to_string(n.recorded_invocations) : key);
        saved |= n.saved_inputs;
      }
      if (n.last_failure && n.last_failure->kind != FailureKind::cancelled)
        failed.push_back(key + (en ? " (" : "（") + failure_kind_name(n.last_failure->kind) + (en ? ")" : "）"));
      else if (n.activations == 0 && (final[n.idx] == CardState::blocked || final[n.idx] == CardState::cancelled))
        never.push_back(key);
    }
    auto names = [en](const std::vector<std::string>& v) {
      constexpr size_t kShown = 6;
      std::string s;
      for (size_t i = 0; i < std::min(v.size(), kShown); ++i) s += (i ? (en ? ", " : "、") : "") + v[i];
      if (v.size() > kShown) s += (en ? " and " + std::to_string(v.size() - kShown) + " more" : " ほか " + std::to_string(v.size() - kShown));
      return s;
    };
    std::string out = en ? (whole ? "whole" : (p_.propagate ? "from " : "only ") + path_key(p_.card))
                         : (whole ? "全体" : path_key(p_.card) + (p_.propagate ? " から" : " だけ"));
    if (saved) out += en ? " (with saved inputs)" : "（保存済みの入力で）";
    if (!ran.empty()) out += ": " + names(ran);
    if (!failed.empty()) out += (en ? " / failed: " : " / 失敗: ") + names(failed);
    if (!never.empty()) out += (en ? " / not run: " : " / 未実行: ") + names(never);
    return out;
  }

  void finalize() {
    const auto& res = *doc_->resolution;
    std::vector<CardState> final(nodes_.size(), CardState::not_started);
    std::vector<std::vector<WaitingInputData>> waiting(nodes_.size());
    for (auto& n : nodes_) {
      if (!n.in_targets || n.node->is_virtual) continue;
      final[n.idx] = n.activations > 0 || n.halted ? n.last : CardState::skipped;
    }
    // Never invoked: blocked when an upstream failed/blocked, or partial inputs arrived.
    for (bool changed = true; changed;) {
      changed = false;
      for (auto& n : nodes_) {
        if (!n.in_targets || n.node->is_virtual || n.activations > 0 || n.halted) continue;
        if (final[n.idx] == CardState::blocked || final[n.idx] == CardState::cancelled) continue;
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
      if (!n.in_targets || n.node->is_virtual || n.activations > 0 || n.halted) continue;
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
      rec.summary.params_json = NixRunner::params_json(*n.node, false);
      rec.summary.invocation = n.invocations + 1;
      rec.summary.state = st;
      rec.summary.waiting = waiting[n.idx];
      History& h = svc_.history();
      h.submit([&h, rec](sql::Db& db) { h.complete_invocation(db, rec); },
               [self = shared_from_this(), s = rec.summary](bool ok) {
                 if (!ok) self->persistence_lost("final state of " + path_key(s.card));
                 self->svc_.server()->broadcast(card_note(s));
               });
    }
    for (auto& n : nodes_)
      if (n.last_inputs) save_last_inputs(*n.node, n.last_inputs->first, n.last_inputs->second, n.last_port_inputs);
    // The terminal state is computed here but becomes visible (RunStatus,
    // notifications) only after every record above is committed.
    svc_.history().flush();
    RunSummaryData sum;
    {
      std::lock_guard<std::mutex> l(mu_);
      finishing_ = true;
      sum = summary_;
      note_summary_ = describe(final);
    }
    auto& st = sum.status;
    if (cancel_ && interrupted_) {
      st.state = RunState::failed;
      st.failure = FailureData::make(FailureKind::interrupted, "the environment stopped while the run was active");
    } else if (cancel_) {
      st.state = RunState::cancelled;
      st.failure = FailureData::make(FailureKind::cancelled, "the run was cancelled");
    } else if (auto pf = persist_failure()) {
      st.state = RunState::failed;
      st.failure = pf;
    } else if (start_failure_ || first_failure_ || stall_failure_ || blocked_failure) {
      st.state = RunState::failed;
      st.failure = start_failure_ ? start_failure_ : first_failure_ ? first_failure_ : stall_failure_ ? stall_failure_ : blocked_failure;
    } else {
      st.state = RunState::succeeded;
      st.failure.reset();
    }
    st.ended_at = now_ms();
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
          History::interrupt_batches(db, *sum.status.run_id);
          // Detached external invocations are still pending after the local
          // graph ends. Keep their progress until recovery commits a result.
          db.prepare("DELETE FROM statuses WHERE run_id=? AND NOT EXISTS ("
                     "SELECT 1 FROM remote_jobs AS j WHERE j.run_id=statuses.run_id AND j.card=statuses.card "
                     "AND j.invocation=statuses.invocation)").bind(1, *sum.status.run_id).run();
        },
        [self, sum](bool ok) mutable {
          if (!ok && sum.status.state == RunState::succeeded) {
            // The success itself could not be recorded: never report it.
            sum.status.state = RunState::failed;
            sum.status.failure = FailureData::make(FailureKind::persistence_failed, "saving the run result failed");
          }
          {
            std::lock_guard<std::mutex> l(self->mu_);
            self->summary_ = sum;
          }
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
  std::string note_summary_;  // what ran (describe), for the commit's note
  std::string run_id_;
  stdfs::path run_dir_;
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::condition_variable task_cv_;
  std::vector<NodeRt> nodes_;
  std::map<std::pair<size_t, std::string>, std::vector<std::pair<size_t, std::string>>> out_edges_;
  std::deque<Work> tasks_;
  std::vector<std::thread> pool_;
  std::condition_variable worker_cv_;
  std::deque<std::function<void(uint32_t)>> worker_tasks_;
  std::vector<std::thread> workers_;
  uint32_t worker_budget_ = 0;
  bool pool_stop_ = false;
  uint32_t running_ = 0;
  uint64_t token_seq_ = 0;
  std::vector<std::string> seed_notes_;  // journaled once the run has started
  std::atomic<bool> cancel_{false};
  std::atomic<bool> explicit_cancel_{false};
  std::atomic<bool> detaching_{false};
  bool finishing_ = false;  // under mu_: the final status is decided, a cancel comes too late
  bool interrupted_ = false;
  std::optional<FailureData> first_failure_;
  std::optional<FailureData> start_failure_;
  std::optional<FailureData> stall_failure_;
  std::mutex persist_mu_;
  std::optional<FailureData> persist_failure_;
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
      History::interrupt_batches(db, run_id);
      // In-progress invocations become interrupted failures; nothing is re-run.
      auto st = db.prepare("SELECT s.card, s.invocation, e.environment_id FROM statuses AS s "
                           "LEFT JOIN active_environments AS e ON e.run_id=s.run_id AND e.card=s.card "
                           "AND e.invocation=s.invocation WHERE s.run_id=? AND NOT EXISTS ("
                           "SELECT 1 FROM remote_jobs AS j WHERE j.run_id=s.run_id AND j.card=s.card AND j.invocation=s.invocation)");
      st.bind(1, run_id);
      std::vector<InvocationRecordData> recs;
      while (st.step()) {
        auto summary = History::status(db, run_id, path_from_key(st.text(0)), st.u64(1));
        if (!summary) continue;
        InvocationRecordData rec;
        rec.summary = std::move(*summary);
        if (rec.summary.environment_id.empty()) rec.summary.environment_id = st.text(2);
        rec.summary.state = CardState::failed;
        rec.summary.failure = r->status.failure;
        recs.push_back(std::move(rec));
      }
      for (const auto& rec : recs) svc_.history().complete_invocation(db, rec);
    });
    log::warn("run " + run_id + " was interrupted by a restart");
  }
}

auto Scheduler::reconcile_remote_job(RemoteJobRecord& job) -> bool {
  auto& h = svc_.history();
  auto& backend = svc_.remote_execution();
  auto& inv = job.invocation;
  const auto before = inv.execution_state;
  const auto accepted = job.accepted;
  const auto reply = backend.poll_recovered(job);
  if (inv.execution_state != before || job.accepted != accepted)
    h.write_sync([&](sql::Db& db) { History::put_remote_job(db, job); });
  h.set_live(inv);
  if (inv.execution_state != before) svc_.server()->broadcast(card_note(inv));
  if (!reply || reply->state == "queued" || reply->state == "running" || reply->state == "cancelling") return false;
  RecoveredRemoteJob recovered;
  try {
    recovered = backend.recover(job, *reply, stop_recovery_);
  } catch (const BrokerDisconnected&) {
    inv.execution_state = RemoteExecutionState::disconnected;
    h.write_sync([&](sql::Db& db) { History::put_remote_job(db, job); });
    h.set_live(inv);
    svc_.server()->broadcast(card_note(inv));
    return false;
  }
  auto& result = recovered.record;
  const auto& logs = recovered.logs;
  auto& artifact = recovered.artifact;
  const auto& environment = recovered.environment;
  inv.state = !inv.failure ? CardState::succeeded : inv.failure->kind == FailureKind::cancelled
               ? CardState::cancelled : CardState::failed;
  inv.ended_at = now_ms();
  if (inv.started_at) inv.duration_ms = static_cast<uint64_t>(std::max<int64_t>(0, *inv.ended_at - *inv.started_at));
  stage_close(inv, !inv.failure ? StageOutcome::done : inv.state == CardState::cancelled
                     ? StageOutcome::cancelled : StageOutcome::failed);
  if (inv.failure) {
    result.outputs.clear();
    artifact.reset();
  }
  result.summary = inv;
  std::map<LogStream, std::vector<LogEnd>> log_ends;
  std::optional<CardPath> completed_card;
  h.write_sync([&](sql::Db& db) {
    // A Card may be renamed while its remote result is downloading. Resolve
    // the current history key in the same transaction as its completion.
    completed_card = History::remote_job_card(db, inv.execution_job_id);
    if (!completed_card) return;
    result.summary.card = *completed_card;
    if (result.display) result.display->card = *completed_card;
    if (artifact) artifact->card = *completed_card;
    // A restart may have happened before the failing member could commit
    // its cancellation intent. A terminal worker failure still stops siblings.
    if (inv.batch_activation && inv.failure &&
        (reply->state == "failed" || reply->state == "succeeded" || inv.failure->kind == FailureKind::timeout))
      History::cancel_remote_batch(db, inv.run_id, *completed_card, *inv.batch_activation);

    if (!environment.id.empty()) History::put_environment(db, inv.run_id, environment);
    if (artifact) History::put_artifact(db, *artifact);
    for (const auto& [stream, text] : logs)
      log_ends[stream] = h.append_log(db, inv.run_id, *completed_card, inv.invocation, stream, text);
    h.complete_invocation(db, result);
  });
  h.clear_live(inv);
  if (!completed_card) return true;
  h.clear_live(result.summary);
  for (const auto& [_, ends] : log_ends)
    for (const auto& end : ends) svc_.server()->broadcast(log_note(end));
  svc_.server()->broadcast(card_note(result.summary));
  svc_.schedule_export();
  return true;
}

auto Scheduler::recover_remote_jobs() -> void {
  const auto jobs = svc_.history().read([](sql::Db& db) { return History::remote_jobs(db); });
  if (jobs.empty()) return;
  std::set<std::string> pending;
  for (const auto& job : jobs) pending.insert(job.invocation.execution_job_id);
  remote_recovery_ = std::thread([this, pending = std::move(pending)]() mutable {
    while (!stop_recovery_.load() && !pending.empty()) {
      try {
        auto jobs = svc_.history().read([](sql::Db& db) { return History::remote_jobs(db); });
        for (auto& job : jobs) {
          if (stop_recovery_.load()) return;
          const auto& id = job.invocation.execution_job_id;
          if (pending.contains(id) && reconcile_remote_job(job)) pending.erase(id);
        }
      } catch (const std::exception& error) {
        log::warn(std::string("external job recovery: ") + error.what());
      }
      if (!pending.empty()) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  });
}

std::set<size_t> Scheduler::draft_waiting(const Resolution& res) {
  std::set<size_t> waits;
  for (size_t i = 0; i < res.nodes.size(); ++i)
    if (res.nodes[i].kind == Template::Draft) waits.insert(i);
  for (bool changed = !waits.empty(); changed;) {
    changed = false;
    for (size_t i = 0; i < res.nodes.size(); ++i) {
      if (res.nodes[i].is_virtual || waits.count(i)) continue;
      for (const auto& p : res.nodes[i].inputs) {
        if (p.optional) continue;
        bool fed = false;
        bool only_waiting = true;
        for (const auto& e : res.edges)
          if (e.to == i && e.to_port == p.name) {
            fed = true;
            only_waiting &= waits.count(e.from) > 0;
          }
        if (fed && only_waiting) {
          waits.insert(i);
          changed = true;
          break;
        }
      }
    }
  }
  return waits;
}

RunPlanData Scheduler::plan(const Resolution& res, const CardPath& card, bool propagate) {
  RunPlanData out;
  const Node* target = res.find(card);
  if (!target) throw Failed(FailureKind::not_found, "unknown card " + path_key(card), {{"card", path_key(card)}});
  size_t t = res.index.at(path_key(card));
  std::set<size_t> targets;
  std::vector<size_t> starts;
  std::vector<size_t> seeds;
  auto inside = [&](size_t i) {
    const auto& p = res.nodes[i].path;
    return p.size() > card.size() && std::equal(card.begin(), card.end(), p.begin());
  };
  if (target->is_virtual) {
    // A subgraph runs like one Card: its inner Cards run; those fed across the
    // boundary use their saved values for the outside ports; inner sources start.
    for (size_t i = 0; i < res.nodes.size(); ++i) {
      if (!inside(i) || res.nodes[i].is_virtual || res.nodes[i].kind == Template::Draft) continue;
      targets.insert(i);
      bool from_outside = false;
      bool any_in = false;
      for (const auto& e : res.edges)
        if (e.to == i) {
          any_in = true;
          from_outside |= !inside(e.from);
        }
      if (from_outside) seeds.push_back(i);
      else if (!any_in) starts.push_back(i);
    }
    if (targets.empty())
      throw Failed(FailureKind::invalid_request, "the subgraph has no executable Cards", {{"card", path_key(card)}});
    if (propagate) {
      std::vector<size_t> stack(targets.begin(), targets.end());
      while (!stack.empty()) {
        size_t i = stack.back();
        stack.pop_back();
        for (const auto& e : res.edges)
          if (e.from == i && !targets.count(e.to) && res.nodes[e.to].kind != Template::Draft) {
            targets.insert(e.to);
            stack.push_back(e.to);
          }
      }
    }
  } else if (!propagate) {
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
  // A Draft is not implemented yet: it does not run, nor do the Cards that
  // need its outputs (a required input fed only by Drafts or by Cards waiting
  // on one). Everything else runs as usual; a Card started on its own keeps
  // its saved inputs.
  std::set<size_t> waits = draft_waiting(res);
  for (size_t i : waits)
    if (i != t || target->kind == Template::Draft) targets.erase(i);
  if (target->kind == Template::Draft)
    throw Failed(FailureKind::invalid_request, path_key(card) + " is a Draft and is not implemented yet", {{"card", path_key(card)}});
  std::erase_if(starts, [&](size_t i) { return !targets.count(i); });
  std::erase_if(seeds, [&](size_t i) { return !targets.count(i); });
  for (size_t i : targets) {
    out.targets.push_back(res.nodes[i].path);
    if (res.nodes[i].kind == Template::Code) out.build_candidates.push_back(res.nodes[i].path);
    out.diagnostics.insert(out.diagnostics.end(), res.nodes[i].diags.begin(), res.nodes[i].diags.end());
    out.diagnostics.insert(out.diagnostics.end(), res.nodes[i].exec_diags.begin(), res.nodes[i].exec_diags.end());
  }
  for (size_t s : starts) out.starts.push_back(res.nodes[s].path);
  for (size_t s : seeds) {
    out.seeds.push_back(res.nodes[s].path);
    out.starts.push_back(res.nodes[s].path);
  }
  // A Card further down that waits for all its inputs would never run without
  // the ports nothing in this run feeds; a port something in this run feeds
  // gets that value alone, and an `any` Card runs on what arrives.
  std::set<size_t> given(starts.begin(), starts.end());
  given.insert(seeds.begin(), seeds.end());
  for (size_t i : targets) {
    if (res.nodes[i].settings.any_mode || given.count(i)) continue;
    std::set<std::string> fed, saved;
    for (const auto& e : res.edges) {
      if (e.to != i) continue;
      if (targets.count(e.from)) fed.insert(e.to_port);
      else if (!waits.count(e.from)) saved.insert(e.to_port);
    }
    for (const auto& port : fed) saved.erase(port);
    if (!saved.empty()) out.joins.push_back({res.nodes[i].path, std::move(saved)});
  }
  return out;
}

std::string Scheduler::accept(DocumentPtr doc, const RunParams& p) {
  std::lock_guard<std::mutex> l(mu_);
  if (active_) throw Failed(FailureKind::busy, "another run is active", {{"run_id", *active_->status().run_id}});
  auto plan = Scheduler::plan(*doc->resolution, p.card, p.propagate);
  const auto services = svc_.remote_targets();
  for (const auto& path : plan.targets)
    if (auto diagnostic = svc_.execution_diagnostic(*doc->resolution->find(path), services))
      plan.diagnostics.push_back(std::move(*diagnostic));
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
  if (!p.commit.empty()) s.status.commit = p.commit;
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
    History::put_substituters(db, *s.status.run_id, svc_.substituters());
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
  std::optional<RunStatusData> active;
  if (exec) active = exec->request_cancel(false);
  svc_.history().write_sync([&](sql::Db& db) { History::cancel_remote_jobs(db, run_id); });
  if (active) return *active;
  auto r = svc_.history().read([&](sql::Db& db) { return History::run(db, run_id); });
  if (!r) throw Failed(FailureKind::not_found, "unknown run " + run_id, {{"run_id", run_id}});
  return r->status;
}

void Scheduler::publish_display(const DisplayData& d) {
  {
    std::lock_guard<std::mutex> l(mu_);
    auto k = path_key(d.card);
    displays_[k] = d;
    gone_.erase(k);
    no_display_.erase(k);
  }
  svc_.server()->broadcast(display_note(d));
}

namespace {

// What a Card is made of: its settings (whatever the spacing of their JSON, a
// client writes its own) and the files of its definition folder.
std::string card_content(const Document& doc, const Node& n) {
  std::string out;
  if (n.def_dir.empty()) return out;
  if (n.project_dir.empty()) {
    if (auto d = doc.definitions.find(n.path.back()); d != doc.definitions.end()) {
      bool quoted = false, escaped = false;
      for (char c : d->second.card_json) {
        if (escaped) escaped = false;
        else if (quoted && c == '\\') escaped = true;
        else if (c == '"') quoted = !quoted;
        else if (!quoted && (c == ' ' || c == '\n' || c == '\t' || c == '\r')) continue;
        out += c;
      }
    }
  }
  for (auto s = doc.sources.lower_bound(n.def_dir); s != doc.sources.end() && s->first.starts_with(n.def_dir); ++s) {
    out += '\0' + s->first + '\0' + s->second.hash;
  }
  return out;
}

}  // namespace

void Scheduler::document_changed(const Document& before, const Document& after,
                                 const std::vector<std::pair<std::string, std::string>>& renamed) {
  if (!before.resolution || !after.resolution) return;
  if (before.resolution == after.resolution && renamed.empty()) return;
  auto inside = [](const std::string& k, const std::string& card) { return k == card || k.starts_with(card + "/"); };
  std::lock_guard<std::mutex> l(mu_);
  for (const auto& [from, to] : renamed) {
    auto moved = [&, &from = from, &to = to](const std::string& k) { return to + k.substr(from.size()); };
    // What a Card that left had is not this Card's. The history has just given
    // it the new name, so that is not asked either.
    std::vector<std::string> left, blocked;
    for (const auto& [k, content] : gone_)
      if (inside(k, from)) left.push_back(k);
    for (const auto& k : no_display_)
      if (inside(k, from)) blocked.push_back(k);
    for (const auto& k : left) no_display_.insert(moved(k));
    for (const auto& k : blocked) {
      no_display_.erase(k);
      no_display_.insert(moved(k));
    }
    std::vector<std::string> keys;
    for (const auto& [k, d] : displays_)
      if (inside(k, from) && !gone_.count(k)) keys.push_back(k);
    for (const auto& k : keys) {
      auto n = displays_.extract(k);
      auto t = moved(k);
      n.mapped().card = path_from_key(t);
      displays_[t] = std::move(n.mapped());
      gone_.erase(t);
      no_display_.erase(t);
    }
  }
  for (const auto& n : before.resolution->nodes) {
    if (after.resolution->find(n.path)) continue;
    auto k = path_key(n.path);
    if (std::any_of(renamed.begin(), renamed.end(), [&](const auto& r) { return inside(k, r.first); })) continue;
    // The first Card that left: an undo goes back through the ones that followed it.
    gone_.try_emplace(k, card_content(before, n));
  }
}

std::optional<Note> Scheduler::latest_display_note(const std::string& card_key) {
  std::lock_guard<std::mutex> l(mu_);
  if (auto g = gone_.find(card_key); g != gone_.end()) {
    auto doc = svc_.store().current();
    const Node* n = doc->resolution ? doc->resolution->find(path_from_key(card_key)) : nullptr;
    if (!n) return std::nullopt;
    if (card_content(*doc, *n) != g->second) {
      displays_.erase(card_key);
      no_display_.insert(card_key);
    }
    gone_.erase(g);
  }
  if (no_display_.count(card_key)) return std::nullopt;
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
  svc_.schedule_export();
  std::shared_ptr<RunExec> done;
  {
    std::lock_guard<std::mutex> l(mu_);
    if (active_.get() == exec) done = std::move(active_);
  }
  if (done) {
    // The run thread finishes on its own; reap it off the callback thread.
    std::lock_guard<std::mutex> l(mu_);
    reapers_.emplace_back([this, done] {
      done->join();
      svc_.note_run_end(done->status(), done->note_summary());
    });
  }
}

void Scheduler::shutdown() {
  stop_recovery_.store(true);
  std::shared_ptr<RunExec> exec;
  {
    std::lock_guard<std::mutex> l(mu_);
    exec = active_;
  }
  if (exec) exec->request_cancel(true);
  if (remote_recovery_.joinable()) remote_recovery_.join();
  if (exec) exec->join();
  std::vector<std::thread> reapers;
  {
    std::lock_guard<std::mutex> l(mu_);
    reapers.swap(reapers_);
  }
  for (auto& t : reapers)
    if (t.joinable()) t.join();
}

}  // namespace tsugumi
