#include "tsugumi/workerd/value_wire.hh"

#include "tsugumi/util/text.hh"

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

Off<D::Failure> failure(FBB& b, const FailureData& f) {
  std::vector<Off<D::KeyValue>> args;
  for (const auto& [k, v] : f.args)
    args.push_back(D::CreateKeyValue(b, b.CreateString(k), b.CreateString(v)));
  auto argv = b.CreateVector(args);
  auto diag =
      f.diagnostic ? b.CreateString(*f.diagnostic) : Off<flatbuffers::String>();
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
    for (const auto* kv : *f->args())
      out.args.emplace_back(kv->key()->str(), kv->value()->str());
  if (f->diagnostic())
    out.diagnostic = text::replace_invalid_utf8(f->diagnostic()->str());
  if (f->current_rev()) out.current_rev = *f->current_rev();
  if (f->retry_after_ms()) out.retry_after_ms = *f->retry_after_ms();
  return out;
}

auto artifact_id(const ::tsugumi::ArtifactId& id) -> ArtifactKey {
  return {reinterpret_cast<const char*>(id.bytes()->data()), id.bytes()->size()};
}

auto artifact_ids(const ArtifactIdVector* ids) -> std::vector<ArtifactKey> {
  std::vector<ArtifactKey> result;
  if (ids)
    for (const auto* id : *ids) result.push_back(artifact_id(*id));
  return result;
}

auto artifact_struct(const ArtifactKey& id) -> ::tsugumi::ArtifactId {
  if (id.size() != 16)
    throw Failed(FailureKind::internal_error, "an artifact id is not 16 bytes");
  return ::tsugumi::ArtifactId(flatbuffers::span<const uint8_t, 16>(
      reinterpret_cast<const uint8_t*>(id.data()), id.size()));
}

auto artifact_ids(FBB& b, std::span<const ArtifactKey> ids) -> Off<ArtifactIdVector> {
  std::vector<::tsugumi::ArtifactId> encoded;
  encoded.reserve(ids.size());
  for (const auto& id : ids) encoded.push_back(artifact_struct(id));
  return b.CreateVectorOfStructs(encoded);
}

Off<D::Port> port(FBB& b, const types::PortType& p) {
  auto name = b.CreateString(p.name);
  Off<flatbuffers::Vector<uint8_t>> bfbs;
  Off<flatbuffers::String> root;
  if (p.type) {
    bfbs = b.CreateVector(reinterpret_cast<const uint8_t*>(p.type->bfbs.data()),
                          p.type->bfbs.size());
    root = b.CreateString(p.type->root_type);
  }
  D::PortBuilder pb(b);
  pb.add_name(name);
  pb.add_execution_shape(p.execution_shape);
  if (p.type) {
    pb.add_schema_bfbs(bfbs);
    pb.add_root_type(root);
  }
  return pb.Finish();
}

}  // namespace wire

std::string FailureData::to_bytes() const {
  wire::FBB b;
  b.Finish(wire::failure(b, *this));
  return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()),
                     b.GetSize());
}

std::optional<FailureData> FailureData::from_bytes(const std::string& bytes) {
  flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(bytes.data()),
                          bytes.size());
  if (bytes.empty() || !v.VerifyBuffer<wire::D::Failure>(nullptr))
    return std::nullopt;
  return wire::failure(flatbuffers::GetRoot<wire::D::Failure>(bytes.data()));
}
}  // namespace tsugumi
