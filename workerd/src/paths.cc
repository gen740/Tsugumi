#include <algorithm>
#include <stdexcept>

#include "tsugumi/workerd/invocation.hh"

namespace tsugumi {
namespace stdfs = std::filesystem;
auto remote::relative_path(std::string_view path) -> stdfs::path {
  if (path.empty() || path.size() > 4096 || path.front() == '/' ||
      path.back() == '/' || path.find("//") != std::string_view::npos ||
      path.find('\\') != std::string_view::npos ||
      std::ranges::any_of(path,
                          [](unsigned char c) { return c < 32 || c == 127; }))
    throw std::runtime_error("invalid remote file path");
  stdfs::path result(path);
  for (const auto& part : result)
    if (part == "." || part == "..")
      throw std::runtime_error("invalid remote file path");
  return result;
}

}  // namespace tsugumi

namespace tsugumi {

auto remote::encode_log_entry(Tsugumi::Data::LogStream stream,
                              std::string_view text) -> std::string {
  flatbuffers::FlatBufferBuilder b(text.size() + 64);
  const auto bytes = b.CreateVector(
      reinterpret_cast<const uint8_t*>(text.data()), text.size());
  b.FinishSizePrefixed(
      RW::CreateLogEntry(b, static_cast<uint8_t>(stream), bytes));
  return {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()};
}

auto remote::decode_log_entries(
    std::string_view bytes,
    const std::function<void(Tsugumi::Data::LogStream, std::string,
                             std::string_view)>& entry) -> size_t {
  size_t at = 0;
  while (bytes.size() - at >= sizeof(flatbuffers::uoffset_t)) {
    const auto* start = reinterpret_cast<const uint8_t*>(bytes.data() + at);
    const auto size = flatbuffers::ReadScalar<flatbuffers::uoffset_t>(start);
    if (size > bytes.size() - at - sizeof(flatbuffers::uoffset_t)) break;
    const auto total = sizeof(flatbuffers::uoffset_t) + size;
    flatbuffers::Verifier::Options options;
    options.check_alignment = false;
    flatbuffers::Verifier v(start, total, options);
    if (!v.VerifySizePrefixedBuffer<RW::LogEntry>(nullptr))
      throw std::runtime_error("invalid remote log entry");
    const auto* log = flatbuffers::GetSizePrefixedRoot<RW::LogEntry>(start);
    const auto stream = log->stream();
    if (stream <= static_cast<uint8_t>(Tsugumi::Data::LogStream::unspecified) ||
        stream > static_cast<uint8_t>(Tsugumi::Data::LogStream::MAX))
      throw std::runtime_error("invalid remote log entry");
    std::string text;
    if (log->text())
      text.assign(reinterpret_cast<const char*>(log->text()->data()),
                  log->text()->size());
    entry(static_cast<Tsugumi::Data::LogStream>(stream), std::move(text),
          bytes.substr(at, total));
    at += total;
  }
  return at;
}

}  // namespace tsugumi
