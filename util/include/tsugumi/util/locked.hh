#pragma once

#include <cassert>
#include <functional>
#include <mutex>
#include <type_traits>
#include <utility>

namespace tsugumi {
namespace lock_scope {
#ifndef NDEBUG
inline thread_local unsigned depth = 0;
#endif
inline auto AssertUnlocked() -> void {
#ifndef NDEBUG
  assert(depth == 0);
#endif
}
}  // namespace lock_scope

template <class T>
class Locked {
 public:
  Locked(std::recursive_mutex& mutex, T value)
      : mutex_(mutex), value_(std::move(value)) {}

  template <class F>
  auto with(F&& fn) -> std::invoke_result_t<F, T&> {
    using Result = std::invoke_result_t<F, T&>;
    static_assert(!std::is_reference_v<Result> && !std::is_pointer_v<Result>);
    std::lock_guard lock(mutex_);
#ifndef NDEBUG
    struct Scope {
      Scope() { ++lock_scope::depth; }
      ~Scope() { --lock_scope::depth; }
    } scope;
#endif
    return std::invoke(std::forward<F>(fn), value_);
  }

 private:
  std::recursive_mutex& mutex_;
  T value_;
};
}  // namespace tsugumi
