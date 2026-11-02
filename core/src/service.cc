#include "service.hh"

#include <unistd.h>

#include <flatbuffers/reflection.h>

#include <algorithm>

#include "nix.hh"
#include "scheduler.hh"
#include "util/crypto.hh"
#include "util/fs.hh"
#include "util/log.hh"
#include "util/process.hh"

#include <regex>

extern const unsigned char tsugumi_rpc_bfbs[];

namespace tsugumi {

namespace R = Tsugumi::RPC;
namespace D = Tsugumi::Data;
using FBB = flatbuffers::FlatBufferBuilder;
namespace stdfs = std::filesystem;

constexpr uint32_t kProtocol = 1;

// ---------------------------------------------------------------------------
// Request digests

namespace {

const std::map<std::string, std::string>& digest_defaults() {
  static const std::map<std::string, std::string> d = {
      {"Tsugumi.RPC.ProjectValidateRequest.mode", "2"},
      {"Tsugumi.RPC.ProjectFileListRequest.depth", "0"},
      {"Tsugumi.RPC.ProjectFileListRequest.include_directories", "0"},
      {"Tsugumi.RPC.ProjectFileListRequest.limit", "100"},
      {"Tsugumi.RPC.ProjectFileReadRequest.offset", "0"},
      {"Tsugumi.RPC.ProjectFileReadRequest.limit_bytes", "262144"},
      {"Tsugumi.RPC.RunPlanRequest.propagate", "1"},
      {"Tsugumi.RPC.RunRequest.propagate", "1"},
      {"Tsugumi.RPC.RunRequest.max_concurrency", "4"},
      {"Tsugumi.RPC.RunRequest.nix_jobs", "1"},
      {"Tsugumi.RPC.RunRequest.nix_cores", "1"},
  };
  return d;
}

void digest_table(const reflection::Schema& s, const reflection::Object& obj,
                  const flatbuffers::Table& t, crypto::Sha256& h);

void put_len(crypto::Sha256& h, std::string_view v) {
  uint64_t n = v.size();
  h.update(&n, sizeof n);
  h.update(v);
}

void digest_scalar(const reflection::Field& f, const flatbuffers::Table& t, crypto::Sha256& h,
                   const std::string& qualified) {
  bool present = t.CheckField(f.offset());
  std::string repr;
  if (present) {
    auto bt = f.type()->base_type();
    if (bt == reflection::Float || bt == reflection::Double) {
      repr = std::to_string(flatbuffers::GetAnyFieldF(t, f));
    } else {
      repr = std::to_string(flatbuffers::GetAnyFieldI(t, f));
    }
  } else {
    auto it = digest_defaults().find(qualified);
    if (it != digest_defaults().end()) repr = it->second;
    else if (!f.optional()) repr = std::to_string(f.default_integer());
    else repr = "\x01" "absent";
  }
  put_len(h, repr);
}

void digest_table(const reflection::Schema& s, const reflection::Object& obj,
                  const flatbuffers::Table& t, crypto::Sha256& h) {
  std::vector<const reflection::Field*> fields(obj.fields()->begin(), obj.fields()->end());
  std::sort(fields.begin(), fields.end(), [](auto* a, auto* b) { return a->id() < b->id(); });
  for (const auto* f : fields) {
    std::string qualified = obj.name()->str() + "." + f->name()->str();
    put_len(h, f->name()->str());
    auto bt = f->type()->base_type();
    switch (bt) {
      case reflection::String: {
        const auto* str = flatbuffers::GetFieldS(t, *f);
        put_len(h, str ? std::string_view(str->c_str(), str->size()) : std::string_view("\x01" "absent"));
        break;
      }
      case reflection::Obj: {
        const auto* sub = s.objects()->Get(f->type()->index());
        const auto* st = flatbuffers::GetFieldT(t, *f);
        if (st) digest_table(s, *sub, *st, h);
        else put_len(h, "\x01" "absent");
        break;
      }
      case reflection::Union: {
        // The type field precedes; resolve the member table.
        auto type_name = f->name()->str() + "_type";
        const auto* tf = obj.fields()->LookupByKey(type_name.c_str());
        auto utype = static_cast<int>(flatbuffers::GetFieldI<uint8_t>(t, *tf));
        put_len(h, std::to_string(utype));
        if (utype) {
          const auto* en = s.enums()->Get(f->type()->index());
          const auto* ev = en->values()->LookupByKey(utype);
          const auto* sub = s.objects()->Get(ev->union_type()->index());
          const auto* st = flatbuffers::GetFieldT(t, *f);
          if (st) digest_table(s, *sub, *st, h);
        }
        break;
      }
      case reflection::Vector: {
        const auto* vec = flatbuffers::GetFieldAnyV(t, *f);
        uint64_t n = vec ? vec->size() : 0;  // absent vectors equal empty ones
        h.update(&n, sizeof n);
        if (!vec) break;
        auto et = f->type()->element();
        if (et == reflection::UByte || et == reflection::Byte) {
          put_len(h, std::string_view(reinterpret_cast<const char*>(vec->Data()), vec->size()));
        } else if (et == reflection::String) {
          for (uint64_t i = 0; i < n; ++i) {
            const auto* str = flatbuffers::GetAnyVectorElemPointer<const flatbuffers::String>(vec, i);
            put_len(h, std::string_view(str->c_str(), str->size()));
          }
        } else if (et == reflection::Obj) {
          const auto* sub = s.objects()->Get(f->type()->index());
          for (uint64_t i = 0; i < n; ++i)
            digest_table(s, *sub, *flatbuffers::GetAnyVectorElemPointer<const flatbuffers::Table>(vec, i), h);
        } else {
          for (uint64_t i = 0; i < n; ++i) put_len(h, std::to_string(flatbuffers::GetAnyVectorElemI(vec, et, i)));
        }
        break;
      }
      case reflection::UType:
        break;  // covered with the union value
      default:
        digest_scalar(*f, t, h, qualified);
    }
  }
}

}  // namespace

std::string request_digest(const R::Request* req) {
  const auto* schema = reflection::GetSchema(tsugumi_rpc_bfbs);
  const char* name = R::EnumNameRequestPayload(req->value_type());
  std::string full = std::string("Tsugumi.RPC.") + name;
  const auto* obj = schema->objects()->LookupByKey(full.c_str());
  crypto::Sha256 h;
  h.update(full);
  digest_table(*schema, *obj, *reinterpret_cast<const flatbuffers::Table*>(req->value()), h);
  return h.finish_hex();
}

// ---------------------------------------------------------------------------

Service::Service(ServiceOptions opts) : opts_(std::move(opts)) {
  cursor_secret_ = crypto::random_token(32);
  mutation_lane_ = std::make_unique<Lane>(1, opts_.limits.mutation_queue_limit,
                                          std::chrono::milliseconds(opts_.limits.queue_wait_ms));
  read_pool_ = std::make_unique<Lane>(6, 1024, std::chrono::milliseconds(opts_.limits.queue_wait_ms));
}

Service::~Service() { shutdown(); }

void Service::open() {
  store_ = std::make_unique<Store>(StoreOptions{opts_.root, opts_.fault});
  store_->open();
  history_ = std::make_unique<History>(store_->state_dir() / "history.sqlite", cursor_secret_, opts_.fault);
  nix_ = std::make_unique<NixRunner>(*this);
  scheduler_ = std::make_unique<Scheduler>(*this);
  scheduler_->recover_interrupted();
}

void Service::shutdown() {
  if (scheduler_) scheduler_->shutdown();
  if (mutation_lane_) mutation_lane_->stop();
  if (read_pool_) read_pool_->stop();
}

std::vector<std::string> Service::capabilities() {
  return {"SessionHello", "ProjectOpen", "ProjectClose", "ProjectInspect", "ProjectSave",
          "ProjectReload", "ProjectValidate", "ProjectImport", "ProjectCacheClear", "OperationGet",
          "ProjectFileList", "ProjectFileStat", "ProjectFileRead", "ProjectFileFormat",
          "CardSDKPrepare", "CardSDKStatus", "RunPlan", "Run", "RunCancel", "RunStatus",
          "RunHistory", "RunInvocations", "RunRecord", "RunValueRead", "ArtifactList",
          "ArtifactRead", "LogRange", "ViewSetVisible", "ViewSave",
          "display:Text", "display:Scalar", "display:LineChart", "format:json"};
}

void Service::reply(const RequestIn& req, std::string bytes) { server_->reply(req.conn, std::move(bytes)); }

void Service::fail(const RequestIn& req, const FailureData& f) {
  reply(req, wire::failure_reply(req.request_id, f));
}

std::pair<std::string, bool> Service::hello(const RequestIn& in) {
  const auto* h = in.request()->value_as_SessionHelloRequest();
  auto failure = [&](FailureKind k, std::string msg) {
    return std::make_pair(wire::failure_reply(in.request_id, FailureData::make(k, std::move(msg))), false);
  };
  if (!h->protocol()) return failure(FailureKind::invalid_request, "protocol is required");
  if (*h->protocol() != kProtocol)
    return failure(FailureKind::protocol_mismatch, "server speaks protocol 1 only");
  if (!crypto::is_uuid(h->client_id()->str())) return failure(FailureKind::invalid_request, "client_id must be a UUID");
  if (in.websocket) {
    if (!h->token() || !crypto::equal_secret(h->token()->str(), opts_.token))
      return failure(FailureKind::unauthenticated, "invalid token");
  } else if (h->token() && !crypto::equal_secret(h->token()->str(), opts_.token)) {
    return failure(FailureKind::unauthenticated, "invalid token");
  }
  if (h->expected_project_id() && h->expected_project_id()->str() != store_->project_id())
    return failure(FailureKind::permission_denied, "this endpoint serves a different project");
  FBB b;
  auto caps = capabilities();
  std::vector<flatbuffers::Offset<flatbuffers::String>> cv;
  for (const auto& c : caps) cv.push_back(b.CreateString(c));
  const auto& l = opts_.limits;
  auto limits = R::CreateLimits(b, l.max_frame_bytes, l.max_connections, l.max_inflight,
                                l.mutation_queue_limit, l.queue_wait_ms, l.outbox_messages,
                                l.outbox_bytes, l.page_limit, l.file_chunk_bytes, l.log_chunk_bytes);
  auto r = R::CreateHelloResult(b, kProtocol, b.CreateString(store_->project_id()),
                                b.CreateString(opts_.server_instance_id), limits, b.CreateVector(cv));
  return {wire::reply(b, in.request_id, R::ReplyPayload::HelloResult, r.Union()), true};
}

void Service::disconnected(ConnId conn) {
  std::lock_guard<std::mutex> l(subs_mu_);
  subscriptions_.erase(conn);
}

void Service::on_lane(Lane& lane, RequestIn req, void (Service::*fn)(RequestIn&)) {
  bool ok = lane.submit([this, req, fn](bool expired) mutable {
    if (expired) return fail(req, FailureData::make(FailureKind::timeout, "the request waited longer than 30 s and was not executed"));
    try {
      (this->*fn)(req);
    } catch (const Failed& f) {
      fail(req, f.failure);
    } catch (const std::exception& e) {
      log::error(std::string("request failed: ") + e.what());
      fail(req, FailureData::make(FailureKind::internal_error, e.what()));
    }
  });
  if (!ok) fail(req, FailureData::make(FailureKind::busy, "the request queue is full"));
}

void Service::dispatch(RequestIn req) {
  using P = R::RequestPayload;
  Lane& m = *mutation_lane_;
  Lane& r = *read_pool_;
  switch (req.request()->value_type()) {
    case P::ProjectOpenRequest: return on_lane(m, std::move(req), &Service::open_project);
    case P::ProjectSaveRequest: return on_lane(m, std::move(req), &Service::save);
    case P::ProjectReloadRequest: return on_lane(m, std::move(req), &Service::reload);
    case P::ProjectImportRequest: return on_lane(m, std::move(req), &Service::import_definitions);
    case P::ProjectCacheClearRequest: return on_lane(m, std::move(req), &Service::cache_clear);
    case P::RunRequest: return on_lane(m, std::move(req), &Service::run);
    case P::CardSDKPrepareRequest: return on_lane(m, std::move(req), &Service::sdk_prepare);
    case P::ProjectCloseRequest: return on_lane(r, std::move(req), &Service::close_project);
    case P::ProjectInspectRequest: return on_lane(r, std::move(req), &Service::inspect);
    case P::ProjectValidateRequest: return on_lane(r, std::move(req), &Service::validate);
    case P::OperationGetRequest: return on_lane(r, std::move(req), &Service::operation_get);
    case P::ProjectFileListRequest: return on_lane(r, std::move(req), &Service::file_list);
    case P::ProjectFileStatRequest: return on_lane(r, std::move(req), &Service::file_stat);
    case P::ProjectFileReadRequest: return on_lane(r, std::move(req), &Service::file_read);
    case P::ProjectFileFormatRequest: return on_lane(r, std::move(req), &Service::file_format);
    case P::CardSDKStatusRequest: return on_lane(r, std::move(req), &Service::sdk_status);
    case P::RunPlanRequest: return on_lane(r, std::move(req), &Service::run_plan);
    case P::RunCancelRequest: return on_lane(r, std::move(req), &Service::run_cancel);
    case P::RunStatusRequest: return on_lane(r, std::move(req), &Service::run_status);
    case P::RunHistoryRequest: return on_lane(r, std::move(req), &Service::run_history);
    case P::RunInvocationsRequest: return on_lane(r, std::move(req), &Service::run_invocations);
    case P::RunRecordRequest: return on_lane(r, std::move(req), &Service::run_record);
    case P::RunValueReadRequest: return on_lane(r, std::move(req), &Service::run_value_read);
    case P::ArtifactListRequest: return on_lane(r, std::move(req), &Service::artifact_list);
    case P::ArtifactReadRequest: return on_lane(r, std::move(req), &Service::artifact_read);
    case P::LogRangeRequest: return on_lane(r, std::move(req), &Service::log_range);
    case P::ViewSetVisibleRequest: return on_lane(r, std::move(req), &Service::view_set_visible);
    case P::ViewSaveRequest: return on_lane(r, std::move(req), &Service::view_save);
    default:
      return fail(req, FailureData::make(FailureKind::unsupported, "request is not supported"));
  }
}

// ---------------------------------------------------------------------------
// Receipts

std::string Service::receipt_reply(uint64_t request_id, const std::string& receipt_bytes, bool replayed) {
  const auto* rc = flatbuffers::GetRoot<R::OperationReceipt>(receipt_bytes.data());
  FBB b;
  R::ReplyPayload type;
  flatbuffers::Offset<void> value;
  const char* tname = nullptr;
  switch (rc->result_type()) {
    case R::ReceiptValue::SaveResult: type = R::ReplyPayload::SaveResult; tname = "Tsugumi.RPC.SaveResult"; break;
    case R::ReceiptValue::ImportResult: type = R::ReplyPayload::ImportResult; tname = "Tsugumi.RPC.ImportResult"; break;
    case R::ReceiptValue::RunAccepted: type = R::ReplyPayload::RunAccepted; tname = "Tsugumi.RPC.RunAccepted"; break;
    case R::ReceiptValue::CacheClearResult: type = R::ReplyPayload::CacheClearResult; tname = "Tsugumi.RPC.CacheClearResult"; break;
    case R::ReceiptValue::SDKPrepareResult: type = R::ReplyPayload::SDKPrepareResult; tname = "Tsugumi.RPC.SDKPrepareResult"; break;
    default: type = R::ReplyPayload::Failure; tname = "Tsugumi.Data.Failure"; break;
  }
  value = wire::copy_table(b, tname, reinterpret_cast<const flatbuffers::Table*>(rc->result()));
  std::string out = wire::reply(b, request_id, type, value);
  if (replayed) {
    auto* m = R::GetMutableMessage(out.data());
    auto* rep = static_cast<R::Reply*>(m->mutable_payload());
    switch (type) {
      case R::ReplyPayload::SaveResult:
        static_cast<R::SaveResult*>(rep->mutable_value())->mutate_replayed(true);
        break;
      case R::ReplyPayload::ImportResult:
        static_cast<R::ImportResult*>(rep->mutable_value())->mutable_save()->mutate_replayed(true);
        break;
      case R::ReplyPayload::RunAccepted:
        static_cast<R::RunAccepted*>(rep->mutable_value())->mutate_replayed(true);
        break;
      default: break;
    }
  }
  return out;
}

bool Service::replay_or_join(RequestIn& req, const std::string& op_id, const std::string& kind,
                             const std::string& digest) {
  if (!crypto::is_uuid(op_id)) throw Failed(FailureKind::invalid_request, "op_id must be a UUID");
  std::optional<Receipt> rc = store_->receipt(op_id);
  if (!rc) {
    auto row = history_->read([&](sql::Db& db) { return History::receipt(db, op_id); });
    if (row) rc = Receipt{op_id, row->kind, row->digest, row->bytes};
  }
  if (rc) {
    if (rc->kind != kind || rc->digest != digest)
      throw Failed(FailureKind::operation_conflict, "op_id was already used for a different request",
                   {{"op_id", op_id}});
    reply(req, receipt_reply(req.request_id, rc->bytes, true));
    return true;
  }
  std::lock_guard<std::mutex> l(ops_mu_);
  auto it = pending_ops_.find(op_id);
  if (it != pending_ops_.end()) {
    if (it->second.digest != kind + ":" + digest)
      throw Failed(FailureKind::operation_conflict, "op_id is pending for a different request",
                   {{"op_id", op_id}});
    it->second.waiters.emplace_back(req.conn, req.request_id);
    return true;
  }
  pending_ops_[op_id] = Pending{kind + ":" + digest, {}};
  return false;
}

void Service::finish_op(const std::string& op_id, const std::string& receipt_bytes) {
  std::vector<std::pair<ConnId, uint64_t>> waiters;
  {
    std::lock_guard<std::mutex> l(ops_mu_);
    auto it = pending_ops_.find(op_id);
    if (it == pending_ops_.end()) return;
    waiters = std::move(it->second.waiters);
    pending_ops_.erase(it);
  }
  for (auto [conn, rid] : waiters) server_->reply(conn, receipt_reply(rid, receipt_bytes, true));
}

namespace {
std::string failure_receipt(const std::string& op_id, const FailureData& f) {
  FBB b;
  auto fo = wire::failure(b, f);
  auto r = R::CreateOperationReceipt(b, b.CreateString(op_id), R::ReceiptValue::Failure, fo.Union());
  b.Finish(r);
  return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize());
}
}  // namespace

