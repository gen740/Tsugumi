// Run, Nix evaluation, history, value, artifact, declared view file and log handlers.
#include <sys/stat.h>

#include <flatbuffers/reflection.h>

#include <algorithm>
#include <charconv>
#include <mutex>
#include <set>
#include <stop_token>

#include "nix.hh"
#include "scheduler.hh"
#include "service.hh"
#include "tsugumi/document/schema_id.hh"
#include "tsugumi/protocol/log_range.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/log.hh"
#include "tsugumi/util/system.hh"
#include "tsugumi/util/text.hh"
#include "tsugumi/workerd/artifact_id.hh"
#include "tsugumi/workerd/frame.hh"
#include "tsugumi/workerd/staging.hh"
#include "tsugumi/workerd/store.hh"
#include "version_record.hh"

namespace tsugumi {

namespace R = Tsugumi::RPC;
namespace D = Tsugumi::Data;
using FBB = flatbuffers::FlatBufferBuilder;
namespace stdfs = std::filesystem;

namespace {
auto artifact_label(const ArtifactData& artifact) -> std::string {
  return std::to_string(artifact.sequence) + "." +
         std::to_string(artifact.counter);
}

constexpr uint32_t kInlineLimit = 256u << 10;
constexpr size_t kPagePayload = (1u << 20) - 8192;

uint32_t page_limit(std::optional<uint32_t> v) {
  uint32_t x = v.value_or(100);
  if (x < 1 || x > 500)
    throw Failed(FailureKind::invalid_request,
                 "limit must be between 1 and 500");
  return x;
}

// The size of a page of file or value content.
uint64_t content_limit(std::optional<uint32_t> limit_arg) {
  uint32_t limit = limit_arg.value_or(1u << 20);
  if (limit < 1 || limit > (1u << 20))
    throw Failed(FailureKind::invalid_request,
                 "limit_bytes must be 1..1048576");
  return limit;
}

auto require_run_id(const flatbuffers::String& s, const char* what) -> void {
  if (!crypto::is_run_id(s.string_view()))
    throw Failed(FailureKind::invalid_request,
                 std::string(what) + " must be a positive project run number");
}

auto packed_inputs(
    const Resolution& graph, const CardPath& path,
    const flatbuffers::Vector<flatbuffers::Offset<R::PackedRunInput>>& inputs)
    -> std::vector<RunParams::InjectedInput> {
  const auto* card = graph.find(path);
  if (!card || card->kind == CardType::Draft)
    throw Failed(FailureKind::invalid_request,
                 "typed inputs require an implemented Card");
  std::map<std::string, std::pair<size_t, std::string>> connected;
  for (const auto& port : card->inputs) {
    auto end = graph.endpoint(path, port.name, false);
    if (end && graph.incoming.at(end->first).contains(end->second))
      connected.emplace(port.name, std::move(*end));
  }
  std::vector<RunParams::InjectedInput> values;
  size_t remaining = TSUGUMI_MESSAGE_MAX;
  for (const auto* input : inputs) {
    const auto name = input->name()->str();
    const auto target = connected.extract(name);
    if (!target)
      throw Failed(FailureKind::input_invalid,
                   "unknown or duplicate starting input " + name);
    const auto& [to, port] = target.mapped();
    const auto schema = input->schema()->str();
    types::TypeRef type;
    for (const auto& edge : graph.edges) {
      if (edge.to != to || edge.to_port != port) continue;
      const auto& producer = graph.nodes[edge.from];
      if (producer.kind == CardType::Merge) {
        const auto candidates = graph.merge_candidates(edge.from, edge.from_port);
        if (auto found = candidates.find(schema); found != candidates.end())
          type = found->second;
      } else if (const auto* output = producer.output(edge.from_port);
                 output && output->type && schema_id(*output->type) == schema) {
        type = output->type;
      }
    }
    if (!type)
      throw Failed(FailureKind::input_invalid,
                   "starting input has a different schema: " + name);
    const auto* bytes = input->bytes();
    if (bytes->size() > remaining)
      throw Failed(FailureKind::resource_limit,
                   "starting input payloads exceed 16 MiB");
    remaining -= bytes->size();
    if (!flatbuffers::Verify(*type->schema(), *type->root(), bytes->data(),
                             bytes->size()))
      throw Failed(FailureKind::input_invalid,
                   "starting input is not a valid FlatBuffer: " + name);
    values.push_back({
        graph.nodes[to].path,
        {port, std::move(type),
         std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()),
         schema},
    });
  }
  if (card->kind == CardType::Merge) {
    if (values.size() != 1)
      throw Failed(FailureKind::input_invalid,
                   "Merge requires exactly one starting input");
  } else if (!connected.empty()) {
    throw Failed(FailureKind::input_missing,
                 "missing starting input " + connected.begin()->first);
  }
  return values;
}

}  // namespace

// ---------------------------------------------------------------------------
// Mutation lane

void Service::run(RequestIn& req) {
  const auto* r = project_request(req)->value_as_RunRequest();
  std::string op_id = r->op_id()->str();
  std::string digest = request_digest(project_request(req));
  // Replays come before any base_rev or busy check.
  if (replay_or_join(req, op_id, "run", digest)) return;
  try {
    if (!r->base_rev())
      throw Failed(FailureKind::invalid_request, "base_rev is required");
    RunParams p;
    p.op_id = op_id;
    p.digest = digest;
    p.card = wire::card_arg(r->card());
    p.propagate = r->propagate().value_or(true);
    p.max_concurrency = r->max_concurrency().value_or(4);
    if (auto jobs = r->nix_jobs()) p.nix_jobs = *jobs;
    if (auto cores = r->nix_cores()) p.nix_cores = *cores;
    if (p.max_concurrency < 1 || p.max_concurrency > 64)
      throw Failed(FailureKind::invalid_request,
                   "max_concurrency must be 1..64");
    const auto invalid_parallelism = [&](std::optional<uint32_t> value) {
      return value && (*value < 1 || *value > opts_.env_cpus);
    };
    if (invalid_parallelism(p.nix_jobs) || invalid_parallelism(p.nix_cores))
      throw Failed(
          FailureKind::invalid_request,
          "nix_jobs and nix_cores must be between 1 and the environment's " +
              std::to_string(opts_.env_cpus) + " CPUs");
    auto doc = store_->current();
    if (*r->base_rev() != doc->rev) {
      FailureData f = FailureData::make(FailureKind::revision_conflict,
                                        "the document changed since base_rev");
      f.current_rev = doc->rev;
      throw Failed(f);
    }
    if (store_->halted())
      throw Failed(FailureKind::persistence_failed,
                   "changes are halted after a persistence failure");
    if (const auto* inputs = r->inputs()) {
      if (!packed_debug())
        throw Failed(FailureKind::unsupported,
                     "typed starting values require a packed debug app");
      p.injected_inputs = packed_inputs(*doc->resolution, p.card, *inputs);
    }
    if (!packed_builds())
      p.commit = versions_->commit_all(
          version_record::commit("run", path_key(p.card)), doc.get());
    std::string receipt = scheduler_->accept(doc, p);
    reply(req, receipt_reply(req.request_id, receipt, false));
    finish_op(op_id, receipt);
  } catch (const Failed& f) {
    store_failure_receipt(op_id, "run", digest, f.failure);
    throw;
  } catch (const std::exception& e) {
    // Any other error ends the operation too (retryable under the same id).
    auto f = FailureData::make(FailureKind::internal_error, e.what());
    store_failure_receipt(op_id, "run", digest, f);
    throw Failed(f);
  }
}

