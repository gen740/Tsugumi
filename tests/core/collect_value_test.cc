#include "collect_value.hh"

#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "tsugumi/document/collect_type.hh"
#include "tsugumi/document/schema_id.hh"
#include "tsugumi/protocol/card_generated.h"

namespace tsugumi {
namespace {

auto output_port() -> types::PortType {
  std::string error;
  auto type = collect_output_type(error);
  EXPECT_TRUE(type) << error;
  return {.name = "out", .type = std::move(type)};
}

TEST(CollectValue, EmptyListHasEmptyItems) {
  const auto output = output_port();
  ASSERT_TRUE(output.type);
  EXPECT_EQ(output.type->root_type, "tsugumi.builtin.Collected");
  const std::atomic<bool> cancel{false};
  const auto value = collect_values(output, {}, cancel);
  EXPECT_EQ(value.name, "out");
  EXPECT_EQ(value.type, output.type);
  EXPECT_EQ(value.schema, schema_id(*output.type));
  flatbuffers::Verifier verifier(
      reinterpret_cast<const uint8_t*>(value.bytes.data()), value.bytes.size());
  ASSERT_TRUE(verifier.VerifyBuffer<builtin::Collected>());
  const auto* collected = flatbuffers::GetRoot<builtin::Collected>(value.bytes.data());
  ASSERT_NE(collected->items(), nullptr);
  EXPECT_EQ(collected->items()->size(), 0u);
}

TEST(CollectValue, PreservesOpaqueSchemaAndBytes) {
  const auto output = output_port();
  ASSERT_TRUE(output.type);
  const std::vector<Value> members{
      {"left", nullptr, std::string("opaque\0", 7), "first"},
      {"right", nullptr, std::string("\xff\0", 2), "second"},
      {"empty", nullptr, {}, "third"},
  };
  const std::atomic<bool> cancel{false};
  const auto value = collect_values(output, members, cancel);
  flatbuffers::Verifier verifier(
      reinterpret_cast<const uint8_t*>(value.bytes.data()), value.bytes.size());
  ASSERT_TRUE(verifier.VerifyBuffer<builtin::Collected>());
  const auto* collected = flatbuffers::GetRoot<builtin::Collected>(value.bytes.data());
  const auto* items = collected->items();
  ASSERT_NE(items, nullptr);
  ASSERT_EQ(items->size(), members.size());
  for (flatbuffers::uoffset_t i = 0; i < items->size(); ++i) {
    const auto* item = items->Get(i);
    const auto& member = members[i];
    EXPECT_EQ(item->schema()->str(), member.schema);
    ASSERT_NE(item->bytes(), nullptr);
    const auto* data = reinterpret_cast<const char*>(item->bytes()->data());
    EXPECT_EQ(std::string_view(data, item->bytes()->size()), member.bytes.view());
    if (!member.bytes.empty())
      EXPECT_EQ((data - value.bytes.data()) % FLATBUFFERS_MAX_ALIGNMENT, 0);
  }
}

TEST(CollectValue, CancellationStopsAnEmptyBatch) {
  const std::atomic<bool> cancel{true};
  try {
    (void)collect_values(output_port(), {}, cancel);
    FAIL() << "cancellation was ignored";
  } catch (const Failed& e) {
    EXPECT_EQ(e.failure.kind, FailureKind::cancelled);
  }
}

TEST(CollectValue, RejectsACollectedVectorLargerThan16MiB) {
  const auto output = output_port();
  const std::vector<Value> members{
      {"first", nullptr, std::string(size_t{9} << 20, 'a'), "first"},
      {"second", nullptr, std::string(size_t{9} << 20, 'a'), "second"},
  };
  const std::atomic<bool> cancel{false};
  try {
    (void)collect_values(output, members, cancel);
    FAIL() << "a vector larger than the worker value limit was accepted";
  } catch (const Failed& failure) {
    EXPECT_EQ(failure.failure.kind, FailureKind::output_invalid);
  }
}

}  // namespace
}  // namespace tsugumi