void Service::store_failure_receipt(const std::string& op_id, const std::string& kind,
                                    const std::string& digest, const FailureData& f) {
  // busy/timeout/persistence failures leave the operation retryable under the same id.
  if (f.kind == FailureKind::busy || f.kind == FailureKind::timeout ||
      f.kind == FailureKind::persistence_failed || f.kind == FailureKind::internal_error) {
    finish_op(op_id, failure_receipt(op_id, f));
    return;
  }
  std::string bytes = failure_receipt(op_id, f);
  try {
    if (kind == "save" || kind == "reload" || kind == "import") store_->record_receipt({op_id, kind, digest, bytes});
    else history_->write_sync([&](sql::Db& db) { History::put_receipt(db, op_id, kind, digest, bytes); });
  } catch (const std::exception& e) {
    log::error(std::string("cannot persist failure receipt: ") + e.what());
  }
  finish_op(op_id, bytes);
}

// ---------------------------------------------------------------------------
// Snapshots and deltas

namespace {

flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<R::SourceInfo>>> manifest(
    FBB& b, const Document& doc, const std::vector<std::string>* only) {
  std::vector<flatbuffers::Offset<R::SourceInfo>> v;
  auto add = [&](const std::string& p, const SourceEntry& e) {
    v.push_back(wire::source_info(b, p, D::SourceKind::file, true, e.hash, e.size));
  };
  if (only) {
    for (const auto& p : *only) add(p, doc.sources.at(p));
  } else {
    for (const auto& [p, e] : doc.sources) add(p, e);
  }
  return b.CreateVector(v);
}

flatbuffers::Offset<R::ViewState> view_state(FBB& b, const schema::ViewStateJson& v) {
  std::vector<flatbuffers::Offset<R::Camera>> cams;
  for (const auto& c : *v.cameras) {
    auto sp = wire::path(b, *c.scope);
    R::CameraBuilder cb(b);
    cb.add_scope(sp);
    cb.add_x(*c.x);
    cb.add_y(*c.y);
    cb.add_zoom(*c.zoom);
    cams.push_back(cb.Finish());
  }
  std::vector<flatbuffers::Offset<D::CardPath>> sel;
  for (const auto& p : *v.selected_cards) sel.push_back(wire::path(b, p));
  auto scope = wire::path(b, *v.scope);
  return R::CreateViewState(b, scope, b.CreateVector(cams), b.CreateVector(sel));
}

schema::ViewStateJson view_json(const R::ViewState* v) {
  auto checked = [](const D::CardPath* p) {
    if (!p) throw Failed(FailureKind::invalid_request, "view paths are required");
    auto path = wire::path(p);
    for (const auto& s : path)
      if (!valid_id(s)) throw Failed(FailureKind::invalid_request, "invalid card id in view");
    return path;
  };
  schema::ViewStateJson out;
  *out.scope = checked(v->scope());
  for (const auto* c : *v->cameras()) {
    if (!c->x() || !c->y() || !c->zoom() || !std::isfinite(*c->x()) || !std::isfinite(*c->y()) ||
        !std::isfinite(*c->zoom()) || *c->zoom() <= 0)
      throw Failed(FailureKind::invalid_request, "camera needs finite x, y and a positive zoom");
    schema::ViewCamera cam;
    *cam.scope = checked(c->scope());
    *cam.x = *c->x();
    *cam.y = *c->y();
    *cam.zoom = *c->zoom();
    out.cameras->push_back(std::move(cam));
  }
  for (const auto* p : *v->selected_cards()) out.selected_cards->push_back(checked(p));
  return out;
}

}  // namespace

