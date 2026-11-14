// Run, SDK, history, value, artifact and log handlers.
#include <sys/stat.h>
#include <flatbuffers/reflection.h>

#include <algorithm>

#include "module_source.hh"
#include "nix.hh"
#include "scheduler.hh"
#include "service.hh"
#include "util/crypto.hh"
#include "util/fs.hh"
#include "util/log.hh"

namespace tsugumi {

namespace R = Tsugumi::RPC;
namespace D = Tsugumi::Data;
using FBB = flatbuffers::FlatBufferBuilder;
namespace stdfs = std::filesystem;

namespace {

constexpr uint32_t kInlineLimit = 256u << 10;
constexpr size_t kPagePayload = (1u << 20) - 8192;

uint32_t page_limit(std::optional<uint32_t> v) {
  uint32_t x = v.value_or(100);
  if (x < 1 || x > 500) throw Failed(FailureKind::invalid_request, "limit must be between 1 and 500");
  return x;
}

// The size of a page of file or value content.
uint64_t content_limit(std::optional<uint32_t> limit_arg) {
  uint32_t limit = limit_arg.value_or(1u << 20);
  if (limit < 1 || limit > (1u << 20)) throw Failed(FailureKind::invalid_request, "limit_bytes must be 1..1048576");
  return limit;
}

std::string sdk_result(FBB& b, const SDKJob& job, flatbuffers::Offset<R::CardSDKPrepareResult>* out) {
  std::vector<flatbuffers::Offset<R::SDKFile>> files;
  if (job.state == SDKJob::State::Ready)
    for (const auto& [p, h, s] : job.files)
      files.push_back(R::CreateSDKFile(b, b.CreateString(p), b.CreateString(h), s));
  *out = R::CreateCardSDKPrepareResult(b, b.CreateString(job.op_id), job.rev,
                                   job.state == SDKJob::State::Ready ? D::SDKState::ready : D::SDKState::pending,
                                   b.CreateVector(files));
  return {};
}

void require_uuid(const flatbuffers::String* s, const char* what) {
  if (!s || !crypto::is_uuid(s->str())) throw Failed(FailureKind::invalid_request, std::string(what) + " must be a UUID");
}

}  // namespace

// ---------------------------------------------------------------------------
// Mutation lane

void Service::run(RequestIn& req) {
  const auto* r = req.request()->value_as_RunRequest();
  std::string op_id = r->op_id()->str();
  std::string digest = request_digest(req.request());
  // Replays come before any base_rev or busy check.
  if (replay_or_join(req, op_id, "run", digest)) return;
  try {
    if (!r->base_rev()) throw Failed(FailureKind::invalid_request, "base_rev is required");
    RunParams p;
    p.op_id = op_id;
    p.digest = digest;
    p.card = wire::card_arg(r->card());
    p.propagate = r->propagate().value_or(true);
    p.english = english(req.conn);
    p.max_concurrency = r->max_concurrency().value_or(4);
    p.nix_jobs = r->nix_jobs().value_or(1);
    p.nix_cores = r->nix_cores().value_or(1);
    if (p.max_concurrency < 1 || p.max_concurrency > 64)
      throw Failed(FailureKind::invalid_request, "max_concurrency must be 1..64");
    if (p.nix_jobs < 1 || p.nix_cores < 1 || p.nix_jobs > opts_.env_cpus || p.nix_cores > opts_.env_cpus)
      throw Failed(FailureKind::invalid_request, "nix_jobs and nix_cores must be between 1 and the environment's " +
                                                     std::to_string(opts_.env_cpus) + " CPUs");
    auto doc = store_->current();
    if (*r->base_rev() != doc->rev) {
      FailureData f = FailureData::make(FailureKind::revision_conflict, "the document changed since base_rev");
      f.current_rev = doc->rev;
      throw Failed(f);
    }
    if (store_->halted()) throw Failed(FailureKind::persistence_failed, "changes are halted after a persistence failure");
    // The version it runs: what is saved, committed when it changed. A git
    // problem never stops a run (it then records no version).
    try {
      p.commit = versions_->commit_all((p.english ? "Run: " : "実行: ") + (p.card.empty() ? std::string("Workflow") : path_key(p.card)));
    } catch (const std::exception& e) {
      log::warn(std::string("versions: no commit for this run: ") + e.what());
    }
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

void Service::sdk_prepare(RequestIn& req) {
  const auto* r = req.request()->value_as_CardSDKPrepareRequest();
  std::string op_id = r->op_id()->str();
  std::string digest = request_digest(req.request());
  if (replay_or_join(req, op_id, "sdk", digest)) return;
  try {
    auto doc = store_->current();
    check_at_rev(r->at_rev(), *doc);
    auto card = wire::card_arg(r->card());
    const auto* node = doc->resolution->find(card);
    if (!node) throw Failed(FailureKind::not_found, "unknown card " + path_key(card), {{"card", path_key(card)}});
    if (node->kind != Template::Code && node->kind != Template::Input)
      throw Failed(FailureKind::invalid_request, "SDKs are generated for code and input Cards only");
    auto job = nix_->prepare_sdk(op_id, *node, doc->rev);
    FBB b;
    flatbuffers::Offset<R::CardSDKPrepareResult> res;
    sdk_result(b, job, &res);
    auto rc = R::CreateOperationReceipt(b, b.CreateString(op_id), R::ReceiptValue::CardSDKPrepareResult, res.Union());
    b.Finish(rc);
    std::string receipt(reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize());
    history_->write_sync([&](sql::Db& db) { History::put_receipt(db, op_id, "sdk", digest, receipt); });
    reply(req, receipt_reply(req.request_id, receipt, false));
    finish_op(op_id, receipt);
  } catch (const Failed& f) {
    store_failure_receipt(op_id, "sdk", digest, f.failure);
    throw;
  } catch (const std::exception& e) {
    // Any other error ends the operation too (retryable under the same id).
    auto f = FailureData::make(FailureKind::internal_error, e.what());
    store_failure_receipt(op_id, "sdk", digest, f);
    throw Failed(f);
  }
}

// ---------------------------------------------------------------------------
// Read pool

void Service::sdk_status(RequestIn& req) {
  const auto* r = req.request()->value_as_CardSDKStatusRequest();
  require_uuid(r->op_id(), "op_id");
  auto job = nix_->sdk_job(r->op_id()->str());
  if (!job) throw Failed(FailureKind::not_found, "unknown SDK job");
  if (job->state == SDKJob::State::Failed) throw Failed(*job->failure);
  FBB b;
  flatbuffers::Offset<R::CardSDKPrepareResult> res;
  sdk_result(b, *job, &res);
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::CardSDKPrepareResult, res.Union()));
}

void Service::run_plan(RequestIn& req) {
  const auto* r = req.request()->value_as_RunPlanRequest();
  auto doc = store_->current();
  check_at_rev(r->at_rev(), *doc);
  auto plan = Scheduler::plan(*doc->resolution, wire::card_arg(r->card()), r->propagate().value_or(true));
  const auto services = remote_targets();
  for (const auto& path : plan.targets)
    if (auto diagnostic = execution_diagnostic(*doc->resolution->find(path), services))
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
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::RunPlanResult, off.Union()));
}

