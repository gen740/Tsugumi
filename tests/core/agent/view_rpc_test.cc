#include <future>
#include <memory>
#include <string>
#include <vector>

#include "project_fixture.hh"
#include "tsugumi/client/host_views.hh"
#include "tsugumi/util/crypto.hh"

namespace tsugumi::test {
namespace {

constexpr auto kViewSession = "eb560243-b30c-4510-aaac-1b2ad4fedf38";
constexpr auto kPng =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGMICAj4DwAD1AHw41"
    "0Q2gAAAABJRU5ErkJggg==";
constexpr auto kText = "日本語";

class ViewRpcFixture : public AgentProjectFixture {
 protected:
  auto Report(client::Connection& renderer, bool closed = false) -> Reply {
    return Call(
        renderer, H::RequestPayload::ViewPresentationRequest, [&](Builder& b) {
          const auto card = Tsugumi::Data::CreateCardPath(
              b, b.CreateVector(std::vector{b.CreateString("plot")}));
          const auto window = H::CreateViewWindow(
              b, b.CreateString(kViewSession), card, b.CreateString("preview"),
              b.CreateString("dawn"), 1, 640, 480, 2);
          return H::CreateViewPresentationRequest(b, b.CreateString(project_id),
                                                  window, closed)
              .Union();
        });
  }

  auto List() -> Reply {
    auto result = client::ListViews(*host, project_id, 5s).get();
    if (result.error) throw std::runtime_error(result.error.message);
    return {std::move(result.bytes)};
  }

  auto Complete(client::Connection& renderer,
                const H::ViewActionDispatchT& dispatch,
                const std::string& png = {}) -> Reply {
    return Call(renderer, H::RequestPayload::ViewActionCompleteRequest,
                [&](Builder& b) {
                  const auto bytes = b.CreateVector(
                      reinterpret_cast<const uint8_t*>(png.data()), png.size());
                  return H::CreateViewActionCompleteRequest(
                             b, b.CreateString(dispatch.request),
                             b.CreateString(dispatch.session), bytes)
                      .Union();
                });
  }
};

TEST_F(ViewRpcFixture, AgentListsSendsInputAndReceivesTheRendererPng) {
  StartProject();
  ModelServer model;
  Configure(model);
  const auto conversation = Conversation("view-tools");

  auto renderer = ConnectHost(true);
  ASSERT_TRUE(Report(*renderer).Code().empty());
  EXPECT_EQ(Report(*host).Code(), "permission_denied");
  const auto listed = List();
  const auto* windows = listed.HostValue()->value_as_ViewWindows();
  ASSERT_NE(windows, nullptr);
  ASSERT_EQ(windows->windows()->size(), 1u);
  EXPECT_EQ(windows->windows()->Get(0)->flags(), 2u);

  auto input = std::make_shared<std::promise<H::ViewActionDispatchT>>();
  auto capture = std::make_shared<std::promise<H::ViewActionDispatchT>>();
  auto input_ready = input->get_future();
  auto capture_ready = capture->get_future();
  renderer->OnEvent([input, capture](const client::Bytes& bytes) {
    const auto* event = H::GetMessage(bytes.data())->payload_as_Event();
    const auto* action = event ? event->value_as_ViewActionDispatch() : nullptr;
    if (!action) return;
    if (action->action_as_ViewText())
      input->set_value(std::move(
          *std::unique_ptr<H::ViewActionDispatchT>(action->UnPack())));
    if (action->action_as_ViewSnapshot())
      capture->set_value(std::move(
          *std::unique_ptr<H::ViewActionDispatchT>(action->UnPack())));
  });
  model.Queue(AnthropicFamilyTool("list", "views", "list", "{}"));
  model.Queue(AnthropicFamilyTool(
      "text", "views", "input",
      json::Encode(json::Members{
          {"session", Quote(kViewSession)},
          {"action_type", Quote("ViewText")},
          {"action", json::Encode(json::Members{{"text", Quote(kText)}})}})));
  model.Queue(AnthropicFamilyTool(
      "capture", "views", "snapshot",
      json::Encode(json::Members{{"session", Quote(kViewSession)}})));
  model.Queue(AnthropicText("View checked."));
  ASSERT_TRUE(
      Send(conversation, "Check the view's input and output.").Code().empty());

  ASSERT_EQ(input_ready.wait_for(5s), std::future_status::ready);
  const auto event = input_ready.get();
  EXPECT_EQ(event.project_id, project_id);
  EXPECT_EQ(event.session, kViewSession);
  ASSERT_NE(event.action.AsViewText(), nullptr);
  EXPECT_EQ(event.action.AsViewText()->text, kText);
  EXPECT_EQ(Complete(*host, event).Code(), "permission_denied");
  ASSERT_TRUE(Complete(*renderer, event).Code().empty());

  ASSERT_EQ(capture_ready.wait_for(5s), std::future_status::ready);
  const auto screenshot = capture_ready.get();
  std::string png;
  ASSERT_TRUE(crypto::base64_decode(kPng, png));
  ASSERT_TRUE(Complete(*renderer, screenshot, png).Code().empty());

  EXPECT_NE(WaitUntilIdle(conversation).find("View checked."),
            std::string::npos);
  const auto requests = model.Requests();
  ASSERT_EQ(requests.size(), 4u);
  EXPECT_NE(requests[1].find(kViewSession), std::string::npos);
  EXPECT_NE(requests.back().find("image/png"), std::string::npos);
  EXPECT_NE(requests.back().find(kPng), std::string::npos);

  ASSERT_TRUE(Report(*renderer, true).Code().empty());
  const auto closed = List();
  const auto* empty = closed.HostValue()->value_as_ViewWindows();
  ASSERT_NE(empty, nullptr);
  EXPECT_TRUE(!empty->windows() || empty->windows()->size() == 0u);
}

TEST_F(ViewRpcFixture, TheRendererDisconnectEndsAWaitingSnapshot) {
  StartProject();
  auto renderer = ConnectHost();
  ASSERT_TRUE(Report(*renderer).Code().empty());
  auto observed = std::make_shared<std::promise<void>>();
  auto delivered = observed->get_future();
  renderer->OnEvent([observed](const client::Bytes& bytes) {
    const auto* event = H::GetMessage(bytes.data())->payload_as_Event();
    if (event && event->value_as_ViewActionDispatch()) observed->set_value();
  });
  auto snapshot = client::SnapshotView(*host, project_id, kViewSession, 5s);
  ASSERT_EQ(delivered.wait_for(5s), std::future_status::ready);
  renderer->Close();
  auto result = snapshot.get();
  ASSERT_FALSE(result.error);
  EXPECT_EQ(Reply{std::move(result.bytes)}.Code(), "view_error");
  const auto closed = List();
  const auto* empty = closed.HostValue()->value_as_ViewWindows();
  ASSERT_NE(empty, nullptr);
  EXPECT_TRUE(!empty->windows() || empty->windows()->size() == 0u);
}

}  // namespace
}  // namespace tsugumi::test