std::optional<Service::ViewRecord> Service::load_view(const std::string& view_id) {
  std::lock_guard<std::mutex> l(view_mu_);
  auto text = fs::read_file(store_->state_dir() / "view.json");
  if (!text) return std::nullopt;
  schema::ViewFile file;
  if (schema::decode(*text, file)) return std::nullopt;
  for (const auto& [id, entry] : file.views->entries())
    if (id == view_id) return ViewRecord{*entry.rev, *entry.view};
  return std::nullopt;
}

RunStatusData Service::current_run_status() {
  if (auto s = scheduler_->active_status()) return *s;
  auto last = history_->read([](sql::Db& db) { return History::latest_run(db); });
  if (last) return last->status;
  return RunStatusData{};
}

flatbuffers::Offset<R::ProjectSnapshot> Service::snapshot(FBB& b, const Document& doc,
                                                          const std::optional<std::string>& view_id) {
  std::vector<flatbuffers::Offset<R::Definition>> defs;
  for (const auto& [id, d] : doc.definitions) defs.push_back(wire::definition(b, id, d));
  auto defs_v = b.CreateVector(defs);
  auto src = manifest(b, doc, nullptr);
  auto res = wire::resolution(b, *doc.resolution);
  auto diags = wire::diagnostics(b, doc.resolution->diags);
  auto run = wire::run_status(b, current_run_status());
  auto pid = b.CreateString(store_->project_id());
  auto pj = b.CreateString(doc.project_json);
  auto lj = b.CreateString(doc.layout_json);
  flatbuffers::Offset<R::ViewState> view;
  flatbuffers::Offset<flatbuffers::String> vid;
  std::optional<uint64_t> vrev;
  if (view_id) {
    vid = b.CreateString(*view_id);
    if (auto rec = load_view(*view_id)) {
      view = view_state(b, rec->view);
      vrev = rec->rev;
    } else {
      vrev = 0;
    }
  }
  R::ProjectSnapshotBuilder sb(b);
  sb.add_project_id(pid);
  sb.add_rev(doc.rev);
  sb.add_execution_rev(doc.execution_rev);
  sb.add_project_json(pj);
  sb.add_layout_json(lj);
  sb.add_definitions(defs_v);
  sb.add_sources(src);
  sb.add_resolution(res);
  sb.add_diagnostics(diags);
  sb.add_run(run);
  if (view_id) {
    if (!view.IsNull()) sb.add_view(view);
    sb.add_view_id(vid);
    sb.add_view_rev(*vrev);
  }
  return sb.Finish();
}

