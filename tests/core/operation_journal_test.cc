#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ctime>

#include "store.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi {
namespace {
class OperationJournal : public testing::Test {
 protected:
  std::filesystem::path root = std::filesystem::temp_directory_path() /
                               ("operations-" + crypto::uuid4());
  const std::string digest = crypto::sha256_hex("semantic request");
  void SetUp() override {
    fs::write_atomic(
        root / "tsugumi.json",
        R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})");
  }
  void TearDown() override { std::filesystem::remove_all(root); }
  auto Commit(Store& store, const OperationIdentity& identity,
              const std::string& name = "changed") -> void {
    ChangeIn change{};
    change.kind = ChangeIn::Kind::SetProject;
    change.json =
        "{\"name\":\"" + name +
        R"(","pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})";
    auto staged = store.stage({change}, crypto::uuid4(), "agent");
    store.commit(
        staged, "save", digest, [](const Delta*) { return "terminal outcome"; },
        [] {}, identity);
  }
  template <class F>
  auto Refuses(F&& call, FailureKind kind) -> void {
    try {
      call();
      FAIL() << "operation unexpectedly succeeded";
    } catch (const Failed& failure) {
      EXPECT_EQ(failure.failure.kind, kind);
      ASSERT_TRUE(failure.failure.diagnostic);
      EXPECT_FALSE(failure.failure.diagnostic->empty());
    }
  }
};

TEST_F(OperationJournal,
       AcknowledgedIdentitiesStayRetiredAcrossCompactionAndRestart) {
  OperationIdentity identity;
  uint64_t cursor;
  {
    Store store({root, ""});
    store.open();
    identity = {store.open_writer(std::time(nullptr)), 1};
    EXPECT_FALSE(store.operation(identity, digest, std::time(nullptr)));
    Commit(store, identity);
    auto outcome = store.operation(identity, digest, std::time(nullptr));
    ASSERT_TRUE(outcome);
    cursor = outcome->cursor;
    EXPECT_EQ(cursor, 1u);
    EXPECT_EQ(outcome->bytes, "terminal outcome");
    Refuses([&] { store.operation(identity, "different", std::time(nullptr)); },
            FailureKind::operation_conflict);
    Refuses(
        [&] { store.acknowledge(identity, cursor - 1, std::time(nullptr)); },
        FailureKind::operation_conflict);
    Refuses(
        [&] {
          store.operation({identity.lane, 2}, digest, std::time(nullptr));
        },
        FailureKind::operation_conflict);
    store.acknowledge(identity, cursor, std::time(nullptr));
    // Force the ordinary UUID receipt cache to compact and discard old replies.
    store.record_receipt(
        {crypto::uuid4(), "save", "legacy", std::string(9u << 20, 'x')});
    Refuses([&] { store.operation(identity, digest, std::time(nullptr)); },
            FailureKind::operation_retired);
  }
  Store reopened({root, ""});
  reopened.open();
  EXPECT_EQ(reopened.current()->rev, 1u);
  EXPECT_EQ(reopened.cursor(), cursor + 1);
  Refuses([&] { reopened.operation(identity, digest, std::time(nullptr)); },
          FailureKind::operation_retired);
  EXPECT_FALSE(
      reopened.operation({identity.lane, 2}, digest, std::time(nullptr)));
}

TEST_F(OperationJournal,
       UnacknowledgedOutcomeSurvivesCompactionWithoutTheUuidCache) {
  OperationIdentity identity;
  {
    Store store({root, ""});
    store.open();
    identity = {store.open_writer(std::time(nullptr)), 1};
    Commit(store, identity);
    store.record_receipt(
        {crypto::uuid4(), "save", "legacy", std::string(9u << 20, 'x')});
  }
  Store reopened({root, ""});
  reopened.open();
  auto outcome = reopened.operation(identity, digest, std::time(nullptr));
  ASSERT_TRUE(outcome);
  EXPECT_EQ(outcome->bytes, "terminal outcome");
  EXPECT_EQ(outcome->cursor, 1u);
}

TEST_F(OperationJournal,
       PendingSemanticIdentitySurvivesRestartAndCannotBeRebound) {
  OperationIdentity identity;
  {
    Store store({root, ""});
    store.open();
    identity = {store.open_writer(std::time(nullptr)), 1};
    store.begin_operation(identity, digest, std::time(nullptr));
    EXPECT_TRUE(store.operation_pending(identity));
    Refuses(
        [&] {
          store.begin_operation(identity, crypto::sha256_hex("different"),
                                std::time(nullptr));
        },
        FailureKind::operation_conflict);
    Refuses([&] { store.acknowledge(identity, 0, std::time(nullptr)); },
            FailureKind::operation_conflict);
  }
  Store reopened({root, ""});
  reopened.open();
  EXPECT_TRUE(reopened.operation_pending(identity));
  Refuses(
      [&] {
        reopened.operation(identity, crypto::sha256_hex("different"),
                           std::time(nullptr));
      },
      FailureKind::operation_conflict);
  Commit(reopened, identity);
  EXPECT_FALSE(reopened.operation_pending(identity));
}

