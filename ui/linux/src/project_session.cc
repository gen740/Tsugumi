#include "project_session.hh"

#include <glib.h>

#include <map>
#include <utility>

#include "main_thread.hh"
#include "strings.hh"

namespace tsugumi::linux_ui {
namespace H = Tsugumi::Host;
namespace R = Tsugumi::RPC;
namespace D = Tsugumi::Data;

namespace {

auto Str(const flatbuffers::String* text) -> std::string {
  return text ? text->str() : std::string();
}

// A top-level Card's id, or "" for a Card inside a Card in Card.
auto TopLevel(const D::CardPath* path) -> std::string {
  if (!path || !path->segments() || path->segments()->size() != 1) return {};
  return path->segments()->Get(0)->str();
}

auto CardPath(const std::string& card) -> std::unique_ptr<D::CardPathT> {
  auto path = std::make_unique<D::CardPathT>();
  path->segments = {card};
  return path;
}

auto OperationId() -> std::string {
  gchar* id = g_uuid_string_random();
  std::string out = id;
  g_free(id);
  return out;
}

}  // namespace

ProjectSession::ProjectSession(std::shared_ptr<HostLink> host, std::string path)
    : host_(std::move(host)), path_(std::move(path)) {}

ProjectSession::~ProjectSession() {
  if (host_listener_ != 0) host_->Unlisten(host_listener_);
}

template <class T, class Value>
auto ProjectSession::ProjectCall(Value value,
                                 std::function<void(Reply<T>)> done) -> void {
  if (!connection_) return;
  linux_ui::Call<ProjectProtocol, T>(*connection_, std::move(value),
                                     std::move(done));
}

auto ProjectSession::running() const -> bool {
  switch (run_state_) {
    case D::RunState::queued:
    case D::RunState::preparing:
    case D::RunState::running:
    case D::RunState::cancelling:
      return true;
    default:
      return false;
  }
}

auto ProjectSession::CardState(const std::string& card) const -> D::CardState {
  const auto found = states_.find(card);
  return found == states_.end() ? D::CardState::unspecified : found->second;
}

auto ProjectSession::Changed() -> void {
  if (on_changed) on_changed();
}

auto ProjectSession::Fail(std::string message, std::string detail) -> void {
  phase_ = Phase::kFailed;
  message_ = std::move(message);
  detail_ = std::move(detail);
  connection_.reset();
  Changed();
}

auto ProjectSession::Open() -> void {
  const std::weak_ptr<ProjectSession> weak = weak_from_this();
  phase_ = Phase::kStarting;
  message_ = L("起動しています");
  Changed();
  // Progress of this project's environment while it starts.
  host_listener_ = host_->Listen([weak](const H::Event& event) {
    const auto self = weak.lock();
    if (!self || self->phase_ != Phase::kStarting) return;
    if (const auto* progress = event.value_as_EnvironmentProgress();
        progress && progress->project()->str() == self->path_) {
      // hostd's stage names are English; words for them are each UI's
      // (presentation), and this UI shows the name as it comes.
      self->detail_ = progress->stage()->str();
      self->Changed();
    }
  });
  // hostd remembers the folder and names it as every client does.
  H::HostRecentRememberRequestT remember;
  remember.path = path_;
  host_->Call<H::RecentProject>(
      std::move(remember), [weak](const Reply<H::RecentProject>& reply) {
        const auto self = weak.lock();
        if (!self) return;
        if (!reply.ok())
          return self->Fail(L("プロジェクトを開けませんでした"), reply.error);
        self->path_ = reply.value->path()->str();
        self->title_ = reply.value->name()->str();
        self->StartEnvironment();
      });
}

auto ProjectSession::StartEnvironment() -> void {
  const std::weak_ptr<ProjectSession> weak = weak_from_this();
  message_ = L("このプロジェクトの tsugumid を起動しています");
  Changed();
  H::EnvironmentRequestT request;
  request.project = path_;
  request.action = H::EnvironmentAction::Start;
  // The reply comes when the environment is ready or has failed.
  host_->Call<H::EnvironmentState>(
      std::move(request), [weak](const Reply<H::EnvironmentState>& reply) {
        const auto self = weak.lock();
        if (!self) return;
        if (!reply.ok())
          return self->Fail(L("実行環境を起動できませんでした"), reply.error);
        const auto& state = *reply.value;
        if (state.phase() != H::EnvironmentPhase::Ready) {
          std::string detail = Str(state.message());
          for (const auto* more : {state.recovery(), state.log()})
            if (more && more->size() > 0) detail += "\n" + more->str();
          return self->Fail(L("実行環境を起動できませんでした"), detail);
        }
        // On this host the project's own socket; its WebSocket listener is
        // for pages, which send an Origin.
        const auto socket = Str(state.unix_socket());
        self->Connect(socket.empty() ? Str(state.endpoint()) : "unix:" + socket,
                      Str(state.token()));
      });
}

auto ProjectSession::Connect(const std::string& endpoint,
                             const std::string& token) -> void {
  const std::weak_ptr<ProjectSession> weak = weak_from_this();
  message_ = L("tsugumid の応答を待っています");
  detail_.clear();
  Changed();
  client::Options options;
  options.protocol = client::Protocol::Project;
  options.endpoint = endpoint;
  connection_ = std::make_shared<client::Connection>(options);
  const auto* identity = connection_.get();
  connection_->OnEvent([weak](client::Bytes bytes) {
    Post([weak, bytes = std::move(bytes)] {
      const auto self = weak.lock();
      if (!self) return;
      const auto* message = R::GetMessage(bytes.data());
      if (const auto* notification = message->payload_as_Notification())
        self->Notified(*notification);
    });
  });
  connection_->OnClosed([weak, identity](const client::Error& error) {
    Post([weak, identity, error] {
      const auto self = weak.lock();
      if (self && self->connection_.get() == identity)
        self->Fail(L("tsugumid との接続が切れました"), error.message);
    });
  });
  connection_->Start([weak, identity, token](const client::Error& error) {
    Post([weak, identity, token, error] {
      const auto self = weak.lock();
      if (!self || self->connection_.get() != identity) return;
      if (error)
        return self->Fail(L("tsugumid に接続できませんでした"), error.message);
      R::SessionHelloRequestT hello;
      hello.token = token;
      self->ProjectCall<R::HelloResult>(
          std::move(hello), [weak](const Reply<R::HelloResult>& reply) {
            const auto self = weak.lock();
            if (!self) return;
            if (!reply.ok())
              return self->Fail(L("tsugumid に接続できませんでした"),
                                reply.error);
            self->Reopen();
          });
    });
  });
}

// Opens the project again for a fresh snapshot: after connecting, when the
// document changed (DocPatched) and when a notification was missed.
auto ProjectSession::Reopen() -> void {
  if (reopening_) {
    reopen_again_ = true;
    return;
  }
  reopening_ = true;
  const std::weak_ptr<ProjectSession> weak = weak_from_this();
  ProjectCall<R::OpenResult>(
      R::ProjectOpenRequestT{}, [weak](const Reply<R::OpenResult>& reply) {
        const auto self = weak.lock();
        if (!self) return;
        self->reopening_ = false;
        if (!reply.ok())
          return self->Fail(L("プロジェクトを開けませんでした"), reply.error);
        self->Opened(*reply.value);
        if (self->reopen_again_) {
          self->reopen_again_ = false;
          self->Reopen();
        }
      });
}

auto ProjectSession::Opened(const R::OpenResult& result) -> void {
  subscription_ = result.subscription_id()->str();
  seq_ = result.seq().value_or(1);
  const auto& snapshot = *result.snapshot();
  rev_ = snapshot.rev().value_or(0);
  // The core's first error about each top-level Card.
  std::map<std::string, std::string, std::less<>> diagnostics;
  for (const auto* resolved : *snapshot.resolution()) {
    const auto* segments = resolved->card()->segments();
    if (segments->size() != 1) continue;
    for (const auto* d : *resolved->diagnostics())
      if (d->severity() != D::Severity::warning) {
        diagnostics.emplace(segments->Get(0)->str(), d->message()->str());
        break;
      }
  }
  std::vector<Definition> definitions;
  for (const auto* definition : *snapshot.definitions()) {
    const auto id = definition->id()->str();
    const auto diagnostic = diagnostics.find(id);
    definitions.push_back(
        {id, definition->card_json()->str(), Str(definition->meta_json()),
         diagnostic != diagnostics.end() ? diagnostic->second : ""});
  }
  SetRun(*snapshot.run());
  if (const auto name = ProjectTitle(snapshot.project_json()->str(), "");
      !name.empty())
    title_ = name;
  CompleteLayout(snapshot.project_json()->str(), snapshot.layout_json()->str(),
                 std::move(definitions));
  ReadInvocations();
}

// Places for Cards without a saved one come from hostd, the same on every
// client; nothing is saved.
auto ProjectSession::CompleteLayout(std::string project_json,
                                    std::string layout_json,
                                    std::vector<Definition> definitions)
    -> void {
  const std::weak_ptr<ProjectSession> weak = weak_from_this();
  H::LayoutCompleteRequestT request;
  request.project_json = project_json;
  request.layout_json = layout_json;
  request.definitions_json = CompletionDefinitions(definitions);
  const auto rev = rev_;
  host_->Call<H::LayoutValidated>(
      std::move(request), [weak, rev, project_json = std::move(project_json),
                           definitions = std::move(definitions)](
                              const Reply<H::LayoutValidated>& reply) {
        const auto self = weak.lock();
        if (!self || self->rev_ != rev) return;
        if (!reply.ok())
          return self->Fail(L("プロジェクトを開けませんでした"), reply.error);
        self->scene_ =
            BuildScene(project_json, reply.value->json()->str(), definitions);
        self->phase_ = Phase::kOpen;
        self->message_.clear();
        self->detail_.clear();
        self->Changed();
      });
}

// The state the Canvas shows is each UI's to derive (presentation, not
// hostd's); this UI shows the latest invocation's.
// The Cards' states in the current run, which notifications then update.
auto ProjectSession::ReadInvocations() -> void {
  if (run_id_.empty()) return;
  const std::weak_ptr<ProjectSession> weak = weak_from_this();
  R::RunLatestInvocationsRequestT request;
  request.run_id = run_id_;
  ProjectCall<R::InvocationPage>(
      std::move(request),
      [weak, run = run_id_](const Reply<R::InvocationPage>& reply) {
        const auto self = weak.lock();
        if (!self || !reply.ok() || self->run_id_ != run) return;
        for (const auto* record : *reply.value->records())
          if (const auto card = TopLevel(record->card()); !card.empty())
            self->states_[card] = record->state();
        self->Changed();
      });
}

auto ProjectSession::Notified(const R::Notification& notification) -> void {
  if (notification.subscription_id()->str() != subscription_) return;
  if (const auto seq = notification.seq(); seq.has_value()) {
    if (*seq != seq_ + 1) return Reopen();
    seq_ = *seq;
  }
  switch (notification.value_type()) {
    case R::NotificationPayload::DocPatched:
      // The document changed: read it again rather than apply the delta.
      return Reopen();
    case R::NotificationPayload::RunEvent:
      return RunEvent(*notification.value_as_RunEvent());
    case R::NotificationPayload::LogAvailable: {
      const auto* log = notification.value_as_LogAvailable();
      LogNotice notice{log->run_id()->str(), TopLevel(log->card()),
                       log->stream(), std::nullopt};
      if (log->end_offset().has_value()) notice.end_offset = *log->end_offset();
      if (on_log) on_log(notice);
      return;
    }
    default:
      return;
  }
}

auto ProjectSession::RunEvent(const R::RunEvent& event) -> void {
  switch (event.value_type()) {
    case R::RunEventValue::RunAcceptedEvent:
      states_.clear();
      SetRun(*event.value_as_RunAcceptedEvent()->status());
      break;
    case R::RunEventValue::RunStatusChanged:
      SetRun(*event.value_as_RunStatusChanged()->status());
      break;
    case R::RunEventValue::RunCardChanged: {
      if (event.run_id()->str() != run_id_) return;
      const auto* invocation = event.value_as_RunCardChanged()->invocation();
      if (const auto card = TopLevel(invocation->card()); !card.empty())
        states_[card] = invocation->state();
      break;
    }
    default:
      return;
  }
  Changed();
}

auto ProjectSession::SetRun(const R::RunStatus& status) -> void {
  const auto run = Str(status.run_id());
  if (run != run_id_) states_.clear();
  run_id_ = run;
  run_state_ = status.state();
}

auto ProjectSession::Run(const std::string& card) -> void {
  const std::weak_ptr<ProjectSession> weak = weak_from_this();
  R::RunRequestT request;
  request.base_rev = rev_;
  request.op_id = OperationId();
  request.origin = "ui";
  request.card = CardPath(card);
  request.propagate = true;
  ProjectCall<R::RunAccepted>(
      std::move(request), [weak](const Reply<R::RunAccepted>& reply) {
        const auto self = weak.lock();
        if (!self) return;
        if (!reply.ok()) {
          self->message_ = L("失敗: {0}", {reply.error});
        } else {
          self->message_.clear();
          self->SetRun(*reply.value->run());
        }
        self->Changed();
      });
}

auto ProjectSession::Cancel() -> void {
  if (run_id_.empty()) return;
  const std::weak_ptr<ProjectSession> weak = weak_from_this();
  R::RunCancelRequestT request;
  request.run_id = run_id_;
  ProjectCall<R::RunStatus>(std::move(request),
                            [weak](const Reply<R::RunStatus>& reply) {
                              const auto self = weak.lock();
                              if (!self || !reply.ok()) return;
                              self->SetRun(*reply.value);
                              self->Changed();
                            });
}

auto ProjectSession::ReadLog(
    const std::string& card, D::LogStream stream, const LogRequest& request,
    std::function<void(std::optional<LogPiece>, std::string error)> done)
    -> void {
  if (run_id_.empty() || !connection_) return done(std::nullopt, "");
  R::LogRangeRequestT range;
  range.run_id = run_id_;
  range.card = CardPath(card);
  range.stream = stream;
  range.limit_bytes = request.limit_bytes;
  if (request.tail) range.tail = true;
  if (request.before != 0) range.before = request.before;
  if (!request.tail && request.before == 0) range.offset = request.offset;
  ProjectCall<R::LogSlice>(
      std::move(range),
      [done = std::move(done)](const Reply<R::LogSlice>& reply) {
        if (!reply.ok()) return done(std::nullopt, reply.error);
        const auto& slice = *reply.value;
        LogPiece piece;
        piece.text = slice.text()->str();
        piece.offset = slice.offset().value_or(0);
        piece.next_offset =
            slice.next_offset().value_or(piece.offset + piece.text.size());
        piece.size_bytes = slice.size_bytes().value_or(piece.next_offset);
        piece.exists = slice.exists().value_or(false);
        done(std::move(piece), "");
      });
}

}  // namespace tsugumi::linux_ui
