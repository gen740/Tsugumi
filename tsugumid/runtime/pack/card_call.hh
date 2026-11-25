#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

#include "tsugumi/card_abi.h"
#include "tsugumi/protocol/card_generated.h"

namespace tsugumi::pack {

struct OutputPort {
  std::string_view name;
  bool required;
};

class ResultMemory {
 public:
  auto allocate(size_t size) -> uint8_t*;

 private:
  friend class CardImage;
  friend class CardResult;
  struct Free {
    auto operator()(uint8_t* data) const -> void;
  };

  std::unique_ptr<uint8_t, Free> data_;
  size_t capacity_ = 0;
};

// Owns the allocation transferred by tsugumi_main. The scheduler keeps this
// owner alive until all calls borrowing its FlatBuffer have returned.
class CardResult {
 public:
  CardResult(CardResult&&) = default;
  auto operator=(CardResult&&) -> CardResult& = default;

  auto payload() const -> tsugumi_bytes { return {data_.get(), size_}; }
  auto root() const -> const Tsugumi::Card::CardResult&;

 private:
  friend class CardImage;
  CardResult(ResultMemory&& memory, size_t size)
      : data_(std::move(memory.data_)), size_(size) {}

  std::unique_ptr<uint8_t, ResultMemory::Free> data_;
  size_t size_;
};

class CardImage {
 public:
  // The ABI permits background Card threads to use their effects after a
  // call. A loaded image stays resident for the life of the fast process.
  explicit CardImage(const std::filesystem::path& library);
  // The invocation's result_alloc effect must allocate from memory.
  auto call(const tsugumi_call& invocation, ResultMemory& memory,
            std::span<const OutputPort> outputs,
            bool verify_flatbuffers) const -> CardResult;

 private:
  tsugumi_main_fn entry_;
};

}  // namespace tsugumi::pack