void Service::run_cancel(RequestIn& req) {
  const auto* r = req.request()->value_as_RunCancelRequest();
  require_uuid(r->run_id(), "run_id");
  auto st = scheduler_->cancel(r->run_id()->str());
  FBB b;
  auto off = wire::run_status(b, st);
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::RunStatus, off.Union()));
}

void Service::run_status(RequestIn& req) {
  const auto* r = req.request()->value_as_RunStatusRequest();
  RunStatusData st;
  if (r->run_id()) {
    require_uuid(r->run_id(), "run_id");
    auto active = scheduler_->active_status();
    if (active && active->run_id == r->run_id()->str()) {
      st = *active;
    } else {
      auto run = history_->read([&](sql::Db& db) { return History::run(db, r->run_id()->str()); });
      if (!run) throw Failed(FailureKind::not_found, "unknown run", {{"run_id", r->run_id()->str()}});
      st = run->status;
    }
  } else {
    st = current_run_status();
  }
  FBB b;
  auto off = wire::run_status(b, st);
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::RunStatus, off.Union()));
}

void Service::run_history(RequestIn& req) {
  const auto* r = req.request()->value_as_RunHistoryRequest();
  std::optional<CardPath> card;
  if (r->card()) card = wire::card_arg(r->card());
  bool artifacts_only = r->artifacts_only().value_or(false);
  uint32_t limit = page_limit(r->limit());
  std::string qd = crypto::sha256_hex("history|" + (card ? path_key(*card) : std::string("\x01")) + "|" +
                                      (artifacts_only ? "1" : "0"));
  auto [hi, after] = r->cursor() ? history_->open_cursor(r->cursor()->str(), qd)
                                 : std::pair<int64_t, std::string>{-1, ""};
  std::vector<int64_t> seqs;
  auto runs = history_->read([&, hi_ = hi, after_ = after](sql::Db& db) {
    int64_t top = hi_ >= 0 ? hi_ : History::max_run_seq(db);
    int64_t below = after_.empty() ? INT64_MAX : std::stoll(after_);
    auto out = History::runs_page(db, card, artifacts_only, below, top, limit + 1, &seqs);
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
  auto cur = more ? b.CreateString(history_->make_cursor(qd, top, std::to_string(last))) : flatbuffers::Offset<flatbuffers::String>();
  R::RunPageBuilder pb(b);
  pb.add_runs(v);
  if (more) pb.add_next_cursor(cur);
  auto off = pb.Finish();
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::RunPage, off.Union()));
}

