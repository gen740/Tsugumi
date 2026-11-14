#include "store.hh"

#include <gtest/gtest.h>

#include <regex>
#include <set>

#include "service.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi {
namespace {

TEST(CardIdentity, MetadataIsNonComputationalOnlyAtDefinitionPaths) {
  EXPECT_TRUE(Document::is_card_meta_file("definitions/card/_meta.json"));
  EXPECT_TRUE(Document::is_card_meta_file("externals/outer/externals/inner/definitions/card/_meta.json"));
  EXPECT_FALSE(Document::is_card_meta_file("definitions/card/src/definitions/data/_meta.json"));
  EXPECT_FALSE(Document::is_card_meta_file("externals/outer/definitions/card/_card.json"));
}

TEST(CardIdentity,
     SavingWiredDraftsResolvesPortsAndSerializesTheSavedDocument) {
  const auto root =
      std::filesystem::canonical(std::filesystem::temp_directory_path()) /
      ("wired-draft-" + crypto::uuid4());
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() { std::filesystem::remove_all(root); }
  } cleanup{root};
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"name":"test","pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})");
  fs::write_atomic(root / "substituters", "");
  ServiceOptions options;
  options.root = root;
  options.scratch_dir = root / "scratch";
  options.run_scratch_dir = root / "run-scratch";
  options.usage_file = root / "usage";
  options.substituters_file = root / "substituters";
  Service service(options);
  Server server({}, &service);
  service.attach(&server);
  service.open();

  namespace R = Tsugumi::RPC;
  R::ProjectSaveRequestT save;
  save.base_rev = 0;
  save.op_id = crypto::uuid4();
  save.origin = "ui:test";
  const auto operation = save.op_id;
  auto add = [&](auto value) {
    auto change = std::make_unique<R::DocumentChangeT>();
    change->value.Set(std::move(value));
    save.changes.push_back(std::move(change));
  };
  R::SetProjectT project;
  project.json =
      R"({"name":"test","pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"p"},{"id":"q"},{"id":"r"}],"connections":[{"from":{"card":"p","port":"out"},"to":{"card":"q","port":"in"}},{"from":{"card":"p","port":"out2"},"to":{"card":"r","port":"in"}},{"from":{"card":"r","port":"out"},"to":{"card":"q","port":"in2"}}]}})";
  add(std::move(project));
  auto definition = [&](const char* id, const char* card, const char* meta) {
    R::PutDefinitionT def;
    def.id = id;
    def.card_json = card;
    def.meta_json = meta;
    add(std::move(def));
  };
  definition(
      "p", R"({"template":"draft"})",
      R"({"name":"p","content":"count words","outputs":[{"name":"out"},{"name":"out2"}]})");
  definition("q",
             R"({"template":"draft","inputs":[{"name":"in"},{"name":"in2"}]})",
             R"({"name":"日本語"})");
  definition("r", R"({"template":"draft","inputs":[{"name":"in"}]})",
             R"({"name":"r","outputs":[{"name":"out"}]})");
  R::SetLayoutT layout;
  const std::string layout_json =
      R"({"cards":{"p":{"x":0,"y":0},"q":{"x":300,"y":0},"r":{"x":600,"y":0}}})";
  layout.json = layout_json;
  add(std::move(layout));
  R::RequestT request;
  request.value.Set(std::move(save));
  R::MessageT message;
  message.request_id = 1;
  message.payload.Set(std::move(request));
  flatbuffers::FlatBufferBuilder b;
  R::FinishMessageBuffer(b, R::Message::Pack(b, &message));
  flatbuffers::Verifier verifier(b.GetBufferPointer(), b.GetSize());
  ASSERT_TRUE(R::VerifyMessageBuffer(verifier));
  // Run the real save handler, receipt copy and notification preparation
  // without a listener.
  service.dispatch(
      {1, 1,
       std::make_shared<const std::string>(
           reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize())});
  // Drain the mutation lane before reading its saved result.
  service.shutdown();
  const auto receipt = service.store().receipt(operation);
  ASSERT_TRUE(receipt.has_value());
  const auto* saved =
      flatbuffers::GetRoot<R::OperationReceipt>(receipt->bytes.data())
          ->result_as_SaveResult();
  ASSERT_NE(saved, nullptr);
  EXPECT_EQ(saved->rev().value(), 1u);
  const auto doc = service.store().current();
  ASSERT_NE(doc->resolution, nullptr);
  ASSERT_NE(doc->resolution->find({"p"}), nullptr);
  ASSERT_NE(doc->resolution->find({"q"}), nullptr);
  EXPECT_NE(doc->resolution->find({"p"})->output("out2"), nullptr);
  EXPECT_NE(doc->resolution->find({"q"})->input("in2"), nullptr);
  EXPECT_EQ(doc->resolution->find({"q"})->name, "日本語");
  EXPECT_EQ(doc->resolution->edges.size(), 3u);
  EXPECT_EQ(doc->layout_json, layout_json);
}

TEST(CardIdentity, CoreReservesUniqueReadableIdentifiersAcrossRestartsAndScopes) {
  auto root = std::filesystem::temp_directory_path() / ("card-identity-" + crypto::uuid4());
  std::filesystem::create_directories(root);
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() { std::filesystem::remove_all(root); }
  } cleanup{root};
  fs::write_atomic(root / "tsugumi.json", R"({"name":"test","pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})");
  fs::write_atomic(root / "layout.json", "{}");
  fs::write_atomic(root / "externals/child/tsugumi.json", R"({"canvas":{"cards":[],"connections":[]}})");
  std::set<std::string> ids;
  std::regex shape("aggregate-[abcdefghjkmnpqrstuvwxyz23456789]{4}");
  {
    Store store({root, ""});
    store.open();
    auto rev = store.current()->rev;
    for (int i = 0; i < 100; ++i) {
      auto id = store.allocate_card_id("Aggregate", "");
      EXPECT_TRUE(std::regex_match(id, shape));
      EXPECT_TRUE(ids.insert(id).second);
    }
    EXPECT_EQ(store.current()->rev, rev);
    EXPECT_THROW(store.allocate_card_id("A", "../"), Failed);
    EXPECT_THROW(store.allocate_card_id("A", "externals/../"), Failed);
    EXPECT_THROW(store.allocate_card_id("", ""), Failed);
    EXPECT_LE(store.allocate_card_id(std::string(4096, 'a'), "").size(), 128u);
    EXPECT_TRUE(store.allocate_card_id("集計", "").starts_with("card-"));
  }
  Store store({root, ""});
  store.open();
  EXPECT_TRUE(ids.insert(store.allocate_card_id("Aggregate", "")).second);
  EXPECT_TRUE(std::regex_match(store.allocate_card_id("Aggregate", "externals/child/"), shape));
  EXPECT_TRUE(std::regex_match(store.allocate_card_id("Aggregate", "externals/child/externals/inner/"), shape));
  auto reservations = fs::read_file(root / ".tsugumi/card-ids").value();
  for (const auto& id : ids) EXPECT_NE(reservations.find(id + '\n'), std::string::npos);
}

}  // namespace
}  // namespace tsugumi