std::string Service::snapshot_reply(uint64_t request_id, const Document& doc,
                                    const std::optional<std::string>& view_id, bool open,
                                    const std::string& sub_id) {
  FBB b(1 << 16);
  auto snap = snapshot(b, doc, view_id);
  std::string out;
  if (open) {
    auto r = R::CreateOpenResult(b, snap, b.CreateString(sub_id), uint64_t(1));
    out = wire::reply(b, request_id, R::ReplyPayload::OpenResult, r.Union());
  } else {
    out = wire::reply(b, request_id, R::ReplyPayload::ProjectSnapshot, snap.Union());
  }
  if (out.size() > kMaxFrameBytes)
    throw Failed(FailureKind::resource_limit, "the snapshot exceeds the 16 MiB frame limit");
  return out;
}

namespace {

flatbuffers::Offset<R::DocumentDelta> delta_offset(FBB& b, const Delta& d) {
  const Document& doc = *d.doc;
  std::vector<flatbuffers::Offset<R::Definition>> ups;
  for (const auto& id : d.definitions_upsert) ups.push_back(wire::definition(b, id, doc.definitions.at(id)));
  auto ups_v = b.CreateVector(ups);
  std::vector<flatbuffers::Offset<flatbuffers::String>> rem;
  for (const auto& id : d.definitions_removed) rem.push_back(b.CreateString(id));
  auto rem_v = b.CreateVector(rem);
  auto src_v = manifest(b, doc, &d.sources_upsert);
  std::vector<flatbuffers::Offset<flatbuffers::String>> srem;
  for (const auto& p : d.sources_removed) srem.push_back(b.CreateString(p));
  auto srem_v = b.CreateVector(srem);
  auto res = wire::resolution(b, *doc.resolution);
  auto diags = wire::diagnostics(b, doc.resolution->diags);
  auto op = b.CreateString(d.op_id);
  auto origin = b.CreateString(d.origin);
  auto pj = d.project_changed ? b.CreateString(doc.project_json) : flatbuffers::Offset<flatbuffers::String>();
  auto lj = d.layout_changed ? b.CreateString(doc.layout_json) : flatbuffers::Offset<flatbuffers::String>();
  R::DocumentDeltaBuilder db(b);
  db.add_base_rev(d.base_rev);
  db.add_rev(d.rev);
  db.add_execution_rev(d.execution_rev);
  db.add_op_id(op);
  db.add_origin(origin);
  if (d.project_changed) db.add_project_json(pj);
  if (d.layout_changed) db.add_layout_json(lj);
  db.add_definitions_upsert(ups_v);
  db.add_definitions_removed(rem_v);
  db.add_sources_upsert(src_v);
  db.add_sources_removed(srem_v);
  db.add_resolution(res);
  db.add_diagnostics(diags);
  return db.Finish();
}

flatbuffers::Offset<R::SaveResult> save_result(FBB& b, const Delta* d, const Document& base) {
  flatbuffers::Offset<R::DocumentDelta> delta;
  if (d) delta = delta_offset(b, *d);
  R::SaveResultBuilder sb(b);
  if (d) sb.add_delta(delta);
  sb.add_rev(d ? d->rev : base.rev);
  sb.add_execution_rev(d ? d->execution_rev : base.execution_rev);
  sb.add_replayed(false);
  return sb.Finish();
}

std::string finish_receipt(FBB& b, const std::string& op_id, R::ReceiptValue type, flatbuffers::Offset<void> v) {
  auto r = R::CreateOperationReceipt(b, b.CreateString(op_id), type, v);
  b.Finish(r);
  return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize());
}

}  // namespace

void Service::publish_delta(const Delta& delta) {
  auto shared = std::make_shared<Delta>(delta);
  Note n;
  n.kind = Note::Kind::Doc;
  n.approx_bytes = 4096 + delta.doc->project_json.size() + delta.doc->layout_json.size();
  n.build = std::make_shared<const Note::Build>([shared](FBB& b) {
    auto d = delta_offset(b, *shared);
    return std::make_pair(R::NotificationPayload::DocPatched, R::CreateDocPatched(b, d).Union());
  });
  server_->broadcast(std::move(n));
}

void Service::check_at_rev(std::optional<uint64_t> at_rev, const Document& doc) const {
  if (!at_rev) throw Failed(FailureKind::invalid_request, "at_rev is required");
  if (*at_rev != doc.rev) {
    FailureData f = FailureData::make(FailureKind::revision_conflict, "the document is at another revision");
    f.current_rev = doc.rev;
    throw Failed(f);
  }
}

// ---------------------------------------------------------------------------
// Mutation lane

void Service::open_project(RequestIn& req) {
  const auto* r = req.request()->value_as_ProjectOpenRequest();
  std::optional<std::string> view_id;
  if (r->view_id()) {
    if (!crypto::is_uuid(r->view_id()->str())) throw Failed(FailureKind::invalid_request, "view_id must be a UUID");
    view_id = r->view_id()->str();
  }
  std::string sub = crypto::uuid4();
  // Runs on the mutation lane: no save can commit between this snapshot and
  // the subscription becoming active, and the reply is queued before any later
  // notification.
  auto doc = store_->current();
  std::string bytes = snapshot_reply(req.request_id, *doc, view_id, true, sub);
  {
    std::lock_guard<std::mutex> l(subs_mu_);
    subscriptions_[req.conn] = sub;
  }
  server_->subscribe(req.conn, sub, std::move(bytes));
}

namespace {

std::vector<ChangeIn> decode_changes(const R::ProjectSaveRequest* r) {
  std::vector<ChangeIn> out;
  for (const auto* c : *r->changes()) {
    ChangeIn ch;
    switch (c->value_type()) {
      case R::DocumentChangeValue::SetProject:
        ch.kind = ChangeIn::Kind::SetProject;
        ch.json = c->value_as_SetProject()->json()->str();
        break;
      case R::DocumentChangeValue::SetLayout:
        ch.kind = ChangeIn::Kind::SetLayout;
        ch.json = c->value_as_SetLayout()->json()->str();
        break;
      case R::DocumentChangeValue::PutDefinition: {
        const auto* p = c->value_as_PutDefinition();
        ch.kind = ChangeIn::Kind::PutDefinition;
        ch.id = p->id()->str();
        ch.json = p->card_json()->str();
        if (p->meta_json()) ch.meta_json = p->meta_json()->str();
        break;
      }
      case R::DocumentChangeValue::RemoveDefinition:
        ch.kind = ChangeIn::Kind::RemoveDefinition;
        ch.id = c->value_as_RemoveDefinition()->id()->str();
        break;
      case R::DocumentChangeValue::PutSource: {
        const auto* p = c->value_as_PutSource();
        ch.kind = ChangeIn::Kind::PutSource;
        ch.path = p->path()->str();
        ch.bytes.assign(reinterpret_cast<const char*>(p->bytes()->data()), p->bytes()->size());
        if (p->base_hash()) ch.base_hash = p->base_hash()->str();
        break;
      }
      case R::DocumentChangeValue::PatchSource: {
        const auto* p = c->value_as_PatchSource();
        ch.kind = ChangeIn::Kind::PatchSource;
        ch.path = p->path()->str();
        ch.base_hash = p->base_hash()->str();
        for (const auto* sp : *p->patches()) {
          if (!sp->offset() || !sp->remove())
            throw Failed(FailureKind::invalid_request, "patch offset and remove are required");
          ch.patches.push_back({*sp->offset(), *sp->remove(), sp->text()->str()});
        }
        break;
      }
      case R::DocumentChangeValue::RemoveSource: {
        const auto* p = c->value_as_RemoveSource();
        ch.kind = ChangeIn::Kind::RemoveSource;
        ch.path = p->path()->str();
        ch.base_hash = p->base_hash()->str();
        break;
      }
      default:
        throw Failed(FailureKind::invalid_request, "unknown or missing document change variant");
    }
    out.push_back(std::move(ch));
  }
  return out;
}

void require_mutation(std::optional<uint64_t> base_rev, const flatbuffers::String* op_id) {
  if (!base_rev) throw Failed(FailureKind::invalid_request, "base_rev is required");
  if (!crypto::is_uuid(op_id->str())) throw Failed(FailureKind::invalid_request, "op_id must be a UUID");
}

}  // namespace

