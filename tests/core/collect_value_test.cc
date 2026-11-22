#include "collect_value.hh"

#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "tsugumi/document/collect_type.hh"
#include "tsugumi/protocol/card_generated.h"

namespace tsugumi {
namespace {

using Envelopes =
    flatbuffers::Vector<flatbuffers::Offset<Tsugumi::Card::CardPort>>;

auto output_port() -> types::PortType {
  std::string error;
  auto type = collect_output_type(error);
  EXPECT_TRUE(type) << error;
  return {"out", PortKind::FlatBuffer, false, std::move(type)};
}

auto envelopes(const Value& value) -> const Envelopes* {
  constexpr flatbuffers::voffset_t kItems = 4;
  return flatbuffers::GetRoot<flatbuffers::Table>(value.bytes.data())
      ->GetPointer<const Envelopes*>(kItems);
}

TEST(CollectValue, EmptyListHasEmptyItems) {
  const auto output = output_port();
  ASSERT_TRUE(output.type);
  EXPECT_EQ(output.type->root_type, "tsugumi.builtin.Collected");
  const std::atomic<bool> cancel{false};
  const auto value = collect_values(output, {}, cancel);
  EXPECT_EQ(value.name, "out");
  EXPECT_EQ(value.kind, PortKind::FlatBuffer);
  EXPECT_EQ(value.type, output.type);
  ASSERT_TRUE(types::verify(*output.type, value.bytes));
  ASSERT_NE(envelopes(value), nullptr);
  EXPECT_EQ(envelopes(value)->size(), 0u);
}

TEST(CollectValue, PreservesOpaqueBytesAndPathsInArrivalOrder) {
  const auto output = output_port();
  ASSERT_TRUE(output.type);
  const std::vector<Value> members{
      {"file", PortKind::Path, nullptr, {}, {}, {}, "/run/out/report.txt"},
      {"reading",
       PortKind::FlatBuffer,
       nullptr,
       std::string("opaque\0", 7),
       {}},
      {"folder", PortKind::Path, nullptr, {}, {}, {}, "/run/out/data"},
  };
  const std::atomic<bool> cancel{false};
  const auto value = collect_values(output, members, cancel);
  ASSERT_TRUE(types::verify(*output.type, value.bytes));
  const auto* items = envelopes(value);
  ASSERT_NE(items, nullptr);
  ASSERT_EQ(items->size(), members.size());
  for (flatbuffers::uoffset_t i = 0; i < items->size(); ++i) {
    const auto* item = items->Get(i);
    const auto& member = members[i];
    EXPECT_EQ(item->name()->str(), member.name);
    EXPECT_EQ(item->kind(), member.kind);
    if (member.kind == PortKind::Path) {
      EXPECT_EQ(item->path()->str(), member.path);
      EXPECT_EQ(item->bytes(), nullptr);
    } else {
      EXPECT_EQ(item->path(), nullptr);
      ASSERT_NE(item->bytes(), nullptr);
      const auto* data = reinterpret_cast<const char*>(item->bytes()->data());
      EXPECT_EQ(std::string_view(data, item->bytes()->size()), member.bytes);
      EXPECT_EQ((data - value.bytes.data()) % FLATBUFFERS_MAX_ALIGNMENT, 0);
    }
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
      {"first",
       PortKind::FlatBuffer,
       nullptr,
       std::string(size_t{9} << 20, 'a'),
       {}},
      {"second",
       PortKind::FlatBuffer,
       nullptr,
       std::string(size_t{9} << 20, 'a'),
       {}},
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
