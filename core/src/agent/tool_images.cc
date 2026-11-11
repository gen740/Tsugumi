#include "agent/tool_images.hh"

#include <array>
#include <optional>
#include <string>
#include <utility>

#include "agent/json.hh"
#include "schema.hh"
#include "util/crypto.hh"

namespace tsugumi::agent {
namespace {

auto Byte(std::string_view bytes, size_t at) -> uint8_t {
  return static_cast<uint8_t>(bytes[at]);
}

auto Big16(std::string_view bytes, size_t at) -> uint16_t {
  return static_cast<uint16_t>((Byte(bytes, at) << 8) | Byte(bytes, at + 1));
}

auto Big32(std::string_view bytes, size_t at) -> uint32_t {
  return (static_cast<uint32_t>(Byte(bytes, at)) << 24) |
         (static_cast<uint32_t>(Byte(bytes, at + 1)) << 16) |
         (static_cast<uint32_t>(Byte(bytes, at + 2)) << 8) |
         Byte(bytes, at + 3);
}

enum class Kind { None, PNG, JPEG };

auto ImageKind(std::string_view bytes) -> Kind {
  if (bytes.starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8)))
    return Kind::PNG;
  if (bytes.size() >= 3 && Byte(bytes, 0) == 0xff && Byte(bytes, 1) == 0xd8 &&
      Byte(bytes, 2) == 0xff)
    return Kind::JPEG;
  return Kind::None;
}

struct Dimensions {
  uint32_t width = 0;
  uint32_t height = 0;
};

auto PNG(std::string_view bytes) -> std::optional<Dimensions> {
  Dimensions dimensions;
  bool data = false;
  size_t at = 8;
  while (at <= bytes.size() && bytes.size() - at >= 12) {
    const auto length = Big32(bytes, at);
    if (length > bytes.size() - at - 12) return std::nullopt;
    const auto type = bytes.substr(at + 4, 4);
    if (crypto::crc32(bytes.data() + at + 4, size_t{length} + 4) !=
        Big32(bytes, at + 8 + length))
      return std::nullopt;
    if (at == 8) {
      if (type != "IHDR" || length != 13) return std::nullopt;
      dimensions = {Big32(bytes, at + 8), Big32(bytes, at + 12)};
      if (!dimensions.width || !dimensions.height) return std::nullopt;
    } else if (type == "IHDR") {
      return std::nullopt;
    }
    if (type == "IDAT") data = true;
    at += size_t{length} + 12;
    if (type == "IEND") {
      return length == 0 && data && at == bytes.size()
                 ? std::optional(dimensions)
                 : std::nullopt;
    }
  }
  return std::nullopt;
}

auto JPEG(std::string_view bytes) -> std::optional<Dimensions> {
  if (bytes.size() < 4 || Byte(bytes, bytes.size() - 2) != 0xff ||
      Byte(bytes, bytes.size() - 1) != 0xd9)
    return std::nullopt;
  Dimensions dimensions;
  size_t at = 2;
  while (at < bytes.size()) {
    if (Byte(bytes, at++) != 0xff) return std::nullopt;
    while (at < bytes.size() && Byte(bytes, at) == 0xff) ++at;
    if (at == bytes.size()) return std::nullopt;
    const auto marker = Byte(bytes, at++);
    if (marker == 0xd9) return std::nullopt;
    if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
    if (bytes.size() - at < 2) return std::nullopt;
    const auto length = Big16(bytes, at);
    if (length < 2 || length > bytes.size() - at) return std::nullopt;
    const bool frame = marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 &&
                       marker != 0xc8 && marker != 0xcc;
    if (frame) {
      if (length < 8) return std::nullopt;
      dimensions = {Big16(bytes, at + 5), Big16(bytes, at + 3)};
      if (!dimensions.width || !dimensions.height) return std::nullopt;
    }
    if (marker == 0xda) {
      return dimensions.width && dimensions.height &&
                     at + length < bytes.size() - 2
                 ? std::optional(dimensions)
                 : std::nullopt;
    }
    at += length;
  }
  return std::nullopt;
}