// ---------------------------------------------------------------------------
// Read pool

void Service::evaluate_nix(RequestIn& req) {
  const auto* r = project_request(req)->value_as_EvaluateNixRequest();
  auto doc = store_->current();
  check_at_rev(r->at_rev(), *doc);
  auto card = wire::card_arg(r->card());
  const auto* node = doc->resolution->find(card);
  if (!node)
    throw Failed(FailureKind::not_found, "unknown card " + path_key(card),
                 {{"card", path_key(card)}});
  if (node->kind != CardType::Code && node->kind != CardType::Draft)
    throw Failed(FailureKind::invalid_request,
                 "Nix evaluation requires a Code or Draft Card",
                 {{"card", path_key(card)}});
  if (r->expression()->size() == 0)
    throw Failed(FailureKind::invalid_request, "expression is empty");

  std::stop_source stop;
  std::stop_callback disconnected(req.connection_stop,
                                  [&] { stop.request_stop(); });
  std::stop_callback shutdown(shutdown_stop_.get_token(),
                              [&] { stop.request_stop(); });
  constexpr size_t kLogLimit = 256u << 10;
  std::mutex log_mutex;
  std::string log_tail;
  const auto append_log = [&](std::string_view line) {
    line = line.substr(line.size() > kLogLimit ? line.size() - kLogLimit : 0);
    if (log_tail.size() + line.size() > kLogLimit)
      log_tail.erase(0, log_tail.size() + line.size() - kLogLimit);
    log_tail.append(line);
  };
  ExpressionRequest call{
      .doc = doc,
      .card = std::move(card),
      .expression = r->expression()->str(),
      .build = r->build(),
      .cancel = stop.get_token(),
      .deadline = std::nullopt,
      .log =
          [&](proc::LineChunk chunk) {
            std::lock_guard lock(log_mutex);
            for (const auto& line : chunk.lines) {
              append_log(line);
              if (!line.ends_with('\n')) append_log("\n");
            }
          },
  };
  if (auto timeout = node->settings.build_timeout_seconds) {
    const auto now = std::chrono::steady_clock::now();
    const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::time_point::max() - now);
    call.deadline = *timeout > static_cast<uint64_t>(remaining.count())
                        ? std::chrono::steady_clock::time_point::max()
                        : now + std::chrono::seconds(*timeout);
  }

  ExpressionResult result;
  auto failure_kind = FailureKind::nix_failed;
  try {
    result = nix_->evaluate_expression(call);
  } catch (const NixInterrupted& e) {
    if (!e.timed_out) throw Failed(FailureKind::cancelled, e.what());
    failure_kind = FailureKind::timeout;
    result.build_error = e.what();
  } catch (const Failed&) {
    throw;
  } catch (const std::exception& e) {
    result.build_error = e.what();
  }

  FBB b;
  std::vector<flatbuffers::Offset<R::NixOutputFile>> files;
  files.reserve(result.files.size());
  for (const auto& file : result.files) {
    // Wire paths are UTF-8. Skip names that cannot be represented losslessly.
    if (text::replace_invalid_utf8(file.path) != file.path) {
      result.truncated = true;
      continue;
    }
    files.push_back(R::CreateNixOutputFile(
        b, b.CreateString(file.path),
        b.CreateVector(reinterpret_cast<const uint8_t*>(file.bytes.data()),
                       file.bytes.size())));
  }
  flatbuffers::Offset<D::Failure> failure;
  flatbuffers::Offset<flatbuffers::String> log;
  if (result.build_error) {
    failure =
        wire::failure(b, FailureData::make(failure_kind, *result.build_error));
    std::lock_guard lock(log_mutex);
    auto tail = text::replace_invalid_utf8(
        result.log_tail.empty() ? log_tail : result.log_tail);
    if (tail.size() > kLogLimit) {
      auto start = tail.size() - kLogLimit;
      while ((static_cast<unsigned char>(tail[start]) & 0xc0) == 0x80) ++start;
      tail.erase(0, start);
    }
    if (!tail.empty()) log = b.CreateString(tail);
  }
  auto res = R::CreateEvaluateNixResult(
      b, doc->rev, result.json ? b.CreateString(*result.json) : 0,
      call.build && failure.IsNull() ? b.CreateVector(files) : 0,
      result.truncated, failure, log);
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::EvaluateNixResult,
                         res.Union()));
}

void Service::run_plan(RequestIn& req) {
  const auto* r = project_request(req)->value_as_RunPlanRequest();
  auto doc = store_->current();
  check_at_rev(r->at_rev(), *doc);
  auto plan = Scheduler::plan(*doc->resolution, wire::card_arg(r->card()),
                              r->propagate().value_or(true));
  const auto services = remote_targets();
  for (const auto& path : plan.targets)
    if (auto diagnostic =
            execution_diagnostic(*doc->resolution->find(path), services))
      plan.diagnostics.push_back(std::move(*diagnostic));
  FBB b;
  auto paths = [&](const std::vector<CardPath>& v) {
    std::vector<flatbuffers::Offset<D::CardPath>> o;
    for (const auto& p : v) o.push_back(wire::path(b, p));
    return b.CreateVector(o);
  };
  auto t = paths(plan.targets);
  auto s = paths(plan.starts);
  auto c = paths(plan.build_candidates);
  auto d = wire::diagnostics(b, plan.diagnostics);
  R::RunPlanResultBuilder pb(b);
  pb.add_rev(doc->rev);
  pb.add_targets(t);
  pb.add_starts(s);
  pb.add_build_candidates(c);
  pb.add_diagnostics(d);
  auto off = pb.Finish();
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::RunPlanResult,
                         off.Union()));
}

void Service::run_cancel(RequestIn& req) {
  const auto* r = project_request(req)->value_as_RunCancelRequest();
  require_run_id(*r->run_id(), "run_id");
  auto st = scheduler_->cancel(r->run_id()->str());
  FBB b;
  auto off = wire::run_status(b, st);
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::RunStatus,
                         off.Union()));
}

