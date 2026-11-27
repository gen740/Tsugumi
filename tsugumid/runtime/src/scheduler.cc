#include "scheduler.hh"

#include <flatbuffers/reflection.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <future>
#include <iterator>
#include <set>
#include <stop_token>

#include "collect_value.hh"
#include "execution_service.hh"
#include "history.hh"
#include "nix.hh"
#include "remote_execution_service.hh"
#include "run_scratch.hh"
#include "service.hh"
#include "tsugumi/document/schema_id.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/flat_file.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/log.hh"
#include "tsugumi/util/system.hh"
#include "tsugumi/workerd/artifact_id.hh"
#include "tsugumi/workerd/log.hh"
#include "tsugumi/workerd/store.hh"
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

Note run_event_note(
    const std::string& run_id,
    std::function<std::pair<R::RunEventValue, flatbuffers::Offset<void>>(FBB&)>
        body) {
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
    if (accepted)
      return std::make_pair(R::RunEventValue::RunAcceptedEvent,
                            R::CreateRunAcceptedEvent(b, st).Union());
    return std::make_pair(R::RunEventValue::RunStatusChanged,
                          R::CreateRunStatusChanged(b, st).Union());
  });
}

Note card_note(const InvocationSummaryData& s) {
  bool final = s.state == CardState::succeeded ||
               s.state == CardState::failed ||
               s.state == CardState::cancelled ||
               s.state == CardState::skipped || s.state == CardState::blocked;
  Note n = run_event_note(s.run_id, [s](FBB& b) {
    auto inv = wire::invocation(b, s);
    return std::make_pair(R::RunEventValue::RunCardChanged,
                          R::CreateRunCardChanged(b, inv).Union());
  });
  // Progress of one invocation may be collapsed to its latest state; final
  // states never are.
  if (!final)
    n.coalesce_key = "card|" + s.run_id + "|" + path_key(s.card) + "|" +
                     std::to_string(s.invocation);
  return n;
}

Note timeline_note(const std::string& run_id, const CardPath& card,
                   uint64_t inv, Phase phase, uint32_t lane, int64_t start,
                   std::optional<int64_t> end) {
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
  n.coalesce_key = "timeline|" + run_id + "|" + path_key(card) + "|" +
                   std::to_string(int(phase));
  return n;
}

Note finished_note(const RunSummaryData& s) {
  return run_event_note(*s.status.run_id, [s](FBB& b) {
    auto sum = wire::run_summary(b, s);
    return std::make_pair(R::RunEventValue::RunFinished,
                          R::CreateRunFinished(b, sum).Union());
  });
}

Note log_note(const LogEnd& e) {
  Note n;
  n.kind = Note::Kind::Log;
  n.coalesce_key = "log|" + e.run_id + "|" + (e.card ? path_key(*e.card) : "") +
                   "|" + (e.invocation ? std::to_string(*e.invocation) : "") +
                   "|" + std::to_string(int(e.stream)) + (e.live ? "|live" : "");
  n.approx_bytes = 256;
  n.build = std::make_shared<const Note::Build>([e](FBB& b) {
    auto run = b.CreateString(e.run_id);
    auto card =
        e.card ? wire::path(b, *e.card) : flatbuffers::Offset<D::CardPath>();
    R::LogAvailableBuilder lb(b);
    lb.add_run_id(run);
    if (e.card) lb.add_card(card);
    if (e.invocation) lb.add_invocation(*e.invocation);
    lb.add_stream(e.stream);
    lb.add_live(e.live);
    if (e.end_offset) lb.add_end_offset(*e.end_offset);
    return std::make_pair(R::NotificationPayload::LogAvailable,
                          lb.Finish().Union());
  });
  return n;
}

std::string journal_line(const char* phase, const char* event,
                         const std::optional<CardPath>& card,
                         std::optional<uint64_t> inv,
                         const std::string& message) {
  std::string line = std::string(phase) + " · " + event + ": ";
  for (char c : message) {
    if (c == '\n')
      line += "\\n";
    else if (c == '\r')
      line += "\\r";
    else if (c == '\\')
      line += "\\\\";
    else
      line += c;
  }
  const auto now = std::time(nullptr);
  std::tm utc{};
  gmtime_r(&now, &utc);
  char timestamp[32]{};
  std::strftime(timestamp, sizeof timestamp, "%Y-%m-%dT%H:%M:%SZ", &utc);
  line += " [";
  if (card)
    line += path_key(*card) + " #" + std::to_string(inv.value_or(0)) + ", ";
  return line + timestamp + "]\n";
}

const char* phase_name(Phase p) {
  switch (p) {
    case Phase::evaluate:
      return "evaluate";
    case Phase::build:
      return "build";
    case Phase::execute:
      return "execute";
    case Phase::validate:
      return "validate";
    default:
      return "run";
  }
}

auto recorded_params(const std::vector<ParamDef>& params)
    -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> result;
  for (const auto& param : params)
    result.emplace_back(param.name, param.data);
  return result;
}

