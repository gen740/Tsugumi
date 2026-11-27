#pragma once

#include <memory>

namespace tsugumi {
template <class T, void (*Free)(T*)>
struct CDeleter {
  auto operator()(T* handle) const -> void { Free(handle); }
};

template <class T, void (*Free)(T*)>
using CHandle = std::unique_ptr<T, CDeleter<T, Free>>;
}  // namespace tsugumi
