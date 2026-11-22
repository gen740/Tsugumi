// Exercise the native CardResult boundary without a sandbox or a store.
#define main tsugumi_worker_main
#include "native_main.cc"
#undef main

#include <flatbuffers/idl.h>
#include <gtest/gtest.h>

namespace {

class NativeContract : public testing::Test {
 protected:
  flatbuffers::FlatBufferBuilder request_buffer;
  flatbuffers::FlatBufferBuilder card_buffer;
  flatbuffers::FlatBufferBuilder response;
  flatbuffers::Parser parser;
  D::FlatBufferDataT file;
  const W::WorkerCall* request = nullptr;

  void SetUp() override {
    ASSERT_TRUE(parser.Parse(
        "table File { value:string (required); } root_type File;"));
    parser.Serialize();
    file.root_type = "File";
    file.schema_bfbs.assign(
        parser.builder_.GetBufferPointer(),
        parser.builder_.GetBufferPointer() + parser.builder_.GetSize());
    W::WorkerCallT call;
    call.params = std::make_unique<D::FlatBufferDataT>();
    for (const char* name : {"first", "second"}) {
      auto port = std::make_unique<D::PortT>();
      port->name = name;
      port->kind = D::PortKind::File;
      port->optional = name == std::string_view("second");
      port->root_type = file.root_type;
      port->schema_bfbs = file.schema_bfbs;
      call.outputs.push_back(std::move(port));
    }
    request_buffer.Finish(W::WorkerCall::Pack(request_buffer, &call));
    request =
        flatbuffers::GetRoot<W::WorkerCall>(request_buffer.GetBufferPointer());
  }

  auto port(const char* name, std::string_view path)
      -> flatbuffers::Offset<C::CardPort> {
    flatbuffers::FlatBufferBuilder payload;
    const auto text = payload.CreateString(path);
    const auto open = payload.StartTable();
    payload.AddOffset(4, text);
    payload.Finish(
        flatbuffers::Offset<flatbuffers::Table>(payload.EndTable(open)));
    const auto bytes =
        card_buffer.CreateVector(payload.GetBufferPointer(), payload.GetSize());
    return C::CreateCardPort(card_buffer, card_buffer.CreateString(name),
                             D::PortKind::File, bytes);
  }

  auto finish(const std::vector<flatbuffers::Offset<C::CardPort>>& ports)
      -> const C::CardResult& {
    card_buffer.Finish(
        C::CreateCardResult(card_buffer, card_buffer.CreateVector(ports)));
    return *flatbuffers::GetRoot<C::CardResult>(card_buffer.GetBufferPointer());
  }
};

TEST_F(NativeContract, ValidRequiredAndAbsentOptionalComplete) {
  const auto& result = finish({port("first", "new/file")});
  EXPECT_FALSE(make_result(result, *request, {}, 1, response));
  const auto* outputs = W::GetWorkerMessage(response.GetBufferPointer())
                            ->value_as_WorkerResult()
                            ->outputs();
  ASSERT_EQ(outputs->size(), 1u);
  EXPECT_EQ(outputs->Get(0)->name()->str(), "first");
  EXPECT_EQ(outputs->Get(0)->forwarded_from(), nullptr);
}

TEST_F(NativeContract, MatchingFileReturnsInputPortName) {
  const auto& result = finish({port("first", "/staged/file")});
  flatbuffers::FlatBufferBuilder names;
  names.Finish(names.CreateString("input"));
  const auto* input =
      flatbuffers::GetRoot<flatbuffers::String>(names.GetBufferPointer());
  EXPECT_FALSE(make_result(result, *request,
                           {{{D::PortKind::File, "/staged/file"}, input}}, 1,
                           response));
  const auto* outputs = W::GetWorkerMessage(response.GetBufferPointer())
                            ->value_as_WorkerResult()
                            ->outputs();
  ASSERT_EQ(outputs->size(), 1u);
  ASSERT_NE(outputs->Get(0)->forwarded_from(), nullptr);
  EXPECT_EQ(outputs->Get(0)->forwarded_from()->str(), "input");
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