void Service::run_latest_invocations(RequestIn& req) {
  const auto* r = req.request()->value_as_RunLatestInvocationsRequest();
  require_uuid(r->run_id(), "run_id");
  std::string run_id = r->run_id()->str();
  const auto fingerprint = store_->current()->execution_fingerprint;
  auto [rows, batches] = history_->read([&](sql::Db& db) {
    const auto run = History::run(db, run_id);
    if (!run) throw Failed(FailureKind::not_found, "unknown run", {{"run_id", run_id}});
    auto latest = run->source_hash == fingerprint ? History::latest_batches(db, run_id) : std::vector<RunBatchData>{};
    return std::pair{History::latest_invocations(db, run_id), std::move(latest)};
  });
  for (auto& s : rows) history_->overlay(s);
  FBB b;
  std::vector<flatbuffers::Offset<R::InvocationSummary>> out;
  for (const auto& s : rows) out.push_back(wire::invocation(b, s));
  auto v = b.CreateVector(out);
  std::vector<flatbuffers::Offset<R::RunBatch>> batch_rows;
  for (const auto& batch : batches) batch_rows.push_back(wire::run_batch(b, batch));
  auto bv = b.CreateVector(batch_rows);
  R::InvocationPageBuilder pb(b);
  pb.add_records(v);
  pb.add_batches(bv);
  auto off = pb.Finish();
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::InvocationPage, off.Union()));
}

void Service::run_invocations(RequestIn& req) {
  const auto* r = req.request()->value_as_RunInvocationsRequest();
  require_uuid(r->run_id(), "run_id");
  std::string run_id = r->run_id()->str();
  auto card = wire::card_arg(r->card());
  uint32_t limit = page_limit(r->limit());
  std::string qd = crypto::sha256_hex("invocations|" + run_id + "|" + path_key(card));
  uint64_t after = 0;
  if (r->cursor()) after = std::stoull(history_->open_cursor(r->cursor()->str(), qd).second);
  auto [rows, batches] = history_->read([&](sql::Db& db) {
    if (!History::run(db, run_id)) throw Failed(FailureKind::not_found, "unknown run", {{"run_id", run_id}});
    if (!History::run_has_card(db, run_id, card))
      throw Failed(FailureKind::not_found, "the run has no record of " + path_key(card), {{"card", path_key(card)}});
    return std::pair{History::invocations(db, run_id, card, after, limit + 1), History::latest_batches(db, run_id, card)};
  });
  for (auto& s : rows) history_->overlay(s);
  FBB b;
  std::vector<flatbuffers::Offset<R::InvocationSummary>> out;
  bool more = rows.size() > limit;
  if (more) rows.resize(limit);
  for (const auto& s : rows) out.push_back(wire::invocation(b, s));
  auto v = b.CreateVector(out);
  auto cur = more ? b.CreateString(history_->make_cursor(qd, 0, std::to_string(rows.back().invocation)))
                  : flatbuffers::Offset<flatbuffers::String>();
  std::vector<flatbuffers::Offset<R::RunBatch>> batch_rows;
  for (const auto& batch : batches) batch_rows.push_back(wire::run_batch(b, batch));
  auto bv = b.CreateVector(batch_rows);
  R::InvocationPageBuilder pb(b);
  pb.add_records(v);
  pb.add_batches(bv);
  if (more) pb.add_next_cursor(cur);
  auto off = pb.Finish();
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::InvocationPage, off.Union()));
}

