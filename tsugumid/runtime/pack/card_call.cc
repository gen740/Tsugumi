#include "card_call.hh"

#include <dlfcn.h>

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace tsugumi::pack {
namespace {

auto check_budget(const tsugumi_call& invocation) -> void {
  const auto* effects = invocation.effects;
  if (!effects) return;
  if (effects->cancelled && effects->cancelled(invocation.host))
    throw std::runtime_error("Card invocation cancelled");
  if (effects->remaining_ms && effects->remaining_ms(invocation.host) == 0)
    throw std::runtime_error("Card invocation exceeded its deadline");
}

auto check_outputs(const Tsugumi::Card::CardResult& result,
                   std::span<const OutputPort> declared) -> void {
  if (result.failure()) return;
  const auto* outputs = result.outputs();
  for (const auto& port : declared) {
    size_t count = 0;
    if (outputs) {
      for (const auto* output : *outputs) {
        if (output->name() && output->name()->string_view() == port.name)
          ++count;
      }
    }
    if (count == 0 && port.required)
      throw std::runtime_error("Card omitted required output " +
                               std::string(port.name));
    if (count > 1)
      throw std::runtime_error("Card returned duplicate output " +
                               std::string(port.name));
  }
}

}  // namespace

auto ResultMemory::Free::operator()(uint8_t* data) const -> void {
  std::free(data);
}

auto ResultMemory::allocate(size_t size) -> uint8_t* {
  if (data_ || size == 0 || size > TSUGUMI_MESSAGE_MAX) return nullptr;
  void* data = nullptr;
  if (posix_memalign(&data, FLATBUFFERS_MAX_ALIGNMENT, size) != 0)
    return nullptr;
  data_.reset(static_cast<uint8_t*>(data));
  capacity_ = size;
  return data_.get();
}

auto CardResult::root() const -> const Tsugumi::Card::CardResult& {
  return *flatbuffers::GetRoot<Tsugumi::Card::CardResult>(data_.get());
}

CardImage::CardImage(const std::filesystem::path& library) {
  void* handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const auto* error = dlerror();
    throw std::runtime_error("Cannot load " + library.string() + ": " +
                             (error ? error : "unknown loader error"));
  }
  entry_ = reinterpret_cast<tsugumi_main_fn>(dlsym(handle, "tsugumi_main"));
  if (!entry_) {
    dlclose(handle);
    throw std::runtime_error(library.string() + " lacks tsugumi_main");
  }
}

auto CardImage::call(const tsugumi_call& invocation, ResultMemory& memory,
                     std::span<const OutputPort> outputs,
                     bool verify_flatbuffers) const -> CardResult {
  size_t input_size = 0;
  for (const auto& input : std::span(invocation.inputs, invocation.input_count)) {
    if (input.bytes.size > TSUGUMI_MESSAGE_MAX - input_size)
      throw std::runtime_error("Card input exceeds 16 MiB");
    input_size += input.bytes.size;
  }
  check_budget(invocation);
  tsugumi_bytes bytes{};
  const int status = entry_(&invocation, &bytes);
  check_budget(invocation);
  if (status != 0)
    throw std::runtime_error("tsugumi_main returned " + std::to_string(status));
  if (!memory.data_ || bytes.data != memory.data_.get() ||
      bytes.size < sizeof(flatbuffers::uoffset_t) || bytes.size > memory.capacity_)
    throw std::runtime_error("Invalid Card result size");
  if (verify_flatbuffers) {
    flatbuffers::Verifier verifier(bytes.data, bytes.size);
    if (!verifier.VerifyBuffer<Tsugumi::Card::CardResult>(nullptr))
      throw std::runtime_error("Invalid Card result FlatBuffer");
  }
  CardResult result(std::move(memory), bytes.size);
  check_outputs(result.root(), outputs);
  return result;
}

}  // namespace tsugumi::pack