void Service::save(RequestIn& req) {
  const auto* r = req.request()->value_as_ProjectSaveRequest();
  std::string op_id = r->op_id()->str();
  std::string digest = request_digest(req.request());
  if (replay_or_join(req, op_id, "save", digest)) return;
  try {
    require_mutation(r->base_rev(), r->op_id());
    auto changes = decode_changes(r);
    auto base = store_->current();
    if (*r->base_rev() != base->rev) {
      FailureData f = FailureData::make(FailureKind::revision_conflict, "the document changed since base_rev");
      f.current_rev = base->rev;
      throw Failed(f);
    }
    auto staged = store_->stage(changes, op_id, r->origin()->str());
    if (staged.effective && execution_fingerprint(*staged.doc) != base->execution_fingerprint) {
      if (auto active = scheduler_->active_status())
        throw Failed(FailureKind::busy, "a run is in progress; computational changes are rejected until it ends",
                     {{"run_id", *active->run_id}});
    }
    std::string receipt;
    store_->commit(staged, "save", digest, [&](const Delta* d) {
      FBB b(1 << 14);
      auto sr = save_result(b, d, *base);
      receipt = finish_receipt(b, op_id, R::ReceiptValue::SaveResult, sr.Union());
      return receipt;
    });
    reply(req, receipt_reply(req.request_id, receipt, false));
    if (staged.delta.doc) publish_delta(staged.delta);
    finish_op(op_id, receipt);
  } catch (const Failed& f) {
    store_failure_receipt(op_id, "save", digest, f.failure);
    throw;
  }
}

void Service::reload(RequestIn& req) {
  const auto* r = req.request()->value_as_ProjectReloadRequest();
  std::string op_id = r->op_id()->str();
  std::string digest = request_digest(req.request());
  if (replay_or_join(req, op_id, "reload", digest)) return;
  try {
    require_mutation(r->base_rev(), r->op_id());
    auto base = store_->current();
    if (*r->base_rev() != base->rev) {
      FailureData f = FailureData::make(FailureKind::revision_conflict, "the document changed since base_rev");
      f.current_rev = base->rev;
      throw Failed(f);
    }
    if (auto active = scheduler_->active_status())
      throw Failed(FailureKind::busy, "a run is in progress", {{"run_id", *active->run_id}});
    auto staged = store_->stage_reload(op_id, r->origin()->str());
    std::string receipt;
    store_->commit(staged, "reload", digest, [&](const Delta* d) {
      FBB b(1 << 14);
      auto sr = save_result(b, d, *base);
      receipt = finish_receipt(b, op_id, R::ReceiptValue::SaveResult, sr.Union());
      return receipt;
    });
    reply(req, receipt_reply(req.request_id, receipt, false));
    if (staged.delta.doc) publish_delta(staged.delta);
    finish_op(op_id, receipt);
  } catch (const Failed& f) {
    store_failure_receipt(op_id, "reload", digest, f.failure);
    throw;
  }
}

void Service::import_definitions(RequestIn& req) {
  const auto* r = req.request()->value_as_ProjectImportRequest();
  std::string op_id = r->op_id()->str();
  std::string digest = request_digest(req.request());
  if (replay_or_join(req, op_id, "import", digest)) return;
  try {
    require_mutation(r->base_rev(), r->op_id());
    auto base = store_->current();
    if (*r->base_rev() != base->rev) {
      FailureData f = FailureData::make(FailureKind::revision_conflict, "the document changed since base_rev");
      f.current_rev = base->rev;
      throw Failed(f);
    }
    // Grants are registered by the launcher in a file that RPC cannot change.
    std::string grant = r->source_grant()->str();
    auto grants_text = fs::read_file(opts_.grants_file);
    if (!grants_text) throw Failed(FailureKind::permission_denied, "no import grants are registered");
    schema::Grants grants;
    if (schema::decode(*grants_text, grants))
      throw Failed(FailureKind::permission_denied, "the import grant registry is unreadable");
    const std::string* granted = nullptr;
    for (const auto& [id, path] : grants.grants->entries())
      if (id == grant) granted = &path;
    if (!granted) throw Failed(FailureKind::permission_denied, "unknown or revoked source grant");
    stdfs::path src_root = *granted;
    std::shared_ptr<Document> src;
    try {
      src = load_from_disk(src_root);
    } catch (const DocumentError& e) {
      throw Failed(FailureKind::invalid_document, std::string("import source: ") + e.what());
    }
    std::vector<std::string> wanted;
    if (r->definitions() && r->definitions()->size() > 0) {
      for (const auto* d : *r->definitions()) wanted.push_back(d->str());
    } else {
      for (const auto& [id, _] : src->definitions) wanted.push_back(id);
    }
    std::vector<ChangeIn> changes;
    std::vector<std::pair<std::string, std::string>> id_map;
    std::set<std::string> taken;
    for (const auto& [id, _] : base->definitions) taken.insert(id);
    for (const auto& id : wanted) {
      auto it = src->definitions.find(id);
      if (it == src->definitions.end())
        throw Failed(FailureKind::not_found, "definition " + id + " is not in the source", {{"definition", id}});
      std::string to = id;
      for (int k = 2; taken.count(to); ++k) to = id + "-" + std::to_string(k);
      taken.insert(to);
      id_map.emplace_back(id, to);
      ChangeIn pd;
      pd.kind = ChangeIn::Kind::PutDefinition;
      pd.id = to;
      pd.json = it->second.card_json;
      pd.meta_json = it->second.meta_json;
      changes.push_back(std::move(pd));
      std::string prefix = "definitions/" + id + "/";
      for (const auto& [path, entry] : src->sources) {
        if (!path.starts_with(prefix)) continue;
        // Content fixed at acceptance; hashed again so the copy matches the manifest.
        auto data = fs::read_file_or_throw(src_root / path);
        if (crypto::sha256_hex(data) != entry.hash)
          throw Failed(FailureKind::external_change, path + " changed while importing", {{"path", path}});
        ChangeIn ps;
        ps.kind = ChangeIn::Kind::PutSource;
        ps.path = "definitions/" + to + "/" + path.substr(prefix.size());
        ps.bytes = std::move(data);
        changes.push_back(std::move(ps));
      }
    }
    if (changes.empty()) throw Failed(FailureKind::invalid_request, "nothing to import");
    auto staged = store_->stage(changes, op_id, r->origin()->str());
    std::string receipt;
    store_->commit(staged, "import", digest, [&](const Delta* d) {
      FBB b(1 << 14);
      auto sr = save_result(b, d, *base);
      std::vector<flatbuffers::Offset<R::IdMapping>> maps;
      for (const auto& [from, to] : id_map)
        maps.push_back(R::CreateIdMapping(b, b.CreateString(from), b.CreateString(to)));
      auto ir = R::CreateImportResult(b, sr, b.CreateVector(maps));
      receipt = finish_receipt(b, op_id, R::ReceiptValue::ImportResult, ir.Union());
      return receipt;
    });
    reply(req, receipt_reply(req.request_id, receipt, false));
    if (staged.delta.doc) publish_delta(staged.delta);
    finish_op(op_id, receipt);
  } catch (const Failed& f) {
    store_failure_receipt(op_id, "import", digest, f.failure);
    throw;
  }
}