auto Service::run_environments(RequestIn& req) -> void {
  const auto* r = req.request()->value_as_RunEnvironmentsRequest();
  require_uuid(r->run_id(), "run_id");
  const auto run = r->run_id()->str();
  const auto offset = r->offset().value_or(0);
  const auto limit = page_limit(r->limit());
  if (offset > INT64_MAX) throw Failed(FailureKind::invalid_request, "offset is too large");
  FBB b;
  std::vector<flatbuffers::Offset<R::BuildEnvironment>> rows;
  std::vector<flatbuffers::Offset<flatbuffers::String>> caches;
  bool more = false;
  history_->read([&](sql::Db& db) {
    if (!History::run(db, run)) throw Failed(FailureKind::not_found, "unknown run");
    auto s = db.prepare("SELECT url FROM run_substituters WHERE run_id=? ORDER BY position");
    s.bind(1, run);
    while (s.step()) caches.push_back(b.CreateString(s.text(0)));
    auto q = db.prepare(
        "SELECT id, derivation, manifest, system, path_count FROM build_environments "
        "WHERE run_id=? ORDER BY rowid LIMIT ? OFFSET ?");
    q.bind(1, run).bind(2, static_cast<int>(limit + 1)).bind(3, offset);
    while (q.step()) {
      const auto id = q.text(0), derivation = q.text(1), manifest = q.text(2), system = q.text(3);
      const auto bytes = id.size() + derivation.size() + manifest.size() + system.size() + 128;
      if (bytes > kPagePayload)
        throw Failed(FailureKind::resource_limit, "recorded environment exceeds the page size");
      if (rows.size() == limit || b.GetSize() + bytes > kPagePayload) {
        more = true;
        break;
      }
      rows.push_back(R::CreateBuildEnvironment(b, b.CreateString(id), b.CreateString(derivation),
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
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::BuildEnvironmentPage, off.Union()));
}

auto Service::run_environment_paths(RequestIn& req) -> void {
  const auto* r = req.request()->value_as_RunEnvironmentPathsRequest();
  require_uuid(r->run_id(), "run_id");
  const auto run = r->run_id()->str();
  const auto id = r->environment_id()->str();
  const auto offset = r->offset().value_or(0);
  const auto limit = page_limit(r->limit());
  if (offset > INT64_MAX) throw Failed(FailureKind::invalid_request, "offset is too large");
  const auto query = r->query() ? r->query()->str() : std::string{};
  FBB b;
  std::vector<flatbuffers::Offset<flatbuffers::String>> paths;
  uint64_t total = 0;
  history_->read([&](sql::Db& db) {
    auto environment = db.prepare("SELECT path_count FROM build_environments WHERE run_id=? AND id=?");
    environment.bind(1, run).bind(2, id);
    if (!environment.step()) throw Failed(FailureKind::not_found, "unknown run environment");
    total = environment.u64(0);
    const auto predicate = std::string{
        "FROM environment_paths AS p WHERE run_id=? AND environment_id=? "
        "AND instr(lower(path), lower(?))>0 "};
    if (!query.empty()) {
      auto count = db.prepare("SELECT count(*) " + predicate);
      count.bind(1, run).bind(2, id).bind(3, query);
      count.step();
      total = count.u64(0);
    }
    auto q = db.prepare("SELECT path " + predicate + "ORDER BY position LIMIT ? OFFSET ?");
    q.bind(1, run).bind(2, id).bind(3, query)
        .bind(4, static_cast<int>(limit)).bind(5, offset);
    while (q.step()) {
      const auto path = q.text(0);
      if (path.size() + 16 > kPagePayload)
        throw Failed(FailureKind::resource_limit, "recorded store path exceeds the page size");
      if (b.GetSize() + path.size() + 16 > kPagePayload) break;
      paths.push_back(b.CreateString(path));
    }
  });
  if (paths.empty() && offset < total)
    throw Failed(FailureKind::integrity_error, "recorded environment has missing store paths");
  auto values = b.CreateVector(paths);
  R::EnvironmentPathPageBuilder page(b);
  page.add_paths(values);
  page.add_total(total);
  if (offset < total && paths.size() < total - offset) page.add_next_offset(offset + paths.size());
  auto off = page.Finish();
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::EnvironmentPathPage, off.Union()));
}

void Service::run_record(RequestIn& req) {
  const auto* r = req.request()->value_as_RunRecordRequest();
  require_uuid(r->run_id(), "run_id");
  std::string run_id = r->run_id()->str();
  auto card = wire::card_arg(r->card());
  bool follow = r->follow_latest().value_or(false);
  if (follow && r->invocation())
    throw Failed(FailureKind::invalid_request, "invocation and follow_latest cannot be combined");
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
    if (!run) throw Failed(FailureKind::not_found, "unknown run", {{"run_id", run_id}});
    if (!History::run_has_card(db, run_id, card))
      throw Failed(FailureKind::not_found, "the run has no record of " + path_key(card), {{"card", path_key(card)}});
    o.run = *run;
    if (r->invocation()) o.selected = *r->invocation();
    else if (follow) o.selected = History::latest_invocation(db, run_id, card, true);
    else o.selected = History::latest_completed(db, run_id, card);
    if (o.selected) {
      o.record = History::invocation(db, run_id, card, *o.selected);
      o.status = History::status(db, run_id, card, *o.selected);
      if (o.status) history_->overlay(*o.status);
      if (o.record && o.record->record_io) {
        o.in_meta = History::value_metas(db, run_id, card, *o.selected, true);
        o.out_meta = History::value_metas(db, run_id, card, *o.selected, false);
        for (const auto& m : o.in_meta)
          o.in_bytes.push_back(History::value_bytes(db, run_id, card, *o.selected, true, m.name).value_or(""));
        for (const auto& m : o.out_meta)
          o.out_bytes.push_back(History::value_bytes(db, run_id, card, *o.selected, false, m.name).value_or(""));
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
      time = o.record->summary.ended_at ? o.record->summary.ended_at : o.record->summary.started_at;
    } else if (o.status) {
      state = o.status->state;
      failure = o.status->failure;
      time = o.status->started_at;
    }
    if (follow && !o.status && !o.record && o.run.status.terminal() &&
        (o.run.status.state == RunState::failed || o.run.status.state == RunState::cancelled)) {
      state = o.run.status.state == RunState::cancelled ? CardState::cancelled : CardState::failed;
      failure = o.run.status.failure;
      time = o.run.status.ended_at;
    }
  }
  if (follow && o.past_failure && state != CardState::running && state != CardState::preparing) {
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
    auto values = [&](const std::vector<StoredValueMeta>& metas, const std::vector<std::string>& data) {
      std::vector<flatbuffers::Offset<D::NamedValue>> v;
      for (size_t i = 0; i < metas.size(); ++i) {
        const auto& m = metas[i];
        auto name = b.CreateString(m.name);
        auto bfbs = b.CreateVector(reinterpret_cast<const uint8_t*>(m.bfbs.data()), m.bfbs.size());
        auto root = b.CreateString(m.root_type);
        flatbuffers::Offset<flatbuffers::Vector<uint8_t>> inl;
        bool include = data[i].size() <= budget;
        if (include) {
          inl = b.CreateVector(reinterpret_cast<const uint8_t*>(data[i].data()), data[i].size());
          budget -= data[i].size();
        } else {
          truncated = true;
        }
        auto art = m.artifact_id ? b.CreateString(*m.artifact_id) : flatbuffers::Offset<flatbuffers::String>();
        auto hash = m.hash.empty() ? flatbuffers::Offset<flatbuffers::String>() : b.CreateString(m.hash);
        flatbuffers::Offset<flatbuffers::String> file_path;
        bool file_missing = false;
        if (m.kind != PortKind::FlatBuffer) {
          file_missing = true;
          auto type = std::make_shared<types::Type>();
          type->bfbs = m.bfbs;
          type->root_type = m.root_type;
          flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(m.bfbs.data()), m.bfbs.size());
          if (reflection::VerifySchemaBuffer(verifier) && type->root() && types::verify(*type, data[i])) {
            Value value{m.name, m.kind, type, data[i], {}};
            if (const auto recorded = path_value(value)) {
              const auto anchor = "/runs/" + run_id + "/values/";
              const auto position = recorded->rfind(anchor);
              if (position != std::string::npos) {
                const auto relative = recorded->substr(position + anchor.size());
                const auto root = store_->state_dir() / "runs" / run_id / "values";
                const auto content = root / relative;
                std::error_code error;
                if (fs::valid_relative_path(relative) && fs::resolves_inside(root, content)) {
                  const auto status = stdfs::symlink_status(content, error);
                  file_missing = error || (m.kind == PortKind::File ? !stdfs::is_regular_file(status)
                                                                  : !stdfs::is_directory(status));
                  if (!file_missing) file_path = b.CreateString(content.string());
                }
              }
            }
          }
        }
        D::NamedValueBuilder nb(b);
        nb.add_name(name);
        nb.add_kind(m.kind);
        nb.add_schema_bfbs(bfbs);
        nb.add_root_type(root);
        nb.add_size_bytes(m.size_bytes);
        if (include) nb.add_inline_bytes(inl);
        if (m.artifact_id) nb.add_artifact_id(art);
        if (!hash.IsNull()) nb.add_content_hash(hash);
        if (!file_path.IsNull()) nb.add_file_path(file_path);
        nb.add_file_missing(file_missing);
        v.push_back(nb.Finish());
      }
      return b.CreateVector(v);
    };
    auto ins = values(o.in_meta, o.in_bytes);
    auto outs = values(o.out_meta, o.out_bytes);
    std::vector<flatbuffers::Offset<R::InputSource>> srcs;
    if (o.record)
      for (const auto& s : o.record->input_sources)
        srcs.push_back(R::CreateInputSource(b, b.CreateString(s.port), wire::path(b, s.card), s.invocation));
    auto srcv = b.CreateVector(srcs);
    auto sum = wire::invocation(b, summary);
    flatbuffers::Offset<D::Display> disp;
    if (o.record && o.record->display) disp = wire::display(b, *o.record->display);
    auto params = summary.params_json.empty() ? flatbuffers::Offset<flatbuffers::String>() : b.CreateString(summary.params_json);
    R::InvocationRecordBuilder rb(b);
    rb.add_summary(sum);
    rb.add_inputs(ins);
    rb.add_outputs(outs);
    rb.add_input_sources(srcv);
    if (!disp.IsNull()) rb.add_display(disp);
    rb.add_record_io(o.record ? o.record->record_io : false);
    if (!params.IsNull()) rb.add_params_json(params);
    rec = rb.Finish();
  }
  auto run = wire::run_summary(b, o.run);
  auto fo = failure ? wire::failure(b, *failure) : flatbuffers::Offset<D::Failure>();
  R::HistoryStateBuilder hb(b);
  hb.add_state(state);
  if (failure) hb.add_failure(fo);
  if (time) hb.add_time(*time);
  auto hs = hb.Finish();
  auto agg = R::CreateAggregate(b, o.agg.calls, o.agg.succeeded, o.agg.failed, o.agg.cancelled, o.agg.total_ms);
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
  std::string out = wire::reply(b, req.request_id, R::ReplyPayload::RecordResult, off.Union());
  if (out.size() > kMaxFrameBytes) throw Failed(FailureKind::resource_limit, "the record metadata exceeds the frame limit");
  reply(req, std::move(out));
}