void Service::run_status(RequestIn& req) {
  const auto* r = project_request(req)->value_as_RunStatusRequest();
  RunStatusData st;
  std::vector<InvocationSummaryData> live;
  if (r->run_id()) {
    require_run_id(*r->run_id(), "run_id");
    auto active = scheduler_->active_status();
    if (active && active->run_id == r->run_id()->str()) {
      st = *active;
      live = history_->live(*st.run_id);
    } else {
      auto run = history_->read(
          [&](sql::Db& db) { return History::run(db, r->run_id()->str()); });
      if (!run)
        throw Failed(FailureKind::not_found, "unknown run",
                     {{"run_id", r->run_id()->str()}});
      st = run->status;
    }
  } else {
    st = current_run_status();
    if (st.run_id && !st.terminal()) live = history_->live(*st.run_id);
  }
  FBB b;
  auto off = wire::run_status(b, st, live);
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::RunStatus,
                         off.Union()));
}

void Service::run_history(RequestIn& req) {
  const auto* r = project_request(req)->value_as_RunHistoryRequest();
  std::optional<CardPath> card;
  if (r->card()) card = wire::card_arg(r->card());
  bool artifacts_only = r->artifacts_only().value_or(false);
  uint32_t limit = page_limit(r->limit());
  auto [hi, after] = r->cursor() ? history_->open_cursor(r->cursor()->str())
                                 : std::pair<int64_t, std::string>{-1, ""};
  std::vector<int64_t> seqs;
  auto runs = history_->read([&, hi_ = hi, after_ = after](sql::Db& db) {
    int64_t top = hi_ >= 0 ? hi_ : History::max_run_seq(db);
    int64_t below = after_.empty() ? INT64_MAX : std::stoll(after_);
    auto out = History::runs_page(db, card, artifacts_only, below, top,
                                  limit + 1, &seqs);
    seqs.push_back(top);  // remember the bound for the cursor
    return out;
  });
  int64_t top = seqs.back();
  seqs.pop_back();
  // Active runs report live state.
  auto active = scheduler_->active_status();
  FBB b;
  std::vector<flatbuffers::Offset<R::RunSummary>> out;
  size_t bytes = 0;
  bool more = false;
  int64_t last = 0;
  for (size_t i = 0; i < runs.size(); ++i) {
    if (out.size() >= limit || bytes > kPagePayload) {
      more = true;
      break;
    }
    auto s = runs[i];
    if (active && active->run_id == s.status.run_id) s.status = *active;
    out.push_back(wire::run_summary(b, s));
    bytes += 512;
    last = seqs[i];
  }
  auto v = b.CreateVector(out);
  auto cur =
      more ? b.CreateString(history_->make_cursor(top, std::to_string(last)))
           : flatbuffers::Offset<flatbuffers::String>();
  R::RunPageBuilder pb(b);
  pb.add_runs(v);
  if (more) pb.add_next_cursor(cur);
  auto off = pb.Finish();
  reply(req,
        wire::reply(b, req.request_id, R::ReplyPayload::RunPage, off.Union()));
}

void Service::run_latest_invocations(RequestIn& req) {
  const auto* r = project_request(req)->value_as_RunLatestInvocationsRequest();
  require_run_id(*r->run_id(), "run_id");
  std::string run_id = r->run_id()->str();
  const auto fingerprint = store_->current()->execution_fingerprint;
  auto [rows, batches] = history_->read([&](sql::Db& db) {
    const auto run = History::run(db, run_id);
    if (!run)
      throw Failed(FailureKind::not_found, "unknown run", {{"run_id", run_id}});
    auto latest = run->source_hash == fingerprint
                      ? History::latest_batches(db, run_id)
                      : std::vector<RunBatchData>{};
    return std::pair{History::latest_invocations(db, run_id),
                     std::move(latest)};
  });
  for (auto& s : rows) history_->overlay(s);
  FBB b;
  std::vector<flatbuffers::Offset<R::InvocationSummary>> out;
  for (const auto& s : rows) out.push_back(wire::invocation(b, s));
  auto v = b.CreateVector(out);
  std::vector<flatbuffers::Offset<R::RunBatch>> batch_rows;
  for (const auto& batch : batches)
    batch_rows.push_back(wire::run_batch(b, batch));
  auto bv = b.CreateVector(batch_rows);
  R::InvocationPageBuilder pb(b);
  pb.add_records(v);
  pb.add_batches(bv);
  auto off = pb.Finish();
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::InvocationPage,
                         off.Union()));
}

void Service::run_definition_paths(RequestIn& req) {
  const auto* r = project_request(req)->value_as_RunDefinitionPathsRequest();
  require_run_id(*r->run_id(), "run_id");
  const auto card = r->card() ? path_key(wire::card_arg(r->card())) : "";
  const auto paths = history_->read([&](sql::Db& db) {
    auto find =
        db.prepare("SELECT card,path FROM run_definition_paths WHERE run_id=?");
    find.bind(1, r->run_id()->str());
    std::set<std::string> paths;
    bool found = false;
    while (find.step()) {
      const auto logical = find.text(0);
      if (logical == card || card.empty()) found = true;
      if (card.empty() || logical == card || card.starts_with(logical + '/'))
        paths.insert(find.text(1));
    }
    if (!found)
      throw Failed(FailureKind::not_found, "No recorded Card definition");
    return std::vector(paths.begin(), paths.end());
  });
  FBB b;
  auto result = R::CreateRunDefinitionPaths(b, b.CreateVectorOfStrings(paths));
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::RunDefinitionPaths,
                         result.Union()));
}

void Service::run_invocations(RequestIn& req) {
  const auto* r = project_request(req)->value_as_RunInvocationsRequest();
  require_run_id(*r->run_id(), "run_id");
  std::string run_id = r->run_id()->str();
  auto card = wire::card_arg(r->card());
  uint32_t limit = page_limit(r->limit());
  uint64_t after = 0;
  if (r->cursor())
    after = std::stoull(history_->open_cursor(r->cursor()->str()).second);
  auto [rows, batches] = history_->read([&](sql::Db& db) {
    if (!History::run(db, run_id))
      throw Failed(FailureKind::not_found, "unknown run", {{"run_id", run_id}});
    if (!History::run_has_card(db, run_id, card))
      throw Failed(FailureKind::not_found,
                   "the run has no record of " + path_key(card),
                   {{"card", path_key(card)}});
    return std::pair{History::invocations(db, run_id, card, after, limit + 1),
                     History::latest_batches(db, run_id, card)};
  });
  for (auto& s : rows) history_->overlay(s);
  FBB b;
  std::vector<flatbuffers::Offset<R::InvocationSummary>> out;
  bool more = rows.size() > limit;
  if (more) rows.resize(limit);
  for (const auto& s : rows) out.push_back(wire::invocation(b, s));
  auto v = b.CreateVector(out);
  auto cur = more ? b.CreateString(history_->make_cursor(
                        0, std::to_string(rows.back().invocation)))
                  : flatbuffers::Offset<flatbuffers::String>();
  std::vector<flatbuffers::Offset<R::RunBatch>> batch_rows;
  for (const auto& batch : batches)
    batch_rows.push_back(wire::run_batch(b, batch));
  auto bv = b.CreateVector(batch_rows);
  R::InvocationPageBuilder pb(b);
  pb.add_records(v);
  pb.add_batches(bv);
  if (more) pb.add_next_cursor(cur);
  auto off = pb.Finish();
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::InvocationPage,
                         off.Union()));
}

