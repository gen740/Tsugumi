// Exercise the native CardResult boundary without a sandbox or a store.
#define main tsugumi_worker_main
#include "native_main.cc"
#undef main

#include <gtest/gtest.h>

namespace {

class NativeContract : public testing::Test {
 protected:
  flatbuffers::FlatBufferBuilder request_buffer;
  flatbuffers::FlatBufferBuilder card_buffer;
  flatbuffers::FlatBufferBuilder response;
  const W::WorkerCall* request = nullptr;

  void SetUp() override {
    W::WorkerCallT call;
    const std::pair<const char*, D::PortKind> ports[] = {
        {"first", D::PortKind::Path},
        {"second", D::PortKind::Path},
        {"value", D::PortKind::FlatBuffer}};
    for (const auto& [name, kind] : ports) {
      auto port = std::make_unique<W::WorkerPortT>();
      port->name = name;
      port->kind = kind;
      port->required = name == std::string_view("first");
      call.outputs.push_back(std::move(port));
    }
    request_buffer.Finish(W::WorkerCall::Pack(request_buffer, &call));
    request =
        flatbuffers::GetRoot<W::WorkerCall>(request_buffer.GetBufferPointer());
  }

  auto port(const char* name, std::string_view path)
      -> flatbuffers::Offset<C::CardPort> {
    return C::CreateCardPort(card_buffer, card_buffer.CreateString(name),
                             D::PortKind::Path, 0,
                             card_buffer.CreateString(path));
  }

  // Arbitrary bytes: the worker never opens a FlatBuffer output.
  auto value(std::string_view bytes) -> flatbuffers::Offset<C::CardPort> {
    return C::CreateCardPort(
        card_buffer, card_buffer.CreateString("value"),
        D::PortKind::FlatBuffer,
        card_buffer.CreateVector(
            reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
  }

  auto finish(const std::vector<flatbuffers::Offset<C::CardPort>>& ports)
      -> const C::CardResult& {
    card_buffer.Finish(
        C::CreateCardResult(card_buffer, card_buffer.CreateVector(ports)));
    return *flatbuffers::GetRoot<C::CardResult>(card_buffer.GetBufferPointer());
  }

  auto outputs() const -> const flatbuffers::Vector<
      flatbuffers::Offset<W::WorkerOutput>>* {
    return W::GetWorkerMessage(response.GetBufferPointer())
        ->value_as_WorkerResult()
        ->outputs();
  }
};

TEST_F(NativeContract, ValidRequiredAndAbsentOptionalComplete) {
  const auto& result = finish({port("first", "new/file")});
  EXPECT_FALSE(make_result(result, *request, {}, 1, response));
  ASSERT_EQ(outputs()->size(), 1u);
  EXPECT_EQ(outputs()->Get(0)->name()->str(), "first");
  EXPECT_EQ(outputs()->Get(0)->path()->str(), "new/file");
  EXPECT_EQ(outputs()->Get(0)->bytes(), nullptr);
  EXPECT_EQ(outputs()->Get(0)->forwarded_from(), nullptr);
}

TEST_F(NativeContract, FlatBufferBytesPassUnopened) {
  const std::string bytes("\x01\x02 not a table", 14);
  const auto& result = finish({port("first", "a"), value(bytes)});
  EXPECT_FALSE(make_result(result, *request, {}, 1, response));
  ASSERT_EQ(outputs()->size(), 2u);
  const auto* out = outputs()->Get(1)->bytes();
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(out->data()),
                             out->size()),
            bytes);
}

TEST_F(NativeContract, MatchingPathReturnsInputPortName) {
  const auto& result = finish({port("first", "/staged/file")});
  EXPECT_FALSE(
      make_result(result, *request, {{"/staged/file", "input"}}, 1, response));
  ASSERT_EQ(outputs()->size(), 1u);
  ASSERT_NE(outputs()->Get(0)->forwarded_from(), nullptr);
  EXPECT_EQ(outputs()->Get(0)->forwarded_from()->str(), "input");
}

TEST_F(NativeContract, PathOutputNeedsAPathAndNoBytes) {
  const auto rejected = [&](flatbuffers::Offset<C::CardPort> port) {
    const auto failure = make_result(finish({port}), *request, {}, 1, response);
    card_buffer.Clear();
    return failure && failure->kind == D::FailureKind::output_invalid;
  };
  EXPECT_TRUE(rejected(C::CreateCardPort(
      card_buffer, card_buffer.CreateString("first"), D::PortKind::Path)));
  EXPECT_TRUE(rejected(C::CreateCardPort(
      card_buffer, card_buffer.CreateString("first"), D::PortKind::Path,
      card_buffer.CreateVector(std::vector<uint8_t>{1}),
      card_buffer.CreateString("file"))));
}

TEST_F(NativeContract, WrongKindIsRejected) {
  const auto& result = finish({C::CreateCardPort(
      card_buffer, card_buffer.CreateString("first"), D::PortKind::FlatBuffer,
      card_buffer.CreateVector(std::vector<uint8_t>{1}))});
  const auto failure = make_result(result, *request, {}, 1, response);
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->kind, D::FailureKind::output_invalid);
}

TEST_F(NativeContract, RequiredOutputFailureIdentifiesThePort) {
  const auto failure = make_result(finish({}), *request, {}, 1, response);
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->kind, D::FailureKind::output_invalid);
  ASSERT_EQ(failure->args.size(), 1u);
  EXPECT_EQ(failure->args.front(),
            std::make_pair(std::string("port"), std::string("first")));
}

TEST_F(NativeContract, CombinedOutputsMustFitOneWorkerResultFrame) {
  const std::string path(8u << 20, 'x');
  const auto& result = finish({port("first", path), port("second", path)});
  const auto failure = make_result(result, *request, {}, 1, response);
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->kind, D::FailureKind::output_invalid);
}

}  // namespace