TEST_F(OperationJournal, LaneAndReceiptBudgetsAreCheckedBeforeAnyFileMutation) {
  Store store({root, ""});
  store.open();
  OperationIdentity identity{store.open_writer(std::time(nullptr)), 1};
  for (size_t n = 1; n < Store::kMaxWriterLanes; ++n)
    store.open_writer(std::time(nullptr));
  Refuses([&] { store.open_writer(std::time(nullptr)); },
          FailureKind::resource_limit);
  auto staged = store.stage({}, crypto::uuid4(), "agent", true);
  Refuses(
      [&] {
        store.commit(
            staged, "save", digest,
            [](const Delta*) {
              return std::string(Store::kMaxOutcomeBytes, 'x');
            },
            [] {}, identity);
      },
      FailureKind::resource_limit);
  EXPECT_FALSE(store.operation(identity, digest, std::time(nullptr)));
  EXPECT_EQ(store.cursor(), 0u);
  Commit(store, identity);
  EXPECT_EQ(store.current()->rev, 1u);
}

TEST_F(OperationJournal, ExpiredAndUnknownLanesNeverBecomeExecutable) {
  OperationIdentity identity;
  const auto now = std::time(nullptr);
  {
    Store store({root, ""});
    store.open();
    identity = {store.open_writer(now), 1};
    Commit(store, identity);
    store.expire_writers(now + Store::kWriterLeaseSeconds + 2);
    Refuses([&] { store.operation(identity, digest, now); },
            FailureKind::operation_retired);
  }
  Store reopened({root, ""});
  reopened.open();
  Refuses([&] { reopened.operation(identity, digest, now); },
          FailureKind::operation_retired);
  Refuses([&] { reopened.operation({crypto::uuid4(), 1}, digest, now); },
          FailureKind::operation_retired);
  EXPECT_NE(reopened.open_writer(now), identity.lane);
}

TEST_F(OperationJournal, NoopAndRefusalAdvanceOnlyTheCursor) {
  Store store({root, ""});
  store.open();
  OperationIdentity identity{store.open_writer(std::time(nullptr)), 1};
  auto staged = store.stage({}, crypto::uuid4(), "agent", true);
  store.commit(
      staged, "save", digest, [](const Delta*) { return "noop"; }, [] {},
      identity);
  EXPECT_EQ(store.current()->rev, 0u);
  EXPECT_EQ(store.cursor(), 1u);
  store.acknowledge(identity, 1, std::time(nullptr));
  ++identity.sequence;
  store.record_receipt(
      {crypto::uuid4(), "save", digest, "validation refusal", identity});
  EXPECT_EQ(store.current()->rev, 0u);
  EXPECT_EQ(store.cursor(), 2u);
}

TEST_F(OperationJournal,
       CrashRecoveryKeepsIntentApplicationAndOutcomeTogether) {
  for (const auto* point :
       {"before_journal", "after_journal", "partial_apply", "before_commit"}) {
    SCOPED_TRACE(point);
    std::string lane;
    uint64_t before;
    {
      Store store({root, ""});
      store.open();
      lane = store.open_writer(std::time(nullptr));
      before = store.current()->rev;
    }
    const auto child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
      Store store({root, point});
      store.open();
      Commit(store, {lane, 1}, point);
      _exit(1);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 97);
    Store recovered({root, ""});
    recovered.open();
    const auto receipt =
        recovered.operation({lane, 1}, digest, std::time(nullptr));
    if (std::string_view(point) == "before_journal") {
      EXPECT_FALSE(receipt);
      EXPECT_EQ(recovered.current()->rev, before);
    } else {
      ASSERT_TRUE(receipt);
      EXPECT_EQ(receipt->bytes, "terminal outcome");
      EXPECT_EQ(recovered.current()->rev, before + 1);
    }
  }
}

TEST_F(OperationJournal, OldJournalFormatIsRefusedWithoutTruncation) {
  fs::write_atomic(root / ".tsugumi/document.journal", "TSJ1preserve me");
  Store store({root, ""});
  EXPECT_THROW(store.open(), fs::Error);
  EXPECT_EQ(fs::read_file_or_throw(root / ".tsugumi/document.journal"),
            "TSJ1preserve me");
}
}  // namespace
}  // namespace tsugumi
