// One FlatBuffer as the bytes of a file, blob or database value.
#pragma once

#include <flatbuffers/flatbuffers.h>

#include <optional>
#include <string>
#include <string_view>

namespace tsugumi::flat {

// The bytes of a value's FlatBuffer.
template <class T>
auto encode(const typename T::NativeTableType& value) -> std::string {
  flatbuffers::FlatBufferBuilder b;
  b.Finish(T::Pack(b, &value));
  return {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()};
}

// The value of verified bytes, or nullopt when they are not a buffer of T.
// Bytes kept in a std::string need not be aligned.
template <class T>
auto decode(std::string_view bytes)
    -> std::optional<typename T::NativeTableType> {
  flatbuffers::Verifier::Options options;
  options.check_alignment = false;
  flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(bytes.data()),
                          bytes.size(), options);
  if (!v.VerifyBuffer<T>(nullptr)) return std::nullopt;
  typename T::NativeTableType out;
  flatbuffers::GetRoot<T>(bytes.data())->UnPackTo(&out);
  return out;
}

}  // namespace tsugumi::flat
