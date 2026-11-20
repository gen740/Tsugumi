#pragma once

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "agent/provider.hh"
#include "host/authorization.hh"
#include "host/state_gate.hh"

namespace tsugumi::host {
class Settings;
}
namespace tsugumi::host {
struct Access;
}
namespace tsugumi::agent {
struct ConversationStore;
struct Providers;
struct Tools;
struct Turns;
struct Helpers {
  struct Dependencies {
    std::function<std::string(std::string)> scrub;
    std::function<void(const std::string&, const std::string&)> event;
  };
  Helpers(host::StateGate& gate, ConversationStore& conversations,
          Providers& providers, host::Settings& settings, Tools& tools,
          Turns& turns, host::Access& access, Dependencies dependencies);
  static auto LastAnswer(const std::vector<Message>& messages) -> Message;
  static auto HelperResult(const std::string& output,
                           const std::string& conversation,
                           const std::string& call, size_t task) -> std::string;
  // Original messages displaced by a checkpoint, excluding model summaries.
  static auto Displaced(const std::vector<Message>& previous,
                        const std::vector<Message>& saved)
      -> std::vector<Message>;
  auto RunHelper(
      const std::string& conversation, const std::string& parent_call,
      size_t index, const std::string& task, std::stop_token stop,
      const std::optional<host::TurnAuthorization>& authorization = {})
      -> std::string;

  std::recursive_mutex& mutex;
  ConversationStore* conversations;
  Providers* providers;
  host::Settings* host_settings;
  Tools* tools;
  Turns* turns;
  host::Access& access_service;
  Dependencies dependencies;
  std::map<std::tuple<std::string, std::string, size_t>,
           std::weak_ptr<std::mutex>>
      helper_gates;
};
}  // namespace tsugumi::agent
