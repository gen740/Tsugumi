#include "wire.hh"

#include <flatbuffers/reflection.h>

#include <cmath>

extern const unsigned char tsugumi_rpc_bfbs[];
extern const unsigned long tsugumi_rpc_bfbs_size;

namespace tsugumi {
namespace wire {

Off<D::CardPath> path(FBB& b, const CardPath& p) {
  std::vector<Off<flatbuffers::String>> segs;
  segs.reserve(p.size());
  for (const auto& s : p) segs.push_back(b.CreateString(s));
  return D::CreateCardPath(b, b.CreateVector(segs));
}

CardPath path(const D::CardPath* p) {
  CardPath out;
  if (!p || !p->segments()) return out;
  for (const auto* s : *p->segments()) out.push_back(s->str());
  return out;
}

CardPath card_arg(const D::CardPath* p, const char* field) {
  if (!p) throw Failed(FailureKind::invalid_request, std::string(field) + " is required");
  auto out = path(p);
  if (out.empty())
    throw Failed(FailureKind::invalid_request, std::string(field) + " must not be empty");
  for (const auto& s : out)
    if (!valid_id(s))
      throw Failed(FailureKind::invalid_request,
                   std::string(field) + " has an invalid segment \"" + s + "\"");
  return out;
}

Off<D::Failure> failure(FBB& b, const FailureData& f) {
  std::vector<Off<D::KeyValue>> args;
  for (const auto& [k, v] : f.args)
    args.push_back(D::CreateKeyValue(b, b.CreateString(k), b.CreateString(v)));
  auto argv = b.CreateVector(args);
  auto diag = f.diagnostic ? b.CreateString(*f.diagnostic) : Off<flatbuffers::String>();
  D::FailureBuilder fb(b);
  fb.add_kind(f.kind);
  fb.add_args(argv);
  if (f.diagnostic) fb.add_diagnostic(diag);
  if (f.current_rev) fb.add_current_rev(*f.current_rev);
  if (f.retry_after_ms) fb.add_retry_after_ms(*f.retry_after_ms);
  return fb.Finish();
}

FailureData failure(const D::Failure* f) {
  FailureData out;
  out.kind = f->kind();
  if (f->args())
    for (const auto* kv : *f->args()) out.args.emplace_back(kv->key()->str(), kv->value()->str());
  if (f->diagnostic()) out.diagnostic = f->diagnostic()->str();
  if (f->current_rev()) out.current_rev = *f->current_rev();
  if (f->retry_after_ms()) out.retry_after_ms = *f->retry_after_ms();
  return out;
}

Off<D::Diagnostic> diagnostic(FBB& b, const Diag& d) {
  auto code = b.CreateString(d.code);
  auto msg = b.CreateString(d.message);
  auto card = d.card ? path(b, *d.card) : Off<D::CardPath>();
  auto p = d.path ? b.CreateString(*d.path) : Off<flatbuffers::String>();
  D::DiagnosticBuilder db(b);
  db.add_severity(d.error ? D::Severity::error : D::Severity::warning);
  db.add_code(code);
  db.add_message(msg);
  if (d.card) db.add_card(card);
  if (d.path) db.add_path(p);
  if (d.offset) db.add_offset(*d.offset);
  return db.Finish();
}

Off<flatbuffers::Vector<Off<D::Diagnostic>>> diagnostics(FBB& b, const std::vector<Diag>& d) {
  std::vector<Off<D::Diagnostic>> v;
  v.reserve(d.size());
  for (const auto& x : d) v.push_back(diagnostic(b, x));
  return b.CreateVector(v);
}

Off<D::Port> port(FBB& b, const types::PortType& p) {
  auto name = b.CreateString(p.name);
  Off<flatbuffers::Vector<uint8_t>> bfbs;
  Off<flatbuffers::String> root;
  if (p.type) {
    bfbs = b.CreateVector(reinterpret_cast<const uint8_t*>(p.type->bfbs.data()), p.type->bfbs.size());
    root = b.CreateString(p.type->root_type);
  }
  D::PortBuilder pb(b);
  pb.add_name(name);
  pb.add_kind(p.kind);
  pb.add_optional(p.optional);
  pb.add_execution_shape(p.execution_shape);
  if (p.type) {
    pb.add_schema_bfbs(bfbs);
    pb.add_root_type(root);
  }
  return pb.Finish();
}

Off<R::ResolvedCard> resolved(FBB& b, const Node& n) {
  std::vector<Off<D::Port>> ins;
  std::vector<Off<D::Port>> outs;
  for (const auto& p : n.inputs) ins.push_back(port(b, p));
  for (const auto& p : n.outputs) outs.push_back(port(b, p));
  auto diags = n.diags;
  diags.insert(diags.end(), n.exec_diags.begin(), n.exec_diags.end());
  for (auto& d : diags)
    if (!d.card) d.card = n.path;
  std::vector<Off<R::CardNixReference>> refs;
  for (const auto& ref : n.nix_references)
    refs.push_back(R::CreateCardNixReference(b, path(b, ref.target), b.CreateString(ref.name), ref.valid));
  return R::CreateResolvedCard(b, path(b, n.path), b.CreateVector(ins), b.CreateVector(outs),
                               diagnostics(b, diags), b.CreateVector(refs), b.CreateString(n.name));
}

Off<flatbuffers::Vector<Off<R::ResolvedCard>>> resolution(FBB& b, const Resolution& r) {
  std::vector<Off<R::ResolvedCard>> v;
  v.reserve(r.nodes.size());
  for (const auto& n : r.nodes) v.push_back(resolved(b, n));
  return b.CreateVector(v);
}

Off<R::SourceInfo> source_info(FBB& b, const std::string& p, D::SourceKind kind, bool exists,
                               const std::optional<std::string>& hash,
                               std::optional<uint64_t> size) {
  auto ps = b.CreateString(p);
  auto hs = hash ? b.CreateString(*hash) : Off<flatbuffers::String>();
  R::SourceInfoBuilder sb(b);
  sb.add_path(ps);
  sb.add_kind(kind);
  sb.add_exists(exists);
  if (hash) sb.add_hash(hs);
  if (size) sb.add_size_bytes(*size);
  return sb.Finish();
}

Off<R::Definition> definition(FBB& b, const std::string& id, const DefinitionDoc& d) {
  return R::CreateDefinition(b, b.CreateString(id), b.CreateString(d.card_json),
                             d.meta_json ? b.CreateString(*d.meta_json) : Off<flatbuffers::String>(),
                             d.link_json ? b.CreateString(*d.link_json) : Off<flatbuffers::String>());
}

Off<R::RunStatus> run_status(FBB& b, const RunStatusData& s) {
  auto id = s.run_id ? b.CreateString(*s.run_id) : Off<flatbuffers::String>();
  auto f = s.failure ? failure(b, *s.failure) : Off<D::Failure>();
  auto commit = s.commit ? b.CreateString(*s.commit) : Off<flatbuffers::String>();
  R::RunStatusBuilder rb(b);
  if (s.run_id) rb.add_run_id(id);
  rb.add_state(s.state);
  if (s.rev) rb.add_rev(*s.rev);
  if (s.execution_rev) rb.add_execution_rev(*s.execution_rev);
  if (s.accepted_at) rb.add_accepted_at(*s.accepted_at);
  if (s.started_at) rb.add_started_at(*s.started_at);
  if (s.ended_at) rb.add_ended_at(*s.ended_at);
  if (s.failure) rb.add_failure(f);
  if (s.commit) rb.add_commit(commit);
  rb.add_partial_run(s.partial_run);
  rb.add_uses_partial_result(s.uses_partial_result);
  return rb.Finish();
}

Off<R::RunSummary> run_summary(FBB& b, const RunSummaryData& s) {
  auto st = run_status(b, s.status);
  auto h = b.CreateString(s.source_hash);
  auto t = path(b, s.target);
  R::RunSummaryBuilder rb(b);
  rb.add_status(st);
  rb.add_source_hash(h);
  rb.add_target(t);
  rb.add_propagate(s.propagate);
  return rb.Finish();
}

Off<R::InvocationSummary> invocation(FBB& b, const InvocationSummaryData& s) {
  auto run = b.CreateString(s.run_id);
  auto card = path(b, s.card);
  auto f = s.failure ? failure(b, *s.failure) : Off<D::Failure>();
  std::vector<Off<R::WaitingInput>> waits;
  for (const auto& w : s.waiting) {
    std::vector<Off<R::UpstreamPort>> ups;
    for (const auto& u : w.upstream)
      ups.push_back(R::CreateUpstreamPort(b, path(b, u.card), b.CreateString(u.port)));
    waits.push_back(R::CreateWaitingInput(b, b.CreateString(w.port), b.CreateVector(ups)));
  }
  auto wv = b.CreateVector(waits);
  std::vector<Off<R::StageTime>> stages;
  for (const auto& st : s.stages) {
    R::StageTimeBuilder sb(b);
    sb.add_phase(st.phase);
    sb.add_outcome(st.outcome);
    sb.add_start(st.start);
    if (st.end) sb.add_end(*st.end);
    stages.push_back(sb.Finish());
  }
  auto sv = b.CreateVector(stages);
  std::vector<Off<flatbuffers::String>> sent;
  for (const auto& p : s.outputs_sent) sent.push_back(b.CreateString(p));
  auto ov = b.CreateVector(sent);
  auto em = s.emulator.empty() ? Off<flatbuffers::String>() : b.CreateString(s.emulator);
  auto env = s.environment_id.empty() ? Off<flatbuffers::String>() : b.CreateString(s.environment_id);
  auto execution = s.execution_service.empty() ? Off<flatbuffers::String>() : b.CreateString(s.execution_service);
  auto job = s.execution_job_id.empty() ? Off<flatbuffers::String>() : b.CreateString(s.execution_job_id);
  auto system = s.execution_system.empty() ? Off<flatbuffers::String>() : b.CreateString(s.execution_system);
  auto resources = s.execution_resources ? R::CreateExecutionResources(b, s.execution_resources->cpu_millis,
                                                                     s.execution_resources->memory_bytes)
                                         : Off<R::ExecutionResources>();
  R::InvocationSummaryBuilder ib(b);
  ib.add_run_id(run);
  ib.add_card(card);
  ib.add_invocation(s.invocation);
  ib.add_state(s.state);
  if (s.phase) ib.add_phase(*s.phase);
  if (s.started_at) ib.add_started_at(*s.started_at);
  if (s.ended_at) ib.add_ended_at(*s.ended_at);
  if (s.duration_ms) ib.add_duration_ms(*s.duration_ms);
  if (s.failure) ib.add_failure(f);
  ib.add_waiting_inputs(wv);
  if (s.build_cached) ib.add_build_cached(*s.build_cached);
  ib.add_stages(sv);
  if (s.worker_reused) ib.add_worker_reused(*s.worker_reused);
  ib.add_outputs_sent(ov);
  if (s.ready_at) ib.add_ready_at(*s.ready_at);
  if (!s.emulator.empty()) ib.add_emulator(em);
  if (!s.environment_id.empty()) ib.add_environment_id(env);
  if (!execution.IsNull()) ib.add_execution_service(execution);
  if (!job.IsNull()) ib.add_execution_job_id(job);
  if (!system.IsNull()) ib.add_execution_system(system);
  if (!resources.IsNull()) ib.add_execution_resources(resources);
  if (s.execution_state) ib.add_execution_state(*s.execution_state);
  if (s.batch_activation) ib.add_batch_activation(*s.batch_activation);
  if (s.batch_index) ib.add_batch_index(*s.batch_index);
  if (s.batch_size) ib.add_batch_size(*s.batch_size);
  if (s.implementation_index) ib.add_implementation_index(*s.implementation_index);
  return ib.Finish();
}

auto run_batch(FBB& b, const RunBatchData& batch) -> Off<R::RunBatch> {
  std::vector<Off<R::RunBatchOutput>> outputs;
  for (const auto& output : batch.outputs)
    outputs.push_back(R::CreateRunBatchOutput(b, b.CreateString(output.port), output.count, output.execution_list));
  return R::CreateRunBatch(b, path(b, batch.card), batch.activation, batch.invocation_count, batch.execution_list,
                          batch.complete, batch.failed, b.CreateVector(outputs));
}

// Patch only the envelope. Presentation bytes and sharing stay untouched;
// unpacking shared strings/rows can expand a small frame into gigabytes.
auto patched_display(const DisplayData& d) -> std::optional<std::string> {
  const auto* original = reinterpret_cast<const D::Display*>(d.bytes.data() + d.root_offset);
  if (path(original->card()) != d.card || original->run_id()->string_view() != d.run_id ||
      original->invocation().value_or(0) != d.invocation) return std::nullopt;
  std::string bytes = d.bytes;
  auto* copy = reinterpret_cast<D::Display*>(bytes.data() + d.root_offset);
  if (copy->revision().value_or(0) != d.revision && !copy->mutate_revision(d.revision)) return std::nullopt;
  flatbuffers::WriteScalar<flatbuffers::uoffset_t>(bytes.data(), d.root_offset);
  return bytes;
}

Off<D::Display> display(FBB& b, const DisplayData& d) {
  if (const auto bytes = patched_display(d)) {
    b.PreAlign(bytes->size(), 8);
    b.PushBytes(reinterpret_cast<const uint8_t*>(bytes->data()), bytes->size());
    return Off<D::Display>(b.GetSize() - d.root_offset);
  }
  const auto* original = reinterpret_cast<const D::Display*>(d.bytes.data() + d.root_offset);
  b.PreAlign(d.bytes.size(), 8);
  b.PushBytes(reinterpret_cast<const uint8_t*>(d.bytes.data()), d.bytes.size());
  Off<void> value;
  if (original->value()) {
    const auto offset = reinterpret_cast<const char*>(original->value()) - d.bytes.data();
    value = Off<void>(b.GetSize() - offset);
  }
  // An envelope can change when its Card moves. Copy the union reference
  // without walking or interpreting the presentation it points to.
  const auto card = path(b, d.card);
  const auto run = b.CreateString(d.run_id);
  return D::CreateDisplay(b, card, run, d.invocation, d.revision, original->value_type(), value);
}

DisplayData display(const D::Display* d, std::string_view buffer) {
  DisplayData out;
  if (!d->card() || !d->run_id())
    throw Failed(FailureKind::output_invalid, "display needs card and run_id");
  out.card = path(d->card());
  out.run_id = d->run_id()->str();
  out.invocation = d->invocation().value_or(0);
  out.revision = d->revision().value_or(0);
  out.bytes = buffer;
  out.root_offset = static_cast<size_t>(reinterpret_cast<const char*>(d) - buffer.data());
  return out;
}

}  // namespace wire

std::string FailureData::to_bytes() const {
  wire::FBB b;
  b.Finish(wire::failure(b, *this));
  return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize());
}