void Service::run_value_read(RequestIn& req) {
  const auto* r = req.request()->value_as_RunValueReadRequest();
  require_uuid(r->run_id(), "run_id");
  std::string run_id = r->run_id()->str();
  auto card = wire::card_arg(r->card());
  if (!r->invocation()) throw Failed(FailureKind::invalid_request, "invocation is required");
  if (r->side() != D::ValueSide::input && r->side() != D::ValueSide::output)
    throw Failed(FailureKind::invalid_request, "side must be input or output");
  bool input = r->side() == D::ValueSide::input;
  std::string port = r->port()->str();
  uint64_t offset = r->offset().value_or(0);
  uint64_t limit = content_limit(r->limit_bytes());
  auto page = history_->read([&](sql::Db& db) {
    if (!History::run(db, run_id)) throw Failed(FailureKind::not_found, "unknown run", {{"run_id", run_id}});
    auto rec = History::invocation(db, run_id, card, *r->invocation());
    if (!rec) throw Failed(FailureKind::not_found, "unknown invocation");
    if (!rec->record_io) throw Failed(FailureKind::recording_disabled, "recordIO was disabled for this invocation");
    auto p = History::value_page(db, run_id, card, *r->invocation(), input, port, offset, limit);
    if (!p) throw Failed(FailureKind::not_found, "no stored value for port " + port, {{"port", port}});
    return *p;
  });
  if (offset > page.size) throw Failed(FailureKind::invalid_range, "offset is past the end");
  reply(req, wire::file_page(req.request_id, port, page.bytes, offset, page.size, page.hash));
}

