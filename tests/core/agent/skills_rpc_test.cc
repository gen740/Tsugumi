// Skill discovery and selection use only the public Host/Agent contracts.
#include <sys/stat.h>

#include <set>

#include "project_fixture.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::test {
namespace {
class SkillsFixture : public AgentProjectFixture {
 protected:
  auto Roots(const std::vector<std::string>& folders) -> Reply {
    const auto before =
        Call(*host, H::RequestPayload::SettingsGetRequest, [](Builder& b) {
          return H::CreateSettingsGetRequest(b, b.CreateString("desktop"))
              .Union();
        });
    std::string encoded = "[";
    for (const auto& folder : folders) {
      if (encoded.size() > 1) encoded += ',';
      encoded += Quote(folder);
    }
    encoded += ']';
    return Call(
        *host, H::RequestPayload::SettingsUpdateRequest, [&](Builder& b) {
          const auto item =
              H::CreateSetting(b, b.CreateString("agent.trustedFolders"),
                               b.CreateString(encoded));
          return H::CreateSettingsUpdateRequest(
                     b, b.CreateString("desktop"),
                     b.CreateString(before.HostValue()
                                        ->value_as_Settings()
                                        ->revision()
                                        ->str()),
                     b.CreateVector(std::vector{item}))
              .Union();
        });
  }
  auto Trust(const std::filesystem::path& path, bool web = false,
             H::ProjectTrustDecision decision =
                 H::ProjectTrustDecision::Unknown) -> H::ProjectTrustDecision {
    auto connection = web ? ConnectHost(true) : nullptr;
    const auto reply =
        Call(connection ? *connection : *host,
             H::RequestPayload::ProjectTrustRequest, [&](Builder& b) {
               return H::CreateProjectTrustRequest(
                          b, b.CreateString(path.string()), decision)
                   .Union();
             });
    if (!reply.Code().empty()) throw std::runtime_error(reply.Code());
    return reply.HostValue()->value_as_ProjectTrust()->decision();
  }
  auto Named(const auto* list, std::string_view name) {
    for (const auto* skill : *list)
      if (skill->name()->string_view() == name) return skill;
    throw std::runtime_error("Missing fixture Skill: " + std::string(name));
  }
  auto List(const std::vector<std::string>& folders = {}, bool project = true,
            bool websocket = false, const std::string& conversation = {})
      -> Reply {
    auto connection = websocket ? ConnectHost(true) : nullptr;
    return Call(
        connection ? *connection : *host, H::RequestPayload::SkillsListRequest,
        [&](Builder& b) {
          std::vector<flatbuffers::Offset<flatbuffers::String>> sources;
          for (const auto& folder : folders)
            sources.push_back(b.CreateString(folder));
          return H::CreateSkillsListRequest(
                     b,
                     project
                         ? b.CreateString((directory.path / "project").string())
                         : 0,
                     b.CreateVector(sources), b.CreateString(conversation))
              .Union();
        });
  }
  auto ScopedSend(const std::string& conversation, const std::string& turn)
      -> Reply {
    return Call(*human, A::RequestPayload::ConversationSendRequest,
                [&](Builder& b) {
                  A::ConversationSendRequestT request;
                  request.conversation_id = conversation;
                  request.turn_id = turn;
                  request.text = "$inspect";
                  request.selected_cards = {"selected"};
                  request.skill_names = {"inspect"};
                  request.scoped_skill = "inspect";
                  return A::ConversationSendRequest::Pack(b, &request).Union();
                });
  }
  auto AccessList(const std::string& conversation, const std::string& id = {})
      -> Reply {
    return Call(*human, A::RequestPayload::AccessListRequest, [&](Builder& b) {
      return A::CreateAccessListRequest(b, b.CreateString(conversation),
                                        id.empty() ? 0 : b.CreateString(id))
          .Union();
    });
  }
  auto WaitForSkillApproval(const std::string& conversation) -> std::string {
    // Agent RPC exposes status through snapshot reads.
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    do {
      const auto list = AccessList(conversation);
      const auto* requests = list.Value()->value_as_AccessList()->requests();
      if (requests)
        for (const auto* item : *requests)
          if (item->access() == Tsugumi::Service::Access::HelperScope &&
              item->state()->string_view() == "pending")
            return item->id()->str();
      std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("No pending Skill approval");
  }
  auto Approve(const std::string& id) -> Reply {
    return Call(
        *human, A::RequestPayload::AccessResolveRequest, [&](Builder& b) {
          return A::CreateAccessResolveRequest(b, b.CreateString(id), true)
              .Union();
        });
  }
  auto Resume(const std::string& conversation) -> Reply {
    return Call(*human, A::RequestPayload::ConversationActionRequest,
                [&](Builder& b) {
                  return A::CreateConversationActionRequest(
                             b, b.CreateString(conversation), A::Action::Resume)
                      .Union();
                });
  }
  auto CancelSkill(const std::string& conversation) -> Reply {
    return Call(*human, A::RequestPayload::ConversationCancelRequest,
                [&](Builder& b) {
                  return A::CreateConversationCancelRequest(
                             b, b.CreateString(conversation))
                      .Union();
                });
  }
  auto DirectSkill(ModelServer& model) -> std::string {
    Configure(model);
    StartProject();
    Write(directory.path / "project/skills/inspect.md",
          "---\nname: inspect\nscope: [read]\n---\nINSPECT_SKILL_SENTINEL");
    const auto conversation = Conversation("direct-skill");
    if (!List({}, true, false, conversation).Code().empty())
      throw std::runtime_error("Cannot register the Skill catalog");
    return conversation;
  }
};
TEST_F(SkillsFixture,
       TrustedFoldersResolveLinksAndUsePathComponentsOnBothTransports) {
  const auto root = directory.path / "trusted";
  const auto nested = root / "nested/deep/project";
  const auto outside = directory.path / "trusted-sibling/project";
  Write(root / "tsugumi.json", "{}");
  Write(nested / "tsugumi.json", "{}");
  Write(outside / "tsugumi.json", "{}");
  std::filesystem::create_directory_symlink(
      root, directory.path / "registration-link");
  std::filesystem::create_directory_symlink(outside, root / "escape");
  std::filesystem::create_directory_symlink(nested, directory.path / "entry");
  ASSERT_TRUE(
      Roots({(directory.path / "registration-link").string(), root.string()})
          .Code()
          .empty());
  const auto settings =
      Call(*host, H::RequestPayload::SettingsGetRequest, [](Builder& b) {
        return H::CreateSettingsGetRequest(b, b.CreateString("desktop"))
            .Union();
      });
  for (const auto* item : *settings.HostValue()->value_as_Settings()->values())
    if (item->key()->str() == "agent.trustedFolders")
      EXPECT_EQ(item->value_json()->str(),
                "[" + Quote(std::filesystem::canonical(root).string()) + "]");
  for (const auto web : {false, true}) {
    EXPECT_EQ(Trust(root, web), H::ProjectTrustDecision::Trusted);
    EXPECT_EQ(Trust(nested, web), H::ProjectTrustDecision::Trusted);
    EXPECT_EQ(Trust(directory.path / "entry", web),
              H::ProjectTrustDecision::Trusted);
    EXPECT_EQ(Trust(outside, web), H::ProjectTrustDecision::Unknown);
    EXPECT_EQ(Trust(root / "escape", web), H::ProjectTrustDecision::Unknown);
  }
  host.reset();
  human.reset();
  external.reset();
  service.Stop();
  StartHost();
  EXPECT_EQ(Trust(nested), H::ProjectTrustDecision::Trusted);
  const auto individual = root / "individual";
  Write(individual / "tsugumi.json", "{}");
  EXPECT_EQ(Trust(individual, false, H::ProjectTrustDecision::Trusted),
            H::ProjectTrustDecision::Trusted);
  ASSERT_TRUE(Roots({}).Code().empty());
  for (const auto web : {false, true}) {
    EXPECT_EQ(Trust(nested, web), H::ProjectTrustDecision::Unknown);
    EXPECT_EQ(Trust(individual, web), H::ProjectTrustDecision::Trusted);
  }
}

TEST_F(SkillsFixture, ReplacingARegisteredRootWithALinkDoesNotTrustItsTarget) {
  const auto root = directory.path / "trusted";
  const auto outside = directory.path / "outside";
  Write(root / "project/tsugumi.json", "{}");
  Write(outside / "project/tsugumi.json", "{}");
  ASSERT_TRUE(Roots({root.string()}).Code().empty());
  std::filesystem::rename(root, directory.path / "moved");
  std::filesystem::create_directory_symlink(outside, root);
  EXPECT_EQ(Trust(root / "project"), H::ProjectTrustDecision::Unknown);
}

TEST_F(SkillsFixture, ReplacingARegisteredDirectoryRequiresNewTrust) {
  const auto root = directory.path / "trusted";
  Write(root / "project/tsugumi.json", "{}");
  ASSERT_TRUE(Roots({root.string()}).Code().empty());
  std::filesystem::rename(root, directory.path / "saved");
  Write(root / "project/tsugumi.json", "{}");
  EXPECT_EQ(Trust(root / "project"), H::ProjectTrustDecision::Unknown);
  ASSERT_TRUE(Roots({}).Code().empty());
  ASSERT_TRUE(Roots({root.string()}).Code().empty());
  EXPECT_EQ(Trust(root / "project"), H::ProjectTrustDecision::Trusted);
}

TEST_F(SkillsFixture, FolderTrustIsAppliedAndRevokedAtTheModelBoundary) {
  ModelServer model;
  Configure(model);
  StartProject();
  Write(directory.path / "project/skills/automatic.md",
        "---\nalways: true\n---\nFOLDER_SKILL_SENTINEL");
  ASSERT_TRUE(Roots({directory.path.string()}).Code().empty());
  const auto conversation = Conversation("folder-trust");
  Grant(conversation);
  model.Queue(AnthropicText("Applied."));
  ASSERT_TRUE(Send(conversation, "Read this project.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  WaitUntilIdle(conversation);
  const auto request = model.Requests()[0];
  const auto at = request.find("FOLDER_SKILL_SENTINEL");
  ASSERT_NE(at, std::string::npos);
  EXPECT_EQ(request.find("FOLDER_SKILL_SENTINEL", at + 1), std::string::npos);
  ASSERT_TRUE(Roots({}).Code().empty());
  const auto next = Conversation("after-folder-removal");
  Grant(next);
  model.Queue(AnthropicText("No automatic project Skill."));
  ASSERT_TRUE(Send(next, "Read this project.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  WaitUntilIdle(next);
  EXPECT_EQ(model.Requests()[1].find("FOLDER_SKILL_SENTINEL"),
            std::string::npos);
}

TEST_F(SkillsFixture, TrustIsUnknownUntilAnsweredAndPersistsAcrossHostRestart) {
  Write(directory.path / "project/tsugumi.json", "{}");
  auto trust = [&](H::ProjectTrustDecision decision =
                       H::ProjectTrustDecision::Unknown) {
    return Call(*host, H::RequestPayload::ProjectTrustRequest, [&](Builder& b) {
      return H::CreateProjectTrustRequest(
                 b, b.CreateString((directory.path / "project").string()),
                 decision)
          .Union();
    });
  };
  EXPECT_EQ(trust().HostValue()->value_as_ProjectTrust()->decision(),
            H::ProjectTrustDecision::Unknown);
  EXPECT_EQ(trust(H::ProjectTrustDecision::Untrusted)
                .HostValue()
                ->value_as_ProjectTrust()
                ->decision(),
            H::ProjectTrustDecision::Untrusted);
  host.reset();
  human.reset();
  external.reset();
  service.Stop();
  StartHost();
  EXPECT_EQ(trust().HostValue()->value_as_ProjectTrust()->decision(),
            H::ProjectTrustDecision::Untrusted);
  EXPECT_EQ(trust().HostValue()->value_as_ProjectTrust()->decision(),
            H::ProjectTrustDecision::Untrusted);
  EXPECT_EQ(trust(H::ProjectTrustDecision::Trusted)
                .HostValue()
                ->value_as_ProjectTrust()
                ->decision(),
            H::ProjectTrustDecision::Trusted);
}

TEST_F(SkillsFixture, TrustFollowsTheFolderIdentityAndSurvivesOrdinarySaves) {
  const auto path = directory.path / "project";
  Write(path / "tsugumi.json", "{}");
  const auto trust = [&](H::ProjectTrustDecision decision =
                             H::ProjectTrustDecision::Unknown) {
    const auto reply =
        Call(*host, H::RequestPayload::ProjectTrustRequest, [&](Builder& b) {
          return H::CreateProjectTrustRequest(b, b.CreateString(path.string()),
                                              decision)
              .Union();
        });
    if (!reply.Code().empty()) throw std::runtime_error(reply.Code());
    return reply.HostValue()->value_as_ProjectTrust()->decision();
  };
  const auto trusted = H::ProjectTrustDecision::Trusted;
  const auto unknown = H::ProjectTrustDecision::Unknown;
  EXPECT_EQ(trust(trusted), trusted);
  Write(path / "tsugumi.json", R"({"name":"ordinary save"})");
  EXPECT_EQ(trust(), trusted);
  Write(path / "saved.json", R"({"name":"atomic save"})");
  std::filesystem::rename(path / "saved.json", path / "tsugumi.json");
  Write(path / "skills/helpers.md", "updated skill");
  EXPECT_EQ(trust(), trusted);
  const auto moved = directory.path / "moved";
  std::filesystem::rename(path, moved);
  std::filesystem::copy(moved, path, std::filesystem::copy_options::recursive);
  EXPECT_EQ(trust(), unknown);
  EXPECT_EQ(trust(), unknown);
  EXPECT_EQ(trust(trusted), trusted);
  std::filesystem::rename(path, directory.path / "copy");
  Write(path / "tsugumi.json", R"({"name":"another checkout"})");
  EXPECT_EQ(trust(), unknown);
  EXPECT_EQ(trust(H::ProjectTrustDecision::Untrusted),
            H::ProjectTrustDecision::Untrusted);
  std::filesystem::rename(path, directory.path / "checkout");
  std::filesystem::rename(moved, path);
  EXPECT_EQ(trust(), unknown);
}

TEST_F(SkillsFixture, AutomaticSkillsReachTheModelOnlyFromTrustedProjects) {
  ModelServer model;
  Configure(model);
  Write(directory.path / "project/skills/helpers.md", "PROJECT_HELPERS_POLICY");
  Write(directory.path / "project/skills/automatic.md",
        "---\nalways: true\n---\nPROJECT_AUTOMATIC_POLICY");
  StartProject();
  const auto untrusted = Conversation("untrusted-skills");
  Grant(untrusted);
  model.Queue(AnthropicText("Untrusted project opened."));
  ASSERT_TRUE(Send(untrusted, "Read this project.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  WaitUntilIdle(untrusted);
  EXPECT_NE(model.Requests()[0].find("[Skill helpers]"), std::string::npos);
  EXPECT_EQ(model.Requests()[0].find("PROJECT_HELPERS_POLICY"),
            std::string::npos);
  EXPECT_EQ(model.Requests()[0].find("PROJECT_AUTOMATIC_POLICY"),
            std::string::npos);
  model.Queue(AnthropicText("The explicitly named Skill applied."));
  ASSERT_TRUE(
      Call(*external, A::RequestPayload::ConversationSendRequest,
           [&](Builder& b) {
             return A::CreateConversationSendRequest(
                        b, b.CreateString(untrusted),
                        b.CreateString("explicit-skill"),
                        b.CreateString("$helpers"), 0, 0,
                        b.CreateVector(std::vector{b.CreateString("helpers")}))
                 .Union();
           })
          .Code()
          .empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  WaitUntilIdle(untrusted);
  EXPECT_NE(model.Requests()[1].find("PROJECT_HELPERS_POLICY"),
            std::string::npos);

  ASSERT_TRUE(
      Call(*host, H::RequestPayload::ProjectTrustRequest,
           [&](Builder& b) {
             return H::CreateProjectTrustRequest(
                        b,
                        b.CreateString((directory.path / "project").string()),
                        H::ProjectTrustDecision::Trusted)
                 .Union();
           })
          .Code()
          .empty());
  const auto trusted = Conversation("trusted-skills");
  Grant(trusted);
  model.Queue(AnthropicText("Trusted Skills applied."));
  ASSERT_TRUE(Send(trusted, "Read this project.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(3));
  WaitUntilIdle(trusted);

  EXPECT_NE(model.Requests()[2].find("PROJECT_HELPERS_POLICY"),
            std::string::npos);
  EXPECT_NE(model.Requests()[2].find("PROJECT_AUTOMATIC_POLICY"),
            std::string::npos);
}

TEST_F(SkillsFixture, AutomaticScopedSkillsAreRefusedBeforeAnyModelRequest) {
  ModelServer model;
  Configure(model);
  Write(directory.path / "project/skills/automatic.md",
        "---\nalways: true\nscope: [read]\n---\nWRITE_SENTINEL");
  StartProject();
  ASSERT_TRUE(
      Call(*host, H::RequestPayload::ProjectTrustRequest,
           [&](Builder& b) {
             return H::CreateProjectTrustRequest(
                        b,
                        b.CreateString((directory.path / "project").string()),
                        H::ProjectTrustDecision::Trusted)
                 .Union();
           })
          .Code()
          .empty());
  const auto list = List();
  const auto* skills = list.HostValue()->value_as_SkillsList()->skills();
  bool found = false;
  for (const auto* skill : *skills)
    if (skill->name()->str() == "automatic") {
      found = true;
      EXPECT_EQ(skill->status()->str(), "invalid_scope");
    }
  ASSERT_TRUE(found);
  const auto conversation = Conversation("invalid-automatic-scope");
  Grant(conversation);
  const auto sent = Send(conversation, "Read this project.");
  ASSERT_TRUE(sent.Code().empty());
  WaitUntilIdle(conversation);
  const auto events = Events(conversation);
  const auto* page = events.Value()->value_as_EventsPage();
  ASSERT_NE(page, nullptr);
  EXPECT_TRUE(std::ranges::any_of(*page->events(), [](const auto* entry) {
    return entry->event()->kind()->str() == "error";
  }));
  EXPECT_TRUE(model.Requests().empty());
}

TEST_F(SkillsFixture, RevokingTrustStopsFurtherAutomaticProjectSkillRequests) {
  ModelServer model;
  Configure(model);
  Write(directory.path / "project/skills/helpers.md", "PROJECT_POLICY");
  StartProject();
  auto decide = [&](H::ProjectTrustDecision decision) {
    return Call(*host, H::RequestPayload::ProjectTrustRequest, [&](Builder& b) {
      return H::CreateProjectTrustRequest(
                 b, b.CreateString((directory.path / "project").string()),
                 decision)
          .Union();
    });
  };
  ASSERT_TRUE(decide(H::ProjectTrustDecision::Trusted).Code().empty());
  const auto conversation = Conversation("revoke-skill-trust");
  Grant(conversation);
  model.Queue(AnthropicTool("list-demos", "demo_list", "{}"), true);
  ASSERT_TRUE(
      Send(conversation, "Read the available examples.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  ASSERT_TRUE(decide(H::ProjectTrustDecision::Untrusted).Code().empty());
  model.Release();
  WaitUntilIdle(conversation);
  const auto events = Events(conversation);
  const auto* page = events.Value()->value_as_EventsPage();
  ASSERT_NE(page, nullptr);
  EXPECT_TRUE(std::ranges::any_of(*page->events(), [](const auto* entry) {
    return entry->event()->kind()->str() == "error";
  }));
  EXPECT_EQ(model.Requests().size(), 1u);
}

TEST_F(SkillsFixture, NoProjectStillHasDemoAndMissingFoldersHaveStatus) {
  const auto empty = List({}, false);
  ASSERT_TRUE(empty.Code().empty());
  EXPECT_NE(Named(empty.HostValue()->value_as_SkillsList()->skills(), "demo"),
            nullptr);
  EXPECT_EQ(ListSize(empty.HostValue()->value_as_SkillsList()->folders()), 0u);
  const auto missing =
      List({(directory.path / "missing").string()}, false, true);
  const auto* result = missing.HostValue()->value_as_SkillsList();
  ASSERT_EQ(ListSize(result->folders()), 1u);
  EXPECT_EQ(result->folders()->Get(0)->status()->str(), "unreadable");
}
TEST_F(SkillsFixture, FrontMatterAndFallbackNamesAreAssociatedWithTheirFiles) {
  const auto root = directory.path / "project/skills";
  Write(root / "a.md",
        "---\nname: Card review!\ndescription: Review "
        "outputs\n---\nBODY_SENTINEL");
  Write(root / "B/SKILL.md", "# Explain runs #\nRead outputs");
  Write(root / "c.MD", "---\nname: unfinished\n# No closing delimiter");
  Write(root / "notes.txt", "ignored");
  const auto reply = List();
  const auto* list = reply.HostValue()->value_as_SkillsList()->skills();
  EXPECT_EQ(Named(list, "demo")->path()->str(), "builtin:demo");
  std::set<std::string> custom;
  for (const auto* skill : *list)
    if (skill->path()->str().starts_with(root.string()))
      custom.insert(skill->name()->str());
  EXPECT_EQ(custom, (std::set<std::string>{"B", "Card-review-", "c"}));
  EXPECT_EQ(Named(list, "B")->path()->str(), (root / "B/SKILL.md").string());
  EXPECT_EQ(Named(list, "B")->description()->str(), "Explain runs");
  EXPECT_EQ(Named(list, "Card-review-")->path()->str(),
            (root / "a.md").string());
  EXPECT_EQ(Named(list, "Card-review-")->description()->str(),
            "Review outputs");
  EXPECT_EQ(Named(list, "c")->path()->str(), (root / "c.MD").string());
  EXPECT_EQ(Named(list, "c")->status()->str(), "invalid_scope");
  EXPECT_EQ(
      std::string(reply.bytes.begin(), reply.bytes.end()).find("BODY_SENTINEL"),
      std::string::npos);
}
TEST_F(SkillsFixture, SwiftUnicodeNamesWhitespaceAndCRLFBehavior) {
  const auto root = directory.path / "project/skills";
  Write(
      root / "a.md",
      "---\nname: \u00a0e\u0301-Ⅳ-¼-👨‍👩‍👦\u200b\ndescription: "
      "\u00a0Review\u200b\n---\nbody");
  Write(root / "b.md", "---\r\nname: ignored\r\n---\r\nbody");
  Write(root / "c.md",
        "---\nname: bad" + std::string("\xff", 1) + "name\n---\nbody");
  const auto reply = List();
  const auto* skills = reply.HostValue()->value_as_SkillsList()->skills();
  EXPECT_EQ(Named(skills, "é-Ⅳ-¼--")->name()->str(), "é-Ⅳ-¼--");
  EXPECT_EQ(Named(skills, "é-Ⅳ-¼--")->description()->str(), "Review");
  EXPECT_EQ(Named(skills, "b")->name()->str(), "b");
  EXPECT_EQ(Named(skills, "b")->description()->str(),
            "---\r\nname: ignored\r\n---\r\nbody");
  EXPECT_EQ(Named(skills, "bad-name")->name()->str(), "bad-name");
}

TEST_F(SkillsFixture, StrictSizeBoundaryAndUnreadableFilesAndFolders) {
  const auto root = directory.path / "project/skills";
  Write(root / "below.md", std::string(512 * 1024 - 1, 'x'));
  Write(root / "exact.md", std::string(512 * 1024, 'x'));
  Write(root / "over.md", std::string(512 * 1024 + 1, 'x'));
  Write(root / "unreadable.md", "private");
  const auto folder = directory.path / "locked";
  Write(folder / "skill.md", "private");
  ASSERT_EQ(::chmod((root / "unreadable.md").c_str(), 0000), 0);
  ASSERT_EQ(::chmod(folder.c_str(), 0000), 0);
  const auto reply = List({folder.string()});
  ::chmod((root / "unreadable.md").c_str(), 0600);
  ::chmod(folder.c_str(), 0700);
  ASSERT_TRUE(reply.Code().empty());
  const auto* result = reply.HostValue()->value_as_SkillsList();
  const auto* list = result->skills();
  ASSERT_GE(ListSize(list), 7u);
  EXPECT_EQ(list->Get(4)->status()->str(), "ready");
  EXPECT_EQ(list->Get(5)->status()->str(), "too_large");
  EXPECT_EQ(list->Get(6)->status()->str(), "too_large");
  // Root can read the mode-0000 file and traverse the locked folder.
  if (::geteuid() != 0) {
    ASSERT_EQ(ListSize(list), 8u);
    EXPECT_EQ(list->Get(7)->status()->str(), "unreadable");
    EXPECT_EQ(result->folders()->Get(1)->status()->str(), "unreadable");
  }
}
TEST_F(SkillsFixture,
       CollisionsRemainInSourceOrderAndDirectorySymlinksAreNotFollowed) {
  const auto root = directory.path / "project/skills";
  const auto first = directory.path / "first";
  const auto second = directory.path / "second";
  for (const auto& dir : {root, first, second})
    Write(dir / "same.md", "---\nname: same\n---\nbody");
  Write(first / "real/SKILL.md", "# linked");
  std::filesystem::create_directory_symlink(first / "real",
                                            first / "directory-link");
  std::filesystem::create_symlink(first / "same.md", first / "file-link.md");
  const auto reply = List({first.string(), second.string()});
  const auto* list = reply.HostValue()->value_as_SkillsList()->skills();
  ASSERT_EQ(ListSize(list), 9u);
  EXPECT_EQ(list->Get(4)->source_folder()->str(), root.string());
  EXPECT_EQ(list->Get(5)->path()->str(), (first / "file-link.md").string());
  EXPECT_EQ(list->Get(6)->name()->str(), "real");
  EXPECT_EQ(list->Get(7)->name()->str(), "same");
  EXPECT_EQ(list->Get(8)->source_folder()->str(), second.string());
}

TEST_F(SkillsFixture,
       NamesSelectEveryCollisionAndSourcesBelongToTheConversation) {
  ModelServer model;
  model.Queue(AnthropicText("First selected skills received."));
  model.Queue(AnthropicText("Second selected skills received."));
  Configure(model);
  const auto first = directory.path / "project/first";
  const auto second = directory.path / "project/second";
  Write(first / "a.md", "---\nname: shared\n---\nFIRST_A_SENTINEL");
  Write(first / "b.md", "---\nname: shared\n---\nFIRST_B_SENTINEL");
  Write(second / "a.md", "---\nname: shared\n---\nSECOND_SENTINEL");
  StartProject();
  const auto one = Conversation("first-catalog");
  const auto two = Conversation("second-catalog");
  ASSERT_TRUE(List({first.string()}, true, false, one).Code().empty());
  ASSERT_TRUE(List({second.string()}, true, false, two).Code().empty());
  const auto inside = List({first.string()}, true, false, one);
  ASSERT_EQ(inside.HostValue()
                ->value_as_SkillsList()
                ->skills()
                ->Get(4)
                ->attachment_folder(),
            nullptr);
  Grant(one);
  Grant(two);
  const auto send = [&](const std::string& conversation) {
    return Call(
        *external, A::RequestPayload::ConversationSendRequest, [&](Builder& b) {
          return A::CreateConversationSendRequest(
                     b, b.CreateString(conversation),
                     b.CreateString("selected"),
                     b.CreateString("Use the selected skill"), 0, 0,
                     b.CreateVector(std::vector{b.CreateString("shared")}))
              .Union();
        });
  };
  ASSERT_TRUE(send(one).Code().empty());
  ASSERT_TRUE(model.WaitForRequests(1)) << Events(one).View();
  WaitUntilIdle(one);
  ASSERT_TRUE(send(two).Code().empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  WaitUntilIdle(two);
  const auto requests = model.Requests();
  EXPECT_NE(requests[0].find("FIRST_A_SENTINEL"), std::string::npos);
  EXPECT_NE(requests[0].find("FIRST_B_SENTINEL"), std::string::npos);

  EXPECT_EQ(requests[0].find("SECOND_SENTINEL"), std::string::npos);
  EXPECT_NE(requests[1].find("SECOND_SENTINEL"), std::string::npos);
  EXPECT_EQ(requests[1].find("FIRST_A_SENTINEL"), std::string::npos);
}

TEST_F(AgentProjectFixture,
       CustomSkillContentsNeedPermissionBeforeEnteringTheModelPrompt) {
  ModelServer model;
  model.Queue(AnthropicText("I received the authorized skill."));
  Configure(model);
  Write(directory.path / "project/skills/private-skill.md",
        "PRIVATE_SKILL_SENTINEL\nFollow the user's selected skill.");
  StartProject();
  const auto discovery =
      Call(*host, H::RequestPayload::SkillsListRequest, [&](Builder& b) {
        return H::CreateSkillsListRequest(
                   b, b.CreateString((directory.path / "project").string()))
            .Union();
      });
  ASSERT_TRUE(discovery.Code().empty());
  const auto conversation = Conversation("custom-skill-permission");
  auto selected_skill = [&] {
    return Call(
        *external, A::RequestPayload::ConversationSendRequest, [&](Builder& b) {
          const auto skills =
              b.CreateVector(std::vector{b.CreateString("private-skill")});
          return A::CreateConversationSendRequest(
                     b, b.CreateString(conversation),
                     b.CreateString("selected-skill"),
                     b.CreateString("Use my selected skill."), 0, 0, skills)
              .Union();
        });
  };
  ASSERT_TRUE(selected_skill().Code().empty());
  EXPECT_FALSE(model.WaitForRequests(1, 100ms));
  // Agent exposes these permission transitions through snapshot reads only.
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (Events(conversation).View().find("permission") == std::string::npos &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  ASSERT_NE(Events(conversation).View().find("permission"), std::string::npos);
  external.reset();
  human.reset();
  service.Stop(true);
  StartHost();
  const auto interrupted = Get(conversation).View();
  EXPECT_TRUE(JsonBool(interrupted, "/resumable"));
  EXPECT_TRUE(JsonBool(interrupted, "/continuable"));
  ASSERT_TRUE(Call(*external, A::RequestPayload::ConversationActionRequest,
                   [&](Builder& b) {
                     return A::CreateConversationActionRequest(
                                b, b.CreateString(conversation),
                                A::Action::Resume)
                         .Union();
                   })
                  .Code()
                  .empty());
  EXPECT_FALSE(model.WaitForRequests(1, 100ms));
  const auto grant = Grant(conversation);
  ASSERT_TRUE(model.WaitForRequests(1));
  const auto completed = WaitUntilIdle(conversation);
  EXPECT_FALSE(JsonBool(completed, "/resumable"));
  EXPECT_FALSE(JsonBool(completed, "/continuable"));
  EXPECT_EQ(model.Requests().size(), 1u);
  EXPECT_NE(model.Requests()[0].find("PRIVATE_SKILL_SENTINEL"),
            std::string::npos);
  Revoke(grant);
  EXPECT_EQ(Send(conversation, "Continue with that skill.", "turn-2").Code(),
            "permission_denied");
}
TEST_F(SkillsFixture, DirectSkillStartsOneHelperAfterOneExactScopePrompt) {
  ModelServer model;
  model.Queue(AnthropicTool(
      "try-write", "project_save",
      R"({"base_rev":0,"changes":[{"value_type":"RemoveDefinition","value":{"id":"selected"}}]})"));
  model.Queue(AnthropicText(
      "Read only: [source](card:selected?file=src%2Fmain.py&lines=1-2) uses a "
      "clock."));
  const auto conversation = DirectSkill(model);
  ASSERT_TRUE(ScopedSend(conversation, "direct-turn").Code().empty());
  const auto approval = WaitForSkillApproval(conversation);
  EXPECT_TRUE(model.Requests().empty());
  ASSERT_TRUE(Approve(approval).Code().empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  WaitUntilIdle(conversation);
  EXPECT_EQ(Get(conversation).Code(), "permission_denied");
  const auto snapshot = Get(conversation, true);
  const auto* helpers = snapshot.Value()
                            ->value_as_ConversationResult()
                            ->conversation()
                            ->helpers();
  ASSERT_EQ(ListSize(helpers), 1u);
  namespace J = ::tsugumi::json;
  const auto scope = J::Object(helpers->Get(0)->spec_json()->str());
  const auto readable = J::Array(J::Get(scope, "read_cards"));
  ASSERT_EQ(readable.size(), 1u);
  EXPECT_EQ(J::String(readable.front()), "selected");
  EXPECT_TRUE(J::Array(J::Get(scope, "write_cards")).empty());
  EXPECT_EQ(helpers->Get(0)->state()->str(), "completed");
  EXPECT_NE(helpers->Get(0)->result_json(), nullptr);
  EXPECT_EQ(model.Requests().size(),
            2u);  // Only the helper; no parent model turn.
  EXPECT_NE(model.Requests()[0].find("INSPECT_SKILL_SENTINEL"),
            std::string::npos);
  EXPECT_NE(model.Requests()[1].find(R"("is_error":true)"), std::string::npos);
  const auto access =
      Call(*human, A::RequestPayload::AccessListRequest, [&](Builder& b) {
        return A::CreateAccessListRequest(b, b.CreateString(conversation))
            .Union();
      });
  EXPECT_EQ(ListSize(access.Value()->value_as_AccessList()->grants()), 0u);
  EXPECT_EQ(ListSize(access.Value()->value_as_AccessList()->requests()), 1u);
  model.Queue(AnthropicText("Second scoped result."));
  ASSERT_TRUE(ScopedSend(conversation, "second-turn").Code().empty());
  const auto second = WaitForSkillApproval(conversation);
  EXPECT_NE(second, approval);
  ASSERT_TRUE(Approve(second).Code().empty());
  WaitUntilIdle(conversation);
  ASSERT_EQ(model.Requests().size(), 3u);
  EXPECT_EQ(model.Requests().back().find("uses a clock"), std::string::npos);
  EXPECT_EQ(
      Send(conversation, "Use the previous report.", "normal-turn").Code(),
      "permission_denied");
  Grant(conversation);
  EXPECT_TRUE(Get(conversation).Code().empty());
}
TEST_F(SkillsFixture, CancellingBeforeOrAfterApprovalInvalidatesTheSkillTurn) {
  ModelServer model;
  const auto conversation = DirectSkill(model);
  for (const bool approved : {false, true}) {
    ASSERT_TRUE(ScopedSend(conversation, approved ? "allowed" : "pending")
                    .Code()
                    .empty());
    const auto approval = WaitForSkillApproval(conversation);
    if (approved) {
      model.Queue(AnthropicText("Cancelled answer."), true);
      ASSERT_TRUE(Approve(approval).Code().empty());
      ASSERT_TRUE(model.WaitForRequests(1));
    }
    ASSERT_TRUE(CancelSkill(conversation).Code().empty());
    if (approved) model.Release();
    const auto idle = WaitUntilIdle(conversation);
    EXPECT_FALSE(JsonBool(idle, "/resumable"));
    const auto access = AccessList(conversation, approval);
    const auto* items = access.Value()->value_as_AccessList()->requests();
    ASSERT_EQ(ListSize(items), 1u);
    EXPECT_EQ(items->Get(0)->state()->string_view(), "cancelled");
    EXPECT_EQ(Approve(approval).Code(), "conflict");
    EXPECT_EQ(Resume(conversation).Code(), "permission_denied");
  }
  EXPECT_EQ(model.Requests().size(), 1u);
}

TEST_F(SkillsFixture, InterruptedSkillRetainsApprovalAndCommitsItsAnswerOnce) {
  ModelServer model;
  const auto conversation = DirectSkill(model);
  ASSERT_TRUE(ScopedSend(conversation, "recovered").Code().empty());
  const auto approval = WaitForSkillApproval(conversation);
  service.Stop(true);
  StartHost();
  EXPECT_TRUE(JsonBool(Get(conversation, true).View(), "/resumable"));
  // Approval can be answered on an interrupted turn before resuming it.
  ASSERT_TRUE(Approve(approval).Code().empty());
  model.Queue(AnthropicText("RECOVERED_SKILL_ANSWER"));
  ASSERT_TRUE(Resume(conversation).Code().empty());
  const auto idle = WaitUntilIdle(conversation);
  EXPECT_FALSE(JsonBool(idle, "/resumable"));
  EXPECT_EQ(Resume(conversation).Code(), "permission_denied");
  const auto completed = Get(conversation, true);
  const auto* messages = completed.Value()
                             ->value_as_ConversationResult()
                             ->conversation()
                             ->messages();
  ASSERT_EQ(ListSize(messages), 2u);
  EXPECT_EQ(model.Requests().size(), 1u);
}
}  // namespace
}  // namespace tsugumi::test