std::optional<FailureData> FailureData::from_bytes(const std::string& bytes) {
  flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  if (bytes.empty() || !v.VerifyBuffer<wire::D::Failure>(nullptr)) return std::nullopt;
  return wire::failure(flatbuffers::GetRoot<wire::D::Failure>(bytes.data()));
}

namespace wire {

std::string reply(FBB& b, uint64_t request_id, R::ReplyPayload type, Off<void> value) {
  auto r = R::CreateReply(b, type, value);
  R::MessageBuilder mb(b);
  mb.add_request_id(request_id);
  mb.add_payload_type(R::Payload::Reply);
  mb.add_payload(r.Union());
  R::FinishMessageBuffer(b, mb.Finish());
  return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize());
}

std::string file_page(uint64_t request_id, const std::string& name, std::string_view slice, uint64_t offset,
                      uint64_t total, const std::string& hash) {
  FBB b(slice.size() + 1024);
  auto nm = b.CreateString(name);
  auto h = b.CreateString(hash);
  auto bytes = b.CreateVector(reinterpret_cast<const uint8_t*>(slice.data()), slice.size());
  R::FileContentBuilder fb(b);
  fb.add_name(nm);
  fb.add_hash(h);
  fb.add_size_bytes(total);
  fb.add_offset(offset);
  fb.add_next_offset(offset + slice.size());
  fb.add_bytes(bytes);
  fb.add_eof(offset + slice.size() == total);
  auto off = fb.Finish();
  return reply(b, request_id, R::ReplyPayload::FileContent, off.Union());
}

