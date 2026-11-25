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
  struct Free {
    auto operator()(uint8_t* data) const -> void;
  };

  explicit CardResult(tsugumi_bytes payload)
      : data_(const_cast<uint8_t*>(payload.data)), size_(payload.size) {}

  std::unique_ptr<uint8_t, Free> data_;
  size_t size_;
};

class CardImage {
 public:
  // The ABI permits background Card threads to use their effects after a
  // call. A loaded image stays resident for the life of the fast process.
  explicit CardImage(const std::filesystem::path& library);
  auto call(const tsugumi_call& invocation, std::span<const OutputPort> outputs,
            bool verify_flatbuffers) const -> CardResult;

 private:
  tsugumi_main_fn entry_;
};

}  // namespace tsugumi::pack