struct Extractor {
  ImageLimits limits;
  ImageResult result;
  size_t encoded = 0;
  bool changed = false;

  auto Extract(std::string_view bytes, std::string base64 = {})
      -> std::optional<std::string> {
    const auto kind = ImageKind(bytes);
    if (kind == Kind::None) return std::nullopt;
    changed = true;
    const std::string name = kind == Kind::PNG ? "PNG" : "JPEG";
    const auto about =
        name + " image, " + std::to_string(bytes.size()) + " bytes";
    const auto dimensions = kind == Kind::PNG ? PNG(bytes) : JPEG(bytes);
    if (!dimensions)
      return json::Quote(about + ": incomplete or invalid image, not shown");
    if (dimensions->width > limits.dimension ||
        dimensions->height > limits.dimension ||
        uint64_t{dimensions->width} * dimensions->height > limits.pixels) {
      return json::Quote(
          about + ": dimensions exceed the model image limit, not shown");
    }
    if (base64.empty()) base64 = crypto::base64(bytes);
    if (base64.size() > limits.encoded_bytes ||
        base64.size() > limits.total_encoded_bytes - encoded ||
        result.images.size() >= limits.images)
      return json::Quote(about + ": too large to show");
    encoded += base64.size();
    result.images.push_back(
        {kind == Kind::PNG ? "image/png" : "image/jpeg", std::move(base64)});
    return json::Quote(about + ": shown with this result");
  }

  auto Walk(std::string_view raw, std::string_view field = {}, size_t depth = 0)
      -> std::string {
    if (depth > 64)
      throw std::runtime_error("tool image result exceeds nesting limit");
    const auto value = schema::capture(raw);
    const auto source = value.source();
    if (source.starts_with('{')) {
      auto object = json::Object(source);
      if (object.size() == 1 && object.contains("base64")) {
        const auto base64 = json::Text(object, "base64");
        if (base64.starts_with("iVBORw0KGgo") || base64.starts_with("/9j/")) {
          if (base64.size() > limits.encoded_bytes) {
            changed = true;
            return json::Quote("Image exceeds the model size limit, not shown");
          }
          std::string bytes;
          if (!crypto::base64_decode(base64, bytes)) {
            changed = true;
            return json::Quote("Invalid base64 image, not shown");
          }
          if (auto replacement = Extract(bytes, base64)) return *replacement;
        }
      }
      for (auto& [key, item] : object) item = Walk(item, key, depth + 1);
      return json::Encode(object);
    }
    if (source.starts_with('[')) {
      if (field == "bytes" || field == "data" || field == "inline_bytes") {
        schema::v::Array<schema::v::Uint8> bytes;
        if (!value[""].decode_into(bytes)) {
          const auto& data = bytes.value();
          const auto view = std::string_view(
              reinterpret_cast<const char*>(data.data()), data.size());
          if (auto replacement = Extract(view)) return *replacement;
          return std::string(raw);
        }
      }
      auto array = json::Array(source);
      for (auto& item : array) item = Walk(item, {}, depth + 1);
      return json::Encode(array);
    }
    return std::string(raw);
  }
};

}  // namespace

auto SplitToolImages(std::string_view result, ImageLimits limits)
    -> ImageResult {
  Extractor extractor{limits, {}, 0, false};
  try {
    auto text = extractor.Walk(result);
    extractor.result.text =
        extractor.changed ? std::move(text) : std::string(result);
    return std::move(extractor.result);
  } catch (const std::exception&) {
    // An unrecognized result remains readable, but is never offered as an
    // image. Project RPC validation remains the source of truth for its type.
    return {std::string(result), {}};
  }
}

}  // namespace tsugumi::agent
