// Exercise the native Card ABI boundary without a sandbox or a store.
#define main tsugumi_worker_main
#include "native_main.cc"
#undef main

#include <gtest/gtest.h>

#include <filesystem>
#include <future>

namespace {

class NativeContract : public testing::Test {
 protected:
  flatbuffers::FlatBufferBuilder response;
  Host host;

  auto port(const char* name, std::string_view bytes,
            const std::vector<tsugumi::ArtifactId>& artifacts = {})
      -> flatbuffers::Offset<C::CardPort> {
    return C::CreateCardPort(
        card_buffer, card_buffer.CreateString(name), 0,
        card_buffer.CreateVector(reinterpret_cast<const uint8_t*>(bytes.data()),
                                 bytes.size()),
        card_buffer.CreateVectorOfStructs(artifacts));
  }

  auto finish(const std::vector<flatbuffers::Offset<C::CardPort>>& ports)
      -> const C::CardResult& {
    card_buffer.Finish(
        C::CreateCardResult(card_buffer, card_buffer.CreateVector(ports)));
    return *flatbuffers::GetRoot<C::CardResult>(card_buffer.GetBufferPointer());
  }

  auto output() const -> const W::WorkerOutput* {
    return W::GetWorkerMessage(response.GetBufferPointer())
        ->value_as_WorkerResult()->outputs()->Get(0);
  }
};

TEST_F(NativeContract, PacksOpaqueBytesAndArtifactIds) {
  const ArtifactId id{1, 2, 3};
  const std::string bytes("\x01\x02 not a table", 14);
  const auto& result = finish({port(
      "first", bytes, {tsugumi::ArtifactId(flatbuffers::span<const uint8_t, 16>(id))})});
  ASSERT_FALSE(make_result(result, host, 1, response));
  EXPECT_EQ(output()->name()->str(), "first");
  EXPECT_EQ(std::string(output()->bytes()->begin(), output()->bytes()->end()), bytes);
  EXPECT_EQ(id_bytes(*output()->artifacts()->Get(0)), id);
}

TEST_F(NativeContract, CombinedOutputsMustFitOneWorkerResultFrame) {
  const std::string bytes(9u << 20, 'x');
  const auto& result = finish({port("first", bytes), port("second", bytes)});
  const auto failure = make_result(result, host, 1, response);
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->kind, D::FailureKind::output_invalid);
}

TEST_F(NativeContract, OversizeFailsImmediatelyAndFullSendCancels) {
  host.call_id = 1;
  const std::vector<uint8_t> message(TSUGUMI_MESSAGE_MAX, 255);
  EXPECT_LT(send_display(&host, "view", message.data(), message.size() + 1), 0);
  ASSERT_EQ(send_display(&host, "view", message.data(), message.size()), 0);
  ASSERT_EQ(send_display(&host, "view", message.data(), message.size()), 0);
  auto blocked = std::async(std::launch::async, [&] {
    return send_display(&host, "view", message.data(), 1);
  });
  EXPECT_EQ(blocked.wait_for(std::chrono::milliseconds(10)),
            std::future_status::timeout);
  {
    std::lock_guard lock(host.view_mutex);
    host.cancelled_id = 1;
  }
  host.view_ready.notify_all();
  EXPECT_LT(blocked.get(), 0);
}

TEST_F(NativeContract, ReceiveTimeoutAndPerThreadStorage) {
  {
    auto& queue = host.views["view"];
    queue.incoming = {"abc", "xyz"};
    queue.incoming_bytes = 6;
  }
  tsugumi_bytes bytes{};
  ASSERT_EQ(receive_display(&host, "view", &bytes, 0), 0);
  auto other = std::async(std::launch::async, [&] {
    tsugumi_bytes second{};
    if (receive_display(&host, "view", &second, 0) != 0) return std::string{};
    return std::string(second.data, second.data + second.size);
  });
  EXPECT_EQ(other.get(), "xyz");
  EXPECT_EQ(std::string(bytes.data, bytes.data + bytes.size), "abc");
  EXPECT_EQ(receive_display(&host, "view", &bytes, 1), 1);
  host.cancelled_id = 1;
  EXPECT_EQ(receive_display(&host, "view", &bytes, 0), 1);
  host.call_id = 1;
  EXPECT_EQ(receive_display(&host, "view", &bytes, 0), 2);
  host.call_id = 0;
  host.stopping = true;
  EXPECT_EQ(receive_display(&host, "view", &bytes, -1), 2);
}

TEST_F(NativeContract, ArtifactsAreNewDirectoriesAndOnlyGrantsCanBeOpened) {
  char directory[] = "/tmp/tsugumi-native-artifacts-XXXXXX";
  ASSERT_NE(::mkdtemp(directory), nullptr);
  struct Removal {
    const char* directory;
    ~Removal() { std::filesystem::remove_all(directory); }
  } cleanup{directory};
  host.artifact_directory = directory;
  tsugumi_artifact_id first{}, second{};
  tsugumi_bytes path{};
  ASSERT_EQ(create_artifact(&host, &first, &path), 0);
  const std::string created(path.data, path.data + path.size);
  EXPECT_TRUE(std::filesystem::is_directory(created));
  EXPECT_EQ(path.data[path.size], 0);
  ASSERT_EQ(create_artifact(&host, &second, &path), 0);
  EXPECT_NE(std::string(path.data, path.data + path.size), created);
  EXPECT_LT(open_artifact(&host, &first, &path), 0);
  ArtifactId key;
  std::ranges::copy(first.bytes, key.begin());
  host.granted.emplace(key, created);
  ASSERT_EQ(open_artifact(&host, &first, &path), 0);
  EXPECT_EQ(std::string(path.data, path.data + path.size), created);
}

}  // namespace