auto Service::run_environments(RequestIn& req) -> void {
  const auto* r = project_request(req)->value_as_RunEnvironmentsRequest();
  require_run_id(*r->run_id(), "run_id");
  const auto run = r->run_id()->str();
  const auto offset = r->offset().value_or(0);
  const auto limit = page_limit(r->limit());
  if (offset > INT64_MAX)
    throw Failed(FailureKind::invalid_request, "offset is too large");
  FBB b;
  std::vector<flatbuffers::Offset<R::BuildEnvironment>> rows;
  std::vector<flatbuffers::Offset<flatbuffers::String>> caches;
  bool more = false;
  history_->read([&](sql::Db& db) {
    if (!History::run(db, run))
      throw Failed(FailureKind::not_found, "unknown run");
    auto s = db.prepare(
        "SELECT url FROM run_substituters WHERE run_id=? ORDER BY position");
    s.bind(1, run);
    while (s.step()) caches.push_back(b.CreateString(s.text(0)));
    auto q = db.prepare(
        "SELECT id, derivation, manifest, system, path_count "
        "FROM build_environments "
        "WHERE run_id=? ORDER BY rowid LIMIT ? OFFSET ?");
    q.bind(1, run).bind(2, static_cast<int>(limit + 1)).bind(3, offset);
    while (q.step()) {
      const auto id = q.text(0), derivation = q.text(1), manifest = q.text(2),
                 system = q.text(3);
      const auto bytes =
          id.size() + derivation.size() + manifest.size() + system.size() + 128;
      if (bytes > kPagePayload)
        throw Failed(FailureKind::resource_limit,
                     "recorded environment exceeds the page size");
      if (rows.size() == limit || b.GetSize() + bytes > kPagePayload) {
        more = true;
        break;
      }
      rows.push_back(R::CreateBuildEnvironment(
          b, b.CreateString(id), b.CreateString(derivation),
          b.CreateString(manifest), b.CreateString(system), q.u64(4)));
    }
  });
  auto values = b.CreateVector(rows);
  auto substituters = b.CreateVector(caches);
  R::BuildEnvironmentPageBuilder page(b);
  page.add_environments(values);
  page.add_substituters(substituters);
  if (more) page.add_next_offset(offset + rows.size());
  auto off = page.Finish();
  reply(req, wire::reply(b, req.request_id,
                         R::ReplyPayload::BuildEnvironmentPage, off.Union()));
}

auto Service::run_environment_paths(RequestIn& req) -> void {
  const auto* r = project_request(req)->value_as_RunEnvironmentPathsRequest();
  require_run_id(*r->run_id(), "run_id");
  const auto run = r->run_id()->str();
  const auto id = r->environment_id()->str();
  const auto offset = r->offset().value_or(0);
  const auto limit = page_limit(r->limit());
  if (offset > INT64_MAX)
    throw Failed(FailureKind::invalid_request, "offset is too large");
  const auto query = r->query() ? r->query()->str() : std::string{};
  if (bool(r->excluding_run_id()) != bool(r->excluding_environment_id()))
    throw Failed(FailureKind::invalid_request,
                 "both excluded run and environment are required");
  if (r->excluding_run_id())
    require_run_id(*r->excluding_run_id(), "excluding_run_id");
  const auto excluded_run =
      r->excluding_run_id() ? r->excluding_run_id()->str() : std::string{};
  const auto excluded_id = r->excluding_environment_id()
                               ? r->excluding_environment_id()->str()
                               : std::string{};
  FBB b;
  std::vector<flatbuffers::Offset<flatbuffers::String>> paths;
  uint64_t total = 0;
  history_->read([&](sql::Db& db) {
    auto environment = db.prepare(
        "SELECT path_count FROM build_environments WHERE run_id=? AND id=?");
    environment.bind(1, run).bind(2, id);
    if (!environment.step())
      throw Failed(FailureKind::not_found, "unknown run environment");
    const auto recorded_count = environment.u64(0);
    auto actual = db.prepare(
        "SELECT count(*) FROM environment_paths WHERE "
        "run_id=? AND environment_id=?");
    actual.bind(1, run).bind(2, id);
    actual.step();
    if (actual.u64(0) != recorded_count)
      throw Failed(FailureKind::integrity_error,
                   "recorded environment has missing store paths");
    if (r->excluding_run_id()) {
      auto other = db.prepare(
          "SELECT path_count FROM build_environments WHERE run_id=? AND id=?");
      other.bind(1, excluded_run).bind(2, excluded_id);
      if (!other.step())
        throw Failed(FailureKind::not_found,
                     "unknown excluded run environment");
      actual.reset();
      actual.bind(1, excluded_run).bind(2, excluded_id);
      actual.step();
      if (actual.u64(0) != other.u64(0))
        throw Failed(FailureKind::integrity_error,
                     "excluded environment has missing store paths");
    }
    // EXCEPT computes the exact store-path difference without materializing
    // closures in a client.
    const auto predicate = std::string{
        "FROM (SELECT path FROM environment_paths WHERE run_id=?1 "
        "AND environment_id=?2 "
        "EXCEPT SELECT path FROM environment_paths WHERE run_id=?4 "
        "AND environment_id=?5) "
        "WHERE instr(lower(path), lower(?3))>0 "};
    auto count = db.prepare("SELECT count(*) " + predicate);
    count.bind(1, run)
        .bind(2, id)
        .bind(3, query)
        .bind(4, excluded_run)
        .bind(5, excluded_id);
    count.step();
    total = count.u64(0);
    auto q = db.prepare("SELECT path " + predicate +
                        "ORDER BY path LIMIT ?6 OFFSET ?7");
    q.bind(1, run)
        .bind(2, id)
        .bind(3, query)
        .bind(4, excluded_run)
        .bind(5, excluded_id)
        .bind(6, static_cast<int>(limit))
        .bind(7, offset);
    while (q.step()) {
      const auto path = q.text(0);
      if (path.size() + 16 > kPagePayload)
        throw Failed(FailureKind::resource_limit,
                     "recorded store path exceeds the page size");
      if (b.GetSize() + path.size() + 16 > kPagePayload) break;
      paths.push_back(b.CreateString(path));
    }
  });
  if (paths.empty() && offset < total)
    throw Failed(FailureKind::integrity_error,
                 "recorded environment has missing store paths");
  auto values = b.CreateVector(paths);
  R::EnvironmentPathPageBuilder page(b);
  page.add_paths(values);
  page.add_total(total);
  if (offset < total && paths.size() < total - offset)
    page.add_next_offset(offset + paths.size());
  auto off = page.Finish();
  reply(req, wire::reply(b, req.request_id,
                         R::ReplyPayload::EnvironmentPathPage, off.Union()));
}

