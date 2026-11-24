#include <bit>
#include <cstring>
#include <string>
#include <string_view>

namespace tsugumi::view {
namespace {

static_assert(std::endian::native == std::endian::little);

class Reader {
 public:
  Reader(Context& context, std::string_view import) : context_(context), import_(import) {}

  [[noreturn]] auto fail(std::string_view field, std::string_view reason) const -> void {
    throw TrapError(std::string(import_) + ": " + std::string(field) + ": " + std::string(reason));
  }

  auto range(uint64_t address, uint64_t size, std::string_view field) const -> const uint8_t* {
    if (address > context_.memory.size || size > context_.memory.size - address) {
      fail(field, "out of bounds");
    }
    return address == 0 ? context_.memory.base : context_.memory.base + address;
  }

  auto required(uint32_t address, uint64_t size, std::string_view field) const -> const uint8_t* {
    if (address == 0) fail(field, "null pointer");
    return range(address, size, field);
  }

  template <class T>
  auto read(uint64_t address, std::string_view field) const -> T {
    T value;
    std::memcpy(&value, range(address, sizeof(T), field), sizeof(T));
    return value;
  }

  template <class T>
  auto handle(Kind kind, uint32_t id, bool optional, std::string_view field) const -> T {
    if (optional && id == 0) return nullptr;
    try {
      return static_cast<T>(context_.handles.get(kind, id));
    } catch (const TrapError& error) {
      fail(field, error.what());
    }
  }

  template <class T>
  auto take(Kind kind, uint32_t id, std::string_view field) const -> T {
    try {
      return static_cast<T>(context_.handles.take(kind, id));
    } catch (const TrapError& error) {
      fail(field, error.what());
    }
  }

  auto string(Arena& arena, uint32_t address, uint32_t length, std::string_view field) const
      -> WGPUStringView {
    if (length == UINT32_MAX) {
      if (address == 0) return {nullptr, WGPU_STRLEN};
      const auto* start = range(address, 0, field);
      const auto* end = static_cast<const uint8_t*>(
          std::memchr(start, 0, context_.memory.size - address));
      if (end == nullptr) fail(field, "unterminated string");
      length = static_cast<uint32_t>(end - start);
    }
    if (address == 0) {
      if (length != 0) fail(field, "null string data");
      return {nullptr, 0};
    }
    const auto* start = range(address, length, field);
    auto* copy = arena.allocate<char>(static_cast<size_t>(length) + 1);
    std::memcpy(copy, start, length);
    copy[length] = '\0';
    return {copy, length};
  }

  auto data(uint32_t address, uint32_t size, std::string_view field) const -> const void* {
    if (address == 0 && size == 0) return nullptr;
    return required(address, size, field);
  }

  template <class T, class Decode>
  auto array(Arena& arena, uint32_t address, uint32_t count, uint32_t stride,
             std::string_view field, Decode decode) const -> const T* {
    if (count == 0) {
      if (address != 0) range(address, 0, field);
      return nullptr;
    }
    required(address, static_cast<uint64_t>(count) * stride, field);
    auto* array = arena.allocate<T>(count);
    for (uint32_t index = 0; index < count; ++index) {
      array[index] = decode(static_cast<uint64_t>(address) + static_cast<uint64_t>(index) * stride);
    }
    return array;
  }

 private:
  Context& context_;
  std::string_view import_;
};

template <class Decode>
auto copy_descriptor(Arena& arena, Decode decode) -> decltype(decode())* {
  auto value = decode();
  auto* storage = arena.allocate<decltype(value)>();
  *storage = value;
  return storage;
}

template <class Decode>
auto copy_optional(Arena& arena, uint32_t address, Decode decode) -> decltype(decode())* {
  if (address == 0) return nullptr;
  return copy_descriptor(arena, decode);
}