std::string_view page_of(std::string_view data, uint64_t offset, uint64_t limit) {
  if (offset > data.size()) throw Failed(FailureKind::invalid_range, "offset is past the end");
  return data.substr(offset, std::min<uint64_t>(limit, data.size() - offset));
}

std::string failure_reply(uint64_t request_id, const FailureData& f) {
  FBB b;
  auto off = failure(b, f);
  return reply(b, request_id, R::ReplyPayload::Failure, off.Union());
}

std::string standalone_display(const DisplayData& d) {
  if (auto bytes = patched_display(d)) return std::move(*bytes);
  FBB b;
  b.Finish(display(b, d));
  return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize());
}

std::optional<DisplayData> standalone_display(const std::string& bytes) {
  flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  if (bytes.size() > (16u << 20) || !v.VerifyBuffer<D::Display>(nullptr)) return std::nullopt;
  try {
    return display(flatbuffers::GetRoot<D::Display>(bytes.data()), bytes);
  } catch (const Failed&) {
    return std::nullopt;
  }
}

Off<void> copy_table(FBB& b, const char* type_name, const flatbuffers::Table* table) {
  const auto* schema = reflection::GetSchema(tsugumi_rpc_bfbs);
  const auto* obj = schema->objects()->LookupByKey(type_name);
  if (!obj) throw std::logic_error(std::string("unknown schema type ") + type_name);
  return flatbuffers::CopyTable(b, *schema, *obj, *table).o;
}

}  // namespace wire
}  // namespace tsugumi