auto Service::run_environment_uses(RequestIn& req) -> void {
  const auto* r = project_request(req)->value_as_RunEnvironmentUsesRequest();
  require_run_id(*r->run_id(), "run_id");
  const auto run = r->run_id()->str(), id = r->environment_id()->str();
  const auto offset = r->offset().value_or(0);
  const auto limit = page_limit(r->limit());
  if (offset > INT64_MAX)
    throw Failed(FailureKind::invalid_request, "offset is too large");
  FBB b;
  std::vector<flatbuffers::Offset<R::EnvironmentUse>> rows;
  bool more = false;
  history_->read([&](sql::Db& db) {
    auto environment =
        db.prepare("SELECT 1 FROM build_environments WHERE run_id=? AND id=?");
    environment.bind(1, run).bind(2, id);
    if (!environment.step())
      throw Failed(FailureKind::not_found, "unknown run environment");
    auto q = db.prepare(
        "SELECT card, invocation FROM ("
        "SELECT card, invocation FROM invocations WHERE run_id=?1 "
        "AND environment_id=?2 UNION ALL SELECT card, invocation "
        "FROM active_environments WHERE run_id=?1 AND environment_id=?2) "
        "ORDER BY card, "
        "invocation LIMIT ?3 OFFSET ?4");
    q.bind(1, run)
        .bind(2, id)
        .bind(3, static_cast<int>(limit + 1))
        .bind(4, offset);
    while (q.step()) {
      const auto card = q.text(0);
      if (card.size() + 64 > kPagePayload)
        throw Failed(FailureKind::resource_limit,
                     "recorded Card path exceeds the page size");
      if (rows.size() == limit ||
          b.GetSize() + card.size() + 64 > kPagePayload) {
        more = true;
        break;
      }
      rows.push_back(
          R::CreateEnvironmentUse(b, b.CreateString(card), q.u64(1)));
    }
  });
  const auto uses = b.CreateVector(rows);
  R::EnvironmentUsePageBuilder page(b);
  page.add_uses(uses);
  if (more) page.add_next_offset(offset + rows.size());
  const auto off = page.Finish();
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::EnvironmentUsePage,
                         off.Union()));
}

void Service::run_record(RequestIn& req) {
  const auto* r = project_request(req)->value_as_RunRecordRequest();
  require_run_id(*r->run_id(), "run_id");
  std::string run_id = r->run_id()->str();
  auto card = wire::card_arg(r->card());
  bool follow = r->follow_latest().value_or(false);
  if (follow && r->invocation())
    throw Failed(FailureKind::invalid_request,
                 "invocation and follow_latest cannot be combined");
  auto active = scheduler_->active_status();
  struct Out {
    RunSummaryData run;
    std::optional<uint64_t> selected;
    std::optional<InvocationRecordData> record;
    std::optional<InvocationSummaryData> status;
    std::vector<StoredValueMeta> in_meta;
    std::vector<StoredValueMeta> out_meta;
    std::vector<std::string> in_bytes;
    std::vector<std::string> out_bytes;
    History::Aggregate agg;
    std::optional<FailureData> past_failure;
  };
  auto o = history_->read([&](sql::Db& db) {
    Out o;
    auto run = History::run(db, run_id);
    if (!run)
      throw Failed(FailureKind::not_found, "unknown run", {{"run_id", run_id}});
    if (!History::run_has_card(db, run_id, card))
      throw Failed(FailureKind::not_found,
                   "the run has no record of " + path_key(card),
                   {{"card", path_key(card)}});
    o.run = *run;
    if (r->invocation())
      o.selected = *r->invocation();
    else if (follow)
      o.selected = History::latest_invocation(db, run_id, card, true);
    else
      o.selected = History::latest_completed(db, run_id, card);
    if (o.selected) {
      o.record = History::invocation(db, run_id, card, *o.selected);
      o.status = History::status(db, run_id, card, *o.selected);
      if (o.status) history_->overlay(*o.status);
      if (o.record) {
        o.in_meta = History::value_metas(db, run_id, card, *o.selected, true);
        o.out_meta = History::value_metas(db, run_id, card, *o.selected, false);
        for (const auto& m : o.in_meta)
          o.in_bytes.push_back(
              History::value_bytes(db, run_id, card, *o.selected, true, m.name)
                  .value_or(""));
        for (const auto& m : o.out_meta)
          o.out_bytes.push_back(
              History::value_bytes(db, run_id, card, *o.selected, false, m.name)
                  .value_or(""));
      }
    }
    o.agg = History::aggregate(db, run_id, card);
    if (follow) o.past_failure = History::latest_failure(db, run_id, card);
    return o;
  });
  if (active && active->run_id == o.run.status.run_id) o.run.status = *active;
  // HistoryState chosen by the core.
  CardState state = CardState::not_started;
  std::optional<FailureData> failure;
  std::optional<int64_t> time;
  if (o.selected) {
    if (follow && o.status) {
      state = o.status->state;
      failure = o.status->failure;
      time = o.status->started_at;
    } else if (o.record) {
      state = o.record->summary.state;
      failure = o.record->summary.failure;
      time = o.record->summary.ended_at ? o.record->summary.ended_at
                                        : o.record->summary.started_at;
    } else if (o.status) {
      state = o.status->state;
      failure = o.status->failure;
      time = o.status->started_at;
    }
    if (follow && !o.status && !o.record && o.run.status.terminal() &&
        (o.run.status.state == RunState::failed ||
         o.run.status.state == RunState::cancelled)) {
      state = o.run.status.state == RunState::cancelled ? CardState::cancelled
                                                        : CardState::failed;
      failure = o.run.status.failure;
      time = o.run.status.ended_at;
    }
  }
  if (follow && o.past_failure && state != CardState::running &&
      state != CardState::preparing) {
    state = CardState::failed;
    failure = o.past_failure;
  }
  bool exists = o.record.has_value() || o.status.has_value();
  bool truncated = false;
  auto doc = store_->current();
  bool orphaned = doc->resolution->find(card) == nullptr;
  FBB b(1 << 16);
  flatbuffers::Offset<R::InvocationRecord> rec;
  if (exists) {
    InvocationSummaryData summary = o.record ? o.record->summary : *o.status;
    uint64_t budget = kInlineLimit;
    auto values = [&](const std::vector<StoredValueMeta>& metas,
                      const std::vector<std::string>& data) {
      std::vector<flatbuffers::Offset<D::NamedValue>> v;
      for (size_t i = 0; i < metas.size(); ++i) {
        const auto& m = metas[i];
        auto name = b.CreateString(m.name);
        auto bfbs = b.CreateVector(
            reinterpret_cast<const uint8_t*>(m.bfbs.data()), m.bfbs.size());
        auto root = b.CreateString(m.root_type);
        flatbuffers::Offset<flatbuffers::Vector<uint8_t>> inl;
        bool include = data[i].size() <= budget;
        if (include) {
          inl = b.CreateVector(reinterpret_cast<const uint8_t*>(data[i].data()),
                               data[i].size());
          budget -= data[i].size();
        } else {
          truncated = true;
        }
        std::vector<flatbuffers::Offset<D::ValueArtifact>> artifacts;
        for (const auto& [id, size, label, state] : m.artifacts)
          artifacts.push_back(D::CreateValueArtifact(
              b, b.CreateString(artifact_id_text(id)), size,
              label.empty() ? 0 : b.CreateString(label), state));
        const auto ids = b.CreateVector(artifacts);
        const auto hash = b.CreateString(m.hash);
        D::NamedValueBuilder nb(b);
        nb.add_name(name);
        nb.add_schema_bfbs(bfbs);
        nb.add_root_type(root);
        nb.add_size_bytes(m.size_bytes);
        if (include) nb.add_inline_bytes(inl);
        nb.add_artifacts(ids);
        nb.add_content_hash(hash);
        v.push_back(nb.Finish());
      }
      return b.CreateVector(v);
    };
    auto ins = values(o.in_meta, o.in_bytes);
    auto outs = values(o.out_meta, o.out_bytes);
    std::vector<flatbuffers::Offset<R::InputSource>> srcs;
    if (o.record)
      for (const auto& s : o.record->input_sources)
        srcs.push_back(R::CreateInputSource(
            b, b.CreateString(s.port), wire::path(b, s.card), s.invocation));
    auto srcv = b.CreateVector(srcs);
    auto sum = wire::invocation(b, summary);
    flatbuffers::Offset<D::InvocationParams> params;
    if (summary.params) {
      std::vector<flatbuffers::Offset<D::InvocationParam>> entries;
      for (const auto& [name, value] : *summary.params)
        entries.push_back(D::CreateInvocationParam(b, b.CreateString(name),
                                                   b.CreateString(value)));
      params = D::CreateInvocationParams(b, b.CreateVector(entries));
    }
    R::InvocationRecordBuilder rb(b);
    rb.add_summary(sum);
    rb.add_inputs(ins);
    rb.add_outputs(outs);
    rb.add_input_sources(srcv);
    rb.add_record_io(o.record ? o.record->record_io : false);
    if (!params.IsNull()) rb.add_params(params);
    rec = rb.Finish();
  }
  auto run = wire::run_summary(b, o.run);
  auto fo =
      failure ? wire::failure(b, *failure) : flatbuffers::Offset<D::Failure>();
  R::HistoryStateBuilder hb(b);
  hb.add_state(state);
  if (failure) hb.add_failure(fo);
  if (time) hb.add_time(*time);
  auto hs = hb.Finish();
  auto agg = R::CreateAggregate(b, o.agg.calls, o.agg.succeeded, o.agg.failed,
                                o.agg.cancelled, o.agg.total_ms);
  R::RecordResultBuilder res(b);
  if (o.selected && exists) res.add_selected_invocation(*o.selected);
  if (exists) res.add_record(rec);
  res.add_run(run);
  res.add_status(hs);
  res.add_aggregate(agg);
  res.add_exists(exists);
  res.add_recording_disabled(o.record && !o.record->record_io);
  res.add_truncated(truncated);
  res.add_orphaned(orphaned);
  auto off = res.Finish();
  std::string out = wire::reply(b, req.request_id,
                                R::ReplyPayload::RecordResult, off.Union());
  if (out.size() > kMaxFrameBytes)
    throw Failed(FailureKind::resource_limit,
                 "the record metadata exceeds the frame limit");
  reply(req, std::move(out));
}