// Opens stage `p` of an invocation, closing a still open previous stage as
// done. A stage with another outcome (cached, reused) is closed at once.
void stage_begin(InvocationSummaryData& s, Phase p,
                 StageOutcome o = StageOutcome::running, int64_t t = now_ms()) {
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

// ---------------------------------------------------------------------------

class RunExec : public std::enable_shared_from_this<RunExec> {
 public:
  RunExec(Service& svc, Scheduler& sched, DocumentPtr doc, RunParams p,
          RunSummaryData summary, RunPlanData plan)
      : svc_(svc),
        sched_(sched),
        doc_(std::move(doc)),
        p_(std::move(p)),
        summary_(std::move(summary)),
        plan_(std::move(plan)) {
    run_id_ = *summary_.status.run_id;
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

  auto live_log(const std::string& run, const std::function<LogRead(sql::Db&)>& read)
      -> LogRead {
    std::lock_guard lock(log_policy_mu_);
    if (run != run_id_ || !live_logs_) return {};
    return read(*live_logs_);
  }

  RunStatusData request_cancel(bool interrupted) {
    std::lock_guard<std::mutex> l(mu_);
    // Once finalize() has taken the final status it is on its way to the
    // history: a "cancelling" written now would land after it and stay as the
    // run's state.
    if (interrupted) detaching_.request_stop();
    if (!interrupted && !finishing_) explicit_cancel_.store(true);
    if (!cancel_ && !finishing_) {
      cancel_ = true;
      interrupted_ = interrupted;
      work_stop_.request_stop();
      summary_.status.state = RunState::cancelling;
      persist_status_locked();
    }
    cv_.notify_all();
    return summary_.status;
  }

 private:
  struct Token {
    std::shared_ptr<const Value> value;
    std::string port;
    CardPath from;
    uint64_t from_inv = 0;
  };
  struct TokenGroup {
    std::string port;
    bool execution_list = false;
    std::vector<Token> members;
    uint64_t seq = 0;
    int64_t arrived_at = now_ms();
    bool partial_result = false;
    bool saved_input = false;
    bool saved_partial_result = false;
  };
  struct BatchCancellation {
    explicit BatchCancellation(std::stop_token run)
        : run_stop(run, [this] { request_stop(); }) {}

    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::stop_source stop_event;

    void request_stop() {
      stop.store(true);
      stop_event.request_stop();
    }

    std::stop_callback<std::function<void()>> run_stop;
  };
  struct NodeRt {
    const Node* node = nullptr;
    size_t idx = 0;
    bool in_targets = false;
    bool start = false;
    bool start_fired = false;
    std::optional<History::LastInput> start_input;
    std::map<std::string, std::deque<TokenGroup>>
        queues;  // connected inputs only
    uint64_t invocations = 0;
    uint64_t recorded_invocations = 0;
    uint64_t activations = 0;
    bool running = false;
    // No future arrival can enable another activation.
    bool closed = false;
    bool blocked_input = false;
    bool empty_collect_ready = false;
    bool halted = false;  // failed hard (e.g. maxRuns) and no longer scheduled
    bool saved_inputs =
        false;  // some of its inputs are saved values, not this run's
    CardState last = CardState::not_started;
    std::optional<FailureData> last_failure;
    std::optional<CardBuildPlan> build;
    std::optional<ExecutionTarget> build_target;
    // Groups of the latest activation, saved at the end of the run.
    std::optional<std::pair<uint64_t, std::vector<SavedInputGroup>>>
        last_inputs;
    // Merge can replay the latest arrival on each port independently.
    std::map<std::string, std::pair<uint64_t, SavedInputGroup>>
        last_port_inputs;
  };
  struct Work {
    size_t node = 0;
    uint64_t invocation = 0;
    std::vector<Token> inputs;
    bool from_saved = false;      // single-Card run on the saved inputs
    int64_t ready_at = now_ms();  // all inputs there (or the start fired)
    uint64_t activation = 0;
    std::vector<TokenGroup> groups;
    std::shared_ptr<BatchCancellation> cancellation;
    std::optional<InvocationSummaryData> prepared;
    const CardBuild* implementation = nullptr;
    PreparedExecution* execution = nullptr;
    std::optional<uint32_t> batch_index, batch_size;
    bool execution_list = false;
    std::optional<FailureData> preparation_failure;
  };
  // ---- persistence helpers -------------------------------------------------

  // A history, log or artifact write that did not commit: the run cannot
  // succeed, because its records would be incomplete.
  void persistence_lost(const std::string& what) {
    std::lock_guard<std::mutex> l(persist_mu_);
    if (!persist_failure_)
      persist_failure_ =
          FailureData::make(FailureKind::persistence_failed,
                            "saving the run history failed: " + what);
  }

  std::optional<FailureData> persist_failure() {
    std::lock_guard<std::mutex> l(persist_mu_);
    return persist_failure_;
  }

  void persist_status_locked() {
    auto s = summary_.status;
    svc_.history().submit(
        [s](sql::Db& db) { History::update_run(db, s); },
        [self = shared_from_this(), s](bool ok) {
          if (!ok) self->persistence_lost("run status");
          self->svc_.server()->broadcast(status_note(s, false));
        });
  }

  void set_state(RunState st) {
    std::lock_guard<std::mutex> l(mu_);
    if (summary_.status.state == st ||
        summary_.status.state == RunState::cancelling)
      return;
    summary_.status.state = st;
    if (st == RunState::running && !summary_.status.started_at)
      summary_.status.started_at = now_ms();
    persist_status_locked();
  }

  // The start of an invocation is persisted (it becomes an interrupted
  // failure after a crash); later stage changes are kept in memory for
  // queries and announced in the same order as the start.
  void card_status(const InvocationSummaryData& s, bool persist = false) {
    svc_.history().set_live(s);
    svc_.history().submit(
        [s, persist](sql::Db& db) {
          if (persist) History::put_status(db, s);
        },
        [self = shared_from_this(), s, persist](bool ok) {
          if (!ok && persist)
            self->persistence_lost("progress of " + path_key(s.card));
          self->svc_.server()->broadcast(card_note(s));
        });
  }

  void journal(const char* phase, const char* event,
               const std::optional<CardPath>& card, std::optional<uint64_t> inv,
               const std::string& message) {
    std::string line = journal_line(phase, event, card, inv, message);
    if (card) {
      card_log(*card, *inv, LogStream::journal, line);
      return;
    }
    auto ends = std::make_shared<std::vector<LogEnd>>();
    std::string run = run_id_;
    svc_.history().submit(
        [line, run, ends](sql::Db& db) {
          *ends = History::append_log(db, run, {}, 0, LogStream::journal, line);
        },
        [self = shared_from_this(), ends](bool ok) {
          if (!ok) self->persistence_lost("journal");
          if (ok)
            for (const auto& e : *ends)
              self->svc_.server()->broadcast(log_note(e));
        });
  }

  void record_card_log(const CardPath& card, uint64_t inv, LogStream stream,
                       std::string text) {
    auto ends = std::make_shared<std::vector<LogEnd>>();
    std::string run = run_id_;
    svc_.history().submit(
        [card, inv, stream, text = std::move(text), run, ends](sql::Db& db) {
          *ends = History::append_log(db, run, card, inv, stream, text);
        },
        [self = shared_from_this(), ends](bool ok) {
          if (!ok) self->persistence_lost("log");
          if (ok)
            for (const auto& e : *ends)
              self->svc_.server()->broadcast(log_note(e));
        });
  }

  auto log_budget(const CardPath& card, uint64_t inv)
      -> workerd::InvocationLog& {
    return log_budgets_
        .try_emplace({path_key(card), inv}, kInvocationLogLimitBytes)
        .first->second;
  }

  void mark_card_log_truncated(const CardPath& card, uint64_t inv,
                               uint64_t limit, bool build = false) {
    const auto run = run_id_;
    svc_.history().submit(
        [run, card, inv, limit, build](sql::Db& db) {
          History::mark_log_truncated(db, run, card, inv, limit, build);
        },
        [self = shared_from_this(), run, card, inv](bool ok) {
          if (!ok) {
            self->persistence_lost("log truncation");
            return;
          }
          self->svc_.server()->broadcast(
              log_note({run, card, inv, LogStream::unspecified, std::nullopt}));
        });
  }

  void card_log(const CardPath& card, uint64_t inv, LogStream stream,
                const std::string& text) {
    card_log(card, inv, stream, {std::span(&text, 1)});
  }

  void card_log(const CardPath& card, uint64_t inv, LogStream stream,
                proc::LineChunk received) {
    if (svc_.packed_builds() && !svc_.packed_debug()) return;
    std::lock_guard<std::mutex> lock(log_policy_mu_);
    if (stream == LogStream::standard_output ||
        stream == LogStream::standard_error) {
      const auto& settings = doc_->resolution->find(card)->settings;
      const auto limit = settings.record_logs ? settings.log_limit_bytes : 0;
      const auto key = std::pair{path_key(card), inv};
      auto& live = live_log_budgets_[key];
      auto& recorded = captured_log_bytes_[key];
      constexpr uint64_t kLiveInvocationBytes = 8u << 20;
      constexpr uint64_t kLiveRunBytes = 128u << 20;
      const auto append = [&](std::string& chunk, uint64_t remaining) {
        for (const auto& line : received.lines) {
          const auto count = std::min<uint64_t>(line.size(), remaining);
          chunk.append(line.data(), count);
          remaining -= count;
          if (count < line.size()) return true;
        }
        return false;
      };

      if (!live.truncated) {
        std::string chunk;
        const auto remaining = std::min(kLiveInvocationBytes - live.bytes,
                                        kLiveRunBytes - live_log_bytes_);
        const bool truncated = append(chunk, remaining) || received.truncated;
        if (!chunk.empty() || truncated) {
          try {
            if (!live_logs_) live_logs_ = History::open_live_logs();
            sql::Transaction transaction(*live_logs_);
            auto ends = History::append_log(*live_logs_, run_id_, card, inv,
                                            stream, chunk);
            if (truncated)
              History::mark_log_truncated(*live_logs_, run_id_, card, inv,
                                          live.bytes + chunk.size());
            transaction.commit();
            live.bytes += chunk.size();
            live_log_bytes_ += chunk.size();
            live.truncated = truncated;
            for (auto& end : ends) {
              end.live = true;
              svc_.server()->broadcast(log_note(end));
            }
            if (truncated)
              svc_.server()->broadcast(
                  log_note({run_id_, card, inv, stream, std::nullopt, true}));
          } catch (const std::exception& error) {
            log::warn(std::string("live log: ") + error.what());
          }
        }
      }
      if (limit && !recorded.truncated) {
        std::string chunk;
        const bool truncated = append(chunk, limit - recorded.bytes);
        recorded.bytes += chunk.size();
        if (!chunk.empty())
          record_card_log(card, inv, stream, std::move(chunk));
        if (truncated) {
          recorded.truncated = true;
          mark_card_log_truncated(card, inv, limit);
        }
      }
      return;
    }
    auto& budget = log_budget(card, inv);
    if (budget.truncated()) return;
    // The budget still measures each log entry. Store the accepted prefix
    // with one insert and publish its end before any truncation notice.
    std::string chunk;
    for (const auto& line : received.lines) {
      if (!budget.append(stream, line)) break;
      chunk += line;
    }
    if (!chunk.empty()) record_card_log(card, inv, stream, std::move(chunk));
    if (received.truncated) budget.truncate();
    if (budget.truncated())
      mark_card_log_truncated(card, inv, kInvocationLogLimitBytes, true);
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
    for (const auto& t : plan_.targets)
      nodes_[res.index.at(path_key(t))].in_targets = true;
    std::set<std::string> seeded;
    for (const auto& s : plan_.seeds) seeded.insert(path_key(s));
    for (const auto& s : plan_.starts)
      if (!seeded.count(path_key(s)))
        nodes_[res.index.at(path_key(s))].start = true;
    if (p_.injected_inputs)
      for (const auto& input : *p_.injected_inputs)
        if (input.card != p_.card) {
          auto& node = nodes_[res.index.at(path_key(input.card))];
          node.queues.at(input.value.name).push_back(injected_group(input.value));
        }
    for (const auto& s : plan_.seeds) {
      NodeRt& n = nodes_[res.index.at(path_key(s))];
      std::set<std::string> outside;
      for (const auto& e : res.edges)
        if (e.to == n.idx && !nodes_[e.from].in_targets &&
            n.queues.at(e.to_port).empty())
          outside.insert(e.to_port);
      if (!outside.empty()) seed_saved(n, outside);
    }
    for (const auto& j : plan_.joins)
      seed_saved(nodes_[res.index.at(path_key(j.card))], j.saved_ports);
    for (auto& node : nodes_) {
      if (!node.start || node.queues.empty() ||
          (p_.injected_inputs && node.node->path == p_.card))
        continue;
      node.start_input = svc_.history().read([&](sql::Db& db) {
        return History::last_input(db, node.node->path);
      });
    }
  }

  auto saved_group(const NodeRt& node, const SavedInputGroup& saved,
                   uint64_t activation) -> TokenGroup {
    if (!node.node->input(saved.port))
      throw Failed(FailureKind::input_missing,
                   "saved input no longer matches " + saved.port);
    TokenGroup group{saved.port, saved.execution_list, {}, ++token_seq_};
    group.partial_result = true;
    group.saved_input = true;
    group.saved_partial_result = saved.partial_result;
    for (const auto& value : saved.values)
      group.members.push_back(
          {std::make_shared<const Value>(value), saved.port, {}, activation});
    return group;
  }

  auto injected_group(const Value& input) -> TokenGroup {
    TokenGroup group{input.name, false, {}, ++token_seq_};
    group.members.push_back(
        {std::make_shared<const Value>(input), input.name, {}, 0});
    return group;
  }

  auto missing_saved_inputs(const Node& node) -> Failed {
    const auto card = path_key(node.path);
    const auto& res = *doc_->resolution;
    std::set<std::string> triggers;
    // A trigger starts the whole resolved workflow, including unwired sources
    // and nested projects (the same scope used by plan()).
    for (const auto& candidate : res.nodes)
      if (!candidate.is_virtual && candidate.kind == CardType::Trigger)
        triggers.insert(path_key(candidate.path));
    std::string next;
    for (const auto& trigger : triggers) {
      if (!next.empty()) next += ", ";
      next += '`' + trigger + '`';
    }
    const auto fix =
        next.empty() ? "run the workflow from its trigger Card once first (it "
                       "saves every Card's inputs)"
                     : "run " + next +
                           " (workflow trigger; choose one) first, then run `" +
                           card + "` again";
    return Failed(FailureKind::input_missing,
                  "no saved inputs for " + card + ": " + fix, {{"card", card}});
  }

  auto reject_start(NodeRt& node, const FailureData& failure) -> void {
    start_failure_ = failure;
    node.halted = true;
    node.last = CardState::failed;
    node.last_failure = failure;
  }

  // Queue one complete saved group per external port. A Merge can replay
  // groups from different activations because it consumes them separately.
  void seed_saved(NodeRt& node, const std::set<std::string>& outside) {
    try {
      const auto last = svc_.history().read([&](sql::Db& db) {
        return History::last_input(db, node.node->path);
      });
      if (!last) throw missing_saved_inputs(*node.node);
      for (const auto& port : outside) {
        std::optional<History::LastInput> one;
        if (node.node->kind == CardType::Merge)
          one = svc_.history().read([&](sql::Db& db) {
            return History::last_port_input(db, node.node->path, port);
          });
        const auto& prior = one ? *one : *last;
        const auto group =
            std::ranges::find(prior.groups, port, &SavedInputGroup::port);
        if (group == prior.groups.end())
          throw Failed(FailureKind::input_missing,
                       "the saved inputs have no " + port);
        node.queues[port].push_back(
            saved_group(node, *group, prior.invocation));
        node.saved_inputs = true;
        seed_notes_.push_back(path_key(node.node->path) + "." + port +
                              " uses the saved values from run " +
                              prior.run_id + ", activation " +
                              std::to_string(prior.invocation));
      }
    } catch (const Failed& failure) {
      reject_start(node, failure.failure);
    }
  }

  // ---- scheduling
  // -------------------------------------------------------------

  bool downstream_has_space(const NodeRt& n) {
    for (const auto& out : n.node->outputs) {
      auto it = out_edges_.find({n.idx, out.name});
      if (it == out_edges_.end()) continue;
      for (const auto& [to, port] : it->second)
        if (nodes_[to].in_targets && !nodes_[to].closed &&
            nodes_[to].queues[port].size() >= kQueueCapacity)
          return false;
    }
    return true;
  }

  // Close exhausted streams before dispatch so a skipped join cannot hold
  // up a producer that still has work for a different consumer.
  auto close_exhausted_inputs() -> void {
    const auto& incoming = doc_->resolution->incoming;
    for (bool changed = true; changed;) {
      changed = false;
      for (auto& node : nodes_) {
        if (!node.in_targets || node.running || node.closed) continue;
        const auto kind = node.node->kind;
        const bool starts_without_input =
            node.start && !node.start_fired &&
            (kind != CardType::Merge || !node.queues.empty());
        if ((!node.halted && starts_without_input) || node.empty_collect_ready)
          continue;
        size_t exhausted = 0;
        bool failed = false;
        for (const auto& [port, queue] : node.queues) {
          if (!queue.empty()) continue;
          const auto& producer = nodes_[incoming[node.idx].at(port)];
          if (producer.in_targets && !producer.closed) continue;
          ++exhausted;
          failed |= producer.last == CardState::failed ||
                    producer.last == CardState::cancelled ||
                    producer.blocked_input;
        }
        if (!node.halted) {
          if (kind == CardType::Merge ? exhausted != node.queues.size()
                                     : exhausted == 0 && !node.queues.empty())
            continue;
          if (kind == CardType::Collect && node.activations == 0 && !failed) {
            node.empty_collect_ready = true;
            changed = true;
            continue;
          }
        }
        node.closed = true;
        node.blocked_input = failed;
        changed = true;
      }
    }
  }

  std::optional<Work> take_ready(NodeRt& n) {
    if (!n.in_targets || n.running || n.halted || n.closed || n.node->is_virtual)
      return std::nullopt;
    Work w;
    w.node = n.idx;
    if (n.empty_collect_ready) {
      if (!downstream_has_space(n)) return std::nullopt;
      n.empty_collect_ready = false;
      if (!n.queues.empty()) {
        // Save the empty input group so this Collect can replay that batch.
        w.groups.push_back({n.queues.begin()->first, true, {}});
      }
      return w;
    }
    if (n.start && !n.start_fired) {
      if (!downstream_has_space(n)) return std::nullopt;
      n.start_fired = true;
      if (p_.injected_inputs && n.node->path == p_.card) {
        for (const auto& input : *p_.injected_inputs)
          w.groups.push_back(injected_group(input.value));
        return w;
      }
      if (!n.queues.empty()) {
        try {
          const auto last = std::exchange(n.start_input, std::nullopt);
          if (!last) throw missing_saved_inputs(*n.node);
          if (n.node->kind != CardType::Merge)
            for (const auto& [port, _] : n.queues)
              if (std::ranges::find(last->groups, port, &SavedInputGroup::port) ==
                  last->groups.end())
                throw Failed(FailureKind::input_missing,
                             "the saved inputs have no value for " + port);
          for (const auto& group : last->groups)
            if (n.queues.contains(group.port))
              w.groups.push_back(saved_group(n, group, last->invocation));
          if (w.groups.empty()) throw missing_saved_inputs(*n.node);
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
    if (n.node->kind == CardType::Merge) {
      std::deque<TokenGroup>* best = nullptr;
      for (auto& [_, queue] : n.queues)
        if (!queue.empty() && (!best || queue.front().seq < best->front().seq))
          best = &queue;
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
    for (const auto& group : w.groups)
      w.ready_at = std::max(w.ready_at, group.arrived_at);
    return w;
  }

  void main() {
    // Graph state is private to this thread until the worker pools start.
    prepare_graph();
    set_state(RunState::preparing);
    journal("run", "started", std::nullopt, std::nullopt, "run started");
    for (const auto& note : seed_notes_)
      journal("run", "seeded", std::nullopt, std::nullopt, note);
    if (svc_.packed_fast()) {
      schedule_fast();
      finalize();
      return;
    }
    for (uint32_t lane = 0;
         lane < std::min(p_.max_concurrency, kMaxParallelWorkers); ++lane)
      workers_.emplace_back([this, lane] { worker_loop(lane); });
    // Coordinators wait for complete activations without occupying a worker
    // slot.
    for (uint32_t lane = 0; lane < p_.max_concurrency; ++lane)
      pool_.emplace_back([this, lane] { pool_loop(lane); });
    {
      std::unique_lock<std::mutex> l(mu_);
      while (true) {
        bool dispatched = false;
        if (!cancel_) {
          close_exhausted_inputs();
          bool progressed = true;
          while (progressed && running_ < p_.max_concurrency) {
            progressed = false;
            for (auto& n : nodes_) {
              if (running_ >= p_.max_concurrency) break;
              auto w = take_ready(n);
              if (!w) continue;
              n.running = true;
              w->activation = ++n.activations;
              w->cancellation =
                  std::make_shared<BatchCancellation>(work_stop_.get_token());
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
        cv_.wait(l);
      }
      pool_stop_ = true;
    }
    task_cv_.notify_all();
    for (auto& t : pool_) t.join();
    worker_cv_.notify_all();
    for (auto& t : workers_) t.join();
    // The environment owns residents until its run series ends.
    finalize();
  }

  void schedule_fast() {
    for (uint32_t lane = p_.max_concurrency; lane > 0; --lane)
      fast_lanes_.push_back(lane - 1);
    std::vector<std::future<void>> fan_out;
    for (;;) {
      std::vector<std::pair<Work, uint32_t>> ready;
      bool parallel = false;
      {
        std::unique_lock lock(mu_);
        if (!cancel_) {
          close_exhausted_inputs();
          for (auto& node : nodes_) {
            if (running_ >= p_.max_concurrency) break;
            auto work = take_ready(node);
            if (!work) continue;
            node.running = true;
            work->activation = ++node.activations;
            work->cancellation =
                std::make_shared<BatchCancellation>(work_stop_.get_token());
            ++running_;
            ready.emplace_back(std::move(*work), fast_lanes_.back());
            fast_lanes_.pop_back();
          }
        }
        if (ready.empty()) {
          if (running_ != 0) {
            cv_.wait(lock);
            continue;
          }
          if (!cancel_ && any_ready_locked())
            stall_failure_ = stall_reason_locked();
          break;
        }
        parallel = running_ > 1;
      }
      std::erase_if(fan_out, [](auto& worker) {
        if (worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
          return false;
        worker.get();
        return true;
      });
      if (parallel) {
        for (auto& [work, lane] : ready)
          fan_out.push_back(
              std::async(std::launch::async,
                         [this, work = std::move(work), lane]() mutable {
                           execute_work(work, lane);
                         }));
      } else {
        auto& [work, lane] = ready.front();
        execute_work(work, lane);
      }
    }
    for (auto& worker : fan_out) worker.get();
  }

  FailureData stall_reason_locked() {
    for (auto& n : nodes_) {
      if (!n.in_targets || n.running || n.halted || n.closed || n.node->is_virtual)
        continue;
      for (const auto& out : n.node->outputs) {
        auto it = out_edges_.find({n.idx, out.name});
        if (it == out_edges_.end()) continue;
        for (const auto& [to, port] : it->second)
          if (nodes_[to].in_targets && !nodes_[to].closed &&
              nodes_[to].queues[port].size() >= kQueueCapacity)
            return FailureData::make(
                FailureKind::execution_limit,
                "the run cannot progress: " + path_key(nodes_[to].node->path) +
                    "." + port + " holds " + std::to_string(kQueueCapacity) +
                    " unconsumed values while its other inputs never arrive",
                {{"card", path_key(nodes_[to].node->path)}, {"port", port}});
      }
    }
    return FailureData::make(FailureKind::execution_limit,
                             "the run cannot progress");
  }

  bool any_ready_locked() {
    for (auto& n : nodes_) {
      if (!n.in_targets || n.running || n.halted || n.closed || n.node->is_virtual)
        continue;
      if (n.empty_collect_ready || (n.start && !n.start_fired)) return true;
      if (n.queues.empty()) continue;
      if (n.node->kind == CardType::Merge) {
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
      execute_work(w, lane);
    }
  }

  void execute_work(Work& work, uint32_t lane) {
    invoke_batch(work, lane);
    {
      std::lock_guard lock(mu_);
      nodes_[work.node].running = false;
      --running_;
      if (svc_.packed_fast()) fast_lanes_.push_back(lane);
    }
    cv_.notify_all();
  }

  // ---- invocation -----------------------------------------------------------

  struct Produced {
    std::vector<Value> outputs;
    std::optional<FailureData> failure;
    std::vector<std::pair<LogStream, std::string>> auxiliary_logs = {};
    bool auxiliary_logs_truncated = false;
    bool detached = false;
    std::optional<InvocationSummaryData> unpublished;
  };

  auto worker_loop(uint32_t lane) -> void {
    while (true) {
      std::function<void(uint32_t)> task;
      {
        std::unique_lock lock(mu_);
        worker_cv_.wait(lock,
                        [&] { return pool_stop_ || !worker_tasks_.empty(); });
        if (worker_tasks_.empty()) return;
        task = std::move(worker_tasks_.front());
        worker_tasks_.pop_front();
      }
      task(lane);
    }
  }

  auto publish_batch(const RunBatchData& batch,
                     const std::vector<Produced>& members = {}) -> void {
    svc_.history().write_sync([&](sql::Db& db) {
      History::put_batch(db, batch);
      if (batch.failed) return;
      for (const auto& member : members) {
        if (!member.unpublished) continue;
        auto summary = *member.unpublished;
        for (const auto& output : member.outputs)
          summary.outputs_sent.push_back(output.name);
        History::update_detail(db, summary);
      }
    });
    svc_.server()->broadcast(run_event_note(run_id_, [batch](FBB& b) {
      const auto data = wire::run_batch(b, batch);
      return std::make_pair(R::RunEventValue::RunBatchChanged,
                            R::CreateRunBatchChanged(b, data).Union());
    }));
  }

  auto prepare_build(NodeRt& rt, Work& work, uint32_t max_implementations,
                     uint32_t lane,
                     std::unique_ptr<PreparedExecution>& prepared)
      -> const CardBuildPlan& {
    const auto& node = *rt.node;
    auto& s = *work.prepared;

    BuildRequest request;
    request.node = &node;
    request.doc = doc_;
    request.jobs = p_.nix_jobs;
    request.cores = p_.nix_cores;
    request.timeout_seconds = node.settings.build_timeout_seconds;
    request.max_implementations = max_implementations;
    request.cancel = work.cancellation->stop_event.get_token();
    prepared = svc_.prepare_execution(
        node, invocation_dir(node, work.invocation), request, s);
    const bool reuses_builds = request.target.kind == WorkerKind::native;
    if (rt.build && reuses_builds && rt.build_target &&
        rt.build_target->kind == request.target.kind &&
        rt.build_target->system == request.target.system) {
      stage_begin(s, Phase::evaluate, StageOutcome::reused);
      stage_begin(s, Phase::build, StageOutcome::reused);
      try {
        enforce_implementation_limit(rt.build->implementations.size(),
                                     max_implementations);
      } catch (const NixImplementationLimit& error) {
        throw batch_limit(node, error.what());
      }
      if (!rt.build->implementations.empty()) {
        card_status(s, true);
        journal("prepare", "started", node.path, s.invocation,
                "invocation started");
      }
      return *rt.build;
    }
    // Until evaluation identifies a member, keep its diagnostics off the
    // invocation history. Use the same budget as the rest of its log, and
    // keep each delivered chunk's accepted prefix together as one history
    // write.
    workerd::InvocationLog pending_budget(kInvocationLogLimitBytes);
    std::vector<std::string> pending_logs;
    bool published = false;
    const auto flush_evaluation_log = [&] {
      std::lock_guard lock(log_policy_mu_);
      auto& budget = log_budget(node.path, s.invocation);
      budget = std::move(pending_budget);
      for (auto& chunk : pending_logs)
        record_card_log(node.path, s.invocation, LogStream::build,
                        std::move(chunk));
      if (budget.truncated())
        mark_card_log_truncated(node.path, s.invocation, kInvocationLogLimitBytes,
                                true);
      pending_logs.clear();
    };
    const auto trace_phase = [&](const StageTimeData& stage, int64_t end) {
      svc_.server()->broadcast(timeline_note(run_id_, node.path, s.invocation,
                                             stage.phase, lane, stage.start,
                                             end));
    };
    request.log = [&](proc::LineChunk received) {
      if (published)
        return card_log(node.path, s.invocation, LogStream::build, received);
      std::string chunk;
      for (const auto& line : received.lines) {
        if (!pending_budget.append(LogStream::build, line)) break;
        chunk += line;
      }
      if (!chunk.empty()) pending_logs.push_back(std::move(chunk));
      if (received.truncated) pending_budget.truncate();
    };
    request.on_phase = [&](Phase phase) {
      if (s.phase == phase) return;
      const auto now = now_ms();
      if (published) trace_phase(s.stages.back(), now);
      stage_begin(s, phase, StageOutcome::running, now);
      if (published) card_status(s);
    };
    request.on_build = [&](const std::string& derivation, bool active) {
      std::erase_if(s.building, [&](const BuildActivityData& b) {
        return b.derivation == derivation;
      });
      if (active) s.building.push_back({derivation, now_ms()});
      if (published) svc_.history().set_live(s);
    };
    request.on_plan = [&](uint32_t implementations) {
      if (implementations == 0) return;
      published = true;
      card_status(s, true);
      flush_evaluation_log();
      journal("prepare", "started", node.path, s.invocation,
              "invocation started");
      for (const auto& stage : s.stages)
        if (stage.end) trace_phase(stage, *stage.end);
    };
    set_state(RunState::preparing);
    try {
      rt.build = svc_.nix().build(request);
    } catch (...) {
      s.building.clear();
      if (!published) flush_evaluation_log();
      throw;
    }
    s.building.clear();
    rt.build_target = request.target;
    if (rt.build->implementations.empty()) {
      if (!pending_logs.empty() || pending_budget.truncated()) {
        std::string diagnostics;
        for (const auto& line : pending_logs) diagnostics += line;
        if (pending_budget.truncated())
          diagnostics += "\nEvaluation log truncated at " +
                         std::to_string(kInvocationLogLimitBytes) + " bytes";
        journal("evaluate", "empty_batch", std::nullopt, std::nullopt,
                path_key(node.path) + " activation " +
                    std::to_string(work.activation) + "\n" + diagnostics);
      }
    } else {
      trace_phase(s.stages.back(), now_ms());
      if (reuses_builds && rt.build->common.cached)
        stage_begin(s, Phase::build, StageOutcome::cached);
    }
    return *rt.build;
  }

  auto batch_limit(const Node& node, std::string message) -> Failed {
    return Failed(FailureKind::execution_limit, std::move(message),
                  {{"card", path_key(node.path)}});
  }

  auto input_combinations(const Node& node,
                          const std::vector<TokenGroup>& groups) -> uint32_t {
    size_t bytes = 0;
    for (const auto& group : groups) {
      for (const auto& member : group.members) {
        if (member.value->bytes.size() > kMaxBatchBytes - bytes)
          throw batch_limit(node, "input batch exceeds 32 MiB");
        bytes += member.value->bytes.size();
      }
    }
    if (node.kind == CardType::Collect) return 1;
    if (std::ranges::any_of(
            groups, [](const auto& group) { return group.members.empty(); }))
      return 0;
    uint32_t count = 1;
    for (const auto& group : groups) {
      if (group.members.size() > kMaxExecutionBatchSize / count)
        throw batch_limit(
            node, "input and run list expansion exceeds 256 combinations");
      count *= static_cast<uint32_t>(group.members.size());
    }
    return count;
  }

  auto batch_inputs(const Node& node, const std::vector<TokenGroup>& groups,
                    uint32_t index) -> std::vector<Token> {
    std::vector<Token> inputs;
    if (node.kind == CardType::Collect) {
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
    if ((svc_.packed_builds() && !svc_.packed_debug()) || batch.from_saved ||
        (rt.node->kind != CardType::Code && batch.groups.empty()))
      return;
    std::vector<SavedInputGroup> saved;
    for (const auto& group : batch.groups) {
      SavedInputGroup values{
          group.port, group.execution_list, {}, group.partial_result};
      for (const auto& member : group.members) {
        values.values.push_back(*member.value);
        values.values.back().name = group.port;
      }
      if (rt.node->kind == CardType::Merge)
        rt.last_port_inputs[group.port] =
            std::make_pair(batch.activation, values);
      saved.push_back(std::move(values));
    }
    rt.last_inputs = std::make_pair(batch.activation, std::move(saved));
  }

  auto route_batch(NodeRt& rt, const RunBatchData& batch,
                   const std::vector<Produced>& members, uint64_t first,
                   bool partial_result) -> void {
    for (const auto& output : batch.outputs) {
      const auto edge = out_edges_.find({rt.idx, output.port});
      if (edge == out_edges_.end()) continue;
      TokenGroup group{output.port, output.execution_list, {}};
      group.partial_result = partial_result;
      for (size_t index = 0; index < members.size(); ++index) {
        const auto value = std::ranges::find(members[index].outputs,
                                             output.port, &Value::name);
        if (value != members[index].outputs.end())
          group.members.push_back({std::make_shared<const Value>(*value),
                                   output.port, rt.node->path, first + index});
      }
      for (const auto& [to, port] : edge->second) {
        if (!nodes_[to].in_targets || nodes_[to].closed) continue;
        if (group.members.empty() && nodes_[to].node->kind != CardType::Collect)
          continue;
        group.port = port;
        group.seq = ++token_seq_;
        for (auto& member : group.members) member.port = port;
        nodes_[to].queues.at(port).push_back(group);
      }
    }
  }

  auto invoke_batch(Work& work, uint32_t lane) -> void {
    auto& rt = nodes_[work.node];
    const auto& node = *rt.node;
    {
      std::lock_guard lock(mu_);
      const bool partial =
          std::ranges::any_of(work.groups, &TokenGroup::saved_input);
      const bool derived =
          std::ranges::any_of(work.groups, [](const TokenGroup& group) {
            return group.saved_input ? group.saved_partial_result
                                     : group.partial_result;
          });
      if ((partial && !summary_.status.partial_run) ||
          (derived && !summary_.status.uses_partial_result)) {
        summary_.status.partial_run |= partial;
        summary_.status.uses_partial_result |= derived;
        persist_status_locked();
      }
    }
    const auto first = rt.invocations + 1;
    work.invocation = first;
    RunBatchData batch;
    batch.run_id = run_id_;
    batch.card = node.path;
    batch.activation = work.activation;
    batch.execution_list =
        node.kind != CardType::Collect &&
        std::ranges::any_of(work.groups, &TokenGroup::execution_list);
    std::unique_ptr<PreparedExecution> prepared;
    std::vector<CardBuild> implementations;
    uint32_t count = 0;
    bool reserved = false;
    const bool code = node.kind == CardType::Code;
    std::optional<FailureData> failure;
    try {
      if (work.activation > node.settings.max_runs)
        throw Failed(
            FailureKind::execution_limit,
            "maxRuns " + std::to_string(node.settings.max_runs) + " exceeded",
            {{"card", path_key(node.path)},
             {"max_runs", std::to_string(node.settings.max_runs)}});
      count = input_combinations(node, work.groups);
      remember_inputs(rt, work);
      if (code && count != 0) {
        work.prepared = new_invocation(work);
        auto& s = *work.prepared;
        s.state = CardState::running;
        stage_begin(s, Phase::prepare);
        const auto& plan = prepare_build(
            rt, work, kMaxExecutionBatchSize / count, lane, prepared);
        batch.execution_list |= plan.execution_list;
        for (size_t index = 0; index < plan.implementations.size(); ++index)
          implementations.push_back(plan.implementation(index));
        count *= static_cast<uint32_t>(implementations.size());
      }
      batch.invocation_count = count;
      if (code) {
        std::lock_guard lock(mu_);
        if (count > kMaxRunWorkers - worker_budget_)
          throw batch_limit(node,
                            "run exceeds the limit of 4096 worker invocations");
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
    const auto worker_probes =
        work.prepared ? work.prepared->worker_probes
                      : std::vector<Tsugumi::Data::WorkerProbeResultT>{};
    auto execute = [&](uint32_t index, uint32_t worker_lane) {
      // The first member carries the batch's preparation stages.
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
      if (index == 0) {
        member.prepared = std::move(work.prepared);
      } else if (code) {
        member.prepared = new_invocation(member);
        member.prepared->worker_probes = worker_probes;
      }
      if (code)
        member.implementation =
            &implementations.at(index % implementations.size());
      const auto inputs_index =
          code ? index / static_cast<uint32_t>(implementations.size()) : index;
      member.inputs = batch_inputs(node, work.groups, inputs_index);
      auto produced = invoke(member, worker_lane);
      {
        std::lock_guard lock(outcome_mu);
        if (!produced.failure && !produced.detached) {
          for (const auto& output : produced.outputs) {
            if (output.bytes.size() > kMaxBatchBytes - bytes) {
              produced.failure = FailureData::make(
                  FailureKind::execution_limit, "output batch exceeds 32 MiB",
                  {{"card", path_key(node.path)}});
              break;
            }
            bytes += output.bytes.size();
          }
        }
        if ((produced.failure || produced.detached) &&
            (!failure ||
             (failure->kind == FailureKind::cancelled && produced.failure &&
              produced.failure->kind != FailureKind::cancelled))) {
          failure = produced.failure.value_or(
              FailureData::make(FailureKind::interrupted, "batch interrupted"));
          work.cancellation->failed.store(!produced.detached &&
                                          !detaching_.stop_requested());
          work.cancellation->request_stop();
        }
        if (failure) produced.outputs.clear();
        members[index] = std::move(produced);
      }
    };

    if (failure) {
      work.preparation_failure = failure;
      invoke(work, lane);
      ++rt.invocations;
      ++rt.recorded_invocations;
    } else if (!code) {
      for (uint32_t index = 0; index < count && !work.cancellation->stop.load();
           ++index)
        execute(index, lane);
      rt.invocations += started.load();
    } else if (svc_.packed_fast() && count != 0) {
      for (uint32_t index = 0; index < count && !work.cancellation->stop.load();
           ++index)
        execute(index, lane);
      if (started.load() == 0) execute(0, lane);
      rt.invocations += count;
    } else if (count != 0) {
      std::vector<std::future<void>> pending;
      {
        std::lock_guard lock(mu_);
        const bool serial = node.settings.reuse_worker;
        const auto jobs = serial ? 1u : count;
        for (uint32_t index = 0; index < jobs; ++index) {
          auto task = std::make_shared<std::packaged_task<void(uint32_t)>>(
              [&, index, serial](uint32_t worker_lane) {
                if (serial) {
                  for (uint32_t member = 0;
                       member < count && !work.cancellation->stop.load();
                       ++member)
                    execute(member, worker_lane);
                  if (started.load() == 0) execute(0, worker_lane);
                } else {
                  execute(index, worker_lane);
                }
              });
          pending.push_back(task->get_future());
          worker_tasks_.push_back(
              [task](uint32_t worker_lane) { (*task)(worker_lane); });
        }
      }
      worker_cv_.notify_all();
      for (auto& future : pending) {
        try {
          future.get();
        } catch (const std::exception& error) {
          std::lock_guard lock(outcome_mu);
          if (!failure)
            failure =
                FailureData::make(FailureKind::internal_error, error.what());
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
    // Decide the batch's outcome under the cancellation lock. Serialize its
    // write and routing with other batches while allowing status and cancel
    // to run during the write.
    std::lock_guard publish(publication_mu_);
    std::unique_lock lock(mu_);
    if (cancel_.load())
      failure = FailureData::make(FailureKind::cancelled, "run cancelled");

    batch.complete = true;
    batch.failed = failure.has_value();
    if (!batch.failed) {
      for (const auto& port : node.outputs) {
        uint32_t sent = 0;
        for (const auto& member : members)
          if (std::ranges::find(member.outputs, port.name, &Value::name) !=
              member.outputs.end())
            ++sent;
        batch.outputs.push_back({port.name, sent, batch.execution_list});
      }
    }
    lock.unlock();
    try {
      publish_batch(batch, members);
    } catch (const Failed&) {
      persistence_lost("batch of " + path_key(node.path));
      failure = persist_failure();
    }
    lock.lock();
    if (failure) {
      const bool cancelled = failure->kind == FailureKind::cancelled;
      rt.last = cancelled ? CardState::cancelled : CardState::failed;
      rt.last_failure = failure;
      if (!first_failure_ && !cancelled) first_failure_ = failure;
      if (failure->kind == FailureKind::execution_limit) rt.halted = true;
    } else {
      rt.last = CardState::succeeded;
      route_batch(
          rt, batch, members, first,
          std::ranges::any_of(work.groups, &TokenGroup::partial_result));
    }
  }

  auto new_invocation(const Work& w) -> InvocationSummaryData {
    const auto& node = *nodes_[w.node].node;
    InvocationSummaryData s;
    s.run_id = run_id_;
    s.card = node.path;
    s.params = recorded_params(node.settings.params);
    s.invocation = w.invocation;
    s.batch_activation = w.activation;
    s.started_at = now_ms();
    s.ready_at = w.ready_at;
    return s;
  }

  auto invoke(Work& w, uint32_t lane) -> Produced {
    NodeRt& rt = nodes_[w.node];
    const Node& node = *rt.node;
    const bool prepared = w.prepared && !w.prepared->stages.empty();
    auto s = w.prepared ? std::move(*w.prepared) : new_invocation(w);
    s.batch_index = w.batch_index;
    s.batch_size = w.batch_size;
    if (w.implementation) {
      s.implementation_index = w.implementation->implementation_index;
      s.implementation_label = w.implementation->implementation_label;
    }
    std::vector<Value> inputs;
    std::vector<InputSourceData> sources;
    for (const auto& t : w.inputs) {
      inputs.push_back(*t.value);
      inputs.back().name = t.port;
      if (!t.from.empty())
        sources.push_back({t.port, t.from, t.from_inv});
    }
    Produced out;
    if (w.preparation_failure) {
      out.failure = w.preparation_failure;
    } else {
      s.state = CardState::running;
      if (!prepared) {
        if (node.kind == CardType::Code) {
          stage_begin(s, Phase::prepare, StageOutcome::reused);
          stage_begin(s, Phase::evaluate, StageOutcome::reused);
          stage_begin(s, Phase::build, StageOutcome::reused);
        } else {
          stage_begin(s, Phase::execute);
        }
        card_status(s, true);
        journal(phase_name(*s.phase), "started", node.path, w.invocation,
                "invocation started");
      }
      try {
        if (w.cancellation->stop.load())
          throw Failed(FailureKind::cancelled,
                       "batch cancelled before execution");
        if (node.kind == CardType::Code)
          out = run_code(rt, w, s, inputs, sources, lane);
        else {
          out = run_builtin(rt, w, s, inputs);
        }
      } catch (const ExecutionDetached&) {
        svc_.history().clear_live(s);
        out.detached = true;
        return out;
      } catch (const Failed& f) {
        out.failure = f.failure;
      } catch (const std::exception& e) {
        out.failure = FailureData::make(FailureKind::internal_error, e.what());
      }
      if (!out.failure && node.kind != CardType::Code) {
        stage_begin(s, Phase::validate);
        const auto started = now_ms();
        try {
          validate_outputs(node, out.outputs);
        } catch (const Failed& f) {
          out.failure = f.failure;
          out.outputs.clear();
        }
        svc_.server()->broadcast(timeline_note(run_id_, node.path, w.invocation,
                                               Phase::validate, lane, started,
                                               now_ms()));
      }
    }
    bool was_cancel =
        out.failure && out.failure->kind == FailureKind::cancelled;
    if (out.failure)
      stage_close(s,
                  was_cancel ? StageOutcome::cancelled : StageOutcome::failed);
    else if (!w.execution_list)
      for (const auto& v : out.outputs) s.outputs_sent.push_back(v.name);
    s.ended_at = now_ms();
    s.duration_ms = static_cast<uint64_t>(*s.ended_at - *s.started_at);
    if (out.failure && out.failure->args.empty())
      out.failure->args.emplace_back("card", path_key(node.path));
    s.state = out.failure
                  ? (was_cancel ? CardState::cancelled : CardState::failed)
                  : CardState::succeeded;
    s.failure = out.failure;
    if (out.failure && out.failure->diagnostic)
      card_log(node.path, w.invocation, LogStream::standard_error,
               "tsugumi: " + std::string(failure_kind_name(out.failure->kind)) +
                   ": " + *out.failure->diagnostic + "\n");
    s.artifact_outputs = History::output_artifacts(out.outputs);
    InvocationRecordData rec;
    rec.summary = s;
    rec.record_io =
        svc_.packed_builds() ? svc_.packed_debug() : node.settings.record_io;
    rec.input_sources = sources;
    if (s.state == CardState::succeeded) {
      // Records, artifacts and saved values are written in the save stage.
      stage_begin(s, Phase::save);
      s.state = CardState::running;
      card_status(s);
      s.state = CardState::succeeded;
    }
    if (rec.record_io) {
      if (node.kind != CardType::Collect || inputs.size() <= 1)
        rec.inputs = inputs;
      rec.outputs = out.outputs;
    }
    // The completion is durable before it is announced or its outputs move on.
    // The record holds the save stage as done; a failed write reports it
    // failed.
    {
      auto done = s;
      stage_close(done, was_cancel    ? StageOutcome::cancelled
                        : out.failure ? StageOutcome::failed
                                      : StageOutcome::done);
      rec.summary.stages = done.stages;
      rec.summary.outputs_sent = done.outputs_sent;
      rec.summary.worker_reused = done.worker_reused;
      rec.summary.emulator = done.emulator;
      rec.summary.secrets = done.secrets;
      try {
        History& h = svc_.history();
        const bool failed_batch =
            out.failure && w.execution && !detaching_.stop_requested();
        if (failed_batch) {
          w.cancellation->failed.store(true);
          w.cancellation->request_stop();
        }
        bool auxiliary_truncated = false;
        if (!out.auxiliary_logs.empty() || out.auxiliary_logs_truncated) {
          std::lock_guard lock(log_policy_mu_);
          auto& budget = log_budget(s.card, s.invocation);
          std::erase_if(out.auxiliary_logs, [&](const auto& entry) {
            return !budget.append(entry.first, entry.second);
          });
          if (out.auxiliary_logs_truncated) budget.truncate();
          auxiliary_truncated = budget.truncated();
        }
        std::map<LogStream, std::vector<LogEnd>> logs;
        h.write_sync([&](sql::Db& db) {
          for (const auto& [stream, text] : out.auxiliary_logs)
            logs[stream] = History::append_log(
                db, s.run_id, s.card, s.invocation, stream, text);
          if (auxiliary_truncated)
            History::mark_log_truncated(db, s.run_id, s.card, s.invocation,
                                        kInvocationLogLimitBytes, true);
          h.complete_invocation(db, rec);
        });
        for (const auto& [_, ends] : logs)
          for (const auto& end : ends) svc_.server()->broadcast(log_note(end));
        // The record holds the save stage as closed just before its commit.
        s.stages = done.stages;
        h.clear_live(s);
      } catch (const Failed&) {
        svc_.history().clear_live(s);
        persistence_lost("invocation " + std::to_string(w.invocation) + " of " +
                         path_key(node.path));
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
            s.state == CardState::succeeded   ? "succeeded"
            : s.state == CardState::cancelled ? "cancelled"
                                              : "failed",
            node.path, w.invocation,
            out.failure ? std::string(failure_kind_name(out.failure->kind)) +
                              ": " + out.failure->diagnostic.value_or("")
                        : "ok");
    if (w.execution_list && !out.failure) out.unpublished = std::move(s);
    return out;
  }

  // Save all groups from the latest activation. A per-port snapshot lets an
  // Merge reuse the last arrival on each outside port independently.
  auto save_last_inputs(
      const Node& node, uint64_t inv,
      std::vector<SavedInputGroup> inputs,
      std::map<std::string, std::pair<uint64_t, SavedInputGroup>> ports)
      -> void {
    if (cancel_.load()) return;
    const auto card = node.path;
    const auto run = run_id_;
    svc_.history().submit(
        [card, run, inv, inputs = std::move(inputs),
         ports = std::move(ports)](sql::Db& db) {
          History::put_last_input(db, card, run, inv, inputs);
          for (const auto& [_, entry] : ports)
            History::put_last_port_input(db, card, run, entry.first, entry.second);
        },
        [self = shared_from_this(), card](bool ok) {
          if (!ok) self->persistence_lost("saved inputs of " + path_key(card));
        });
  }

  auto validate_outputs(const Node& node, std::vector<Value>& outputs) -> void {
    if (node.kind == CardType::Code || node.kind == CardType::Collect ||
        node.kind == CardType::Merge)
      return;
    std::set<std::string> seen;
    for (auto& value : outputs) {
      const auto* port = node.output(value.name);
      if (!port || !seen.insert(value.name).second)
        throw Failed(FailureKind::output_invalid,
                     "unknown or duplicate output " + value.name,
                     {{"port", value.name}});
      value.type = port->type;
    }
  }

  stdfs::path invocation_dir(const Node& node, uint64_t inv) {
    return private_disk_scratch(svc_.run_scratch() / "runs") / run_id_ /
           "work" / path_key(node.path) / std::to_string(inv);
  }

  // ---- builtins
  // --------------------------------------------------------------

  auto run_builtin(NodeRt& rt, const Work& w, InvocationSummaryData& s,
                   const std::vector<Value>& in) -> Produced {
    const Node& node = *rt.node;
    Produced p;
    int64_t t0 = now_ms();
    switch (node.kind) {
      case CardType::Trigger: {
        FBB b;
        b.Finish(flatbuffers::Offset<void>(b.EndTable(b.StartTable())));
        p.outputs.push_back(
            {"out",
             types::empty_type(),
             std::string(reinterpret_cast<const char*>(b.GetBufferPointer()),
                         b.GetSize()),
             schema_id(*types::empty_type())});
        break;
      }
      case CardType::Collect:
        p.outputs.push_back(collect_values(node.outputs.front(), in,
                                           w.cancellation->stop));
        break;
      case CardType::Merge: {
        if (in.size() != 1)
          throw Failed(FailureKind::input_missing,
                       "merge consumes exactly one arrival");
        auto value = in.front();
        value.name = node.outputs.front().name;
        p.outputs.push_back(std::move(value));
        break;
      }
      default:
        throw Failed(
            FailureKind::unsupported,
            std::string(card_type_name(node.kind)) + " cards cannot run");
    }
    svc_.server()->broadcast(timeline_note(run_id_, node.path, s.invocation,
                                           Phase::execute, 0, t0, now_ms()));
    return p;
  }

  // ---- code Cards
  // -------------------------------------------------------------

  auto run_code(NodeRt& rt, Work& w, InvocationSummaryData& s,
                const std::vector<Value>& in,
                const std::vector<InputSourceData>& sources, uint32_t lane)
      -> Produced {
    const Node& node = *rt.node;
    const auto inv = s.invocation;
    s.artifact_sequence = svc_.history().allocate_invocation_sequence(s);
    int64_t last_transfer = 0;
    auto result = w.execution->execute(
        {node, *w.implementation, in, sources, s, invocation_dir(node, inv),
         w.cancellation->stop,
         [&] {
           return explicit_cancel_.load() || w.cancellation->failed.load();
         },
         detaching_.get_token(),
         [&](LogStream stream, const std::string& text) {
           card_log(node.path, inv, stream, text);
         },
         [service = &svc_](WorkerDisplayData display,
                           WorkerProc::DisplayCredit credit) {
           service->display(std::move(display), std::move(credit));
         },
         [&](int64_t begin, int64_t end) {
           svc_.server()->broadcast(timeline_note(
               run_id_, node.path, inv, Phase::execute, lane, begin, end));
         },
         [&](const std::function<void(sql::Db&)>& records) {
           stage_begin(s, Phase::execute);
           svc_.history().write_sync([&](sql::Db& db) {
             if (records) records(db);
             History::put_status(db, s);
           });
           card_status(s);
           set_state(RunState::running);
         },
         [&] { card_status(s); },
         [&](workerd::InvocationResult& completed) {
           stage_begin(s, Phase::validate);
           const auto started = now_ms();
           const auto trace_validation = [&] {
             svc_.server()->broadcast(timeline_note(run_id_, node.path, inv,
                                                    Phase::validate, lane,
                                                    started, now_ms()));
           };
           try {
             validate_outputs(node, completed.outputs);
             if (svc_.packed_debug())
               for (const auto& value : completed.outputs)
                 if (!flatbuffers::Verify(
                         *value.type->schema(), *value.type->root(),
                         reinterpret_cast<const uint8_t*>(value.bytes.data()),
                         value.bytes.size()))
                   throw Failed(FailureKind::output_invalid,
                                "invalid FlatBuffer for output " + value.name);
           } catch (...) {
             trace_validation();
             throw;
           }
           trace_validation();
         },
         node.settings.record_logs ? node.settings.log_limit_bytes : 0,
         [&](std::string_view direction, std::string_view name, uint64_t done,
             uint64_t total) {
           const auto now = now_ms();
           if (done && done != total && s.transfer_name == name &&
               now - last_transfer < 100)
             return;
           last_transfer = now;
           s.transfer_direction = direction;
           s.transfer_name = name;
           s.transfer_done = done;
           s.transfer_total = total;
           card_status(s);
         },
         [&](int64_t begin, int64_t end) {
           stage_begin(s, Phase::validate, StageOutcome::running, begin);
           svc_.server()->broadcast(timeline_note(
               run_id_, node.path, inv, Phase::validate, lane, begin, end));
         },
         w.cancellation->stop_event.get_token()});
    s.transfer_direction.clear();
    s.transfer_name.clear();
    Produced p;
    p.failure = std::move(result.failure);
    p.auxiliary_logs = std::move(result.auxiliary_logs);
    p.auxiliary_logs_truncated = result.auxiliary_logs_truncated;
    if (!p.failure) {
      for (auto& value : result.outputs) {
        if (svc_.packed_builds()) {
          const std::string_view bytes = value.bytes;
          for (const auto& id : History::output_artifact_ids(*value.type, bytes))
            if (svc_.resolve_artifact(id).empty())
              throw Failed(FailureKind::output_invalid,
                           "unavailable artifact UUID in output " + value.name,
                           {{"artifact", artifact_id_text(id)}});
        }
        value.schema = schema_id(*value.type);
      }
      p.outputs = std::move(result.outputs);
    }
    return p;
  }

  // ---- convergence
  // ------------------------------------------------------------

  // One line for the note of the run's commit, from what this run did rather
  // than what it planned. Failure messages and logs stay out of git, as the
  // outputs do: they are in the run's record.
  auto record(const std::vector<CardState>& final) const -> std::string {
    const auto* target = doc_->resolution->find(p_.card);
    Tsugumi::RPC::VersionRunFactsT record;
    record.whole = p_.card.empty() || (p_.propagate && target &&
                                      target->kind == CardType::Trigger);
    record.card = path_key(p_.card);
    record.propagate = p_.propagate;
    for (const auto& n : nodes_) {
      if (!n.in_targets || n.node->is_virtual) continue;
      const auto key = path_key(n.node->path);
      if (n.recorded_invocations > 0 &&
          !(record.whole && n.node->kind == CardType::Trigger)) {
        auto count = std::make_unique<Tsugumi::RPC::VersionInvocationCountT>();
        count->card = key;
        count->count = n.recorded_invocations;
        record.ran.push_back(std::move(count));
        record.saved_inputs |= n.saved_inputs;
      }
      if (n.last_failure && n.last_failure->kind != FailureKind::cancelled) {
        auto failure = std::make_unique<Tsugumi::RPC::VersionRunFailureT>();
        failure->card = key;
        failure->kind = failure_kind_name(n.last_failure->kind);
        record.failures.push_back(std::move(failure));
      } else if (n.activations == 0 && (final[n.idx] == CardState::blocked ||
                                        final[n.idx] == CardState::cancelled)) {
        record.never.push_back(key);
      }
    }
    return flat::encode<Tsugumi::RPC::VersionRunFacts>(record);
  }

  void finalize() {
    const auto& res = *doc_->resolution;
    std::vector<CardState> final(nodes_.size(), CardState::not_started);
    std::vector<std::vector<WaitingInputData>> waiting(nodes_.size());
    for (auto& n : nodes_) {
      if (!n.in_targets || n.node->is_virtual) continue;
      final[n.idx] =
          n.activations > 0 || n.halted ? n.last : CardState::skipped;
    }
    // Missing inputs from failed producers block their consumers. A missing
    // output from a successful producer, including a skipped chain, does not.
    for (bool changed = true; changed;) {
      changed = false;
      for (auto& n : nodes_) {
        if (!n.in_targets || n.node->is_virtual || n.activations > 0 ||
            n.halted)
          continue;
        if (final[n.idx] == CardState::blocked ||
            final[n.idx] == CardState::cancelled)
          continue;
        bool upstream_bad = n.blocked_input;
        for (const auto& e : res.edges)
          if (e.to == n.idx && n.queues.at(e.to_port).empty() &&
              nodes_[e.from].in_targets &&
              (final[e.from] == CardState::failed ||
               final[e.from] == CardState::blocked ||
               final[e.from] == CardState::cancelled))
            upstream_bad = true;
        const bool has_partial_inputs =
            std::ranges::any_of(n.queues, [](const auto& port) {
              return !port.second.empty();
            });
        if (upstream_bad || (cancel_ && has_partial_inputs)) {
          final[n.idx] = cancel_ ? CardState::cancelled : CardState::blocked;
          changed = true;
        }
      }
    }
    std::optional<FailureData> blocked_failure;
    for (auto& n : nodes_) {
      if (!n.in_targets || n.node->is_virtual || n.activations > 0 || n.halted)
        continue;
      CardState st = final[n.idx];
      if (st == CardState::blocked) {
        for (auto& [port, q] : n.queues) {
          if (!q.empty()) continue;
          WaitingInputData wi;
          wi.port = port;
          for (const auto& e : res.edges)
            if (e.to == n.idx && e.to_port == port)
              wi.upstream.push_back({res.nodes[e.from].path, e.from_port});
          waiting[n.idx].push_back(std::move(wi));
        }
        if (!blocked_failure)
          blocked_failure =
              FailureData::make(FailureKind::input_missing,
                                path_key(n.node->path) + " is missing inputs",
                                {{"card", path_key(n.node->path)}});
      }
      if (cancel_ && st == CardState::skipped)
        continue;  // never reached before the cancel
      InvocationRecordData rec;
      rec.summary.run_id = run_id_;
      rec.summary.card = n.node->path;
      rec.summary.params = recorded_params(n.node->settings.params);
      rec.summary.invocation = n.invocations + 1;
      rec.summary.state = st;
      rec.summary.ended_at = now_ms();
      rec.summary.waiting = waiting[n.idx];
      History& h = svc_.history();
      h.submit([&h, rec](sql::Db& db) { h.complete_invocation(db, rec); },
               [self = shared_from_this(), s = rec.summary](bool ok) {
                 if (!ok)
                   self->persistence_lost("final state of " + path_key(s.card));
                 self->svc_.server()->broadcast(card_note(s));
               });
    }
    for (auto& n : nodes_)
      if (n.last_inputs)
        save_last_inputs(*n.node, n.last_inputs->first,
                         std::move(n.last_inputs->second),
                         std::move(n.last_port_inputs));
    // Include any failures writing the final invocation records or inputs.
    svc_.history().flush();
    RunSummaryData sum;
    {
      std::lock_guard<std::mutex> l(mu_);
      finishing_ = true;
      sum = summary_;
    }
    auto& st = sum.status;
    if (cancel_ && interrupted_) {
      st.state = RunState::failed;
      st.failure =
          FailureData::make(FailureKind::interrupted,
                            "the environment stopped while the run was active");
    } else if (cancel_) {
      st.state = RunState::cancelled;
      st.failure =
          FailureData::make(FailureKind::cancelled, "the run was cancelled");
    } else if (auto pf = persist_failure()) {
      st.state = RunState::failed;
      st.failure = pf;
    } else if (start_failure_ || first_failure_ || stall_failure_ ||
               blocked_failure) {
      st.state = RunState::failed;
      st.failure = start_failure_   ? start_failure_
                   : first_failure_ ? first_failure_
                   : stall_failure_ ? stall_failure_
                                    : blocked_failure;
    } else {
      st.state = RunState::succeeded;
      st.failure.reset();
    }
    st.ended_at = now_ms();
    journal("run",
            sum.status.state == RunState::succeeded   ? "succeeded"
            : sum.status.state == RunState::cancelled ? "cancelled"
                                                      : "failed",
            std::nullopt, std::nullopt,
            sum.status.failure ? sum.status.failure->diagnostic.value_or("")
                               : "run finished");
    sched_.finished(std::move(sum), record(final));
  }

  Service& svc_;
  Scheduler& sched_;
  DocumentPtr doc_;
  RunParams p_;
  RunSummaryData summary_;
  const RunPlanData plan_;
  std::string run_id_;
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::condition_variable task_cv_;
  std::vector<NodeRt> nodes_;
  std::map<std::pair<size_t, std::string>,
           std::vector<std::pair<size_t, std::string>>>
      out_edges_;
  std::deque<Work> tasks_;
  std::vector<std::thread> pool_;
  std::condition_variable worker_cv_;
  std::deque<std::function<void(uint32_t)>> worker_tasks_;
  std::vector<std::thread> workers_;
  std::vector<uint32_t> fast_lanes_;  // guarded by mu_ once scheduling starts
  uint32_t worker_budget_ = 0;
  bool pool_stop_ = false;
  uint32_t running_ = 0;
  uint64_t token_seq_ = 0;
  std::vector<std::string> seed_notes_;  // journaled once the run has started
  std::stop_source work_stop_;
  std::atomic<bool> cancel_{false};
  std::atomic<bool> explicit_cancel_{false};
  std::stop_source detaching_;
  bool finishing_ =
      false;  // under mu_: the final status is decided, a cancel comes too late
  bool interrupted_ = false;
  std::optional<FailureData> first_failure_;
  std::optional<FailureData> start_failure_;
  std::optional<FailureData> stall_failure_;
  std::mutex log_policy_mu_;
  std::unique_ptr<sql::Db> live_logs_;
  struct Capture {
    uint64_t bytes = 0;
    bool truncated = false;
  };
  uint64_t live_log_bytes_ = 0;
  std::map<std::pair<std::string, uint64_t>, Capture> live_log_budgets_;
  std::map<std::pair<std::string, uint64_t>, Capture> captured_log_bytes_;
  std::map<std::pair<std::string, uint64_t>, workerd::InvocationLog>
      log_budgets_;
  std::mutex publication_mu_;
  std::mutex persist_mu_;
  std::optional<FailureData> persist_failure_;
};

// ---------------------------------------------------------------------------

Scheduler::Scheduler(Service& svc)
    : svc_(svc), reaper_([this] { reap_completed(); }) {}

Scheduler::~Scheduler() { shutdown(); }

auto Scheduler::live_log(const std::string& run_id,
                         const std::function<LogRead(sql::Db&)>& read) -> LogRead {
  std::shared_ptr<RunExec> run;
  {
    std::lock_guard lock(mu_);
    run = active_;
  }
  return run ? run->live_log(run_id, read) : LogRead{};
}

void Scheduler::recover_interrupted() {
  auto runs = svc_.history().read([](sql::Db& db) {
    auto result = History::unfinished_runs(db);
    std::set<std::string> known(result.begin(), result.end());
    auto pending = db.prepare(
        "SELECT DISTINCT j.run_id FROM remote_jobs AS j "
        "JOIN runs AS r ON r.run_id=j.run_id");
    while (pending.step()) {
      auto id = pending.text(0);
      if (known.insert(id).second) result.push_back(std::move(id));
    }
    return result;
  });
  svc_.collect_interrupted_artifacts(runs);
  for (const auto& run_id : runs) {
    Service::DetachedArtifacts detached;
    try {
      svc_.history().write_sync([&](sql::Db& db) {
        auto r = History::run(db, run_id);
        if (!r) return;
        svc_.expire_artifacts(db, run_id, detached);
        r->status.state = RunState::failed;
        r->status.failure =
            FailureData::make(FailureKind::interrupted,
                              "tsugumid restarted while the run was active");
        r->status.ended_at = now_ms();
        History::update_run(db, r->status);
        History::interrupt_batches(db, run_id);
        // In-progress invocations become interrupted failures; nothing is
        // re-run.
        auto st = db.prepare(
            "SELECT s.card, s.invocation, e.environment_id FROM statuses AS s "
            "LEFT JOIN active_environments AS e ON e.run_id=s.run_id AND "
            "e.card=s.card "
            "AND e.invocation=s.invocation WHERE s.run_id=?");
        st.bind(1, run_id);
        std::vector<InvocationRecordData> recs;
        while (st.step()) {
          auto summary =
              History::status(db, run_id, path_from_key(st.text(0)), st.u64(1));
          if (!summary) continue;
          InvocationRecordData rec;
          rec.summary = std::move(*summary);
          if (rec.summary.environment_id.empty())
            rec.summary.environment_id = st.text(2);
          rec.summary.state = CardState::failed;
          rec.summary.ended_at = r->status.ended_at;
          if (rec.summary.started_at)
            rec.summary.duration_ms = static_cast<uint64_t>(std::max<int64_t>(
                0, *rec.summary.ended_at - *rec.summary.started_at));
          stage_close(rec.summary, StageOutcome::failed);
          auto remote = db.prepare(
              "SELECT job_id FROM remote_jobs WHERE run_id=? AND card=? AND "
              "invocation=?");
          remote.bind(1, run_id)
              .bind(2, path_key(rec.summary.card))
              .bind(3, rec.summary.invocation);
          if (remote.step()) {
            rec.summary.execution_job_id = remote.text(0);
            rec.summary.execution_state = RemoteExecutionState::interrupted;
          }
          rec.summary.failure = r->status.failure;
          recs.push_back(std::move(rec));
        }
        for (const auto& rec : recs)
          svc_.history().complete_invocation(db, rec);
      });
    } catch (...) {
      svc_.finish_artifact_expiry(detached, false);
      throw;
    }
    svc_.finish_artifact_expiry(detached, true);
    log::warn("run " + run_id + " was interrupted by a restart");
  }
}

auto Scheduler::start_remote_cleanup() -> void {
  remote_cleanup_ = std::thread([this] {
    while (!stopping_.load()) {
      try {
        svc_.remote_execution().release_completed();
      } catch (const std::exception& error) {
        log::warn(std::string("external job cleanup: ") + error.what());
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  });
}

namespace {

struct PlanEdges {
  explicit PlanEdges(const Resolution& res)
      : incoming(res.nodes.size()), outgoing(res.nodes.size()) {
    for (const auto& edge : res.edges) {
      incoming[edge.to].push_back(&edge);
      outgoing[edge.from].push_back(&edge);
    }
  }

  std::vector<std::vector<const Edge*>> incoming;
  std::vector<std::vector<const Edge*>> outgoing;
};

// Cards waiting for a Draft propagate that wait downstream. A Merge waits
// only when none of its connected inputs can supply an arrival.
auto find_draft_waiting(const Resolution& res, const PlanEdges& edges)
    -> std::set<size_t> {
  std::set<size_t> waits;
  std::vector<size_t> waiting;
  std::vector<size_t> remaining(res.nodes.size());
  for (size_t i = 0; i < res.nodes.size(); ++i) {
    if (res.nodes[i].kind == CardType::Draft) {
      waits.insert(i);
      waiting.push_back(i);
    } else if (res.nodes[i].kind == CardType::Collect) {
      // Collect can emit an empty batch even if its producer never runs.
      remaining[i] = edges.incoming[i].size() + 1;
    } else if (res.nodes[i].kind == CardType::Merge) {
      remaining[i] = edges.incoming[i].size();
    } else {
      remaining[i] = 1;
    }
  }
  for (size_t i = 0; i < waiting.size(); ++i) {
    for (const auto* edge : edges.outgoing[waiting[i]]) {
      const auto to = edge->to;
      if (!waits.contains(to) && --remaining[to] == 0) {
        waits.insert(to);
        waiting.push_back(to);
      }
    }
  }
  return waits;
}

}  // namespace



RunPlanData Scheduler::plan(const Resolution& res, const CardPath& card,
                            bool propagate) {
  RunPlanData out;
  const Node* target = res.find(card);
  if (!target)
    throw Failed(FailureKind::not_found, "unknown card " + path_key(card),
                 {{"card", path_key(card)}});
  const PlanEdges edges(res);
  size_t t = res.index.at(path_key(card));
  std::set<size_t> targets;
  std::vector<size_t> starts;
  std::vector<size_t> seeds;
  auto inside = [&](size_t i) {
    const auto& p = res.nodes[i].path;
    return p.size() > card.size() &&
           std::equal(card.begin(), card.end(), p.begin());
  };
  if (target->is_virtual) {
    // A subgraph runs like one Card: its inner Cards run; those fed across the
    // boundary use their saved values for the outside ports; inner sources
    // start.
    for (size_t i = 0; i < res.nodes.size(); ++i) {
      if (!inside(i) || res.nodes[i].is_virtual ||
          res.nodes[i].kind == CardType::Draft)
        continue;
      targets.insert(i);
      const auto& incoming = edges.incoming[i];
      const bool from_outside = std::ranges::any_of(
          incoming, [&](const Edge* edge) { return !inside(edge->from); });
      if (from_outside)
        seeds.push_back(i);
      else if (incoming.empty())
        starts.push_back(i);
    }
    if (targets.empty())
      throw Failed(FailureKind::invalid_request,
                   "the subgraph has no executable Cards",
                   {{"card", path_key(card)}});
    if (propagate) {
      std::vector<size_t> stack(targets.begin(), targets.end());
      while (!stack.empty()) {
        size_t i = stack.back();
        stack.pop_back();
        for (const auto* edge : edges.outgoing[i]) {
          const auto to = edge->to;
          if (!targets.count(to) && res.nodes[to].kind != CardType::Draft) {
            targets.insert(to);
            stack.push_back(to);
          }
        }
      }
    }
  } else if (!propagate) {
    targets.insert(t);
    starts.push_back(t);
  } else if (target->kind == CardType::Trigger) {
    // Whole run: every executable Card; Start Cards are the trigger and all
    // sources without connected inputs.
    for (size_t i = 0; i < res.nodes.size(); ++i) {
      if (res.nodes[i].is_virtual || res.nodes[i].kind == CardType::Draft)
        continue;
      targets.insert(i);
      if (edges.incoming[i].empty()) starts.push_back(i);
    }
  } else {
    std::vector<size_t> stack = {t};
    while (!stack.empty()) {
      size_t i = stack.back();
      stack.pop_back();
      if (!targets.insert(i).second) continue;
      for (const auto* edge : edges.outgoing[i]) stack.push_back(edge->to);
    }
    starts.push_back(t);
  }
  // A Draft is not implemented yet: it does not run, nor do the Cards that
  // need its outputs (a connected input fed by a Draft or by a Card waiting
  // on one). Everything else runs as usual; a Card started on its own keeps
  // its saved inputs.
  const auto waits = find_draft_waiting(res, edges);
  for (size_t i : waits)
    if (i != t || target->kind == CardType::Draft) targets.erase(i);
  if (target->kind == CardType::Draft)
    throw Failed(FailureKind::invalid_request,
                 path_key(card) + " is a Draft and is not implemented yet",
                 {{"card", path_key(card)}});
  std::erase_if(starts, [&](size_t i) { return !targets.count(i); });
  std::erase_if(seeds, [&](size_t i) { return !targets.count(i); });
  for (size_t i : targets) {
    out.targets.push_back(res.nodes[i].path);
    if (res.nodes[i].kind == CardType::Code)
      out.build_candidates.push_back(res.nodes[i].path);
    out.diagnostics.insert(out.diagnostics.end(), res.nodes[i].diags.begin(),
                           res.nodes[i].diags.end());
    out.diagnostics.insert(out.diagnostics.end(),
                           res.nodes[i].exec_diags.begin(),
                           res.nodes[i].exec_diags.end());
  }
  for (size_t s : starts) out.starts.push_back(res.nodes[s].path);
  for (size_t s : seeds) {
    out.seeds.push_back(res.nodes[s].path);
    out.starts.push_back(res.nodes[s].path);
  }
  // A Card further down that waits for all its inputs would never run without
  // the ports nothing in this run feeds; a port something in this run feeds
  // gets that value alone. Merge runs on what arrives.
  std::set<size_t> given(starts.begin(), starts.end());
  given.insert(seeds.begin(), seeds.end());
  for (size_t i : targets) {
    if (res.nodes[i].kind == CardType::Merge || given.count(i)) continue;
    std::set<std::string> fed, saved;
    for (const auto* edge : edges.incoming[i]) {
      if (targets.count(edge->from))
        fed.insert(edge->to_port);
      else if (!waits.count(edge->from))
        saved.insert(edge->to_port);
    }
    for (const auto& port : fed) saved.erase(port);
    if (!saved.empty())
      out.joins.push_back({res.nodes[i].path, std::move(saved)});
  }
  return out;
}

std::string Scheduler::accept(DocumentPtr doc, const RunParams& p) {
  const auto check_available = [&] {
    if (stopping_.load())
      throw Failed(FailureKind::interrupted, "the project is shutting down");
    if (active_)
      throw Failed(FailureKind::busy, "another run is active",
                   {{"run_id", *active_->status().run_id}});
  };
  {
    std::lock_guard lock(mu_);
    check_available();
  }
  auto plan = Scheduler::plan(*doc->resolution, p.card, p.propagate);
  if (p.injected_inputs)
    for (const auto& input : *p.injected_inputs)
      if (std::ranges::find(plan.targets, input.card) == plan.targets.end())
        throw Failed(FailureKind::input_invalid,
                     "starting input would not reach " + path_key(input.card));
  if (!svc_.packed_builds()) {
    const auto services = svc_.remote_targets();
    for (const auto& path : plan.targets)
      if (auto diagnostic =
              svc_.execution_diagnostic(*doc->resolution->find(path), services))
        plan.diagnostics.push_back(std::move(*diagnostic));
  }
  std::vector<std::string> errors;
  for (const auto& d : plan.diagnostics) {
    if (!d.error) continue;
    // A partial run's start Card gets its inputs from the saved inputs.
    if (d.code == "input_missing" && d.card && *d.card == p.card) continue;
    errors.push_back((d.card ? path_key(*d.card) + ": " : "") + d.message);
  }
  if (!errors.empty()) {
    std::string msg = "the run plan has errors: " + errors.front();
    if (errors.size() > 1)
      msg += " (and " + std::to_string(errors.size() - 1) + " more)";
    throw Failed(FailureKind::validation_failed, msg,
                 {{"count", std::to_string(errors.size())}});
  }
  for (const auto& path : plan.build_candidates)
    svc_.host_path_binds(*doc->resolution->find(path));
  RunSummaryData s;
  s.status.state = RunState::queued;
  s.status.rev = doc->rev;
  s.status.execution_rev = doc->execution_rev;
  s.status.accepted_at = now_ms();
  if (!p.commit.empty()) s.status.commit = p.commit;
  s.source_hash = doc->execution_fingerprint;
  s.target = p.card;
  s.propagate = p.propagate;
  std::string receipt;
  std::lock_guard lock(mu_);
  check_available();
  // The accepted sequence and receipt commit together, before any processing.
  svc_.history().write_sync([&](sql::Db& db) {
    s.status.run_id = History::next_run_id(db);
    FBB b;
    auto st = wire::run_status(b, s.status);
    auto ra = R::CreateRunAccepted(b, b.CreateString(p.op_id), st, false);
    auto rc = R::CreateOperationReceipt(
        b, b.CreateString(p.op_id), R::ReceiptValue::RunAccepted, ra.Union());
    b.Finish(rc);
    receipt.assign(reinterpret_cast<const char*>(b.GetBufferPointer()),
                   b.GetSize());
    History::insert_run(db, s, p.op_id);
    std::set<CardPath> cards;
    const auto include = [&](CardPath path) {
      while (!path.empty() && cards.insert(path).second) path.pop_back();
    };
    include(p.card);
    for (const auto& target : plan.targets) include(target);
    auto write = db.prepare(
        "INSERT INTO run_definition_paths (run_id,card,path) VALUES(?,?,?)");
    for (const auto& card : cards) {
      const auto& definition = doc->resolution->find(card)->def_dir;
      write.reset();
      write.bind(1, *s.status.run_id)
          .bind(2, path_key(card))
          .bind(3, definition + "_card.json")
          .run();
    }
    if (svc_.options().fault == "run_acceptance")
      throw sql::Error("injected: run acceptance write failed");
    History::put_substituters(db, *s.status.run_id, svc_.substituters());
    History::put_receipt(db, p.op_id, "run", p.digest, receipt);
  });
  svc_.server()->broadcast(status_note(s.status, true));
  active_ = std::make_shared<RunExec>(svc_, *this, doc, p, s, std::move(plan));
  active_->start();
  return receipt;
}

std::optional<RunStatusData> Scheduler::active_status() {
  std::lock_guard<std::mutex> l(mu_);
  if (!active_) return std::nullopt;
  return active_->status();
}

RunStatusData Scheduler::cancel(const std::string& run_id) {
  {
    std::lock_guard<std::mutex> l(mu_);
    if (active_ && active_->status().run_id == run_id)
      return active_->request_cancel(false);
  }
  auto r = svc_.history().read(
      [&](sql::Db& db) { return History::run(db, run_id); });
  if (!r)
    throw Failed(FailureKind::not_found, "unknown run " + run_id,
                 {{"run_id", run_id}});
  return r->status;
}

namespace {

// What a Card is made of: its settings (whatever the spacing of their JSON, a
// client writes its own) and the files of its definition folder.
std::string card_content(const Document& doc, const Node& n) {
  std::string out;
  if (n.def_dir.empty()) return out;
  if (n.project_dir.empty()) {
    if (auto d = doc.definitions.find(n.path.back());
        d != doc.definitions.end()) {
      bool quoted = false, escaped = false;
      for (char c : d->second.card_json) {
        if (escaped)
          escaped = false;
        else if (quoted && c == '\\')
          escaped = true;
        else if (c == '"')
          quoted = !quoted;
        else if (!quoted && (c == ' ' || c == '\n' || c == '\t' || c == '\r'))
          continue;
        out += c;
      }
    }
  }
  for (auto s = doc.sources.lower_bound(n.def_dir);
       s != doc.sources.end() && s->first.starts_with(n.def_dir); ++s) {
    out += '\0' + s->first + '\0' + s->second.hash;
  }
  return out;
}

}  // namespace

void Scheduler::document_changed(const Document& before,
                                 const Document& after) {
  if (!before.resolution || !after.resolution) return;
  if (before.resolution == after.resolution) return;
  for (const auto& n : before.resolution->nodes) {
    if (!n.settings.reuse_worker) continue;
    const auto* next = after.resolution->find(n.path);
    if (!next || card_content(before, n) != card_content(after, *next))
      svc_.invalidate_resident(path_key(n.path));
  }
}

auto Scheduler::finished(RunSummaryData sum, std::string record) -> void {
  std::unique_lock lock(mu_);
  Service::DetachedArtifacts detached;
  bool committed = false;
  // Release the slot before the terminal status can appear in history. Keep
  // admission locked until the result commits and both events are queued.
  auto done = std::move(active_);
  try {
    svc_.history().write_sync([&](sql::Db& db) {
      svc_.expire_artifacts(db, *sum.status.run_id, detached);
      History::update_run(db, sum.status);
      History::interrupt_batches(db, *sum.status.run_id);
      // Detached external invocations are still pending after the local
      // graph ends. Startup records interruption before remote cleanup.
      db.prepare(
            "DELETE FROM statuses WHERE run_id=? AND NOT EXISTS ("
            "SELECT 1 FROM remote_jobs AS j WHERE "
            "j.run_id=statuses.run_id AND j.card=statuses.card "
            "AND j.invocation=statuses.invocation)")
          .bind(1, *sum.status.run_id)
          .run();
    });
    committed = true;
  } catch (const Failed&) {
    if (sum.status.state == RunState::succeeded) {
      sum.status.state = RunState::failed;
      sum.status.failure = FailureData::make(FailureKind::persistence_failed,
                                             "saving the run result failed");
    }
  }
  svc_.server()->broadcast(status_note(sum.status, false));
  svc_.server()->broadcast(finished_note(sum));
  completions_.push_back(
      {std::move(done), std::move(sum.status), std::move(record)});
  finished_.notify_all();
  lock.unlock();
  svc_.finish_artifact_expiry(detached, committed);
}

auto Scheduler::reap_completed() -> void {
  std::unique_lock lock(mu_);
  while (true) {
    finished_.wait(lock, [this] {
      return !completions_.empty() || (stopping_.load() && !active_);
    });
    if (completions_.empty()) return;
    auto completion = std::move(completions_.front());
    completions_.pop_front();
    lock.unlock();
    completion.run->join();
    svc_.note_run_end(completion.status, completion.record);
    completion.run.reset();
    lock.lock();
  }
}

void Scheduler::shutdown() {
  {
    std::unique_lock lock(mu_);
    stopping_.store(true);
    if (active_) active_->request_cancel(true);
    finished_.wait(lock, [this] { return !active_; });
    finished_.notify_all();
  }
  if (remote_cleanup_.joinable()) remote_cleanup_.join();
  if (reaper_.joinable()) reaper_.join();
}

}  // namespace tsugumi