void Service::cache_clear(RequestIn& req) {
  const auto* r = req.request()->value_as_ProjectCacheClearRequest();
  std::string op_id = r->op_id()->str();
  std::string digest = request_digest(req.request());
  if (replay_or_join(req, op_id, "cache_clear", digest)) return;
  try {
    if (scheduler_->active_status() || nix_->sdk_jobs_active())
      throw Failed(FailureKind::busy, "cache can only be cleared while no run or SDK job is active");
    auto result = nix_->clear_cache();
    FBB b;
    std::vector<flatbuffers::Offset<flatbuffers::String>> del, ret;
    for (const auto& d : result.deleted) del.push_back(b.CreateString(d));
    for (const auto& d : result.retained) ret.push_back(b.CreateString(d));
    auto cr = R::CreateCacheClearResult(b, b.CreateVector(del), b.CreateVector(ret), result.freed_bytes);
    std::string receipt = finish_receipt(b, op_id, R::ReceiptValue::CacheClearResult, cr.Union());
    history_->write_sync([&](sql::Db& db) { History::put_receipt(db, op_id, "cache_clear", digest, receipt); });
    reply(req, receipt_reply(req.request_id, receipt, false));
    finish_op(op_id, receipt);
  } catch (const Failed& f) {
    store_failure_receipt(op_id, "cache_clear", digest, f.failure);
    throw;
  }
}

// ---------------------------------------------------------------------------
// Read pool: documents, files, views

void Service::close_project(RequestIn& req) {
  const auto* r = req.request()->value_as_ProjectCloseRequest();
  {
    std::lock_guard<std::mutex> l(subs_mu_);
    auto it = subscriptions_.find(req.conn);
    if (it != subscriptions_.end() && it->second == r->subscription_id()->str()) subscriptions_.erase(it);
  }
  FBB b;
  auto ack = R::CreateAck(b);
  server_->unsubscribe(req.conn, r->subscription_id()->str(),
                       wire::reply(b, req.request_id, R::ReplyPayload::Ack, ack.Union()));
}

void Service::inspect(RequestIn& req) {
  auto doc = store_->current();
  reply(req, snapshot_reply(req.request_id, *doc, std::nullopt, false, ""));
}

void Service::validate(RequestIn& req) {
  const auto* r = req.request()->value_as_ProjectValidateRequest();
  auto doc = store_->current();
  check_at_rev(r->at_rev(), *doc);
  auto mode = r->mode();
  if (mode == D::ValidateMode::unspecified) mode = D::ValidateMode::executable;
  else if (mode != D::ValidateMode::structure && mode != D::ValidateMode::executable)
    throw Failed(FailureKind::invalid_request, "unknown validate mode");
  bool exec = mode == D::ValidateMode::executable;
  std::vector<Diag> diags;
  if (!r->targets() || r->targets()->size() == 0) {
    diags = doc->resolution->all_diagnostics(exec);
  } else {
    diags = doc->resolution->diags;
    for (const auto* t : *r->targets()) {
      auto p = wire::card_arg(t, "targets");
      const auto* n = doc->resolution->find(p);
      if (!n) throw Failed(FailureKind::not_found, "unknown card " + path_key(p), {{"card", path_key(p)}});
      diags.insert(diags.end(), n->diags.begin(), n->diags.end());
      if (exec) diags.insert(diags.end(), n->exec_diags.begin(), n->exec_diags.end());
    }
  }
  bool valid = std::none_of(diags.begin(), diags.end(), [](const Diag& d) { return d.error; });
  FBB b;
  auto dv = wire::diagnostics(b, diags);
  R::ValidationResultBuilder vb(b);
  vb.add_rev(doc->rev);
  vb.add_valid(valid);
  vb.add_diagnostics(dv);
  auto off = vb.Finish();
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::ValidationResult, off.Union()));
}

void Service::operation_get(RequestIn& req) {
  const auto* r = req.request()->value_as_OperationGetRequest();
  std::string op_id = r->op_id()->str();
  if (!crypto::is_uuid(op_id)) throw Failed(FailureKind::invalid_request, "op_id must be a UUID");
  std::optional<std::string> bytes;
  if (auto rc = store_->receipt(op_id)) bytes = rc->bytes;
  if (!bytes) {
    auto row = history_->read([&](sql::Db& db) { return History::receipt(db, op_id); });
    if (row) bytes = row->bytes;
  }
  bool pending = false;
  if (!bytes) {
    std::lock_guard<std::mutex> l(ops_mu_);
    pending = pending_ops_.count(op_id) > 0;
  }
  FBB b;
  flatbuffers::Offset<R::OperationReceipt> rc;
  if (bytes) {
    rc = flatbuffers::Offset<R::OperationReceipt>(
        wire::copy_table(b, "Tsugumi.RPC.OperationReceipt",
                         reinterpret_cast<const flatbuffers::Table*>(
                             flatbuffers::GetRoot<R::OperationReceipt>(bytes->data())))
            .o);
  }
  R::OperationResultBuilder ob(b);
  ob.add_state(bytes ? D::OperationState::completed : pending ? D::OperationState::pending : D::OperationState::unknown);
  if (bytes) ob.add_receipt(rc);
  auto off = ob.Finish();
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::OperationResult, off.Union()));
}

namespace {

void check_path_arg(const std::string& p, bool allow_root) {
  if (!fs::valid_relative_path(p, allow_root))
    throw Failed(FailureKind::invalid_path, "invalid path " + p, {{"path", p}});
}

uint32_t chunk_limit(std::optional<uint32_t> v, uint32_t def, uint32_t lo, uint32_t hi) {
  uint32_t x = v.value_or(def);
  if (x < lo || x > hi)
    throw Failed(FailureKind::invalid_request,
                 "limit must be between " + std::to_string(lo) + " and " + std::to_string(hi));
  return x;
}

std::string file_content_reply(uint64_t rid, const std::string& name, const std::string& data,
                               uint64_t offset, uint32_t limit) {
  if (offset > data.size()) throw Failed(FailureKind::invalid_range, "offset is past the end");
  uint64_t n = std::min<uint64_t>(limit, data.size() - offset);
  FBB b(n + 1024);
  auto nm = b.CreateString(name);
  auto h = b.CreateString(crypto::sha256_hex(data));
  auto bytes = b.CreateVector(reinterpret_cast<const uint8_t*>(data.data() + offset), n);
  R::FileContentBuilder fb(b);
  fb.add_name(nm);
  fb.add_hash(h);
  fb.add_size_bytes(data.size());
  fb.add_offset(offset);
  fb.add_next_offset(offset + n);
  fb.add_bytes(bytes);
  fb.add_eof(offset + n == data.size());
  auto off = fb.Finish();
  return wire::reply(b, rid, R::ReplyPayload::FileContent, off.Union());
}

}  // namespace