void Service::run_value_read(RequestIn& req) {
  const auto* r = project_request(req)->value_as_RunValueReadRequest();
  require_run_id(*r->run_id(), "run_id");
  std::string run_id = r->run_id()->str();
  auto card = wire::card_arg(r->card());
  if (!r->invocation())
    throw Failed(FailureKind::invalid_request, "invocation is required");
  if (r->side() != D::ValueSide::input && r->side() != D::ValueSide::output)
    throw Failed(FailureKind::invalid_request, "side must be input or output");
  bool input = r->side() == D::ValueSide::input;
  std::string port = r->port()->str();
  uint64_t offset = r->offset().value_or(0);
  uint64_t limit = content_limit(r->limit_bytes());
  auto page = history_->read([&](sql::Db& db) {
    if (!History::run(db, run_id))
      throw Failed(FailureKind::not_found, "unknown run", {{"run_id", run_id}});
    auto rec = History::invocation(db, run_id, card, *r->invocation());
    if (!rec) throw Failed(FailureKind::not_found, "unknown invocation");
    auto p = History::value_page(db, run_id, card, *r->invocation(), input,
                                 port, offset, limit);
    if (!p)
      throw Failed(rec->record_io ? FailureKind::not_found
                                  : FailureKind::recording_disabled,
                   "no stored value for port " + port,
                   {{"port", port}});
    return *p;
  });
  if (offset > page.size)
    throw Failed(FailureKind::invalid_range, "offset is past the end");
  reply(req, wire::file_page(req.request_id, port, page.bytes, offset,
                             page.size, page.hash));
}

namespace {
auto artifact_invocation_path(const std::string& run, const CardPath& card,
                              uint64_t invocation) -> stdfs::path {
  stdfs::path relative = run;
  for (const auto& segment : card) relative /= segment;
  return relative / ("@" + std::to_string(invocation));
}

auto artifact_location(const stdfs::path& state, const ArtifactData& artifact)
    -> stdfs::path {
  const auto root = state / "artifacts";
  constexpr std::string_view prefix = ".tsugumi/artifacts/";
  if (artifact.location.starts_with(prefix) &&
      fs::valid_relative_path(artifact.location) &&
      !stdfs::is_symlink(stdfs::symlink_status(root))) {
    const auto path = root / artifact.location.substr(prefix.size());
    if (fs::resolves_inside(root, path)) return path;
  }
  throw Failed(FailureKind::integrity_error, "invalid artifact location");
}

auto artifact_key(std::string_view text) -> ArtifactKey {
  if (auto id = parse_artifact_id(text)) return std::move(*id);
  throw Failed(FailureKind::invalid_request, "invalid artifact UUID");
}

auto remove_artifact_tree(const stdfs::path& tree) -> uint64_t {
  const auto status = stdfs::symlink_status(tree);
  if (!stdfs::exists(status)) return 0;
  if (!stdfs::is_directory(status))
    throw Failed(FailureKind::invalid_path,
                 "artifact storage is not a directory");
  const auto size = fs::directory_size(tree);
  fs::clear_staging(tree);
  stdfs::remove(tree);
  return size;
}
}  // namespace

