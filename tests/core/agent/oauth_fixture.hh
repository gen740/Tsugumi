#pragma once
#include <condition_variable>
#include <mutex>

#include "fixture.hh"
#include "oauth_provider.hh"

namespace tsugumi::test {
inline auto SettingsGet(client::Connection& host,
                        const std::string& profile = "desktop") -> Reply {
  return Call(host, H::RequestPayload::SettingsGetRequest, [&](Builder& b) {
    return H::CreateSettingsGetRequest(b, b.CreateString(profile)).Union();
  });
}
inline auto SettingsPatch(client::Connection& host, const std::string& profile,
                          const std::map<std::string, std::string>& change)
    -> Reply {
  const auto before = SettingsGet(host, profile);
  const auto revision =
      before.HostValue()->value_as_Settings()->revision()->str();
  return Call(host, H::RequestPayload::SettingsUpdateRequest, [&](Builder& b) {
    std::vector<flatbuffers::Offset<H::Setting>> changes;
    for (const auto& [k, v] : change)
      changes.push_back(
          H::CreateSetting(b, b.CreateString(k), b.CreateString(v)));
    return H::CreateSettingsUpdateRequest(b, b.CreateString(profile),
                                          b.CreateString(revision),
                                          b.CreateVector(changes))
        .Union();
  });
}
class OAuthServer : public OAuthProvider {
 public:
  auto Begin(client::Connection& host, const std::string& profile = "desktop",
             bool replace_pending = false) -> Reply {
    return Call(host, H::RequestPayload::SignInBeginRequest, [&](Builder& b) {
      return H::CreateSignInBeginRequest(b, b.CreateString(profile), 0, false,
                                         b.CreateString(server.URL()),
                                         b.CreateString(resource),
                                         replace_pending)
          .Union();
    });
  }
  auto Status(client::Connection& host, const std::string& attempt,
              const std::string& profile = "desktop") -> Reply {
    return Call(host, H::RequestPayload::SignInStatusRequest, [&](Builder& b) {
      return H::CreateSignInStatusRequest(b, b.CreateString(profile),
                                          b.CreateString(attempt))
          .Union();
    });
  }
  auto SignIn(client::Connection& host, const std::string& endpoint,
              const std::string& profile = "desktop", uint64_t expiry = 3600,
              std::string client = {}, bool plan = true) -> Reply {
    resource = endpoint;
    struct Events {
      std::mutex mutex;
      std::condition_variable changed;
      uint64_t count = 0;
    };
    const auto events = std::make_shared<Events>();
    host.OnEvent([events, profile](const client::Bytes& bytes) {
      const auto* event = H::GetMessage(bytes.data())->payload_as_Event();
      const auto* changed = event ? event->value_as_SignInChanged() : nullptr;
      if (!changed || changed->profile()->string_view() != profile) return;
      std::lock_guard lock(events->mutex);
      ++events->count;
      events->changed.notify_one();
    });
    // A second sign-in on the same server must check its own exchange.
    const auto first_request = server.Requests().size();
    const auto begin = Begin(host, profile);
    if (!begin.Code().empty()) throw std::runtime_error(begin.Code());
    const auto* info = begin.HostValue()->value_as_SignInBegin();
    {
      std::unique_lock lock(events->mutex);
      if (!events->changed.wait_for(lock, 5s, [&] { return events->count != 0; }))
        throw std::runtime_error("test sign-in begin event timed out");
    }
    if (client.empty()) client = "client-" + profile;
    const auto fields = Authorize(info->url()->str(), client, expiry, plan);
    // Wake on Host notifications. Bound the fixture's wait so a missing
    // terminal notification fails the test instead of hanging the suite.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    uint64_t seen = 1;
    while (true) {
      {
        std::unique_lock lock(events->mutex);
        if (!events->changed.wait_until(
                lock, deadline, [&] { return events->count != seen; }))
          throw std::runtime_error("test sign-in event timed out");
        seen = events->count;
      }
      auto status = Status(host, info->attempt_id()->str(), profile);
      const auto state =
          status.HostValue()->value_as_SignInStatus()->state()->str();
      if (state != "pending" && state != "committing") {
        const auto exchange =
            URLFields("?" + server.Requests().at(first_request));
        const auto& verifier = exchange.at("code_verifier");
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned length = 0;
        EVP_Digest(verifier.data(), verifier.size(), digest, &length,
                   EVP_sha256(), nullptr);
        if (fields.at("code_challenge_method") != "S256" ||
            Base64URL({reinterpret_cast<const char*>(digest), length}) !=
                fields.at("code_challenge") ||
            exchange.at("redirect_uri") != fields.at("redirect_uri") ||
            exchange.at("client_id") != client ||
            exchange.at("resource") != resource)
          throw std::runtime_error(
              "test OAuth exchange did not bind PKCE and registration");
        return status;
      }
    }
  }
  std::string resource;
};
}  // namespace tsugumi::test