void Service::file_list(RequestIn& req) {
  const auto* r = req.request()->value_as_ProjectFileListRequest();
  auto doc = store_->current();
  check_at_rev(r->at_rev(), *doc);
  std::string path = r->path()->str();
  check_path_arg(path, true);
  uint32_t depth = r->depth().value_or(0);
  if (depth > 8) throw Failed(FailureKind::invalid_request, "depth must be 0..8");
  bool dirs = r->include_directories().value_or(false);
  uint32_t limit = chunk_limit(r->limit(), 100, 1, 500);
  // Entries visible through File APIs: the confirmed sources and document files.
  std::set<std::string> files;
  for (const auto& [p, _] : doc->sources) files.insert(p);
  files.insert("tsugumi.json");
  files.insert("layout.json");
  for (const auto& [id, d] : doc->definitions) {
    files.insert("definitions/" + id + "/_card.json");
    if (d.meta_json) files.insert("definitions/" + id + "/_meta.json");
  }
  std::string prefix = path == "." ? "" : path + "/";
  std::map<std::string, bool> entries;  // path -> is_directory
  for (const auto& f : files) {
    if (!prefix.empty() && !f.starts_with(prefix)) continue;
    std::string rest = f.substr(prefix.size());
    size_t level = 0, pos = 0;
    while (true) {
      size_t slash = rest.find('/', pos);
      if (slash == std::string::npos) {
        if (level <= depth) entries[f] = false;
        break;
      }
      if (dirs && level <= depth) entries[prefix + rest.substr(0, slash)] = true;
      ++level;
      pos = slash + 1;
      if (level > depth) break;
    }
  }
  if (!prefix.empty() && entries.empty() && !files.count(path))
    throw Failed(FailureKind::not_found, path + " does not exist", {{"path", path}});
  std::string qd = crypto::sha256_hex("list|" + path + "|" + std::to_string(depth) + "|" + (dirs ? "1" : "0") +
                                      "|" + std::to_string(doc->rev));
  std::string after;
  if (r->cursor()) after = history_->open_cursor(r->cursor()->str(), qd).second;
  FBB b;
  std::vector<flatbuffers::Offset<R::SourceInfo>> out;
  std::string last;
  bool more = false;
  size_t bytes = 0;
  for (auto it = entries.upper_bound(after); it != entries.end(); ++it) {
    if (out.size() >= limit || bytes > (1u << 20) - 4096) {
      more = true;
      break;
    }
    const auto& [p, is_dir] = *it;
    std::optional<std::string> hash;
    std::optional<uint64_t> size;
    if (!is_dir) {
      if (auto s = doc->sources.find(p); s != doc->sources.end()) {
        hash = s->second.hash;
        size = s->second.size;
      } else if (p == "tsugumi.json") {
        hash = crypto::sha256_hex(doc->project_json);
        size = doc->project_json.size();
      } else if (p == "layout.json") {
        hash = crypto::sha256_hex(doc->layout_json);
        size = doc->layout_json.size();
      } else {
        auto id = p.substr(12, p.find('/', 12) - 12);
        const auto& d = doc->definitions.at(id);
        const std::string& text = p.ends_with("_card.json") ? d.card_json : *d.meta_json;
        hash = crypto::sha256_hex(text);
        size = text.size();
      }
    }
    out.push_back(wire::source_info(b, p, is_dir ? D::SourceKind::directory : D::SourceKind::file, true, hash, size));
    bytes += p.size() + 120;
    last = p;
  }
  auto ev = b.CreateVector(out);
  auto cur = more ? b.CreateString(history_->make_cursor(qd, 0, last)) : flatbuffers::Offset<flatbuffers::String>();
  R::SourcePageBuilder pb(b);
  pb.add_rev(doc->rev);
  pb.add_entries(ev);
  if (more) pb.add_next_cursor(cur);
  auto off = pb.Finish();
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::SourcePage, off.Union()));
}

namespace {

// Returns the confirmed bytes of a document or source file, or nullopt.
std::optional<std::string> confirmed_bytes(const Document& doc, const Store& store, const std::string& p) {
  if (p == "tsugumi.json") return doc.project_json;
  if (p == "layout.json") return doc.layout_json;
  if (Document::is_document_file(p)) {
    auto id = p.substr(12, p.find('/', 12) - 12);
    auto it = doc.definitions.find(id);
    if (it == doc.definitions.end()) return std::nullopt;
    if (p.ends_with("_card.json")) return it->second.card_json;
    return it->second.meta_json;
  }
  if (!doc.sources.count(p)) return std::nullopt;
  return store.read_source(doc, p);
}

}  // namespace

void Service::file_stat(RequestIn& req) {
  const auto* r = req.request()->value_as_ProjectFileStatRequest();
  auto doc = store_->current();
  check_at_rev(r->at_rev(), *doc);
  std::string path = r->path()->str();
  check_path_arg(path, true);
  FBB b;
  flatbuffers::Offset<R::SourceInfo> info;
  bool is_dir = path == ".";
  if (!is_dir) {
    std::string prefix = path + "/";
    for (const auto& [p, _] : doc->sources)
      if (p.starts_with(prefix)) is_dir = true;
    if (path == "definitions" || (path.starts_with("definitions/") &&
                                  doc->definitions.count(path.substr(12))))
      is_dir = true;
  }
  if (is_dir) {
    info = wire::source_info(b, path, D::SourceKind::directory, true, std::nullopt, std::nullopt);
  } else if (auto bytes = confirmed_bytes(*doc, *store_, path)) {
    info = wire::source_info(b, path, D::SourceKind::file, true, crypto::sha256_hex(*bytes), bytes->size());
  } else if (auto sdk = nix_->sdk_file(path)) {
    info = wire::source_info(b, path, D::SourceKind::file, true, crypto::sha256_hex(*sdk), sdk->size());
  } else {
    info = wire::source_info(b, path, D::SourceKind::file, false, std::nullopt, std::nullopt);
  }
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::SourceInfo, info.Union()));
}

void Service::file_read(RequestIn& req) {
  const auto* r = req.request()->value_as_ProjectFileReadRequest();
  auto doc = store_->current();
  check_at_rev(r->at_rev(), *doc);
  std::string path = r->path()->str();
  check_path_arg(path, false);
  uint32_t limit = chunk_limit(r->limit_bytes(), 262144, 1, 1u << 20);
  std::optional<std::string> bytes = confirmed_bytes(*doc, *store_, path);
  if (!bytes) bytes = nix_->sdk_file(path, doc->rev);  // throws when the SDK rev is stale
  if (!bytes) throw Failed(FailureKind::not_found, path + " does not exist", {{"path", path}});
  if (r->if_hash() && r->if_hash()->str() != crypto::sha256_hex(*bytes))
    throw Failed(FailureKind::hash_conflict, path + " does not match if_hash",
                 {{"path", path}, {"expected_hash", r->if_hash()->str()}, {"actual_hash", crypto::sha256_hex(*bytes)}});
  reply(req, file_content_reply(req.request_id, path.substr(path.rfind('/') + 1), *bytes,
                                r->offset().value_or(0), limit));
}

namespace {

// Indents compact JSON (strings are copied verbatim, so escapes survive).
std::string pretty_json(const std::string& compact) {
  std::string out;
  int depth = 0;
  bool in_str = false, esc = false;
  auto newline = [&] { out += '\n' + std::string(static_cast<size_t>(depth) * 2, ' '); };
  for (size_t i = 0; i < compact.size(); ++i) {
    char c = compact[i];
    if (in_str) {
      out += c;
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') in_str = false;
      continue;
    }
    switch (c) {
      case '"': in_str = true; out += c; break;
      case '{': case '[':
        out += c;
        if (i + 1 < compact.size() && (compact[i + 1] == '}' || compact[i + 1] == ']')) { out += compact[++i]; break; }
        ++depth;
        newline();
        break;
      case '}': case ']': --depth; newline(); out += c; break;
      case ',': out += c; newline(); break;
      case ':': out += ": "; break;
      default: out += c;
    }
  }
  return out + "\n";
}

std::optional<D::FormatLanguage> language_of(const std::string& path) {
  auto ext = stdfs::path(path).extension().string();
  if (ext == ".json") return D::FormatLanguage::json;
  if (ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".hh" || ext == ".hpp" || ext == ".h") return D::FormatLanguage::cpp;
  if (ext == ".py") return D::FormatLanguage::python;
  if (ext == ".nix") return D::FormatLanguage::nix;
  if (ext == ".sh" || ext == ".bash") return D::FormatLanguage::bash;
  if (ext == ".fbs") return D::FormatLanguage::fbs;
  return std::nullopt;
}

std::optional<std::string> find_tool(const std::string& name) {
  const char* path = std::getenv("PATH");
  if (!path) return std::nullopt;
  std::string p(path);
  for (size_t s = 0; s <= p.size();) {
    size_t e = p.find(':', s);
    auto cand = stdfs::path(p.substr(s, e == std::string::npos ? std::string::npos : e - s)) / name;
    if (!cand.parent_path().empty() && ::access(cand.c_str(), X_OK) == 0) return cand.string();
    if (e == std::string::npos) break;
    s = e + 1;
  }
  return std::nullopt;
}

// Byte offset of 1-based line/column in text.
uint64_t offset_of(const std::string& text, uint64_t line, uint64_t col) {
  uint64_t off = 0, l = 1;
  while (l < line && off < text.size()) {
    if (text[off] == '\n') ++l;
    ++off;
  }
  return std::min<uint64_t>(off + (col > 0 ? col - 1 : 0), text.size());
}

}  // namespace