auto Service::resolve_artifact(const ArtifactKey& id) -> stdfs::path {
  return history_->read([&](sql::Db& db) -> stdfs::path {
    const auto record = History::artifact(db, id);
    if (!record || !record->sealed_ms) return {};
    const auto path = artifact_location(store_->state_dir(), *record);
    return stdfs::is_directory(stdfs::symlink_status(path)) ? path
                                                            : stdfs::path{};
  });
}

auto Service::expire_artifacts(sql::Db& db, const std::string& run_id) -> void {
  for (const auto& artifact : History::transient_artifacts(db, run_id)) {
    remove_artifact_tree(artifact_location(store_->state_dir(), artifact));
    db.prepare("DELETE FROM artifacts WHERE id=?")
        .bind_blob(1, artifact.id)
        .run();
  }
}

auto Service::detach_run_artifacts(const std::string& run_id) -> stdfs::path {
  const auto artifacts = store_->state_dir() / "artifacts";
  const auto dir = artifacts / run_id;
  const auto detached = artifacts / (".delete-" + run_id);
  if (stdfs::is_symlink(artifacts) || stdfs::is_symlink(dir) ||
      stdfs::is_symlink(detached))
    throw Failed(FailureKind::invalid_path,
                 "artifact storage is a symbolic link");
  if (stdfs::exists(dir)) stdfs::rename(dir, detached);
  return detached;
}

auto Service::remove_run_artifacts(const std::string& run_id) -> uint64_t {
  return remove_artifact_tree(detach_run_artifacts(run_id));
}

auto Service::recover_artifact_deletions() -> void {
  const auto root = store_->state_dir() / "artifacts";
  if (!stdfs::is_directory(stdfs::symlink_status(root))) return;
  constexpr std::string_view prefix = ".delete-";
  for (const auto& entry : stdfs::directory_iterator(root)) {
    const auto name = entry.path().filename().string();
    if (!name.starts_with(prefix) ||
        !stdfs::is_directory(entry.symlink_status()))
      continue;
    const auto run = name.substr(prefix.size());
    if (!crypto::is_run_id(run)) continue;
    // A user deletion was interrupted after moving the directory aside.
    history_->write_sync([&](sql::Db& db) {
      History::delete_category(db, run, R::RunRecordCategory::Artifacts);
    });
    remove_artifact_tree(entry.path());
  }
}

auto Service::artifact_destination(const InvocationSummaryData& invocation)
    -> stdfs::path {
  const auto state = store_->state_dir();
  const auto destination =
      state / "artifacts" /
      artifact_invocation_path(invocation.run_id, invocation.card,
                               invocation.invocation);
  if (!fs::resolves_inside(state, destination))
    throw Failed(FailureKind::invalid_path, "artifact destination escapes the project");
  return destination;
}

auto Service::record_artifacts(const InvocationSummaryData& invocation,
                               std::span<const CreatedArtifact> created)
    -> void {
  if (created.empty()) return;
  const auto destination = artifact_destination(invocation);
  std::vector<ArtifactData> records;
  const auto sealed = now_ms();
  for (size_t counter = 0; counter < created.size(); ++counter) {
    const auto& artifact = created[counter];
    const auto target = destination / std::to_string(counter);
    records.push_back(
        {artifact.id, invocation.run_id, invocation.card, invocation.invocation,
         invocation.artifact_sequence, counter, artifact.keep,
         target.lexically_relative(store_->state_dir().parent_path())
             .generic_string(),
         invocation.started_at.value_or(sealed), sealed,
         fs::directory_size(target), "tree:" + fs::sha256_tree(target)});
  }
  history_->write_sync([&](sql::Db& db) {
    const auto run = History::run(db, invocation.run_id);
    for (const auto& artifact : records) {
      if (!artifact.keep && (!run || run->status.terminal())) {
        remove_artifact_tree(artifact_location(store_->state_dir(), artifact));
        continue;
      }
      if (artifact.id.size() != 16 || History::artifact(db, artifact.id))
        throw Failed(FailureKind::output_invalid,
                     "invalid or duplicate artifact UUID");
      History::put_artifact(db, artifact);
    }
  });
}

void Service::artifact_list(RequestIn& req) {
  const auto* r = project_request(req)->value_as_ArtifactListRequest();
  const auto* ids = r->artifact_ids();
  if (ids && (r->run_id() || r->card() || r->invocation() || r->cursor() ||
              r->limit()))
    throw Failed(FailureKind::invalid_request,
                 "artifact_ids cannot be combined with run filters");
  if (ids && ids->size() > 500)
    throw Failed(FailureKind::invalid_request,
                 "at most 500 artifact ids may be requested");
  std::string run_id;
  if (!ids) {
    if (!r->run_id())
      throw Failed(FailureKind::invalid_request, "run_id is required");
    require_run_id(*r->run_id(), "run_id");
    run_id = r->run_id()->str();
  }
  std::optional<CardPath> card;
  if (r->card()) card = wire::card_arg(r->card());
  if (r->invocation() && !card)
    throw Failed(FailureKind::invalid_request, "invocation requires card");
  std::optional<uint64_t> inv;
  if (r->invocation()) inv = *r->invocation();
  uint32_t limit = page_limit(r->limit());
  std::string after;
  if (r->cursor()) after = history_->open_cursor(r->cursor()->str()).second;
  auto rows = history_->read([&](sql::Db& db) {
    if (ids) {
      std::vector<ArtifactData> found;
      std::set<ArtifactKey> seen;
      for (const auto* text : *ids) {
        auto key = artifact_key(text->string_view());
        if (!seen.insert(key).second) continue;
        if (auto artifact = History::artifact(db, key);
            artifact && artifact->sealed_ms)
          found.push_back(std::move(*artifact));
      }
      return found;
    }
    if (!History::run(db, run_id))
      throw Failed(FailureKind::not_found, "unknown run", {{"run_id", run_id}});
    return History::artifacts(db, run_id, card, inv, after, limit + 1);
  });
  bool more = !ids && rows.size() > limit;
  if (more) rows.resize(limit);
  FBB b;
  std::vector<flatbuffers::Offset<R::ArtifactInfo>> out;
  for (const auto& a : rows) {
    std::vector<flatbuffers::Offset<R::ArtifactMember>> members;
    const auto path = artifact_location(store_->state_dir(), a);
    for (const auto& name : fs::list_beneath(path, "").files) {
      const auto size = stdfs::file_size(path / name);
      members.push_back(R::CreateArtifactMember(
          b, b.CreateString(name), size, b.CreateString(fs::sha256_file(path / name))));
    }
    out.push_back(R::CreateArtifactInfo(
        b, b.CreateString(artifact_id_text(a.id)), b.CreateString(a.run_id),
        wire::path(b, a.card), a.invocation, a.size_bytes,
        a.tree_hash ? b.CreateString(*a.tree_hash) : 0, b.CreateVector(members),
        b.CreateString(artifact_label(a)), a.keep));
  }
  auto v = b.CreateVector(out);
  flatbuffers::Offset<flatbuffers::String> cur;
  if (more) {
    const auto& l = rows.back();
    char inv_buf[32], counter_buf[32];
    std::snprintf(inv_buf, sizeof inv_buf, "%020llu",
                  static_cast<unsigned long long>(l.invocation));
    std::snprintf(counter_buf, sizeof counter_buf, "%020llu",
                  static_cast<unsigned long long>(l.counter));
    cur = b.CreateString(history_->make_cursor(
        0, path_key(l.card) + "\x1f" + inv_buf + "\x1f" + counter_buf));
  }
  R::ArtifactPageBuilder pb(b);
  pb.add_artifacts(v);
  if (more) pb.add_next_cursor(cur);
  auto off = pb.Finish();
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::ArtifactPage,
                         off.Union()));
}

