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
  pb.add_kind(p.kind);
  pb.add_optional(p.optional);
  pb.add_execution_shape(p.execution_shape);
  if (p.type) {
    pb.add_schema_bfbs(bfbs);
    pb.add_root_type(root);
  }
  return pb.Finish();
}

// Patch only the envelope. Presentation bytes and sharing stay untouched;
// unpacking shared strings/rows can expand a small frame into gigabytes.
auto patched_display(const DisplayData& d) -> std::optional<std::string> {
  const auto* original =
      reinterpret_cast<const D::Display*>(d.bytes.data() + d.root_offset);
  if (path(original->card()) != d.card ||
      original->run_id()->string_view() != d.run_id ||
      original->invocation().value_or(0) != d.invocation)
    return std::nullopt;
  std::string bytes = d.bytes;
  auto* copy = reinterpret_cast<D::Display*>(bytes.data() + d.root_offset);
  if (copy->revision().value_or(0) != d.revision &&
      !copy->mutate_revision(d.revision))
    return std::nullopt;
  flatbuffers::WriteScalar<flatbuffers::uoffset_t>(bytes.data(), d.root_offset);
  return bytes;
}

Off<D::Display> display(FBB& b, const DisplayData& d) {
  if (const auto bytes = patched_display(d)) {
    b.PreAlign(bytes->size(), 8);
    b.PushBytes(reinterpret_cast<const uint8_t*>(bytes->data()), bytes->size());
    return Off<D::Display>(b.GetSize() - d.root_offset);
  }
  const auto* original =
      reinterpret_cast<const D::Display*>(d.bytes.data() + d.root_offset);
  b.PreAlign(d.bytes.size(), 8);
  b.PushBytes(reinterpret_cast<const uint8_t*>(d.bytes.data()), d.bytes.size());
  Off<void> value;
  if (original->value()) {
    const auto offset =
        reinterpret_cast<const char*>(original->value()) - d.bytes.data();
    value = Off<void>(b.GetSize() - offset);
  }
  // An envelope can change when its Card moves. Copy the union reference
  // without walking or interpreting the presentation it points to.
  const auto card = path(b, d.card);
  const auto run = b.CreateString(d.run_id);
  return D::CreateDisplay(b, card, run, d.invocation, d.revision,
                          original->value_type(), value);
}

DisplayData display(const D::Display* d, std::string_view buffer) {
  DisplayData out;
  out.card = path(d->card());
  out.run_id = d->run_id()->str();
  out.invocation = d->invocation().value_or(0);
  out.revision = d->revision().value_or(0);
  out.bytes = buffer;
  out.root_offset =
      static_cast<size_t>(reinterpret_cast<const char*>(d) - buffer.data());
  return out;
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

namespace wire {

std::string standalone_display(const DisplayData& d) {
  if (auto bytes = patched_display(d)) return std::move(*bytes);
  FBB b;
  b.Finish(display(b, d));
  return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()),
                     b.GetSize());
}

std::optional<DisplayData> standalone_display(const std::string& bytes) {
  flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(bytes.data()),
                          bytes.size());
  if (bytes.size() > (16u << 20) || !v.VerifyBuffer<D::Display>(nullptr))
    return std::nullopt;
  return display(flatbuffers::GetRoot<D::Display>(bytes.data()), bytes);
}

}  // namespace wire
}  // namespace tsugumi
