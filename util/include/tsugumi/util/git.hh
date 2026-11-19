#pragma once

#include <git2.h>

namespace tsugumi::git {
struct Init {
  Init() { git_libgit2_init(); }
};
inline auto init_once() -> void { static Init i; }

template <class T, void (*F)(T*)>
struct Ptr {
  T* p = nullptr;
  Ptr() = default;
  Ptr(const Ptr&) = delete;
  Ptr& operator=(const Ptr&) = delete;
  ~Ptr() {
    if (p) F(p);
  }
  auto out() -> T** { return &p; }
  auto get() const -> T* { return p; }
  explicit operator bool() const { return p != nullptr; }
};
}  // namespace tsugumi::git