void Service::artifact_list(RequestIn& req) {
  const auto* r = req.request()->value_as_ArtifactListRequest();
  require_uuid(r->run_id(), "run_id");
  std::string run_id = r->run_id()->str();
  std::optional<CardPath> card;
  if (r->card()) card = wire::card_arg(r->card());
  if (r->invocation() && !card) throw Failed(FailureKind::invalid_request, "invocation requires card");
  std::optional<uint64_t> inv;
  if (r->invocation()) inv = *r->invocation();
  uint32_t limit = page_limit(r->limit());
  std::string qd = crypto::sha256_hex("artifacts|" + run_id + "|" + (card ? path_key(*card) : "\x01") + "|" +
                                      (inv ? std::to_string(*inv) : ""));
  std::string after;
  if (r->cursor()) after = history_->open_cursor(r->cursor()->str(), qd).second;
  auto rows = history_->read([&](sql::Db& db) {
    if (!History::run(db, run_id)) throw Failed(FailureKind::not_found, "unknown run", {{"run_id", run_id}});
    return History::artifacts(db, run_id, card, inv, after, limit + 1);
  });
  bool more = rows.size() > limit;
  if (more) rows.resize(limit);
  FBB b;
  std::vector<flatbuffers::Offset<R::ArtifactInfo>> out;
  for (const auto& a : rows) {
    std::vector<flatbuffers::Offset<R::ArtifactMember>> mem;
    for (const auto& m : a.members)
      mem.push_back(R::CreateArtifactMember(b, b.CreateString(m.path), m.size_bytes, b.CreateString(m.hash)));
    auto mv = b.CreateVector(mem);
    out.push_back(R::CreateArtifactInfo(b, b.CreateString(a.artifact_id), b.CreateString(a.run_id),
                                        wire::path(b, a.card), a.invocation, b.CreateString(a.output), a.kind,
                                        b.CreateString(a.name), mv));
  }
  auto v = b.CreateVector(out);
  flatbuffers::Offset<flatbuffers::String> cur;
  if (more) {
    const auto& l = rows.back();
    char inv_buf[32];
    std::snprintf(inv_buf, sizeof inv_buf, "%020llu", static_cast<unsigned long long>(l.invocation));
    cur = b.CreateString(history_->make_cursor(
        qd, 0, path_key(l.card) + "\x1f" + inv_buf + "\x1f" + l.output + "\x1f" + l.artifact_id));
  }
  R::ArtifactPageBuilder pb(b);
  pb.add_artifacts(v);
  if (more) pb.add_next_cursor(cur);
  auto off = pb.Finish();
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::ArtifactPage, off.Union()));
}

