#include "scheduler.hh"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <deque>
#include <flatbuffers/reflection.h>
#include <set>

#include "history.hh"
#include "schema.hh"
#include "nix.hh"
#include "service.hh"
#include "util/crypto.hh"
#include "util/fs.hh"
#include "util/log.hh"
#include "wire.hh"
#include "worker_pool.hh"
#include "worker_proc.hh"

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
// Descriptor slots of the folders passed to a sandboxed worker (out, work, tmp).
constexpr int32_t kOutSlot = 900;

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

// Returns a copy of a File/Directory value whose `value` field is `p`.
Value with_path(const Value& v, const std::string& p) {
  Value out = v;
  std::vector<uint8_t> buf(v.bytes.begin(), v.bytes.end());
  const auto* root = v.type->root();
  const auto* f = root->fields()->LookupByKey("value");
  auto* t = flatbuffers::GetAnyRoot(buf.data());
  const auto* s = flatbuffers::GetFieldS(*t, *f);
  if (!s) throw Failed(FailureKind::output_invalid, "file value " + v.name + " has no path");
  if (p.size() <= s->size()) {
    // Shrinking in place: flatbuffers' ResizeContext erases the bytes before
    // the string when it shrinks, which corrupts the buffer. A shorter string
    // with trailing padding is still a valid buffer.
    auto* at = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(s));
    flatbuffers::WriteScalar<flatbuffers::uoffset_t>(at, static_cast<flatbuffers::uoffset_t>(p.size()));
    std::memcpy(at + sizeof(flatbuffers::uoffset_t), p.c_str(), p.size() + 1);
  } else {
    flatbuffers::SetString(*v.type->schema(), p, s, &buf, root);
  }
  out.bytes.assign(reinterpret_cast<const char*>(buf.data()), buf.size());
  return out;
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
    int64_t arrived_at = now_ms();  // when it entered the queue
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
    uint64_t display_rev = 0;
    // The call's output folder as the Card sees it (a passed descriptor) and its real path.
    std::optional<std::pair<std::string, std::string>> out_alias;
    std::optional<DisplayData> display;
    // Inputs of the latest invocation; saved for single-Card runs at the end of the run.
    std::optional<std::pair<uint64_t, std::vector<Value>>> last_inputs;
    std::map<std::string, std::pair<uint64_t, Value>> last_port_inputs;  // latest value per port
  };
  struct Work {
    size_t node;
    uint64_t invocation;
    std::vector<Token> inputs;
    bool from_saved = false;  // single-Card run on the saved inputs
    int64_t ready_at = now_ms();  // all inputs there (or the start fired)
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
                          [this, s](bool ok) {
                            if (!ok) persistence_lost("run status");
                            svc_.server()->broadcast(status_note(s, false));
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
                          [this, s, persist](bool ok) {
                            if (!ok && persist) persistence_lost("progress of " + path_key(s.card));
                            svc_.server()->broadcast(card_note(s));
                          });
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
          if (!ok) persistence_lost("journal");
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
          if (!ok) persistence_lost("log");
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
    std::set<std::string> seeded;
    for (const auto& s : plan.seeds) seeded.insert(path_key(s));
    for (const auto& s : plan.starts)
      if (!seeded.count(path_key(s))) nodes_[res.index.at(path_key(s))].start = true;
    for (const auto& s : plan.seeds) seed_boundary(nodes_[res.index.at(path_key(s))]);
  }

  // Queues the saved values of the ports an inner Card receives from outside
  // the subgraph being run. All of them come from one saved invocation.
  void seed_boundary(NodeRt& n) {
    const auto& res = *doc_->resolution;
    std::set<std::string> outside;
    for (const auto& e : res.edges)
      if (e.to == n.idx && !nodes_[e.from].in_targets) outside.insert(e.to_port);
    auto fail = [&](FailureKind k, const std::string& msg) {
      start_failure_ = FailureData::make(k, msg, {{"card", path_key(n.node->path)}});
      n.halted = true;
      n.last = CardState::failed;
      n.last_failure = start_failure_;
    };
    // `all` Cards take every outside port from one saved invocation; `any`
    // Cards consume one port per invocation, so each port has its own latest value.
    auto last = svc_.history().read([&](sql::Db& db) { return History::last_input(db, n.node->path); });
    if (!last) return fail(FailureKind::input_missing, "no saved inputs for " + path_key(n.node->path) +
                                                           "; run the whole workflow once first");
    for (const auto& port : outside) {
      std::optional<History::LastInput> one;
      if (n.node->settings.any_mode)
        one = svc_.history().read([&](sql::Db& db) { return History::last_port_input(db, n.node->path, port); });
      const auto& pool = one ? one->inputs : last->inputs;
      const Value* v = nullptr;
      for (const auto& x : pool)
        if (x.name == port) v = &x;
      if (!v) return fail(FailureKind::input_missing, "the saved inputs of " + path_key(n.node->path) + " have no " + port);
      const auto* p = n.node->input(port);
      if (!p || !p->type || !types::compatible(*p->type, *v->type))
        return fail(FailureKind::type_mismatch, "saved input " + port + " of " + path_key(n.node->path) +
                                                    " no longer matches the port type");
      if (v->kind != PortKind::FlatBuffer) {
        auto path = path_value(*v);
        std::error_code ec;
        if (!path || !stdfs::exists(*path, ec))
          return fail(FailureKind::input_missing, "the saved file for " + port + " no longer exists");
      }
      Token t;
      t.value = *v;
      t.from_inv = last->invocation;
      t.seq = ++token_seq_;
      n.queues[port].push_back(std::move(t));
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
    Work w{n.idx, 0, {}};
    if (n.start && !n.start_fired) {
      if (!downstream_has_space(n)) return std::nullopt;
      n.start_fired = true;
      if (!n.queues.empty()) {
        // A start Card with connected inputs runs on its last saved inputs.
        auto last = svc_.history().read([&](sql::Db& db) { return History::last_input(db, n.node->path); });
        if (!last) {
          start_failure_ = FailureData::make(FailureKind::input_missing,
                                             "no saved inputs for " + path_key(n.node->path), {{"card", path_key(n.node->path)}});
          n.halted = true;
          n.last = CardState::failed;
          n.last_failure = start_failure_;
          return std::nullopt;
        }
        auto fail_start = [&](FailureKind k, const std::string& msg, const std::string& port) {
          start_failure_ = FailureData::make(k, msg, {{"card", path_key(n.node->path)}, {"port", port}});
          n.halted = true;
          n.last = CardState::failed;
          n.last_failure = start_failure_;
        };
        for (const auto& port : n.node->inputs) {
          bool have = false;
          for (const auto& v : last->inputs) have |= v.name == port.name;
          if (!have && !port.optional) {
            fail_start(FailureKind::input_missing, "the saved inputs have no value for " + port.name, port.name);
            return std::nullopt;
          }
        }
        w.from_saved = true;
        for (auto& v : last->inputs) {
          if (v.kind != PortKind::FlatBuffer) {
            auto p = path_value(v);
            std::error_code ec;
            if (!p || !stdfs::exists(*p, ec)) {
              fail_start(FailureKind::input_missing, "the saved file for input " + v.name + " no longer exists", v.name);
              return std::nullopt;
            }
          }
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
              w->invocation = ++n.invocations;
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
    // Ready when the last consumed input arrived (a start Card: when it fired).
    s.ready_at = w.inputs.empty() ? w.ready_at : 0;
    for (const auto& t : w.inputs) s.ready_at = std::max(*s.ready_at, t.arrived_at);
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
      stage_begin(s, node.kind == Template::Code ? Phase::prepare : Phase::execute);
      card_status(s, true);
      // The Card's saved inputs are written once at the end of the run.
      if ((node.kind == Template::Code || !inputs.empty()) && !w.from_saved) {
        std::lock_guard<std::mutex> l(mu_);
        rt.last_inputs = std::make_pair(w.invocation, inputs);
        for (const auto& v : inputs) rt.last_port_inputs[v.name] = std::make_pair(w.invocation, v);
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
        stage_begin(s, Phase::validate);
        int64_t vs = now_ms();
        try {
          validate_outputs(rt, w.invocation, out.outputs, inputs);
        } catch (const Failed& f) {
          out.failure = f.failure;
          out.outputs.clear();
        }
        svc_.server()->broadcast(timeline_note(run_id_, node.path, w.invocation, Phase::validate, lane, vs, now_ms()));
      }
    }
    bool was_cancel = out.failure && out.failure->kind == FailureKind::cancelled;
    if (out.failure) stage_close(s, was_cancel ? StageOutcome::cancelled : StageOutcome::failed);
    else for (const auto& v : out.outputs) s.outputs_sent.push_back(v.name);
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
      rec.inputs = inputs;
      rec.outputs = out.outputs;
    }
    {
      std::lock_guard<std::mutex> l(mu_);
      rec.display = rt.display && rt.display->invocation == w.invocation ? rt.display : std::nullopt;
    }
    if (s.state == CardState::succeeded) {
      // Records, artifacts and saved values are written in the save stage.
      stage_begin(s, Phase::save);
      s.state = CardState::running;
      card_status(s);
      s.state = CardState::succeeded;
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
    // The completion is durable before it is announced or its outputs move on.
    // The record holds the save stage as done; a failed write reports it failed.
    {
      auto done = s;
      stage_close(done, out.failure ? StageOutcome::failed : StageOutcome::done);
      rec.summary.stages = done.stages;
      rec.summary.outputs_sent = done.outputs_sent;
      rec.summary.worker_reused = done.worker_reused;
      try {
        History& h = svc_.history();
        h.write_sync([&h, rec](sql::Db& db) { h.complete_invocation(db, rec); });
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

  // The inputs of the Card's latest invocation become its saved inputs for a
  // later single-Card run (and each port keeps its latest value). Written
  // once per Card at the end of the run. File/Directory contents are copied
  // out of the run's work area (which CacheClear removes) and the saved values
  // point to the copy. Exactly one invocation's set is kept, never a mix of runs.
  void save_last_inputs(const Node& node, uint64_t inv, const std::vector<Value>& inputs,
                        const std::map<std::string, std::pair<uint64_t, Value>>& ports) {
    auto base = svc_.store().state_dir() / "inputs" / path_key(node.path);
    auto dir = base / (run_id_ + "-" + std::to_string(inv));
    std::vector<Value> saved;
    std::vector<std::pair<uint64_t, Value>> per_port;
    bool copied = false;
    auto copy_into = [&](const Value& v, const stdfs::path& where) -> Value {
      if (v.kind == PortKind::FlatBuffer) return v;
      auto src = path_value(v);
      if (!src) throw Failed(FailureKind::output_invalid, "input " + v.name + " has no path");
      copied = true;
      auto dest = where / stdfs::path(*src).filename();
      std::error_code ec;
      stdfs::remove_all(where, ec);
      stdfs::create_directories(where);
      if (v.kind == PortKind::File) {
        fs::write_atomic(dest, fs::read_file_or_throw(*src), 0644);
      } else {
        auto listing = fs::list_tree(*src);
        stdfs::create_directories(dest);
        for (const auto& f : listing.files)
          fs::write_atomic(dest / f, fs::read_file_or_throw(stdfs::path(*src) / f), 0644);
      }
      return with_path(v, dest.string());
    };
    try {
      for (const auto& v : inputs) saved.push_back(copy_into(v, dir / v.name));
      for (const auto& [name, p] : ports) per_port.emplace_back(p.first, copy_into(p.second, base / ("port-" + name)));
    } catch (const std::exception& e) {
      persistence_lost(std::string("saved inputs of ") + path_key(node.path) + ": " + e.what());
      return;
    }
    auto card = node.path;
    auto run = run_id_;
    svc_.history().submit([card, run, inv, saved, per_port](sql::Db& db) {
                            History::put_last_input(db, card, run, inv, saved);
                            for (const auto& [i, v] : per_port) History::put_last_port_input(db, card, run, i, v);
                          },
                          [this, base, dir, card, copied](bool ok) {
                            if (!ok) {
                              persistence_lost("saved inputs of " + path_key(card));
                              return;
                            }
                            if (!copied) return;
                            // Older saved copies of this Card are no longer referenced.
                            std::error_code ec;
                            if (stdfs::is_directory(base, ec))
                              for (const auto& e : stdfs::directory_iterator(base, ec))
                                if (e.path() != dir && !e.path().filename().string().starts_with("port-"))
                                  stdfs::remove_all(e.path(), ec);
                          });
  }

  void validate_outputs(NodeRt& rt, uint64_t inv, std::vector<Value>& outputs, const std::vector<Value>& inputs) {
    const Node& node = *rt.node;
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
        if (p && rt.out_alias) {
          const auto& [alias, real] = *rt.out_alias;
          if (*p == alias || p->starts_with(alias + "/")) {
            v = with_path(v, real + p->substr(alias.size()));
            p = path_value(v);
          }
        }
        auto out_dir = invocation_dir(node, inv) / "out";
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
        const auto& out = node.outputs.front();
        if (!out.type) throw Failed(FailureKind::output_invalid, "merge: the output type is not resolved");
        std::string err;
        auto bytes = types::wrap_in_union(*out.type, v.name, *v.type, v.bytes, &err);
        if (!bytes) throw Failed(FailureKind::output_invalid, "merge: " + err);
        p.outputs.push_back({"out", PortKind::FlatBuffer, out.type, std::move(*bytes), {}});
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
      if (s.phase != Phase::prepare) stage_begin(s, Phase::prepare);
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
      br.log = [this, card, inv](const std::string& line) { card_log(card, inv, LogStream::build, line); };
      int64_t stage_start = t0;
      br.on_phase = [&](Phase p) {
        // Closing the previous stage on the timeline, then reporting the new one.
        svc_.server()->broadcast(timeline_note(run_id_, node.path, s.invocation, *s.phase, lane, stage_start, now_ms()));
        stage_start = now_ms();
        stage_begin(s, p);
        card_status(s);
      };
      rt.build = svc_.nix().build(br);
      svc_.server()->broadcast(timeline_note(run_id_, node.path, s.invocation, *s.phase, lane, stage_start, now_ms()));
      journal("build", "output", node.path, s.invocation,
              rt.build->cached ? "evaluated; the store already had the result" : "built");
      // Evaluated (the resident evaluator reuses nixpkgs); nothing to build.
      if (rt.build->cached) stage_begin(s, Phase::build, StageOutcome::cached);
    } else {
      // Built by an earlier invocation of this run.
      stage_close(s, StageOutcome::reused);
      stage_begin(s, Phase::evaluate, StageOutcome::reused);
      stage_begin(s, Phase::build, StageOutcome::reused);
    }
    s.build_cached = rt.build->cached;
    stage_begin(s, Phase::execute);
    card_status(s);
    set_state(RunState::running);
    // Cards run as nobody: switched to by root (container), or mapped in
    // bubblewrap's user namespace when tsugumid is not root (shared VM).
    std::optional<uid_t> uid;
    if (::geteuid() == 0 || !std::string(TSUGUMI_BWRAP).empty()) uid = kCardUid;
    WorkerSpawn sp;
    sp.worker_binary = svc_.options().worker_binary.string();
    sp.bash = TSUGUMI_BASH;
    sp.build = *rt.build;
    sp.sandbox = node.settings.sandbox;
    sp.uid = uid;
    sp.gid = uid;
    // Every File / Directory input lives in these trees: one set of binds
    // serves any input, so the worker is reused whatever the paths.
    sp.ro_paths = {(svc_.store().state_dir() / "runs").string(), (svc_.store().state_dir() / "inputs").string()};
    if (!svc_.options().grants_file.empty()) sp.hidden_paths.push_back(svc_.options().grants_file.parent_path().string());
    sp.hidden_paths.push_back(svc_.store().state_dir().string());
    // The resident worker of this Card, if it runs the same build under the same conditions.
    std::string key = rt.build->package.value_or("") + "|" + rt.build->bash.value_or("") + "|" + rt.build->pre_run +
                      "|" + rt.build->post_run + "|" + (sp.sandbox ? "s" : "u") + "|" +
                      (uid ? std::to_string(*uid) : "-");
    std::string card_key = path_key(node.path);
    auto lease = svc_.workers().acquire(card_key, key, sp, rt.build->reuse_worker);
    auto& worker = *lease.worker;
    s.worker_reused = lease.reused;
    // Folders: without filesystem: true, work/ and tmp/ are the worker's own
    // (local, emptied between calls); out/ exists only for Cards with File or
    // Directory outputs and stays (downstream Cards and artifacts use it).
    bool needs_out = false;
    for (const auto& o : node.outputs) needs_out |= o.kind != PortKind::FlatBuffer;
    auto dir = invocation_dir(node, s.invocation);
    std::string work = worker.work_dir(), tmp = worker.tmp_dir(), out;
    std::vector<std::pair<std::string, int32_t>> project_dirs;  // real path, descriptor slot
    auto make_dir = [&](const stdfs::path& d) {
      stdfs::create_directories(d);
      if (::geteuid() == 0) ::chown(d.c_str(), kCardUid, kCardUid);
      return d.string();
    };
    if (needs_out) out = make_dir(dir / "out");
    if (node.settings.filesystem) {
      work = make_dir(dir / "work");
      tmp = make_dir(dir / "tmp");
    }
    // Under bubblewrap the project folders of this call reach the Card as
    // descriptors (/proc/self/fd/<slot>); the run trees are bound read-only.
    std::vector<int> fds;
    std::vector<int32_t> slots;
    rt.out_alias.reset();
    auto pass = [&](std::string& path, int32_t slot) {
      if (path.empty() || !worker.passes_directories()) return;
      int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (fd < 0) throw Failed(FailureKind::worker_failed, "cannot open " + path);
      fds.push_back(fd);
      slots.push_back(slot);
      std::string alias = "/proc/self/fd/" + std::to_string(slot);
      if (slot == kOutSlot) rt.out_alias = std::make_pair(alias, path);
      path = alias;
    };
    struct Close {
      std::vector<int>& fds;
      ~Close() { for (int f : fds) ::close(f); }
    } close_fds{fds};
    pass(out, kOutSlot);
    if (node.settings.filesystem) {
      pass(work, kOutSlot + 1);
      pass(tmp, kOutSlot + 2);
    }
    auto ptype = NixRunner::params_type(node);
    std::string params = NixRunner::params_buffer(node);
    std::vector<std::pair<std::string, std::string>> env = {
        {"PATH", TSUGUMI_CARD_PATH},
        {"HOME", work},
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
    uint64_t call_id = worker.next_call_id();
    std::string frame = worker_call_frame(call_id, run_id_, node.path, s.invocation, in, ptype->bfbs,
                                          ptype->root_type, params, node.outputs, work, tmp, out, env, deadline_ms,
                                          slots);
    // Logs and displays of this call belong to this run and invocation.
    CardPath card = node.path;
    uint64_t inv = s.invocation;
    worker.set_logs([this, card, inv](LogStream stream, const std::string& text) { card_log(card, inv, stream, text); });
    int64_t t0 = now_ms();
    auto outcome = worker.call(frame, call_id, fds, &cancel_, deadline, [this, &rt](const DisplayData& d) {
      on_display(rt, d);
    });
    svc_.server()->broadcast(timeline_note(run_id_, node.path, s.invocation, Phase::execute, lane, t0, now_ms()));
    if (!node.settings.filesystem) worker.clear_scratch();
    // Kept only after a clean call: a failed, cancelled or timed out worker
    // (and anything it started) ends.
    svc_.workers().release(card_key, key, std::move(lease.worker),
                           rt.build->reuse_worker && !outcome.failure);
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
    // Never invoked because a Draft upstream is not implemented yet: blocked,
    // with the Draft named as the reason (Drafts never run or succeed).
    std::map<size_t, std::string> draft_reason;
    for (auto& n : nodes_) {
      if (!n.in_targets || n.node->is_virtual || n.invocations > 0 || n.halted) continue;
      for (const auto& e : res.edges) {
        if (e.to != n.idx || nodes_[e.from].in_targets || res.nodes[e.from].kind != Template::Draft) continue;
        const auto* port = n.node->input(e.to_port);
        if (port && !port->optional) {
          final[n.idx] = cancel_ ? CardState::cancelled : CardState::blocked;
          draft_reason[n.idx] = path_key(res.nodes[e.from].path);
        }
      }
    }
    // Never invoked: blocked when an upstream failed/blocked, or partial inputs arrived.
    for (bool changed = true; changed;) {
      changed = false;
      for (auto& n : nodes_) {
        if (!n.in_targets || n.node->is_virtual || n.invocations > 0 || n.halted) continue;
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
        if (!blocked_failure) {
          auto d = draft_reason.find(n.idx);
          blocked_failure =
              d != draft_reason.end()
                  ? FailureData::make(FailureKind::input_missing,
                                      path_key(n.node->path) + " depends on Draft " + d->second + ", which is not implemented yet",
                                      {{"card", path_key(n.node->path)}, {"draft", d->second}})
                  : FailureData::make(FailureKind::input_missing, path_key(n.node->path) + " is missing inputs",
                                      {{"card", path_key(n.node->path)}});
        }
      }
      if (cancel_ && st == CardState::skipped) continue;  // never reached before the cancel
      InvocationRecordData rec;
      rec.summary.run_id = run_id_;
      rec.summary.card = n.node->path;
      rec.summary.invocation = n.invocations + 1;
      rec.summary.state = st;
      rec.summary.waiting = waiting[n.idx];
      History& h = svc_.history();
      h.submit([&h, rec](sql::Db& db) { h.complete_invocation(db, rec); },
               [this, s = rec.summary](bool ok) {
                 if (!ok) persistence_lost("final state of " + path_key(s.card));
                 svc_.server()->broadcast(card_note(s));
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
      sum = summary_;
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
          // Progress rows never outlive their run (a lost completion leaves no "running" card).
          db.prepare("DELETE FROM statuses WHERE run_id=?").bind(1, *sum.status.run_id).run();
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
  std::optional<FailureData> first_failure_, start_failure_, stall_failure_;
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
      for (const auto& rec : recs) svc_.history().complete_invocation(db, rec);
    });
    log::warn("run " + run_id + " was interrupted by a restart");
  }
}

RunPlanData Scheduler::plan(const Resolution& res, const CardPath& card, bool propagate) {
  RunPlanData out;
  const Node* target = res.find(card);
  if (!target) throw Failed(FailureKind::not_found, "unknown card " + path_key(card), {{"card", path_key(card)}});
  size_t t = res.index.at(path_key(card));
  std::set<size_t> targets;
  std::vector<size_t> starts, seeds;
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
      bool from_outside = false, any_in = false;
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