void Service::artifact_read(RequestIn& req) {
  const auto* r = project_request(req)->value_as_ArtifactReadRequest();
  const auto id = artifact_key(r->artifact_id()->string_view());
  auto a = history_->read([&](sql::Db& db) { return History::artifact(db, id); });
  if (!a || !a->sealed_ms)
    throw Failed(FailureKind::not_found, "unknown artifact");
  if (!r->member() || !fs::valid_relative_path(r->member()->string_view()))
    throw Failed(FailureKind::invalid_path, "invalid artifact member");
  const auto root = artifact_location(store_->state_dir(), *a);
  const auto member = r->member()->str();
  const auto file = root / member;
  if (!fs::resolves_inside(root, file))
    throw Failed(FailureKind::invalid_path, "invalid artifact member");

  uint64_t offset = r->offset().value_or(0);
  uint64_t limit = content_limit(r->limit_bytes());
  const auto page = fs::read_range_inside(root, file, offset, limit);
  if (!page)
    throw Failed(FailureKind::not_found, "the artifact content is missing");
  if (offset > page->size)
    throw Failed(FailureKind::invalid_range, "offset is past the end");
  if (!a->tree_hash || *a->tree_hash != "tree:" + fs::sha256_tree(root))
    throw Failed(FailureKind::integrity_error, "the artifact content is corrupt");
  reply(req, wire::file_page(req.request_id,
                             stdfs::path(member).filename().string(),
                             page->bytes, offset, page->size,
                             fs::sha256_file(file)));
}

void Service::card_view_read(RequestIn& req) {
  const auto* r = project_request(req)->value_as_CardViewReadRequest();
  const auto session = view_uuid(*r->session());
  std::shared_ptr<const ViewBlob> blob;
  std::string name;
  {
    std::lock_guard lock(subs_mu_);
    const auto* view = owned_view(req, session);
    if (!view) throw Failed(FailureKind::not_found, "unknown view session");
    blob = view->blob;
    name = view->name;
  }
  const auto offset = r->offset().value_or(0);
  const auto bytes = blob->read(offset, content_limit(r->limit_bytes()));
  reply(req, wire::file_page(req.request_id, name, bytes, offset, blob->size,
                             blob->path));
}

void Service::log_range(RequestIn& req) {
  const auto* r = project_request(req)->value_as_LogRangeRequest();
  require_run_id(*r->run_id(), "run_id");
  std::string run_id = r->run_id()->str();
  std::optional<CardPath> card;
  if (r->card()) card = wire::card_arg(r->card());
  std::optional<uint64_t> inv;
  if (r->invocation()) {
    if (!card)
      throw Failed(FailureKind::invalid_request, "invocation requires card");
    inv = *r->invocation();
  }
  auto stream = r->stream();
  if (stream != LogStream::standard_output &&
      stream != LogStream::standard_error && stream != LogStream::journal &&
      stream != LogStream::build)
    throw Failed(
        FailureKind::invalid_request,
        "stream must be standard_output, standard_error, build or journal");
  bool tail = r->tail().value_or(false);
  uint64_t offset = r->offset().value_or(0);
  const auto before = r->before();
  if (before && (tail || offset != 0))
    throw Failed(FailureKind::invalid_request,
                 "before cannot be combined with tail or offset");
  if (tail && offset != 0)
    throw Failed(FailureKind::invalid_request,
                 "tail and offset cannot be combined");
  uint32_t limit = r->limit_bytes().value_or(65536);
  if (limit < protocol::kMinLogRangeBytes ||
      limit > protocol::kMaxLogRangeBytes)
    throw Failed(FailureKind::invalid_request,
                 "limit_bytes must be " +
                     std::to_string(protocol::kMinLogRangeBytes) + ".." +
                     std::to_string(protocol::kMaxLogRangeBytes));
  if (r->terminal_since() && (!r->terminal() || tail || before))
    throw Failed(FailureKind::invalid_request,
                 "terminal_since requires a forward terminal read");
  const auto read = [&](sql::Db& db) {
    return History::read_log(db, run_id, card, inv, stream, offset, tail,
                             limit, before, r->terminal(), r->terminal_since());
  };
  auto slice = r->live() && (stream == LogStream::standard_output ||
                             stream == LogStream::standard_error)
      ? scheduler_->live_log(run_id, read)
      : history_->read([&](sql::Db& db) {
    if (!History::run(db, run_id))
      throw Failed(FailureKind::not_found, "unknown run", {{"run_id", run_id}});
    return read(db);
  });
  FBB b(slice.text.size() + 512);
  auto t = b.CreateString(slice.text);
  R::LogSliceBuilder lb(b);
  lb.add_text(t);
  lb.add_offset(slice.offset);
  lb.add_next_offset(slice.next_offset);
  lb.add_size_bytes(slice.size);
  lb.add_line_number(slice.line_number);
  lb.add_starts_line(slice.starts_line);
  if (slice.raw_size) lb.add_raw_size_bytes(*slice.raw_size);
  lb.add_reset(slice.reset);
  lb.add_exists(slice.exists);
  lb.add_truncated(slice.truncated);
  if (slice.log_truncated_at_bytes)
    lb.add_log_truncated_at_bytes(*slice.log_truncated_at_bytes);
  auto off = lb.Finish();
  reply(req,
        wire::reply(b, req.request_id, R::ReplyPayload::LogSlice, off.Union()));
}

}  // namespace tsugumi