// Formatting never changes the document: the text comes and goes in the
// request/reply only. Formatters are external tools of the environment.
void Service::file_format(RequestIn& req) {
  const auto* r = req.request()->value_as_ProjectFileFormatRequest();
  std::string rel = r->path()->str();
  check_path_arg(rel, false);
  auto lang = r->language() != D::FormatLanguage::unspecified ? std::optional(r->language()) : language_of(rel);
  if (!lang) throw Failed(FailureKind::unsupported, "the language of " + rel + " is not known; pass language");
  FBB b;
  std::vector<Diag> diags;
  std::string text = r->text()->str();
  auto syntax = [&](const std::string& message, std::optional<uint64_t> offset) {
    Diag d;
    d.code = "syntax_error";
    d.message = message;
    d.path = rel;
    d.offset = offset;
    diags.push_back(d);
  };
  if (*lang == D::FormatLanguage::json) {
    try {
      schema::Json j;
      j.value() = schema::capture(text);
      text = pretty_json(schema::encode(j));
    } catch (const std::exception& e) {
      syntax(e.what(), std::nullopt);
    }
  } else {
    // tool, arguments before the file, whether it rewrites the file in place
    struct Tool { const char* exe; std::vector<std::string> args; bool in_place; const char* ext; };
    Tool tool;
    switch (*lang) {
      case D::FormatLanguage::cpp:
        // clang-format when the environment has it, otherwise the much smaller astyle (the image).
        tool = {"clang-format", {"--style={BasedOnStyle: Google, ColumnLimit: 110}"}, false, ".cc"};
        if (!find_tool("clang-format"))
          tool = {"astyle", {"--style=google", "--indent=spaces=2", "--max-code-length=110", "--suffix=none", "--quiet"}, true, ".cc"};
        break;
      case D::FormatLanguage::python: tool = {"ruff", {"format", "--no-cache"}, true, ".py"}; break;
      case D::FormatLanguage::nix: tool = {"nixfmt", {}, true, ".nix"}; break;
      case D::FormatLanguage::bash: tool = {"shfmt", {"-i", "2"}, false, ".sh"}; break;
      default: throw Failed(FailureKind::unsupported, "no formatter is provided for this language");
    }
    std::string exe = find_tool(tool.exe).value_or("");
    if (exe.empty())
      throw Failed(FailureKind::unsupported, std::string("the formatter ") + tool.exe + " is not installed in this environment",
                   {{"formatter", tool.exe}});
    auto dir = store_->state_dir() / "format" / crypto::uuid4();
    auto file = dir / (std::string("source") + tool.ext);
    fs::write_atomic(file, text, 0600);
    struct Cleanup { stdfs::path d; ~Cleanup() { std::error_code ec; stdfs::remove_all(d, ec); } } cleanup{dir};
    proc::Spec spec;
    spec.argv = {exe};
    spec.argv.insert(spec.argv.end(), tool.args.begin(), tool.args.end());
    spec.argv.push_back(file.string());
    for (const char* k : {"PATH", "HOME", "LANG"})
      if (const char* v = std::getenv(k)) spec.env.push_back(std::string(k) + "=" + v);
    spec.cwd = dir.string();
    std::string err;
    std::atomic<bool> stop{false};
    auto res = proc::run(spec, &stop, std::chrono::steady_clock::now() + std::chrono::seconds(20),
                         [&](const std::string& line) { if (err.size() < 8192) err += line; });
    if (res.cancelled) throw Failed(FailureKind::cancelled, "formatting was cancelled");
    if (res.timed_out) throw Failed(FailureKind::timeout, std::string(tool.exe) + " did not finish within 20 s");
    if (!proc::exited_ok(res.status)) {
      // Syntax errors: keep the text unchanged and point at the position.
      static const std::regex pos(R"((\d+):(\d+))");
      std::smatch m;
      std::optional<uint64_t> off;
      if (std::regex_search(err, m, pos)) off = offset_of(text, std::stoull(m[1]), std::stoull(m[2]));
      std::string msg = err.empty() ? std::string(tool.exe) + " failed" : err;
      std::string shown = file.string();
      for (size_t at; (at = msg.find(shown)) != std::string::npos;) msg.replace(at, shown.size(), rel);
      syntax(msg, off);
    } else {
      text = tool.in_place ? fs::read_file_or_throw(file) : res.out;
    }
  }
  auto t = b.CreateString(text);
  auto dv = wire::diagnostics(b, diags);
  auto off = R::CreateFormatResult(b, t, dv);
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::FormatResult, off.Union()));
}

void Service::view_save(RequestIn& req) {
  const auto* r = req.request()->value_as_ViewSaveRequest();
  std::string view_id = r->view_id()->str();
  if (!crypto::is_uuid(view_id)) throw Failed(FailureKind::invalid_request, "view_id must be a UUID");
  if (!r->base_view_rev()) throw Failed(FailureKind::invalid_request, "base_view_rev is required");
  schema::ViewStateJson view = view_json(r->view());
  std::lock_guard<std::mutex> l(view_mu_);
  auto path = store_->state_dir() / "view.json";
  schema::ViewFile file;
  if (auto text = fs::read_file(path)) {
    if (schema::decode(*text, file)) file = schema::ViewFile{};
  }
  uint64_t current = 0;
  schema::ViewEntry* existing = nullptr;
  for (auto& [id, entry] : file.views->entries())
    if (id == view_id) existing = &entry;
  if (existing) current = *existing->rev;
  if (*r->base_view_rev() != current) {
    FailureData f = FailureData::make(FailureKind::revision_conflict, "the view was saved by another window");
    f.current_rev = current;
    throw Failed(f);
  }
  if (existing) {
    *existing->rev = current + 1;
    *existing->view = view;
  } else {
    schema::ViewEntry e;
    *e.rev = current + 1;
    *e.view = view;
    file.views->emplace_back(view_id, std::move(e));
  }
  fs::write_atomic(path, schema::encode(file), 0600);
  FBB b;
  auto off = R::CreateViewSaveResult(b, b.CreateString(view_id), current + 1);
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::ViewSaveResult, off.Union()));
}

void Service::view_set_visible(RequestIn& req) {
  const auto* r = req.request()->value_as_ViewSetVisibleRequest();
  std::set<std::string> cards;
  for (const auto* c : *r->cards()) cards.insert(path_key(wire::card_arg(c, "cards")));
  bool enabled = r->enabled().value_or(true);
  FBB b;
  auto ack = R::CreateAck(b);
  std::string ack_bytes = wire::reply(b, req.request_id, R::ReplyPayload::Ack, ack.Union());
  std::vector<Note> initial;
  if (enabled)
    for (const auto& key : cards)
      if (auto n = scheduler_->latest_display_note(key)) initial.push_back(std::move(*n));
  server_->set_visible(req.conn, r->subscription_id()->str(), std::move(cards), enabled,
                       std::move(ack_bytes), std::move(initial));
}

}  // namespace tsugumi