auto Service::module_read(RequestIn& req) -> void {
  const auto* r = req.request()->value_as_ModuleReadRequest();
  const auto identity = r->module_()->str();
  const stdfs::path root(identity);
  if (!valid_module_identity(identity) || stdfs::is_symlink(root))
    throw Failed(FailureKind::invalid_path, "invalid module identity");
  std::string source;
  try {
    if (stdfs::is_directory(root)) source = module_source(root);
    else if (auto cached = cached_module_source(store_->state_dir() / "modules", identity)) source = std::move(*cached);
    else throw std::runtime_error("module source is unavailable");
  } catch (const std::exception& e) {
    throw Failed(FailureKind::output_invalid, e.what());
  }
  const auto size = source.size();
  const auto offset = r->offset().value_or(0);
  if (offset > size) throw Failed(FailureKind::invalid_request, "module offset exceeds source size");
  const auto bytes = std::string_view(source).substr(offset, content_limit(r->limit_bytes()));
  reply(req, wire::file_page(req.request_id, "index.js", bytes, offset, size, crypto::sha256_hex(source)));
}

void Service::artifact_read(RequestIn& req) {
  const auto* r = req.request()->value_as_ArtifactReadRequest();
  require_uuid(r->artifact_id(), "artifact_id");
  auto a = history_->read([&](sql::Db& db) { return History::artifact(db, r->artifact_id()->str()); });
  // Rows may come from a copied history: they name folders only as UUIDs.
  if (!a || !crypto::is_uuid(a->run_id) || !crypto::is_uuid(a->artifact_id) || a->members.empty())
    throw Failed(FailureKind::not_found, "unknown artifact");
  const ArtifactMemberData* member = nullptr;
  if (a->kind == PortKind::File) {
    if (r->member()) throw Failed(FailureKind::invalid_request, "File artifacts have no members");
    member = &a->members.front();
  } else {
    if (!r->member()) throw Failed(FailureKind::invalid_request, "member is required for Directory artifacts");
    std::string m = r->member()->str();
    if (!fs::valid_relative_path(m)) throw Failed(FailureKind::invalid_path, "invalid member path");
    for (const auto& x : a->members)
      if (x.path == m) member = &x;
    if (!member) throw Failed(FailureKind::not_found, "no member " + m);
  }
  auto file = store_->state_dir() / "runs" / a->run_id / "artifacts" / a->artifact_id / member->path;
  uint64_t offset = r->offset().value_or(0);
  uint64_t limit = content_limit(r->limit_bytes());
  struct stat st{};
  if (::stat(file.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) throw Failed(FailureKind::not_found, "the artifact content is missing");
  auto size = static_cast<uint64_t>(st.st_size);
  int64_t mtime = static_cast<int64_t>(st.st_mtime) * 1000000000 +
#ifdef __APPLE__
                  st.st_mtimespec.tv_nsec;
#else
                  st.st_mtim.tv_nsec;
#endif
  // The whole content is checked against its hash once (streamed); while the
  // file keeps that size and time, later pages read their range only.
  bool checked;
  {
    std::lock_guard<std::mutex> l(verified_mu_);
    auto v = verified_.find(file.string());
    checked = v != verified_.end() && v->second == std::make_pair(size, mtime);
  }
  if (!checked) {
    if (fs::sha256_file(file) != member->hash) throw Failed(FailureKind::integrity_error, "the artifact content is corrupt");
    std::lock_guard<std::mutex> l(verified_mu_);
    verified_[file.string()] = {size, mtime};
  }
  if (offset > size) throw Failed(FailureKind::invalid_range, "offset is past the end");
  auto slice = fs::read_range(file, offset, std::min<uint64_t>(limit, size - offset));
  if (!slice) throw Failed(FailureKind::not_found, "the artifact content is missing");
  reply(req, wire::file_page(req.request_id, stdfs::path(member->path).filename().string(), *slice, offset, size,
                             member->hash));
}

void Service::log_range(RequestIn& req) {
  const auto* r = req.request()->value_as_LogRangeRequest();
  require_uuid(r->run_id(), "run_id");
  std::string run_id = r->run_id()->str();
  std::optional<CardPath> card;
  if (r->card()) card = wire::card_arg(r->card());
  std::optional<uint64_t> inv;
  if (r->invocation()) {
    if (!card) throw Failed(FailureKind::invalid_request, "invocation requires card");
    inv = *r->invocation();
  }
  auto stream = r->stream();
  if (stream != LogStream::stdout && stream != LogStream::stderr && stream != LogStream::journal &&
      stream != LogStream::build)
    throw Failed(FailureKind::invalid_request, "stream must be stdout, stderr or journal");
  bool tail = r->tail().value_or(false);
  uint64_t offset = r->offset().value_or(0);
  if (tail && offset != 0) throw Failed(FailureKind::invalid_request, "tail and offset cannot be combined");
  uint32_t limit = r->limit_bytes().value_or(65536);
  if (limit < 4 || limit > (256u << 10)) throw Failed(FailureKind::invalid_request, "limit_bytes must be 4..262144");
  auto slice = history_->read([&](sql::Db& db) {
    if (!History::run(db, run_id)) throw Failed(FailureKind::not_found, "unknown run", {{"run_id", run_id}});
    return history_->read_log(db, run_id, card, inv, stream, offset, tail, limit);
  });
  FBB b(slice.text.size() + 512);
  auto t = b.CreateString(slice.text);
  R::LogSliceBuilder lb(b);
  lb.add_text(t);
  lb.add_offset(slice.offset);
  lb.add_next_offset(slice.next_offset);
  lb.add_size_bytes(slice.size);
  lb.add_exists(slice.exists);
  lb.add_truncated(slice.truncated);
  auto off = lb.Finish();
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::LogSlice, off.Union()));
}

}  // namespace tsugumi
